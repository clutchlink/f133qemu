/*
 * Allwinner sun6i-style DMA controller (F133 / sun20iw1, "dma@3002000")
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Register map from linux drivers/dma/sun6i-dma.c (the D1/F133 uses the
 * sun50i-a100 variant of that IP).  Simplified model for the radio
 * firmware emulation: full register file, and LLI descriptor chains are
 * executed synchronously on the channel ENABLE write, byte by byte
 * through the system address space.  That is enough for the firmware,
 * which starts a channel and then polls DMA_STAT (0x30) / the
 * requestor's transfer-complete flag rather than relying on paced DRQ
 * lines:
 *
 *   0x000/0x004  IRQ_EN(0/1)    per-channel interrupt enables
 *   0x010/0x014  IRQ_STAT(0/1)  interrupt status, write-1-to-clear;
 *                               bits 4*chan+1/+2 = package/queue done
 *   0x028        GATE           channel clock gating (storage)
 *   0x030        STAT           per-channel busy bits (0 = idle/done)
 *   0x100+ch*0x40 channel registers:
 *        +0x00  ENABLE  bit0 = start; writing it runs the LLI chain
 *        +0x04  PAUSE
 *        +0x08  LLI_ADDR  physical address of the first descriptor
 *        +0x0c  CUR_CFG / +0x10 CUR_SRC / +0x14 CUR_DST / +0x18 CUR_CNT
 *        +0x1c  CUR_PARA
 *
 * Descriptor in guest memory (struct sun6i_dma_lli, 24 bytes LE):
 *   cfg, src, dst, len, para, p_lli_next
 * A p_lli_next of 0xfffff800 (LLI_LAST_ITEM, what boot0 writes as -0x800)
 * ends the chain.  A src or dst inside a peripheral window (e.g. the
 * SPI0 RX fifo data port at 0x04025300) transfers through that device's
 * MMIO handlers, so a SPI read DMA drains the RX fifo - each pop lets
 * the SPI controller clock more bytes - exactly like the real DRQ path.
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/memory.h"
#include "system/address-spaces.h"
#include "system/dma.h"
#include "trace.h"
#include "hw/dma/sunxi_dma.h"

/* common registers */
#define REG_IRQ_EN0    (0x000 / 4)
#define REG_IRQ_EN1    (0x004 / 4)
#define REG_IRQ_STAT0  (0x010 / 4)
#define REG_IRQ_STAT1  (0x014 / 4)
#define REG_STAT       (0x030 / 4)

/* per-channel register offsets */
#define CH_ENABLE   0x00
#define CH_PAUSE    0x04
#define CH_LLI_ADDR 0x08
#define CH_CFG      0x0c
#define CH_SRC      0x10
#define CH_DST      0x14
#define CH_CNT      0x18
#define CH_PARA     0x1c

#define CH_REGS_BASE  0x100
#define CH_REGS_SIZE  0x40

#define CH_START      0x1

#define LLI_SIZE      24
#define LLI_LAST      0xfffff800u
#define LLI_CHAIN_MAX 1024

/* interrupt status layout: 4 bits per channel, bit1 of each group is the
 * package/chain-done flag (HALF=0 PKG=1 QUEUE=2) */
static void sunxi_dma_update_irq(SunxiDMAState *s)
{
    uint32_t pend = (s->irq_stat[0] & s->irq_en[0]) |
                    (s->irq_stat[1] & s->irq_en[1]);
    qemu_set_irq(s->irq, pend != 0);
}

/* Is addr inside a memory region (SRAM/DRAM)?  Everything else is a
 * peripheral window: the io-mode side of a transfer is a fifo port and is
 * re-accessed at its base address - the controller does not increment it
 * (boot0's SPI0 read DMA: src=0x04025300 RX window, dst=DRAM). */
static bool sunxi_dma_addr_is_mem(uint64_t addr)
{
    return (addr >= 0x20000 && addr < 0x40000) || addr >= 0x40000000;
}

/* Copy len bytes src->dst through the system address space.  Byte-wide
 * accesses: a peripheral window (SPI fifo) hands over exactly one byte
 * per access, RAM accepts anything. */
static void sunxi_dma_memcopy(SunxiDMAState *s, uint64_t src, uint64_t dst,
                              uint64_t len)
{
    bool src_mem = sunxi_dma_addr_is_mem(src);
    bool dst_mem = sunxi_dma_addr_is_mem(dst);
    uint64_t off;

    for (off = 0; off < len; off++) {
        uint8_t byte;
        dma_memory_read(&address_space_memory, src_mem ? src + off : src,
                        &byte, 1, MEMTXATTRS_UNSPECIFIED);
        dma_memory_write(&address_space_memory, dst_mem ? dst + off : dst,
                         &byte, 1, MEMTXATTRS_UNSPECIFIED);
    }
}

static void sunxi_dma_exec(SunxiDMAState *s, int ch)
{
    SunxiDMAChannel *c = &s->chan[ch];
    uint64_t addr = c->lli_addr;
    unsigned nseg = 0;

    s->stat |= 1u << ch;

    while (nseg++ < LLI_CHAIN_MAX && (addr & 3) == 0) {
        uint32_t lli[LLI_SIZE / 4];
        uint64_t src, dst, len, next;
        uint32_t peek = 0;

        dma_memory_read(&address_space_memory, addr, lli, sizeof(lli),
                        MEMTXATTRS_UNSPECIFIED);
        src  = lli[1];
        dst  = lli[2];
        len  = lli[3];
        next = lli[5];

        c->cur_src = lli[1];
        c->cur_dst = lli[2];
        c->cur_cnt = lli[3];

        sunxi_dma_memcopy(s, src, dst, len);

        c->cur_src = src + len;
        c->cur_dst = dst + len;
        c->cur_cnt = 0;

        /* peek at the destination for the trace log, but only when it is
         * memory: a peek at a peripheral window would have side effects */
        if (sunxi_dma_addr_is_mem(dst)) {
            dma_memory_read(&address_space_memory, dst, &peek, sizeof(peek),
                            MEMTXATTRS_UNSPECIFIED);
        }
        trace_sunxi_dma_exec(ch, addr, lli[0], (uint32_t)src, (uint32_t)dst,
                             (uint32_t)len, (uint32_t)next, peek);

        if (next == 0 || (next & LLI_LAST) == LLI_LAST) {
            break;
        }
        addr = next;
    }

    s->stat &= ~(1u << ch);
    c->enable &= ~CH_START;

    /* The last descriptor completes both its package and the queue. */
    s->irq_stat[ch / 8] |= 3u << ((ch % 8) * 4 + 1);
    sunxi_dma_update_irq(s);
}

static uint64_t sunxi_dma_read(void *opaque, hwaddr addr, unsigned size)
{
    SunxiDMAState *s = opaque;
    uint32_t val = 0;

    if (addr < CH_REGS_BASE) {
        switch (addr) {
        case 0x000:
            val = s->irq_en[0];
            break;
        case 0x004:
            val = s->irq_en[1];
            break;
        case 0x010:
            val = s->irq_stat[0];
            break;
        case 0x014:
            val = s->irq_stat[1];
            break;
        case 0x030:
            val = s->stat;
            break;
        default:
            val = s->regs[addr / 4];
            break;
        }
    } else if (addr >= CH_REGS_BASE &&
               addr < CH_REGS_BASE + CH_REGS_SIZE * SUNXI_DMA_NCHAN) {
        int ch = (addr - CH_REGS_BASE) / CH_REGS_SIZE;
        SunxiDMAChannel *c = &s->chan[ch];

        switch ((addr - CH_REGS_BASE) % CH_REGS_SIZE) {
        case CH_ENABLE:
            val = c->enable;
            break;
        case CH_PAUSE:
            val = c->pause;
            break;
        case CH_LLI_ADDR:
            val = c->lli_addr;
            break;
        case CH_CFG:
            val = c->cfg;
            break;
        case CH_SRC:
            val = c->cur_src;
            break;
        case CH_DST:
            val = c->cur_dst;
            break;
        case CH_CNT:
            val = c->cur_cnt;
            break;
        case CH_PARA:
            val = c->para;
            break;
        default:
            break;
        }
    }

    trace_sunxi_dma_read((uint32_t)addr, val);
    return val;
}

static void sunxi_dma_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    SunxiDMAState *s = opaque;

    trace_sunxi_dma_write((uint32_t)addr, (uint32_t)val);

    if (addr < CH_REGS_BASE) {
        switch (addr) {
        case 0x000:
            s->irq_en[0] = val;
            sunxi_dma_update_irq(s);
            return;
        case 0x004:
            s->irq_en[1] = val;
            sunxi_dma_update_irq(s);
            return;
        case 0x010:
            s->irq_stat[0] &= ~val;
            sunxi_dma_update_irq(s);
            return;
        case 0x014:
            s->irq_stat[1] &= ~val;
            sunxi_dma_update_irq(s);
            return;
        default:
            s->regs[addr / 4] = val;
            return;
        }
    }
    if (addr >= CH_REGS_BASE &&
        addr < CH_REGS_BASE + CH_REGS_SIZE * SUNXI_DMA_NCHAN) {
        int ch = (addr - CH_REGS_BASE) / CH_REGS_SIZE;
        SunxiDMAChannel *c = &s->chan[ch];

        switch ((addr - CH_REGS_BASE) % CH_REGS_SIZE) {
        case CH_ENABLE:
            c->enable = val;
            if ((val & CH_START) && !(s->stat & (1u << ch))) {
                sunxi_dma_exec(s, ch);
            }
            return;
        case CH_PAUSE:
            c->pause = val;
            return;
        case CH_LLI_ADDR:
            c->lli_addr = val;
            return;
        case CH_CFG:
            c->cfg = val;
            return;
        case CH_SRC:
            c->cur_src = val;
            return;
        case CH_DST:
            c->cur_dst = val;
            return;
        case CH_CNT:
            c->cur_cnt = val;
            return;
        case CH_PARA:
            c->para = val;
            return;
        default:
            return;
        }
    }
}

static const MemoryRegionOps sunxi_dma_ops = {
    .read = sunxi_dma_read,
    .write = sunxi_dma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void sunxi_dma_reset(DeviceState *dev)
{
    SunxiDMAState *s = SUNXI_DMA(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->chan, 0, sizeof(s->chan));
    s->irq_en[0] = s->irq_en[1] = 0;
    s->irq_stat[0] = s->irq_stat[1] = 0;
    s->stat = 0;
}

static void sunxi_dma_realize(DeviceState *dev, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    SunxiDMAState *s = SUNXI_DMA(dev);

    sysbus_init_irq(sbd, &s->irq);

    memory_region_init_io(&s->iomem, OBJECT(s), &sunxi_dma_ops, s,
                          TYPE_SUNXI_DMA, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
}

static void sunxi_dma_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = sunxi_dma_realize;
    device_class_set_legacy_reset(dc, sunxi_dma_reset);
}

static const TypeInfo sunxi_dma_info = {
    .name = TYPE_SUNXI_DMA,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SunxiDMAState),
    .class_init = sunxi_dma_class_init,
};

static void sunxi_dma_register_types(void)
{
    type_register_static(&sunxi_dma_info);
}

type_init(sunxi_dma_register_types)
