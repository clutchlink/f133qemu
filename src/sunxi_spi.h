/*
 * Allwinner sun6i-style SPI controller (F133 / sun20iw1)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_SSI_SUNXI_SPI_H
#define HW_SSI_SUNXI_SPI_H

#include "hw/core/sysbus.h"
#include "qemu/fifo8.h"

#define TYPE_SUNXI_SPI "sunxi-spi"
#define SUNXI_SPI(obj) \
    OBJECT_CHECK(SunxiSPIState, (obj), TYPE_SUNXI_SPI)

typedef struct SunxiSPIState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    SSIBus *spi;
    qemu_irq irq;
    qemu_irq *cs_lines;
    bool cs_active;
    /* in-flight transfer */
    bool xfer_active;
    bool xfer_dhb;
    uint32_t xfer_remaining;
    uint32_t xfer_discard_remaining;

    uint32_t regs[0x40 / 4];
    uint32_t int_status;    /* 0x14 INT_STA: write-1-to-clear flags */
    Fifo8 tx_fifo;
    Fifo8 rx_fifo;
} SunxiSPIState;

#endif
