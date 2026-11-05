/*
 * QEMU Allwinner F133 (sun20iw1) radio firmware emulation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Boots the radio head-unit firmware: boot0 -> OpenSBI -> Melis RTOS.
 *
 * The SoC implements the peripherals the firmware actually touches; all
 * others are registered as unimplemented stubs so accesses are logged.
 * Peripheral base addresses come from the linux D1 device trees plus
 * addresses observed in the firmware (see notes/mmio.md).
 */

#include "qemu/osdep.h"
#include "cpu-qom.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/core/loader.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/usb/hcd-ehci.h"
#include "hw/i2c/i2c.h"
#include "hw/i2c/allwinner-i2c.h"
#include "ui/input.h"
#include "hw/intc/riscv_aclint.h"
#include "hw/intc/sifive_plic.h"
#include "hw/char/serial-mm.h"
#include "hw/misc/unimp.h"
#include "hw/ssi/ssi.h"
#include "hw/ssi/sunxi_spi.h"
#include "hw/dma/sunxi_dma.h"
#include "system/blockdev.h"
#include "hw/riscv/f133.h"
#include "hw/riscv/boot.h"
#include "hw/riscv/machines-qom.h"
#include "hw/riscv/riscv_hart.h"
#include "system/system.h"
#include "system/memory.h"
#include "system/address-spaces.h"
#include "ui/pixel_ops.h"
#include "target/riscv/cpu.h"

/* BROM hands off to boot0 loaded in SRAM */
#define F133_SRAM_BASE  f133_memmap[F133_DEV_SRAM].base
#define F133_SRAM_SIZE  f133_memmap[F133_DEV_SRAM].size

static void f133_soc_init(Object *obj);

const MemMapEntry f133_memmap[] = {
    [F133_DEV_SRAM] =       { 0x00020000,  0x00028000 },
    /* DRAM is 64 MB: OpenSBI's boot page tables map exactly 32 x 2 MiB
     * megapages (0x40000000-0x43ffffff), and the platform config expects
     * the CLINT block immediately after DRAM at 0x44000000.  boot0's
     * report is normalized to match (see F133_MBUS_SIZE_GOOD). */
    [F133_DEV_DRAM] =       { 0x40000000,  0x04000000 },
    [F133_DEV_PIO] =        { 0x02000000,  0x00000800 },
    [F133_DEV_CCU] =        { 0x02001000,  0x00001000 },
    [F133_DEV_UNK2002000] = { 0x02002000,  0x00001000 },
    [F133_DEV_LEDC] =       { 0x02008000,  0x00000400 },
    [F133_DEV_GPADC] =      { 0x02009000,  0x00000400 },
    [F133_DEV_THS] =        { 0x02009400,  0x00000400 },
    [F133_DEV_DMIC] =       { 0x02031000,  0x00000400 },
    [F133_DEV_I2S1] =       { 0x02033000,  0x00001000 },
    [F133_DEV_I2S2] =       { 0x02034000,  0x00001000 },
    [F133_DEV_TIMER] =      { 0x02050000,  0x000000a0 },
    [F133_DEV_WDT] =        { 0x020500a0,  0x00000020 },
    [F133_DEV_UNK1C08000] = { 0x01c08000,  0x00001000 },
    [F133_DEV_UNK2030000] = { 0x02030000,  0x00001000 },
    [F133_DEV_UART0] =      { 0x02500000,  0x00000400 },
    [F133_DEV_UART1] =      { 0x02500400,  0x00000400 },
    [F133_DEV_UART2] =      { 0x02500800,  0x00000400 },
    [F133_DEV_UART3] =      { 0x02500c00,  0x00000400 },
    [F133_DEV_UART4] =      { 0x02501000,  0x00000400 },
    [F133_DEV_UART5] =      { 0x02501400,  0x00000400 },
    [F133_DEV_I2C0] =       { 0x02502000,  0x00000400 },
    [F133_DEV_I2C1] =       { 0x02502400,  0x00000400 },
    [F133_DEV_I2C2] =       { 0x02502800,  0x00000400 },
    [F133_DEV_I2C3] =       { 0x02502c00,  0x00000400 },
    [F133_DEV_CAN0] =       { 0x02504000,  0x00000400 },
    [F133_DEV_CAN1] =       { 0x02504400,  0x00000400 },
    [F133_DEV_SYSCON] =     { 0x03000000,  0x00001000 },
    [F133_DEV_DMA] =        { 0x03002000,  0x00001000 },
    [F133_DEV_SID] =        { 0x03006000,  0x00001000 },
    [F133_DEV_CRYPTO] =     { 0x03040000,  0x00000800 },
    [F133_DEV_HSTIMER] =    { 0x03008000,  0x00001000 },
    [F133_DEV_UNK300B000] = { 0x0300b000,  0x00001000 },
    [F133_DEV_MBUS] =       { 0x03102000,  0x00001000 },
    [F133_DEV_DRAMC] =      { 0x03103000,  0x00001000 },
    [F133_DEV_MMC0] =       { 0x04020000,  0x00001000 },
    [F133_DEV_MMC1] =       { 0x04021000,  0x00001000 },
    [F133_DEV_MMC2] =       { 0x04022000,  0x00001000 },
    [F133_DEV_SPI0] =       { 0x04025000,  0x00001000 },
    [F133_DEV_SPI1] =       { 0x04026000,  0x00001000 },
    [F133_DEV_USB0] =       { 0x04100000,  0x00011000 },
    [F133_DEV_USB1] =       { 0x04200000,  0x00001000 },
    [F133_DEV_EMAC] =       { 0x04500000,  0x00010000 },
    [F133_DEV_DE_CLK] =     { 0x05000000,  0x00010000 },
    [F133_DEV_MIXER0] =     { 0x05100000,  0x00100000 },
    [F133_DEV_MIXER1] =     { 0x05200000,  0x00100000 },
    [F133_DEV_DSI] =        { 0x05450000,  0x00001000 },
    [F133_DEV_DPHY] =       { 0x05451000,  0x00001000 },
    [F133_DEV_TCON_TOP] =   { 0x05460000,  0x00001000 },
    [F133_DEV_TCON_LCD0] =  { 0x05461000,  0x00001000 },
    [F133_DEV_TCON_TV0] =   { 0x05470000,  0x00001000 },
    [F133_DEV_RISCV_CFG] =  { 0x06010000,  0x00001000 },
    [F133_DEV_UNK6E20000] = { 0x06e20000,  0x00001000 },
    [F133_DEV_UNK7000000] = { 0x07000000,  0x00001000 },
    [F133_DEV_PPU] =        { 0x07001000,  0x00001000 },
    [F133_DEV_PRCM] =       { 0x07010000,  0x00001000 },
    [F133_DEV_UNK7022000] = { 0x07022000,  0x00001000 },
    [F133_DEV_RTC] =        { 0x07090000,  0x00000400 },
    [F133_DEV_PLIC] =       { 0x10000000,  0x04000000 },
    /* T-Head CLINT: the platform config baked into the firmware (runtime
     * table at 0x4000c308, observed via gdb) uses the SiFive-style layout
     * at base 0x44000000 -- msip at +0x0, mtimecmp0 at +0x4000, mtime at
     * +0xbff8 (0x2500c00 = UART3 console).  This matches OpenSBI's boot
     * page tables, which map DRAM only up to 0x44000000.  Keeping the
     * CLINT at the old 0x14000000 left mtimecmp writes landing in plain
     * DRAM: MTIP asserted forever -> S-mode interrupt storm at boot.
     * The real C906 CLINT has no MMIO mtime register, but leaving the
     * mtimer window available is harmless. */
    [F133_DEV_CLINT] =      { 0x44000000,  0x00010000 },
};

static void f133_create_plic(F133SoCState *s)
{
    g_autofree char *plic_hart_config = NULL;

    plic_hart_config = riscv_plic_hart_config_string(1);

    s->plic = sifive_plic_create(f133_memmap[F133_DEV_PLIC].base,
                                 plic_hart_config, 1, C906_CPU_HARTID,
                                 F133_PLIC_NUM_SOURCES,
                                 F133_PLIC_NUM_PRIORITIES,
                                 F133_PLIC_PRIORITY_BASE,
                                 F133_PLIC_PENDING_BASE,
                                 F133_PLIC_ENABLE_BASE,
                                 F133_PLIC_ENABLE_STRIDE,
                                 F133_PLIC_CONTEXT_BASE,
                                 F133_PLIC_CONTEXT_STRIDE,
                                 f133_memmap[F133_DEV_PLIC].size);
}

/* SID (efuse): boot0 reads the chip-id word at 0x3006200 and refuses to
 * boot ("can't support chip type %d") unless it sees 0x7400. */
static uint64_t f133_sid_read(void *opaque, hwaddr addr, unsigned size)
{
    switch (addr) {
    case 0x200:
        return 0x7400;
    default:
        return 0;
    }
}

static void f133_sid_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    /* efuse is read-only on real hardware; ignore writes */
}

static const MemoryRegionOps f133_sid_ops = {
    .read = f133_sid_read,
    .write = f133_sid_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* Minimal GPIO register storage.  This board's SD0 detect is PF6, active
 * low; an unpopulated socket leaves the input high.  This is not a complete
 * GPIO or pin-mux model. */
static uint64_t f133_pio_read(void *opaque, hwaddr addr, unsigned size)
{
    F133SoCState *s = opaque;
    uint32_t value = s->pio_regs[addr / 4];

    if (addr == 0x100 && ((s->pio_regs[0xf0 / 4] >> 24) & 7) != 1) {
        value |= 1u << 6;
    }
    return value;
}

static void f133_pio_write(void *opaque, hwaddr addr, uint64_t value,
                           unsigned size)
{
    F133SoCState *s = opaque;
    if (addr == 0x2d4) {
        s->pio_regs[addr / 4] &= ~value;
    } else {
        s->pio_regs[addr / 4] = value;
    }
    if (addr == 0x2d0 || addr == 0x2d4) {
        qemu_set_irq(s->pio_g_irq, !!(s->pio_regs[0x2d0 / 4] &
                                       s->pio_regs[0x2d4 / 4]));
    }
}

static const MemoryRegionOps f133_pio_ops = {
    .read = f133_pio_read,
    .write = f133_pio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* Minimal single-contact GT911, with a 16-bit big-endian register pointer. */
typedef struct F133Touch {
    I2CSlave parent;
    F133SoCState *soc;
    uint8_t regs[65536];
    uint16_t address;
    unsigned address_bytes;
    int x, y;
    bool down, changed;
    QEMUTimer *report_timer;
} F133Touch;

static void f133_touch_report_irq(void *opaque)
{
    F133Touch *s = opaque;
    F133SoCState *soc = s->soc;
    if (!soc || !(s->regs[0x814e] & 0x80)) {
        return;
    }
    soc->pio_regs[0x2d4 / 4] |= 1u << 7;
    qemu_set_irq(soc->pio_g_irq, !!(soc->pio_regs[0x2d0 / 4] &
                                     soc->pio_regs[0x2d4 / 4]));
    timer_mod(s->report_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 20);
}

static int f133_touch_event(I2CSlave *slave, enum i2c_event event)
{
    F133Touch *s = (F133Touch *)slave;
    if (event == I2C_START_SEND) {
        s->address_bytes = 0;
    }
    return 0;
}

static int f133_touch_send(I2CSlave *slave, uint8_t data)
{
    F133Touch *s = (F133Touch *)slave;
    if (s->address_bytes == 0) {
        s->address = (uint16_t)data << 8;
        s->address_bytes++;
    } else if (s->address_bytes == 1) {
        s->address |= data;
        s->address_bytes++;
    } else {
        s->regs[s->address++] = data;
    }
    return 0;
}

static uint8_t f133_touch_recv(I2CSlave *slave)
{
    F133Touch *s = (F133Touch *)slave;
    return s->regs[s->address++];
}

static void f133_touch_input(DeviceState *dev, QemuConsole *src,
                             QemuInputEvent *event)
{
    F133Touch *s = (F133Touch *)dev;
    if (event->type == INPUT_EVENT_KIND_ABS) {
        if (event->abs.axis == INPUT_AXIS_X) {
            s->x = event->abs.value * 1023 / 0x7fff;
        } else {
            s->y = event->abs.value * 599 / 0x7fff;
        }
        s->changed = s->down;
    } else if (event->type == INPUT_EVENT_KIND_BTN &&
               event->btn.button == INPUT_BUTTON_LEFT) {
        s->down = event->btn.down;
        s->changed = true;
    }
}

static void f133_touch_sync(DeviceState *dev)
{
    F133Touch *s = (F133Touch *)dev;
    F133SoCState *soc = s->soc;
    if (!s->changed || !soc) {
        return;
    }
    s->changed = false;
    s->regs[0x814e] = 0x80 | s->down;
    s->regs[0x814f] = 0;
    s->regs[0x8150] = s->x;
    s->regs[0x8151] = s->x >> 8;
    s->regs[0x8152] = s->y;
    s->regs[0x8153] = s->y >> 8;
    s->regs[0x8154] = 10;
    s->regs[0x8155] = 0;
    f133_touch_report_irq(s);
}

static const QemuInputHandler f133_touch_input_handler = {
    .name = "F133 GT911 touchscreen",
    .mask = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS,
    .event = f133_touch_input,
    .sync = f133_touch_sync,
};

static void f133_touch_realize(DeviceState *dev, Error **errp)
{
    F133Touch *s = (F133Touch *)dev;
    QemuInputHandlerState *handler;
    memcpy(&s->regs[0x8140], "911\0", 4);
    s->regs[0x8144] = 0x60;
    s->regs[0x8145] = 0x10;
    s->regs[0x8047] = 0x41;
    s->regs[0x8048] = 0x00;
    s->regs[0x8049] = 0x04;
    s->regs[0x804a] = 0x58;
    s->regs[0x804b] = 0x02;
    s->report_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, f133_touch_report_irq, s);
    handler = qemu_input_handler_register(dev, &f133_touch_input_handler);
    qemu_input_handler_activate(handler);
}

static void f133_touch_class_init(ObjectClass *klass, const void *data)
{
    I2CSlaveClass *i2c = I2C_SLAVE_CLASS(klass);
    DEVICE_CLASS(klass)->realize = f133_touch_realize;
    i2c->event = f133_touch_event;
    i2c->send = f133_touch_send;
    i2c->recv = f133_touch_recv;
}

/* TCON LCD register storage and an approximate 60 Hz vertical blank. */
static void f133_tcon_irq_update(F133SoCState *s)
{
    uint32_t gint = s->tcon_lcd_regs[1];
    qemu_set_irq(s->tcon_irq, ((gint >> 16) & gint & 0xffff) != 0);
}

static void f133_tcon_vblank(void *opaque)
{
    F133SoCState *s = opaque;
    s->tcon_lcd_regs[1] |= 1u << 15;
    f133_tcon_irq_update(s);
    timer_mod(s->tcon_vblank_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 17);
}

static uint64_t f133_tcon_read(void *opaque, hwaddr addr, unsigned size)
{
    F133SoCState *s = opaque;
    return s->tcon_lcd_regs[addr / 4] >> ((addr & 3) * 8);
}

static void f133_tcon_write(void *opaque, hwaddr addr, uint64_t value,
                            unsigned size)
{
    F133SoCState *s = opaque;
    unsigned shift = (addr & 3) * 8;
    uint32_t mask = (uint32_t)(0xffffffffULL >> (32 - size * 8)) << shift;
    uint32_t old = s->tcon_lcd_regs[addr / 4];
    uint32_t bits = (uint32_t)value << shift;

    if (addr / 4 == 1) {
        /* Status bits clear on writing zero; enable bits are ordinary RW. */
        uint32_t lowmask = mask & 0xffff;
        uint32_t highmask = mask & 0xffff0000;
        s->tcon_lcd_regs[1] = (old & ~(highmask | lowmask)) |
            (bits & highmask) | (old & lowmask & bits);
        f133_tcon_irq_update(s);
    } else {
        s->tcon_lcd_regs[addr / 4] = (old & ~mask) | (bits & mask);
    }
}

static const MemoryRegionOps f133_tcon_ops = {
    .read = f133_tcon_read,
    .write = f133_tcon_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

/* DE mixer register storage. Scanout below supports the first RGB888 UI
 * plane only; scaling, blending and other planes are not yet implemented. */
static uint64_t f133_mixer_read(void *opaque, hwaddr addr, unsigned size)
{
    F133SoCState *s = opaque;
    return s->mixer0_regs[addr / 4] >> ((addr & 3) * 8);
}

static void f133_mixer_write(void *opaque, hwaddr addr, uint64_t value,
                             unsigned size)
{
    F133SoCState *s = opaque;
    unsigned shift = (addr & 3) * 8;
    uint32_t mask = (uint32_t)(0xffffffffULL >> (32 - size * 8)) << shift;
    uint32_t *reg = &s->mixer0_regs[addr / 4];
    *reg = (*reg & ~mask) | (((uint32_t)value << shift) & mask);
}

static const MemoryRegionOps f133_mixer_ops = {
    .read = f133_mixer_read,
    .write = f133_mixer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static bool f133_display_update(void *opaque)
{
    F133SoCState *s = opaque;
    uint32_t attr = s->mixer0_regs[0x3000 / 4];
    uint32_t size = s->mixer0_regs[0x3004 / 4];
    uint32_t pitch = s->mixer0_regs[0x300c / 4];
    uint32_t base = s->mixer0_regs[0x3010 / 4];
    unsigned width = (size & 0x1fff) + 1;
    unsigned height = ((size >> 16) & 0x1fff) + 1;
    DisplaySurface *surface;
    g_autofree uint8_t *row = NULL;

    if (!(attr & 1) || ((attr >> 8) & 0x1f) != 8 ||
        width > 2048 || height > 2048 || pitch < width * 3 ||
        base < 0x40000000 || (uint64_t)base + (uint64_t)pitch * height >
        0x44000000) {
        return true;
    }
    surface = qemu_console_surface(s->display);
    if (surface_width(surface) != width || surface_height(surface) != height) {
        qemu_console_resize(s->display, width, height);
        surface = qemu_console_surface(s->display);
    }
    row = g_malloc(width * 3);
    for (unsigned y = 0; y < height; y++) {
        uint32_t *dst = (uint32_t *)(surface_data(surface) +
                                     y * surface_stride(surface));
        address_space_read(&address_space_memory, base + (uint64_t)y * pitch,
                           MEMTXATTRS_UNSPECIFIED, row, width * 3);
        for (unsigned x = 0; x < width; x++) {
            dst[x] = rgb_to_pixel32(row[x * 3 + 2], row[x * 3 + 1],
                                    row[x * 3]);
        }
    }
    qemu_console_update(s->display, 0, 0, width, height);
    return true;
}

static const GraphicHwOps f133_display_ops = {
    .gfx_update = f133_display_update,
};

/* CCU: minimal state-keeping model. PLL registers (window 0x000-0x100)
     * report the LOCK bit (28) once LOCK_ENABLE (29) is set — the firmware
     * polls it during "set pll start". */
#define F133_CCU_LOCK      (1u << 28)
#define F133_CCU_LOCK_EN   (1u << 29)
#define F133_CCU_REGS      0x400

static uint64_t f133_ccu_read(void *opaque, hwaddr addr, unsigned size)
{
    uint32_t *regs = opaque;
    uint32_t idx = addr / 4;
    uint64_t val = regs[idx];

    if (idx < 0x40 && (val & F133_CCU_LOCK_EN)) {
        val |= F133_CCU_LOCK;
    }
    return val;
}

static void f133_ccu_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    uint32_t *regs = opaque;

    regs[addr / 4] = val;
}

static const MemoryRegionOps f133_ccu_ops = {
    .read = f133_ccu_read,
    .write = f133_ccu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* DRAMC (0x3103000): RAM-backed registers; +0x010 bit 0 reads as 1
     * ("command done") — boot0 writes host commands (0x52/0x53, 0x520/0x5a1)
     * to +0x000 and polls this bit. */
#define F133_DRAMC_DONE (1u << 0)
#define F133_DRAMC_REGS 0x400

static uint64_t f133_dramc_read(void *opaque, hwaddr addr, unsigned size)
{
    uint32_t *regs = opaque;
    uint32_t idx = addr / 4;
    uint64_t val = regs[idx];

    if (idx == 4 || idx == 6) {
        val |= F133_DRAMC_DONE;
    }
    return val;
}

static void f133_dramc_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    uint32_t *regs = opaque;

    regs[addr / 4] = val;
}

static const MemoryRegionOps f133_dramc_ops = {
    .read = f133_dramc_read,
    .write = f133_dramc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* reads past the DRAM end: return data that never matches a pattern */
static uint64_t f133_nodram_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0xf1f0f1f0ULL | (addr & 0xfff0);
}

static void f133_nodram_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
}

static const MemoryRegionOps f133_nodram_ops = {
    .read = f133_nodram_read,
    .write = f133_nodram_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
};

/* Catch-all benign window for the whole address space below DRAM.
 *
 * Rationale (bring-up bypass, see notes/stubs.md): QEMU raises a real
 * load/store access fault (mcause=5/7, mtval=address) on accesses to fully
 * unassigned physical addresses, while on silicon such reads just return
 * open-bus garbage.  Any peripheral the firmware probes but we have not
 * modelled yet therefore kills the guest (boot0 died probing 0x50000000;
 * the Melis kernel died probing the DE33 display top block at 0x0540000c).
 * Mapping the entire low 1 GiB with a region that returns 0 / ignores
 * writes converts those faults into benign no-ops -- but it must be added
 * BEFORE every real device so that they shadow it (QEMU: the most recently
 * added equal-priority subregion wins).  We still record what the guest
 * touched: the first access to each 4 KiB page is logged to the
 * "-d unimp, guest_errors" log so we know exactly which registers the
 * kernel pokes and can model them for real later (M6/M7 display path). */
#define F133_UNIMP_LO_BASE  0x00000000
#define F133_UNIMP_LO_SIZE  0x40000000
#define F133_UNIMP_PAGES    (F133_UNIMP_LO_SIZE >> 12)
static uint8_t f133_unimp_lo_touched[F133_UNIMP_PAGES >> 3];

static void f133_unimp_lo_note(const char *op, hwaddr addr, unsigned size)
{
    unsigned page = addr >> 12;
    unsigned bit = 1u << (page & 7);

    if (!(f133_unimp_lo_touched[page >> 3] & bit)) {
        f133_unimp_lo_touched[page >> 3] |= bit;
        qemu_log_mask(LOG_UNIMP,
                      "f133: first %s of unimplemented 0x%08" HWADDR_PRIx
                      " (size %u); further accesses to this page are "
                      "silent\n", op, addr, size);
    }
}

static uint64_t f133_unimp_lo_read(void *opaque, hwaddr addr, unsigned size)
{
    f133_unimp_lo_note("read", addr, size);
    return 0;
}

static void f133_unimp_lo_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    f133_unimp_lo_note("write", addr, size);
}

static const MemoryRegionOps f133_unimp_lo_ops = {
    .read = f133_unimp_lo_read,
    .write = f133_unimp_lo_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
};

/* Sunxi UART tail (0x20..0x3ff of each 0x400 UART block).  serial_mm only
 * maps the 16550 window; the firmware/OpenSBI console code also reads the
 * USR-style status word at +0x7c (polled non-zero = "ready").  Everything
 * else RAZ/WI. */
static uint64_t f133_uart_tail_read(void *opaque, hwaddr addr, unsigned size)
{
    if (addr == 0x5c) {          /* 0x7c: USR — TFC | RFC, idle = 0x3 */
        return 0x3;
    }
    return 0;
}

static void f133_uart_tail_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    /* HALT / DMA-mode control bits: ignored */
}

static const MemoryRegionOps f133_uart_tail_ops = {
    .read = f133_uart_tail_read,
    .write = f133_uart_tail_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

/* MBUS (0x3102000): RAM-backed registers. Register 0 encodes the DRAM
     * geometry the firmware decodes as
     *   size_MB = 1 << (bits[11:8] + bits[7:4] + bits[3:2] - 14)
     * Boot0's DRAM init derives these fields from the stubbed DRAM
     * controller state, producing a bogus 4160 MB report that makes its
     * pattern test crawl.  Reads that are not the immediate read-back of a
     * write (i.e. the geometry probe) therefore get the size fields
     * normalized to 256 MB, while verify loops still see exactly what they
     * wrote. */
#define F133_MBUS_REGS 0x400
#define F133_MBUS_SIZE_MASK 0xfffu
#define F133_MBUS_SIZE_GOOD 0x8a8u   /* 8+10+2 = 20 -> 1<<6 = 64 MB, 1 ch */

static bool f133_mbus_last_was_write[F133_MBUS_REGS];

static uint64_t f133_mbus_read(void *opaque, hwaddr addr, unsigned size)
{
    uint32_t *regs = opaque;
    uint64_t val = regs[addr / 4];

    if (addr == 0 && !f133_mbus_last_was_write[addr / 4]) {
        val = (val & ~F133_MBUS_SIZE_MASK) | F133_MBUS_SIZE_GOOD;
    }
    f133_mbus_last_was_write[addr / 4] = false;
    return val;
}

static void f133_mbus_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    uint32_t *regs = opaque;

    regs[addr / 4] = val;
    f133_mbus_last_was_write[addr / 4] = true;
}

static const MemoryRegionOps f133_mbus_ops = {
    .read = f133_mbus_read,
    .write = f133_mbus_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void f133_soc_realize(DeviceState *dev, Error **errp)
{
    F133SoCState *s = RISCV_F133_SOC(dev);
    MemoryRegion *sys_mem = get_system_memory();

    /* Benign catch-all for everything below DRAM that no real device
     * claims: added FIRST so every other subregion (SRAM, CCU, UARTs, ...)
     * shadows it; without it, probes of unmodelled peripherals raise load
     * access faults and kill the guest (see f133_unimp_lo_read). */
    memset(f133_unimp_lo_touched, 0, sizeof(f133_unimp_lo_touched));
    memory_region_init_io(&s->unimp_lo, OBJECT(dev), &f133_unimp_lo_ops, NULL,
                          "f133.unimp-lo", F133_UNIMP_LO_SIZE);
    memory_region_add_subregion(sys_mem, F133_UNIMP_LO_BASE, &s->unimp_lo);

    sysbus_realize(SYS_BUS_DEVICE(&s->cpu), &error_fatal);

    /* SRAM (holds boot0 at 0x20000) */
    memory_region_init_ram(&s->sram, OBJECT(dev), "f133.sram",
                           F133_SRAM_SIZE, &error_fatal);
    memory_region_add_subregion(sys_mem, F133_SRAM_BASE, &s->sram);

    /* RTC block: the firmware uses registers here as boot-stage scratch
     * (writes a magic and reads it back), so back them with RAM. */
    memory_region_init_ram(&s->rtc_scratch, OBJECT(dev), "f133.rtc-scratch",
                           0x400, &error_fatal);
    memory_region_add_subregion(sys_mem, f133_memmap[F133_DEV_RTC].base,
                                &s->rtc_scratch);

    /* SID efuse window */
    memory_region_init_io(&s->sid, OBJECT(dev), &f133_sid_ops, NULL,
                          "f133.sid", 0x1000);
    memory_region_add_subregion(sys_mem, f133_memmap[F133_DEV_SID].base,
                                &s->sid);

    memory_region_init_io(&s->pio, OBJECT(dev), &f133_pio_ops, s,
                          "f133.pio", f133_memmap[F133_DEV_PIO].size);
    memory_region_add_subregion(sys_mem, f133_memmap[F133_DEV_PIO].base,
                                &s->pio);

    /* CCU */
    s->ccu_regs = g_new0(uint32_t, F133_CCU_REGS);
    memory_region_init_io(&s->ccu, OBJECT(dev), &f133_ccu_ops, s->ccu_regs,
                          "f133.ccu", 0x1000);
    memory_region_add_subregion(sys_mem, f133_memmap[F133_DEV_CCU].base,
                                &s->ccu);

    /* MBUS + DRAM controller */
    s->mbus_regs = g_new0(uint32_t, F133_MBUS_REGS);
    s->mbus_regs[0] = F133_MBUS_SIZE_GOOD;
    memory_region_init_io(&s->mbus, OBJECT(dev), &f133_mbus_ops, s->mbus_regs,
                          "f133.mbus", 0x1000);
    memory_region_add_subregion(sys_mem, f133_memmap[F133_DEV_MBUS].base,
                                &s->mbus);
    s->dramc_regs = g_new0(uint32_t, F133_DRAMC_REGS);
    memory_region_init_io(&s->dramc, OBJECT(dev), &f133_dramc_ops,
                          s->dramc_regs, "f133.dramc", 0x1000);
    memory_region_add_subregion(sys_mem, f133_memmap[F133_DEV_DRAMC].base,
                                &s->dramc);

    /* T-Head CLINT: msip + mtimecmp. The real C906 CLINT has no MMIO
     * mtime register, but leaving the mtimer window available is harmless
     * and lets us observe accesses. */
    riscv_aclint_swi_create(f133_memmap[F133_DEV_CLINT].base,
                            C906_CPU_HARTID, 1, false);
    riscv_aclint_mtimer_create(f133_memmap[F133_DEV_CLINT].base + 0x4000,
                               RISCV_ACLINT_DEFAULT_MTIMER_SIZE,
                               C906_CPU_HARTID, 1,
                               RISCV_ACLINT_DEFAULT_MTIMECMP,
                               RISCV_ACLINT_DEFAULT_MTIME,
                               F133_TIMEBASE_FREQ, true);

    /* PLIC */
    f133_create_plic(s);
    DeviceState *ehci1 = qdev_new("f133-ehci-usb");
    sysbus_realize_and_unref(SYS_BUS_DEVICE(ehci1), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(ehci1), 0, 0x04200000);
    sysbus_connect_irq(SYS_BUS_DEVICE(ehci1), 0, qdev_get_gpio_in(s->plic, 49));
    s->pio_g_irq = qdev_get_gpio_in(s->plic, 95); /* PG: peripheral 79 */

    DeviceState *twi3 = qdev_new("allwinner.i2c-f133");
    sysbus_realize_and_unref(SYS_BUS_DEVICE(twi3), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(twi3), 0, f133_memmap[F133_DEV_I2C3].base);
    sysbus_connect_irq(SYS_BUS_DEVICE(twi3), 0, qdev_get_gpio_in(s->plic, 28));
    F133Touch *touch = (F133Touch *)i2c_slave_create_simple(
        AW_I2C(twi3)->bus, "f133.gt911", 0x5d);
    touch->soc = s;

    s->tcon_irq = qdev_get_gpio_in(s->plic, 106); /* peripheral IRQ 90 */
    s->tcon_vblank_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                        f133_tcon_vblank, s);
    timer_mod(s->tcon_vblank_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 17);
    memory_region_init_io(&s->tcon_lcd, OBJECT(dev), &f133_tcon_ops, s,
                          "f133.tcon-lcd", 0x1000);
    memory_region_add_subregion(sys_mem, f133_memmap[F133_DEV_TCON_LCD0].base,
                                &s->tcon_lcd);
    memory_region_init_io(&s->mixer0, OBJECT(dev), &f133_mixer_ops, s,
                          "f133.mixer0", 0x100000);
    memory_region_add_subregion(sys_mem, f133_memmap[F133_DEV_MIXER0].base,
                                &s->mixer0);
    s->display = qemu_graphic_console_create(dev, 0, &f133_display_ops, s);

    /* UARTs: dw-apb-uart, reg-io-width=4, reg-shift=2.
     * The radio's debug console is UART3 (0x02500c00) — that's where the
     * firmware points its console descriptor; UART3 gets serial_hd(0). */
    for (int i = 0; i < F133_UART_COUNT; i++) {
        serial_mm_init(sys_mem, f133_memmap[F133_DEV_UART0 + i].base, 2,
                       qdev_get_gpio_in(s->plic, F133_UART0_IRQ + i),
                       F133_TIMEBASE_FREQ,
                       serial_hd(i == 3 ? 0 : 1 + i),
                       DEVICE_LITTLE_ENDIAN);
        /* Sunxi extras beyond the 16550 window: serial_mm only maps
         * 8 << regshift = 0x20 bytes, but OpenSBI/console code reads the
         * status word at +0x7c (USR: TFC|RFC = FIFOs empty when idle).
         * Unmapped it raises a load access fault (mcause 5) and console
         * init dies; map a disjoint filler for 0x20..0x3ff. */
        memory_region_init_io(&s->uart_tail[i], OBJECT(s),
                              &f133_uart_tail_ops, s, "sunxi-uart-tail",
                              f133_memmap[F133_DEV_UART0 + i].size - 0x20);
        memory_region_add_subregion(sys_mem,
                                    f133_memmap[F133_DEV_UART0 + i].base + 0x20,
                                    &s->uart_tail[i]);
    }

    /* SPI0 controller + SPI NOR flash (the firmware's boot device) */
    DeviceState *flash_dev;
    DriveInfo *dinfo;

    object_initialize_child(OBJECT(dev), "spi0", &s->spi0, TYPE_SUNXI_SPI);
    sysbus_realize(SYS_BUS_DEVICE(&s->spi0), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->spi0), 0,
                    f133_memmap[F133_DEV_SPI0].base);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->spi0), 0,
                       qdev_get_gpio_in(s->plic, 15)); /* SPI0 irq: 15+16 */

    flash_dev = qdev_new("w25q128");
    dinfo = drive_get(IF_MTD, 0, 0);
    if (dinfo) {
        qdev_prop_set_drive_err(flash_dev, "drive",
                                blk_by_legacy_dinfo(dinfo), &error_fatal);
    } else {
        warn_report("no -drive if=mtd given: SPI NOR is blank");
    }
    qdev_realize_and_unref(flash_dev, BUS(s->spi0.spi), &error_fatal);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->spi0), 1,
                       qdev_get_gpio_in_named(flash_dev, SSI_GPIO_CS, 0));

    /* DMA controller: boot0 drains the SPI NOR read path through it
     * (LLI descriptors in SRAM, channel 0; see notes/boot0-progress.md) */
    object_initialize_child(OBJECT(dev), "dma", &s->dma, TYPE_SUNXI_DMA);
    sysbus_realize(SYS_BUS_DEVICE(&s->dma), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->dma), 0,
                    f133_memmap[F133_DEV_DMA].base);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->dma), 0,
                       qdev_get_gpio_in(s->plic, F133_DMA_IRQ));

    /* remaining UARTs + everything else: stub for now */
#define UNIMP(name, dev) \
    create_unimplemented_device(name, f133_memmap[dev].base, f133_memmap[dev].size)

    UNIMP("i2c0", F133_DEV_I2C0);
    UNIMP("i2c1", F133_DEV_I2C1);
    UNIMP("i2c2", F133_DEV_I2C2);
    UNIMP("can0", F133_DEV_CAN0);
    UNIMP("can1", F133_DEV_CAN1);
    UNIMP("syscfg@2002000", F133_DEV_UNK2002000);
    UNIMP("ledc", F133_DEV_LEDC);
    UNIMP("gpadc", F133_DEV_GPADC);
    UNIMP("ths", F133_DEV_THS);
    UNIMP("dmic", F133_DEV_DMIC);
    UNIMP("i2s1", F133_DEV_I2S1);
    UNIMP("i2s2", F133_DEV_I2S2);
    UNIMP("timer", F133_DEV_TIMER);
    UNIMP("wdt", F133_DEV_WDT);
    UNIMP("unk@2030000", F133_DEV_UNK2030000);
    UNIMP("legacy@1c08000", F133_DEV_UNK1C08000);
    UNIMP("syscon", F133_DEV_SYSCON);
    UNIMP("crypto", F133_DEV_CRYPTO);
    UNIMP("hstimer", F133_DEV_HSTIMER);
    UNIMP("unk@300b000", F133_DEV_UNK300B000);
    UNIMP("mmc0", F133_DEV_MMC0);
    UNIMP("mmc1", F133_DEV_MMC1);
    UNIMP("mmc2", F133_DEV_MMC2);
    UNIMP("spi1", F133_DEV_SPI1);
    UNIMP("usb0", F133_DEV_USB0);
    create_unimplemented_device("usb1-phy", 0x04200800, 0x800);
    UNIMP("emac", F133_DEV_EMAC);
    UNIMP("de-clk", F133_DEV_DE_CLK);
    UNIMP("mixer1", F133_DEV_MIXER1);
    UNIMP("dsi", F133_DEV_DSI);
    UNIMP("dphy", F133_DEV_DPHY);
    UNIMP("tcon-top", F133_DEV_TCON_TOP);
    UNIMP("tcon-tv0", F133_DEV_TCON_TV0);
    UNIMP("riscv-cfg", F133_DEV_RISCV_CFG);
    UNIMP("unk@6e20000", F133_DEV_UNK6E20000);
    UNIMP("unk@7000000", F133_DEV_UNK7000000);
    UNIMP("ppu", F133_DEV_PPU);
    UNIMP("prcm", F133_DEV_PRCM);
    UNIMP("unk@7022000", F133_DEV_UNK7022000);

#undef UNIMP
}

static void f133_soc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = f133_soc_realize;
}

static const TypeInfo f133_soc_type_info = {
    .name = TYPE_RISCV_F133_SOC,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(F133SoCState),
    .instance_init = f133_soc_init,
    .class_init = f133_soc_class_init,
};

static void f133_soc_register_types(void)
{
    type_register_static(&f133_soc_type_info);
}

type_init(f133_soc_register_types)

static void f133_soc_init(Object *obj)
{
    F133SoCState *s = RISCV_F133_SOC(obj);
    RISCVHartArrayState *cpu = &s->cpu;

    object_initialize_child(obj, "cpu", cpu, TYPE_RISCV_HART_ARRAY);

    qdev_prop_set_uint32(DEVICE(cpu), "hartid-base", C906_CPU_HARTID);
    qdev_prop_set_string(DEVICE(cpu), "cpu-type", TYPE_RISCV_CPU_THEAD_C906);
    qdev_prop_set_uint32(DEVICE(cpu), "num-harts", 1);
    /* BROM loads boot0 to SRAM 0x20000 and jumps there */
    qdev_prop_set_uint64(DEVICE(cpu), "resetvec", F133_SRAM_BASE);
}

static void f133_machine_init(MachineState *machine)
{
    F133MachineState *s = RISCV_F133_MACHINE(machine);
    MemoryRegion *sys_mem = get_system_memory();

    object_initialize_child(OBJECT(machine), "soc", &s->soc,
                            TYPE_RISCV_F133_SOC);
    qdev_realize(DEVICE(&s->soc), NULL, &error_fatal);

    /* DRAM + "no DRAM" garbage window.
     * boot0 detects the DRAM size by walking addresses and checking a
     * pre-written pattern; on real hardware reads past the DRAM end return
     * garbage so the check fails.  In QEMU unmapped reads fault instead,
     * so back the window after machine RAM with a device that returns
     * non-matching data.  The window must not cover the CLINT block that
     * sits immediately after DRAM at 0x44000000 (added earlier as a SoC
     * subregion; a later overlapping subregion would shadow it). */
    memory_region_add_subregion(sys_mem, f133_memmap[F133_DEV_DRAM].base,
                                machine->ram);
    /* Melis programs EHCI with its 0xc0000000 uncached DRAM addresses.
     * This is an emulation alias; the underlying bus translation is not yet
     * modeled. Give it priority over the open-bus DRAM probe window. */
    memory_region_init_alias(&s->soc.usb_dma_alias, OBJECT(s), "f133.usb-dma-alias",
                             machine->ram, 0, machine->ram_size);
    memory_region_add_subregion_overlap(sys_mem, 0xc0000000,
                                        &s->soc.usb_dma_alias, 1);
    hwaddr dram_end = f133_memmap[F133_DEV_DRAM].base + machine->ram_size;
    hwaddr nodram_start = MAX(dram_end,
                              f133_memmap[F133_DEV_CLINT].base +
                              f133_memmap[F133_DEV_CLINT].size);
    /* Extend the garbage window all the way to the 4 GiB boundary.  boot0's
     * DRAM-size walk probes row offsets up to +128 MB on a second rank
     * (probe at 0x48000000 + 1<<27 = 0x50000000), and QEMU raises a load
     * access fault (mcause=5, mtval=probe address) on accesses to fully
     * unassigned physical addresses, while real hardware returns open-bus
     * garbage.  Nothing else in the F133 map lives above the CLINT (all
     * peripherals are below 0x44000000), so covering the whole upper half
     * of the address space is safe and lets every out-of-range probe read
     * benign non-matching data like on silicon. */
    hwaddr nodram_end = 0x100000000;
    if (nodram_start < nodram_end) {
        memory_region_init_io(&s->nodram, OBJECT(s), &f133_nodram_ops,
                              NULL, "f133.nodram", nodram_end - nodram_start);
        memory_region_add_subregion(sys_mem, nodram_start, &s->nodram);
    }

    /* The firmware boots exactly like on real hardware: BROM already
     * loaded boot0 into SRAM.  Take the boot0 image with -bios. */
    if (machine->firmware && strcmp(machine->firmware, "none") != 0) {
        ssize_t sz = load_image_targphys(machine->firmware, F133_SRAM_BASE,
                                         F133_SRAM_SIZE, &error_fatal);
        if (sz < 0) {
            error_report("could not load boot0 image '%s'", machine->firmware);
            exit(EXIT_FAILURE);
        }
        info_report("loaded boot0 '%s' (%zd bytes) at 0x%08" HWADDR_PRIX,
                    machine->firmware, sz, F133_SRAM_BASE);
    } else if (!machine->firmware) {
        error_report("F133 emulation boots the radio firmware's boot0; "
                     "pass it with -bios boot0.bin");
        exit(EXIT_FAILURE);
    }
}

static void f133_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Allwinner F133 (sun20iw1) radio firmware";
    mc->init = f133_machine_init;
    mc->max_cpus = 1;
    mc->default_cpus = 1;
    mc->default_ram_id = "riscv.f133.ram";
    mc->default_ram_size = f133_memmap[F133_DEV_DRAM].size;
}

static const TypeInfo f133_machine_typeinfo = {
    .name = TYPE_RISCV_F133_MACHINE,
    .parent = TYPE_MACHINE,
    .class_init = f133_machine_class_init,
    .instance_size = sizeof(F133MachineState),
    .interfaces = riscv64_machine_interfaces,
};

static void f133_ehci_class_init(ObjectClass *oc, const void *data)
{
    SYS_BUS_EHCI_CLASS(oc)->portnr = 1;
}

static void f133_machine_register_types(void)
{
    static const TypeInfo ehci_type = {
        .name = "f133-ehci-usb",
        .parent = TYPE_AW_H3_EHCI,
        .class_init = f133_ehci_class_init,
    };
    static const TypeInfo twi_type = {
        .name = "allwinner.i2c-f133",
        .parent = TYPE_AW_I2C_SUN6I,
    };
    static const TypeInfo touch_type = {
        .name = "f133.gt911",
        .parent = TYPE_I2C_SLAVE,
        .instance_size = sizeof(F133Touch),
        .class_init = f133_touch_class_init,
    };
    type_register_static(&twi_type);
    type_register_static(&ehci_type);
    type_register_static(&touch_type);
    type_register_static(&f133_machine_typeinfo);
}

type_init(f133_machine_register_types)
