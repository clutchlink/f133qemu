/*
 * QEMU Allwinner F133 (sun20iw1) radio firmware emulation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Emulates enough of the F133 (T-Head C906, D1-family SoC) to boot the
 * firmware found in the radio head-unit image:
 *   boot0 (BT0) -> OpenSBI -> Melis RTOS -> radio UI
 *
 * MMIO map from the linux sun20i-d1s.dtsi / sunxi-d1s-t113.dtsi plus
 * addresses observed in the firmware binaries (see notes/ in the project
 * repository).
 */
#ifndef HW_RISCV_F133_H
#define HW_RISCV_F133_H

#include "hw/core/boards.h"
#include "hw/riscv/riscv_hart.h"
#include "qemu/timer.h"
#include "ui/console.h"

#define C906_CPU_HARTID (0)
#define F133_UART_COUNT 6
#define F133_UART0_IRQ  18     /* SOC_PERIPHERAL_IRQ(2) */
#define F133_DMA_IRQ    66     /* SOC_PERIPHERAL_IRQ(50) */
#define F133_PLIC_NUM_SOURCES    176   /* ndev=175 per sun20i-d1 dts */
#define F133_PLIC_NUM_PRIORITIES 7
#define F133_PLIC_PRIORITY_BASE  0x00
#define F133_PLIC_PENDING_BASE   0x1000
#define F133_PLIC_ENABLE_BASE    0x2000
#define F133_PLIC_ENABLE_STRIDE  0x80
#define F133_PLIC_CONTEXT_BASE   0x200000
#define F133_PLIC_CONTEXT_STRIDE 0x1000
#define F133_TIMEBASE_FREQ       24000000UL

enum {
    F133_DEV_SRAM,
    F133_DEV_DRAM,
    /* APB0 / low speed */
    F133_DEV_PIO,
    F133_DEV_CCU,
    F133_DEV_UNK2002000,
    F133_DEV_LEDC,
    F133_DEV_GPADC,
    F133_DEV_THS,
    F133_DEV_DMIC,
    F133_DEV_I2S1,
    F133_DEV_I2S2,
    F133_DEV_TIMER,
    F133_DEV_WDT,
    F133_DEV_UNK1C08000,
    F133_DEV_UNK2030000,
    /* 0x025xxxxx IO */
    F133_DEV_UART0,
    F133_DEV_UART1,
    F133_DEV_UART2,
    F133_DEV_UART3,
    F133_DEV_UART4,
    F133_DEV_UART5,
    F133_DEV_I2C0,
    F133_DEV_I2C1,
    F133_DEV_I2C2,
    F133_DEV_I2C3,
    F133_DEV_CAN0,
    F133_DEV_CAN1,
    /* 0x03xxxxxx system */
    F133_DEV_SYSCON,
    F133_DEV_DMA,
    F133_DEV_SID,
    F133_DEV_CRYPTO,
    F133_DEV_HSTIMER,
    F133_DEV_UNK300B000,
    F133_DEV_MBUS,
    F133_DEV_DRAMC,
    /* 0x04xxxxxx */
    F133_DEV_MMC0,
    F133_DEV_MMC1,
    F133_DEV_MMC2,
    F133_DEV_SPI0,
    F133_DEV_SPI1,
    F133_DEV_USB0,
    F133_DEV_USB1,
    F133_DEV_EMAC,
    /* display */
    F133_DEV_DE_CLK,
    F133_DEV_MIXER0,
    F133_DEV_MIXER1,
    F133_DEV_DSI,
    F133_DEV_DPHY,
    F133_DEV_TCON_TOP,
    F133_DEV_TCON_LCD0,
    F133_DEV_TCON_TV0,
    /* cpu / always-on */
    F133_DEV_RISCV_CFG,
    F133_DEV_UNK6E20000,
    F133_DEV_UNK7000000,
    F133_DEV_PPU,
    F133_DEV_PRCM,
    F133_DEV_UNK7022000,
    F133_DEV_RTC,
    /* interrupt controllers */
    F133_DEV_PLIC,
    F133_DEV_CLINT,
};

extern const MemMapEntry f133_memmap[];

#define TYPE_RISCV_F133_SOC "riscv.f133.soc"
#define RISCV_F133_SOC(obj) \
    OBJECT_CHECK(F133SoCState, (obj), TYPE_RISCV_F133_SOC)

typedef struct F133SoCState {
    /*< private >*/
    DeviceState parent_obj;

    /*< public >*/
    RISCVHartArrayState cpu;
    MemoryRegion sram;
    MemoryRegion usb_dma_alias;
    MemoryRegion rtc_scratch;
    MemoryRegion sid;
    MemoryRegion pio;
    uint32_t pio_regs[0x800 / 4];
    qemu_irq pio_g_irq;
    MemoryRegion tcon_lcd;
    uint32_t tcon_lcd_regs[0x1000 / 4];
    QEMUTimer *tcon_vblank_timer;
    qemu_irq tcon_irq;
    MemoryRegion mixer0;
    uint32_t mixer0_regs[0x100000 / 4];
    QemuConsole *display;
    MemoryRegion ccu;
    MemoryRegion mbus;
    MemoryRegion dramc;
    uint32_t *mbus_regs;
    uint32_t *ccu_regs;
    uint32_t *dramc_regs;
    SunxiSPIState spi0;
    SunxiDMAState dma;
    MemoryRegion uart_tail[F133_UART_COUNT];
    /* benign catch-all for unassigned low address space (see f133.c) */
    MemoryRegion unimp_lo;
    DeviceState *plic;
} F133SoCState;

#define TYPE_RISCV_F133_MACHINE MACHINE_TYPE_NAME("f133")
#define RISCV_F133_MACHINE(obj) \
    OBJECT_CHECK(F133MachineState, (obj), TYPE_RISCV_F133_MACHINE)

typedef struct F133MachineState {
    /*< private >*/
    MachineState parent_obj;

    /*< public >*/
    F133SoCState soc;
    MemoryRegion nodram;
} F133MachineState;

#endif
