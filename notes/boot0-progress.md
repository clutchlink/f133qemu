# Bring-up log — boot0 (M3)

Chronological record of what was stubbed/changed to get boot0 this far.
QEMU machine: `-M f133` (`qemu/hw/riscv/f133.c`), boot0 via `-bios`.

## Working so far (as of this session)

1. **Machine skeleton** (M2): C906 hart (resetvec 0x20000), SRAM, DRAM,
   PLIC @0x10000000, ACLINT @0x14000000, 6× UART (console = UART3).
2. **boot0 banner on UART3** (M3 in progress): `[1]HELLO! BOOT0 is starting!`
   / `BOOT0 commit : 44f17d7ce8-dirty`.
3. **boot0 advances through**: chip-id check (SID 0x3006200 must read
   0x7400), RTC stage handshake, CCU PLL lock poll ("set pll start"), CCU
   misc config, **DRAM init polls** (DRAMC cmd/done bits) — with SPI NOR
   (w25q128 + real flash image) attached and the SPI controller model in
   place.

## Fixes, in the order they were needed

| Problem | Fix |
|---|---|
| boot0 stack at 0x48000 faulted (SRAM too small) | SRAM size 0x20000 → 0x28000 |
| No UART output | boot0's console is **UART3** (0x02500c00), not UART0 — descriptor pointer at SRAM 0x2b500 |
| "can't support chip type %d" → jump to 0x20 | SID device returning 0x7400 @0x3006200 |
| QEMU host segfault | SID write had no `.write` op → NULL fn call; added no-op write |
| PLL lock poll spin (CCU+0x000) | stateful CCU: LOCK(28) auto-set when LOCK_ENABLE(29) written |
| DRAMC cmd poll spins (+0x010, +0x018 bit0) | stateful DRAMC, done bit forced |
| DRAM pattern test fault (tval 0x48000000 etc.) | reads past DRAM must return garbage (real HW behavior); added `f133.nodram` device after machine RAM |
| DRAM geometry garbage (4160 MB) → multi-minute test | MBUS read normalizes size fields to 256 MB unless it's an immediate write-read-back (verify loops stay honest) |

## Current blocker (next step)

After DRAM init, boot0 touches **DMA (0x03002000: writes 0x000, 0x010)**
during init, then jumps to 0x20 (failure path — `epc:0x20` fault_fetch).
Hypothesis: DRAM training uses the DMA engine (or reads DMA status) and our
stub returning 0 fails a check.

Next actions:
1. Trace the DMA accesses (offsets/values) with `-d unimp` around the DRAM
   init and disassemble the failure branch in the DRAM init function
   (0x27xxx region of boot0 / the code just before the `jump(0x20)`).
2. Either implement a minimal stateful DMA model like DRAMC/CCU, or find
   the specific register whose read-back matters.
3. After DMA: boot0 should read spinor ID (the SPI device is ready —
   `spinor id is: %02x %02x %02x` should appear), read the package +
   melis-config from flash, and `Jump to second Boot.` into OpenSBI at
   0x40004000 (blob loads at 0x40000000, FW_DYNAMIC in a2).

## Tools & techniques that worked

- `tools/carve.py`, `tools/parse_gpt.py` (extract/, GPT: bootA/ROOTFS/UDISK)
- capstone RISC-V + C for static decoding; **gdb on the live QEMU** for
  everything ambiguous (breakpoints beat speculation)
- `-d unimp` to enumerate MMIO the firmware touches; per-device logging
  (mbus) for write/read sequences
- `notes/mmio.md` has the address map; `notes/spi0.md` the SPI protocol.
## Status update (this session)

- **M3 done, M4 reached**: boot0 loads the full package (19 DMA bursts,
  0x10000–0x110000 → DRAM 0x41000000), LZMA-decompresses, and jumps to
  OpenSBI entry 0x40004000. The CS-per-burst fix in `hw/ssi/sunxi_spi.c`
  (xfer_active-gated CS frame) was the key — see history above.
- **OpenSBI console works** after adding the UART tail region
  (f133_uart_tail_ops) — trap dumps print on serial.
- **Current blocker**: OpenSBI store-faults on `*(base + 0x1ffffc) = 1`
  where the base global (+0x70 of struct @0x4000C248) reads 0. Details,
  disassembly and next steps: `notes/opensbi.md`.
