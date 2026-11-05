# F133 MMIO map (learned from the firmware)

Peripheral base addresses confirmed by firmware traces (QEMU `-d unimp`,
gdb samples) plus the linux D1 device trees. "implemented" = has a real
model in `qemu/hw/riscv/f133.c`; "stub" = unimplemented-device logging.

| Address | Size | Peripheral | Status | Notes |
|---|---|---|---|---|
| 0x00020000 | 0x28000 | SRAM A1 | implemented (RAM) | boot0 loads here (BROM entry 0x20000); boot0 uses sp=0x48000 |
| 0x01c08000 | 0x1000 | legacy sunxi | stub | referenced by OpenSBI region code |
| 0x02000000 | 0x800 | pio (pinctrl) | minimal register storage | boot0 configures SPI0 pins; PF6 is held high for an empty active-low SD detect switch |
| 0x020002d0 | 0x08 | PG interrupt enable/status | partial | PG7 GT911 interrupt, write-one-to-clear status; PLIC source 95 |
| 0x02502c00 | 0x400 | I2C3 | partial | GT911 at address 0x5d, PLIC source 28; legacy engine registers only |
| 0x02001000 | 0x1000 | CCU | implemented (stateful) | PLL_CPU_CTRL @0x000: LOCK=bit28, LOCK_ENABLE=bit29; boot0 polls after "set pll start"; also 0x90c, 0xa5c, 0xd00 |
| 0x02002000 | 0x1000 | syscfg? | stub | heavily referenced by OpenSBI region (x15) |
| 0x02030000 | 0x1000 | unk (pll/arisc?) | stub | boot0 "set pll start" writes 0x31c, 0x348 |
| 0x02031000 | 0x400 | dmic | stub | |
| 0x02033000+ | | i2s1/i2s2 | stub | |
| 0x02050000 | 0xa0 | timer | stub | |
| 0x020500a0 | 0x20 | wdt | stub | |
| 0x02500000+i*0x400 | | uart0-5 | implemented | dw-apb-uart, reg-shift=2; **console = UART3 (0x02500c00)** |
| 0x02502000+ | 0x400 | i2c0-3 | stub | |
| 0x02504000 | 0x400 | can0/1 | stub | |
| 0x03000000 | 0x1000 | syscon | stub | boot0 reads/writes 0x150 (LDO), 0x160, 0x16c |
| 0x03002000 | 0x1000 | dma | stub | touched by boot0 DRAM init (writes 0x000, 0x010) — current blocker |
| 0x03006000 | 0x1000 | sid (efuse) | implemented | read 0x200 = 0x7400 (chip id boot0 requires; else "can't support chip type"); read 0x218/0x21c by "board init"; write 0x50 (ignored) |
| 0x0300b000 | 0x1000 | unk | stub | |
| 0x03102000 | 0x1000 | mbus | implemented | reg0 encodes DRAM geometry; sizefn = 1<<(b[11:8]+b[7:4]+b[3:2]-14) MB per channel, b[1:0]=channel count; boot0 verifies writes via read-back |
| 0x03103000 | 0x1000 | dramc | implemented | +0x000 = host command (0x52/0x53, 0x520/0x5a1); +0x010 & +0x018 bit0 = done, polled |
| 0x04020000+ | 0x1000 | mmc0/1/2 | stub | |
| 0x04025000 | 0x1000 | **SPI0** | implemented (hw/ssi/sunxi_spi.c) | see notes/spi0.md |
| 0x04026000 | 0x1000 | spi1 | stub | |
| 0x04100000 | | usb0 | stub | |
| 0x04200000 | | usb1 EHCI | partial | one high-speed port, PLIC source 49; see notes/usb-bringup.md |
| 0xc0000000 | RAM size | DRAM alias | experimental | permits EHCI DMA using firmware addresses; actual bus translation unverified |
| 0x04500000 | 0x10000 | emac | stub | |
| 0x05000000 | 0x10000 | de clk | stub | display pipeline (M6/M7 target) |
| 0x05100000/0x05200000 | 0x100000 | mixer0/1 | stub | display |
| 0x05450000+ | | dsi/dphy/tcon | stub | display; lcd0 config in melis-config |
| 0x06010000 | 0x1000 | riscv_cfg | stub | smaeul: RESET_ENTRY regs |
| 0x06e20000 | 0x1000 | unk | stub | OpenSBI references (x14) |
| 0x07000000 | 0x1000 | unk | stub | boot0 reads 0x5d4 (bit1 polled) |
| 0x07001000 | 0x1000 | ppu | stub | |
| 0x07010000 | 0x1000 | prcm/r_ccu | stub | OpenSBI references |
| 0x07022000 | 0x1000 | unk | stub | |
| 0x07090000 | 0x400 | rtc | implemented (RAM) | boot0 stage handshake: writes magic @+0x100, retries until read-back matches |
| 0x10000000 | 0x4000000 | PLIC | implemented | thead,c900-plic, ndev=175, M+S contexts |
| 0x14000000 | 0x4000000 | CLINT | implemented | T-Head CLINT has no MMIO mtime (C906 uses CSR time); msip @base, mtimecmp @+0x4000 |

DRAM: base 0x40000000. Real size unknown; we report **256 MB** (see
`F133_MBUS_SIZE_GOOD` in f133.c) and back 256 MB with machine RAM. Beyond
RAM a "no-DRAM" device returns garbage so boot0's size probe stops.

## CPU

- `thead-c906` (TYPE_RISCV_CPU_THEAD_C906): xtheadba/bb/bs/cmo/condmov/
  fmemidx/fmv/mac/memidx/mempair/sync all enabled in TCG — boot0 uses
  T-Head CSRs (0x7c0, 0x7c2) and custom-0 (th.*) instructions freely.
- Boot0 writes mxstatus-ish bits early (`csrrs 0x7c0 = 0x40000000`,
  `csrrs 0x7c2 = 0x30013`).
- Note: `-cpu` warning "disabling zfa extension ... privilege spec version
  does not match" is benign.
