# SPI0 controller (0x04025000) — semantics derived from boot0's spinor driver

Register map observed from the firmware (differs from classic sun6i in the
FIFO/status area):

| Offset | Name | Observed usage |
|---|---|---|
| 0x04 | GCR | SRST(31): reset sequence `0x80000000` → `0x80000001` → `0x02` → `0x80`; EN(0), MASTER(1) |
| 0x08 | TCR | `0x184` (mode), `0x80` (SS level, bit7), **`0x80000080` = XCH(31) start**; driver expects XCH self-cleared after transfer |
| 0x10 | ISR | cleared to 0 at init |
| 0x14 | FCR1 | `0x3f77` at init; driver later reads bits[10:8] (0x700) and bit0 |
| 0x18 | FCR2 | `0x80208020` at init; driver RMWs bit8 |
| 0x1c | FSR | **[7:0] = RX fifo level** (drain loop polls until 0) |
| 0x24 | DMA_CTL | cleared to 0 |
| 0x30 | CNT | transfer byte count (4 for READ ID); driver expects 0 after transfer |
| 0x34 | BC1 | `0x1` |
| 0x38 | BC2 | `0x4`, also cleared at init |
| 0x200+ | TX window | byte write `0x9f` (JEDEC ID cmd) |
| 0x300+ | RX window | byte reads (drain), plus a bulk copy loop reading 0x4025300 as a range (burst mode) |

## Driver flow (READ ID example)

1. `CNT = 4`, `BC2 = 4`
2. `TCR = 0x80` (SS active), then `TCR = 0x80000080` (XCH) — **before** the
   command byte is pushed! Our model defers the transfer when the TX fifo
   is empty at XCH and runs it when the first TXDATA byte arrives.
3. `TXDATA = 0x9f`
4. poll `FSR[7:0]` (RX level); read `RXDATA` bytes (lbu) until `s1` bytes
   read or fifo dry; loop until `s1` exhausted.
5. check `FCR1` bits (0x700, bit0), `CNT == 0`, `TCR` bit31 cleared
   (XCH self-clear expected), reg0x18 RMW bit8.

## Flash

- `w25q128` added to `hw/block/m25p80.c` (JEDEC 0xef4018, 16 MiB — matches
  the radio's flash dump size).
- Backing file: `-drive if=mtd,format=raw,file=<firmware>.bin`.
- CS line: sysbus irq 1 of the controller → `ssi-gpio-cs` of the flash;
  CS asserted while `GCR.EN && TCR.SS`.

Model source: `qemu/hw/ssi/sunxi_spi.c` (trace: `-d sunxi_spi`... actually
trace events `sunxi_spi_read/write` — enable with
`-trace enable=sunxi_spi*`).