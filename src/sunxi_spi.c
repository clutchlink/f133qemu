/*
 * Allwinner sun6i-style SPI controller (as used by the Allwinner F133 /
 * sun20iw1 SPI0/SPI1) — QEMU model for the radio firmware emulation.
 *
 * Register map matches the sun6i-a31/d1 layout (see Linux spi-sun6i.c);
 * semantics were cross-checked against the firmware's SPI flash driver
 * (boot0 spinor): see the project notes.  Highlights:
 *   0x04 GCR   bit31 SRST (resets fifos, self-clearing), bit0 EN, bit1 MASTER
 *   0x08 TCR   bit31 XCH (start transfer, self-clearing), bit7 SS level,
 *              bit8 DHB (discard the first, "hot" received byte)
 *   0x10 INT_CTL  interrupt enable (plain storage)
 *   0x14 INT_STA  interrupt status: write-1-to-clear; bit12 TC = transfer
 *                 complete (set by hardware when XCH finishes).  boot0
 *                 clears it with a write of 0x3f77 and then checks that
 *                 the error flags (bits[10:8]) read back as 0.
 *   0x18 FIFO_CTL bit15 RF_RST / bit31 TF_RST reset the FIFOs (self-clearing)
 *   0x1c FSR   [7:0] RX fifo level, [15:8] TX fifo level
 *   0x30 CNT   (BURST_CNT) transfer byte count, counts down to 0
 *   0x200+     TX fifo window (byte accesses push)
 *   0x300+     RX fifo window (byte accesses pop)
 *
 * CS is hardware-managed: one XCH burst == one CS frame.  CS asserts when
 * XCH starts the burst and deasserts when BURST_CNT completes (TC).  This
 * matters: SPI NOR models (m25p80) only resynchronise to a command byte on
 * CS deassert, so holding CS across bursts swallows the next command as
 * stream data.  Real hardware must behave the same way - otherwise the
 * shipping boot0 could never re-read flash after its 6-byte-wide JEDEC id
 * read (only 4 bytes are clocked, leaving the chip mid-id-response).
 *
 * XCH with an empty TX fifo defers the transfer until data is pushed —
 * the firmware writes TCR.XCH before pushing the flash command byte.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/ssi/ssi.h"
#include "qemu/fifo8.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "trace.h"
#include "hw/ssi/sunxi_spi.h"

/* GCR */
#define SUNXI_SPI_GCR_SRST   (1u << 31)
#define SUNXI_SPI_GCR_TP_EN  (1u << 7)
#define SUNXI_SPI_GCR_MASTER (1u << 1)
#define SUNXI_SPI_GCR_EN     (1u << 0)

/* TCR */
#define SUNXI_SPI_TCR_XCH    (1u << 31)
#define SUNXI_SPI_TCR_DHB    (1u << 8)   /* discard first ("hot") rx byte */
#define SUNXI_SPI_TCR_SS     (1u << 7)

/* INT_STA (0x14) bits: transfer complete; write-1-to-clear */
#define SUNXI_SPI_INT_STA_TC     (1u << 12)

/* FIFO_CTL (0x18) reset bits */
#define SUNXI_SPI_FIFO_CTL_RF_RST (1u << 15)
#define SUNXI_SPI_FIFO_CTL_TF_RST (1u << 31)

#define FIFO_CAPACITY        64

#define REG_GCR   (0x04 / 4)
#define REG_TCR   (0x08 / 4)
#define REG_IER   (0x0c / 4)
#define REG_INTCTL (0x10 / 4)   /* interrupt enable */
#define REG_INTSTA (0x14 / 4)   /* interrupt status (W1C) */
#define REG_FIFOCTL (0x18 / 4)
#define REG_FSR   (0x1c / 4)
#define REG_WCR   (0x20 / 4)
#define REG_DMACTL (0x24 / 4)
#define REG_DMASTA (0x28 / 4)
#define REG_CCA   (0x2c / 4)
#define REG_CNT   (0x30 / 4)
#define REG_XMIT_CNT (0x34 / 4)
#define REG_BC2   (0x38 / 4)
#define NREGS     (0x40 / 4)

static void sunxi_spi_cs_update(SunxiSPIState *s)
{
    /* Hardware-managed SS: CS is low (active) only while an XCH burst is
     * in flight.  Deasserting when BURST_CNT hits zero gives the slave a
     * clean frame boundary before the next command. */
    bool active = (s->regs[REG_GCR] & SUNXI_SPI_GCR_EN) &&
                  (s->regs[REG_TCR] & SUNXI_SPI_TCR_SS) &&
                  s->xfer_active;

    if (active != s->cs_active) {
        s->cs_active = active;
        qemu_set_irq(s->cs_lines[0], !active);
    }
}

static void sunxi_spi_reset_fifo(SunxiSPIState *s)
{
    fifo8_reset(&s->tx_fifo);
    fifo8_reset(&s->rx_fifo);
}

/*
 * Pump the current transfer: clock bytes while the TX side has data (up to
 * XMIT_CNT real bytes), then clock dummy zeros until BURST_CNT (CNT) total
 * bytes are done.  The pump stalls when the RX fifo is full and resumes when
 * the driver drains it (RX window reads call back in).  TCR.DHB discards the
 * received bytes of the transmit ("hot") phase and, for quad reads, the
 * single-lane dummy phase counted in BC2. Those RX bytes are garbage
 * (boot0 read-id: XMIT=1/BURST=4 -> 3 id bytes; fast read: XMIT=5/BURST=517
 * -> 512 data bytes).
 */
static void sunxi_spi_run_transfer(SunxiSPIState *s)
{
    while (s->xfer_active && s->xfer_remaining &&
           !fifo8_is_full(&s->rx_fifo)) {
        uint8_t tx = 0, rx;
        bool xmit_phase = s->regs[REG_XMIT_CNT] > 0;

        if (xmit_phase) {
            if (fifo8_is_empty(&s->tx_fifo)) {
                /* transmit side not filled yet: wait for the driver */
                break;
            }
            tx = fifo8_pop(&s->tx_fifo);
            s->regs[REG_XMIT_CNT]--;
        }
        rx = ssi_transfer(s->spi, tx);
        if (!(s->xfer_dhb && s->xfer_discard_remaining)) {
            fifo8_push(&s->rx_fifo, rx);
        }
        if (s->xfer_discard_remaining) {
            s->xfer_discard_remaining--;
        }
        s->xfer_remaining--;
    }
    s->regs[REG_CNT] = s->xfer_remaining;

    if (s->xfer_active && s->xfer_remaining == 0) {
        s->xfer_active = false;
        s->int_status |= SUNXI_SPI_INT_STA_TC;
        /* frame over: release CS so the slave resyncs for the next one */
        sunxi_spi_cs_update(s);
    }
}

static void sunxi_spi_tcr_write(SunxiSPIState *s, uint32_t val)
{
    s->regs[REG_TCR] = val;

    if (val & SUNXI_SPI_TCR_XCH) {
        s->regs[REG_TCR] &= ~SUNXI_SPI_TCR_XCH;
        /* start a new burst: BURST_CNT total bytes, XMIT_CNT from the TX
         * fifo (the rest are clocked as dummy zeros) */
        s->xfer_active = true;
        s->xfer_dhb = (val & SUNXI_SPI_TCR_DHB) != 0;
        s->xfer_remaining = s->regs[REG_CNT];
        s->xfer_discard_remaining = s->regs[REG_XMIT_CNT];
        /* Observed quad reads count the dummy byte in BC2, after the TX
         * FIFO bytes. Other modes use the transmit count as before. */
        if (s->regs[REG_BC2] & (1u << 29)) {
            s->xfer_discard_remaining = MAX(s->xfer_discard_remaining,
                                            s->regs[REG_BC2] & 0x00ffffff);
        }
        s->int_status &= ~SUNXI_SPI_INT_STA_TC;
        /* assert CS for the new frame before any byte is clocked */
        sunxi_spi_cs_update(s);
        sunxi_spi_run_transfer(s);
    }
    sunxi_spi_cs_update(s);
}

static uint64_t sunxi_spi_read(void *opaque, hwaddr addr, unsigned size)
{
    SunxiSPIState *s = opaque;
    uint32_t val;

    switch (addr) {
    case 0x14:  /* INT_STA */
        val = s->int_status;
        break;
    case 0x1c:  /* FSR */
        val = (fifo8_num_used(&s->tx_fifo) << 8) |
               fifo8_num_used(&s->rx_fifo);
        break;
    case 0x28:  /* DMA_STA */
        val = 0;
        break;
    default:
        if (addr >= 0x200 && addr < 0x240) {
            val = fifo8_is_empty(&s->tx_fifo) ? 0 : fifo8_pop(&s->tx_fifo);
            break;
        }
        if (addr >= 0x300 && addr < 0x340) {
            /* RX fifo window: every access pops a byte.  Clock more first
             * if the fifo ran dry mid-burst (the DMA pump can get ahead),
             * then refill the slot we are about to free. */
            if (fifo8_is_empty(&s->rx_fifo)) {
                sunxi_spi_run_transfer(s);
            }
            val = fifo8_is_empty(&s->rx_fifo) ? 0 : fifo8_pop(&s->rx_fifo);
            sunxi_spi_run_transfer(s);
            break;
        }
        if (addr / 4 < NREGS) {
            val = s->regs[addr / 4];
            break;
        }
        val = 0;
        break;
    }

    trace_sunxi_spi_read((uint32_t)addr, (uint32_t)val);
    return val;
}

static void sunxi_spi_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    SunxiSPIState *s = opaque;

    trace_sunxi_spi_write((uint32_t)addr, (uint32_t)val);

    if (addr >= 0x200 && addr < 0x240) {
        int i;
        for (i = size - 1; i >= 0; i--) {
            if (!fifo8_is_full(&s->tx_fifo)) {
                fifo8_push(&s->tx_fifo, (val >> (8 * i)) & 0xff);
            }
        }
        /* a burst may be waiting for transmit data */
        sunxi_spi_run_transfer(s);
        return;
    }
    if (addr >= 0x300 && addr < 0x340) {
        return; /* writes to the RX window are ignored */
    }
    if (addr / 4 >= NREGS) {
        return;
    }

    switch (addr) {
    case 0x04:  /* GCR */
        s->regs[REG_GCR] = val;
        if (val & SUNXI_SPI_GCR_SRST) {
            sunxi_spi_reset_fifo(s);
            s->regs[REG_GCR] &= ~SUNXI_SPI_GCR_SRST;
            s->xfer_active = false;
        }
        sunxi_spi_cs_update(s);
        break;
    case 0x08:  /* TCR */
        sunxi_spi_tcr_write(s, val);
        break;
    case 0x10:  /* INT_CTL: plain storage (interrupt enable mask) */
        s->regs[addr / 4] = val;
        break;
    case 0x14:  /* INT_STA: write-1-to-clear */
        s->int_status &= ~val;
        break;
    case 0x18:  /* FIFO_CTL: reset bits self-clear */
        if (val & SUNXI_SPI_FIFO_CTL_RF_RST) {
            fifo8_reset(&s->rx_fifo);
        }
        if (val & SUNXI_SPI_FIFO_CTL_TF_RST) {
            fifo8_reset(&s->tx_fifo);
        }
        s->regs[addr / 4] = val & ~(SUNXI_SPI_FIFO_CTL_RF_RST |
                                    SUNXI_SPI_FIFO_CTL_TF_RST);
        break;
    case 0x30:  /* CNT */
        s->regs[REG_CNT] = val;
        break;
    default:
        s->regs[addr / 4] = val;
        break;
    }
}

static const MemoryRegionOps sunxi_spi_ops = {
    .read = sunxi_spi_read,
    .write = sunxi_spi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void sunxi_spi_reset(DeviceState *dev)
{
    SunxiSPIState *s = SUNXI_SPI(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->int_status = 0;
    sunxi_spi_reset_fifo(s);
    s->xfer_active = false;
    s->xfer_dhb = false;
    s->xfer_remaining = 0;
    s->xfer_discard_remaining = 0;
    s->cs_active = false;
}

static void sunxi_spi_realize(DeviceState *dev, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    SunxiSPIState *s = SUNXI_SPI(dev);

    s->spi = ssi_create_bus(dev, "spi");
    sysbus_init_irq(sbd, &s->irq);

    s->cs_lines = g_new0(qemu_irq, 1);
    sysbus_init_irq(sbd, &s->cs_lines[0]);
    qemu_set_irq(s->cs_lines[0], 1);   /* CS released (active low) */

    memory_region_init_io(&s->mmio, OBJECT(s), &sunxi_spi_ops, s,
                          TYPE_SUNXI_SPI, 0x1000);
    sysbus_init_mmio(sbd, &s->mmio);

    fifo8_create(&s->tx_fifo, FIFO_CAPACITY);
    fifo8_create(&s->rx_fifo, FIFO_CAPACITY);
}

static void sunxi_spi_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = sunxi_spi_realize;
    device_class_set_legacy_reset(dc, sunxi_spi_reset);
}

static const TypeInfo sunxi_spi_info = {
    .name = TYPE_SUNXI_SPI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SunxiSPIState),
    .class_init = sunxi_spi_class_init,
};

static void sunxi_spi_register_types(void)
{
    type_register_static(&sunxi_spi_info);
}

type_init(sunxi_spi_register_types)
