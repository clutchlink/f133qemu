/*
 * Allwinner sun6i-style DMA controller ("new" descriptor DMA)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The DMA at 0x03002000 on the F133 (sun20iw1) is the same IP as the
 * sun6i-a31 DMA, linux compatible "allwinner,sun20i-d1-dma" (driver:
 * drivers/dma/sun6i-dma.c).  16 channels, 4 interrupt-status bits per
 * channel (HALF/PKG/QUEUE + spare), descriptors ("LLI") in memory.
 */
#ifndef HW_DMA_SUNXI_DMA_H
#define HW_DMA_SUNXI_DMA_H

#include "hw/core/sysbus.h"

#define TYPE_SUNXI_DMA "sunxi-dma"
#define SUNXI_DMA(obj) \
    OBJECT_CHECK(SunxiDMAState, (obj), TYPE_SUNXI_DMA)

#define SUNXI_DMA_NCHAN 16

typedef struct SunxiDMAChannel {
    uint32_t enable;    /* +0x00 */
    uint32_t pause;     /* +0x04 */
    uint32_t lli_addr;  /* +0x08 physical address of first descriptor */
    uint32_t cfg;       /* +0x0c */
    uint32_t cur_src;   /* +0x10 */
    uint32_t cur_dst;   /* +0x14 */
    uint32_t cur_cnt;   /* +0x18 */
    uint32_t para;      /* +0x1c */
} SunxiDMAChannel;

typedef struct SunxiDMAState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t regs[0x40 / 4];            /* common register file */
    uint32_t irq_en[2];                 /* 0x000/0x004: 4 bits per channel */
    uint32_t irq_stat[2];               /* 0x010/0x014: write-1-to-clear */
    uint32_t stat;                      /* 0x030: per-channel busy bits */

    SunxiDMAChannel chan[SUNXI_DMA_NCHAN];
} SunxiDMAState;

#endif
