# OpenSBI stage (M4) — progress log

> **Status update:** The fault described below is historical, not the current
> blocker. With the current uncommitted QEMU tree, a normal boot from boot0 and
> SPI NOR reaches `sbi2ekernel` and Melis `scheduler startup`. The precise
> change that eliminated the old fault has not been isolated; do not assume the
> base-address hypothesis below was confirmed. No OpenSBI banner was observed.
> See `notes/melis-progress.md` for the subsequent failures.

Payload: `melis-lzma` package → LZMA → CHK `opensbi` image (0x216ee0 bytes,
saved at `/tmp/opencode/opensbi-chk.bin`). Load addr 0x40000000 (header
+0x2c), entry 0x40004000. Image offset X ↔ runtime 0x40000000+X.

## Fixed: UART register window too small (console-init fault)

- Symptom: mcause=5 (load access fault), mtval=0x02500c7c (UART3+0x7c),
  mepc=0x40007664 — OpenSBI console init polled `*(u32*)(UART3+0x7c)`
  (sunxi USR-style status, wait-nonzero loop) and died: `serial_mm_init`
  maps only `8 << regshift` = 0x20 bytes per UART.
- Fix (in `qemu/hw/riscv/f133.c`, `include/hw/riscv/f133.h`):
  `f133_uart_tail_ops` region mapped at each UART base+0x20, size
  0x400-0x20. Offset 0x7c (=region addr 0x5c) returns 0x3 (USR: TFC|RFC —
  both FIFOs empty when idle); everything else RAZ/WI. New
  `MemoryRegion uart_tail[4]` in F133SoCState.
- Result: console works — OpenSBI's `sbi_trap_error` register dump prints
  on serial. The OpenSBI banner is still not reached; next fault below.

## Current blocker: store access fault to `*(u32*)(base + 0x1ffffc) = 1`

- Symptom: mcause=7 (store access fault), mtval=0x001ffffc (4 bytes BELOW
  SRAM base 0x20000), mepc=0x40003c64, before the OpenSBI banner.
- Faulting function (starts 0x40003c46, tiny, returns 0):
  ```
  fence w,o
  a5 = *(u64*)(0x4000C248 + 0x70)      ; = *(0x4000C2B8), runtime value 0x0
  a5 += 0x1ffffc                       ; lui 0x200; addi -4
  *(u32*)a5 = 1
  return 0
  ```
- Caller (indirect call via wrapper ~0x40000d00–0x40000dba): reads two
  LE u64s from struct at s2=0x4000b3f0 — u64 at +96 → pointer; u64 at
  (that ptr)+88 → function pointer; `jalr` with a0=1, a1=4, a2=1;
  non-zero return → branch to 0x40000af0 (likely error path).
- The global struct @0x4000C248 (file bytes are 0xff fill; runtime):
  +0x68 = 0x0000000041000000, +0x70 = 0, +0x78 = 0x0000000004000000
  (64 MiB — DRAM size), +0x80 = 0x000000004000b3f0.
  Hypothesis: +0x70 should be a DRAM/region base (0x40000000?) and the
  code plants a flag word at the last 4 bytes of the first 2 MiB of that
  region (0x40000000 + 0x1ffffc = 0x41fffffc). If the base were 0x20000
  (SRAM) you'd get 0x21ffffc; with 0 you get 0x1ffffc → unmapped.
- Open question (next session): who initializes +0x70? Candidates:
  boot0 handoff structure, DTB parsing (a1 at entry?), or code that
  never ran because of the earlier fault chain. Check what boot0 passes
  in a0/a1 at 0x40004000, and disassemble around 0x40000af0 (loop/error
  path) and 0x4000b3f0 (s2 struct: u64s at +88/+96).

## Register-state crumbs (at fault)

- mstatus=0x8000000a00007800, mtvec=0x400008e8, sp=0x4000e1b0,
  s2=0x4000b3f0, s4=0x4000c000, s8=0x2000.
- "page/access fault handler failed (error -2)" = OpenSBI's trap handler
  ran but couldn't resolve (it can't fix an unmapped store) → panic dump.

## Notes / tips

- gdb attach to a *running* instance fails with "Cannot execute this
  command while the target is running" — always start QEMU with `-S`
  and set breakpoints before `continue`.
- capstone RVC decoding is unreliable in this image; use gdb's built-in
  `x/i` on the live target instead.
