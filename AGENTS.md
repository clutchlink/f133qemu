# AGENTS.md — Allwinner F133 radio firmware emulation

Guidance for AI agents (and humans) working in this repo. Read this first.

## 1. Project goal

Emulate the firmware of a radio head-unit based on the **Allwinner F133**
(sun20iw1, T-Head **C906** RV64 core) using a **fork of QEMU**, so that the
full boot chain runs and the device's **UI renders to a screen** we can see
in QEMU.

Scope decisions (agreed with the owner):
- **In scope:** boot0 (BT0) → OpenSBI → Melis RTOS kernel → Melis app/UI →
  framebuffer rendering to a visible QEMU display.
- **No-op stubs OK:** FM/radio tuner, Bluetooth, WiFi/network (ssv6x5x),
  audio codec output (log calls, return benign success values).
- We emulate the **real boot flow** (boot0 executed from emulated SPI NOR),
  not a "skip the bootloader" shortcut, as the final deliverable. Loading
  individual images directly is fine as an intermediate bring-up technique.

## 2. The firmware (verified facts)

Source image: `F133-20220107_HZ-BX-B5678-UI3-V1.9-20240419-1616.bin`
(16 MiB = 0x1000000; likely a raw SPI NOR dump, W25Q128-class).

All offsets below were verified programmatically — trust them, but re-derive
anything you extend (see `tools/` for the scripts).

| File offset | Size | Content |
|---|---|---|
| `0x000000` | ≤ 0x10000 | **BT0/boot0** (`eGON.BT0` magic at 0x4). Strings: `HELLO! BOOT0 is starting!`, `spinor id is: %02x %02x %02x`, `Jump to second Boot.`, DRAM init, image-type names (`opensbi`, `u-boot-lzma`, `melis-lzma`, `melis-config`, `boot0 lzma dtb`). Last nonzero byte in this region: `0xffff`. |
| `0x010000` | — | **`sunxi-package` table.** Two entries. Record layout (verified): at record base `R`: 4-byte tag (`MIE;` / `IIE;`), NUL-terminated name at `R+4`, then u32 rel-offset and u32 size at `R+0x44`. Entry offsets are relative to `0x10000`. |
| — | entry 1 | `MIE;melis-lzma`: rel `0x400`, size `0xfa1c7` → file `0x10400`. **LZMA-alone** → decompresses to `0x216ee0` bytes (see below). |
| — | entry 2 | `IIE;melis-config`: rel `0xfa800`, size `0xc400` → file `0x10a800`. **NOT compressed** — raw config tree (`product`, `platform`, … sections). |
| `0x116c00`–`0x1000000` | ~14.6 MiB | Likely user/app data (UI resources, fonts, filesystem content). Not yet mapped — Phase 4 territory. |

### The `melis-lzma` payload (`0x216ee0` bytes after LZMA decode)

Single Allwinner **CHK image**: `CHKv1.0` header, name `opensbi`, plus a
`jal x0, +0x4000` at offset 0. Hypothesis (to confirm in Ghidra): loaded at
DRAM base `0x40000000` (word `0x40000000` at header offset 0x2c), execution
enters at `0x40004000`. RISC-V code (CSR-heavy OpenSBI-style) from ≈`0x4000`.
An **embedded ELF** sits at offset `0x7e00` — very likely the Melis kernel
payload (OpenSBI FW_PAYLOAD style). No DTB magic (`d0 0d fe ed`) inside; the
DTB is the separate LZMA'ed one referenced by boot0 (`boot0 lzma dtb`).
Melis kernel strings/symbols (devfs, minfs, rootfs, spinor, udisk, lwip,
ssv6x5x wifi, sunxi sdmmc/daudio) start ≈ `0x16a200`.

Build provenance strings (for locating a matching source tree if needed):
`melis-4.x-release-1.2` (paths like `/home/linzh/share/melis-4.x-release-1.2/source/...`).

### Expected boot chain

BROM (in-mask ROM, not in our image) → **boot0** (SRAM, loads rest from
SPI NOR) → **OpenSBI** (M-mode, at `0x40004000`) → **Melis RTOS** kernel
(payload) → Melis app/UI. U-Boot is *not* present in this firmware (boot0
merely knows the image type).

## 3. Hardware reference (sun20iw1 / F133)

Key MMIO regions (verify each against boot0/OpenSBI code during bring-up):
- UART0 console: `0x02500000` (16550-compatible, sunxi flavor).
- CCU (clock control): `0x02002000`.
- SPI NOR controller (SPI0): `0x04025000`-ish region — confirm from boot0.
- DRAM: base `0x40000000` (size TBD from boot0 DRAM params; 64/128 MiB typical).
- CLINT/PLIC: T-Head C906 uses vendor extensions; check OpenSBI build
  (`THEAD/C906` `plat` in the sbi blob) for actual addresses.
- Display pipeline (target feature): sunxi DE + TCON + LCD panel config lives
  in the `melis-config`/script data — extract panel timing from there.

C906 specifics that will matter: T-Head vendor CSRs (e.g. `mxstatus` `0x7c2`,
cache ops via `0x7c1`/`0x7d*`), CLIC-style interrupt controller options,
and C906-standard non-coherent DMA (OpenSBI/melis may issue `sync`/cache
maintenance via vendor CSRs — QEMU upstream already implements much of this
for the C906 CPU model).

## 4. Repo layout (conventions)

```
/allwinner-f133/
├── AGENTS.md                  # this file
├── F133-*.bin                 # original firmware — READ-ONLY, never modify
├── extract/                   # carved/decompressed artifacts (generated)
├── tools/                     # python scripts for carving/parsing (add here)
├── notes/                     # markdown notes: RE findings, MMIO maps, call logs
├── qemu/                      # the QEMU fork (git clone + topic branch f133)
└── run/                       # runtime artifacts: flash images, logs, screenshots
```

Rules:
- **Never edit the original `.bin`.** All derived data goes to `extract/` or `run/`.
- Every carving/parse script goes in `tools/` and must be runnable from a
  clean checkout (`python3 tools/<name>.py`), writing to `extract/`.
- Put RE findings (address maps, decoded headers, boot logs annotated) in
  `notes/` — AGENTS.md stays high-level and stable.
- The QEMU fork lives in `qemu/`; our changes go on topic branch `f133`
  (base: latest stable upstream master). Keep upstream files untouched
  except for the additions we own: `hw/riscv/f133*.c`, `hw/char/sunxi_uart.c`
  (or reuse `serial-mm`), Kconfig/meson wiring, docs.

## 5. Environment

- Host: Linux x86_64, 24 cores, 30 GiB RAM, ~205 GiB free, network OK.
- Available: `git`, `gcc`, `meson`, `ninja`, `python3`, `make`.
- QEMU build: standard meson flow; build dir out-of-tree, e.g.
  `qemu/build`, configure with `--target-list=riscv64-softmmu`.
- A Ghidra MCP server may be attached to a session (`tools.ghidra.*`); call
  `get_program_info()` first to see which binary is loaded. Use it for
  boot0/OpenSBI/Melis RE sessions; record conclusions in `notes/`.

## 6. Milestones (in order — do not skip ahead)

1. **M1 — Carve & document.** ✅ DONE: `tools/carve.py`, `tools/parse_gpt.py`,
   `tools/analyze_payload.py` → `extract/` (boot0, melis-lzma, melis-config);
   flash map: GPT with bootA (sunxi-package @0x10000, melis-lzma @0x10400,
   melis-config @0x10a800), ROOTFS @0x124000 (~14.3 MB), UDISK @0xf14000.
   See `notes/mmio.md`.
2. **M2 — QEMU fork builds.** ✅ DONE: `qemu/` on branch `f133` (base
   v11.1.2), machine `-M f133` + sunxi-spi device + w25q128 flash entry;
   commit 256974b.
3. **M3 — boot0 console.** 🚧 IN PROGRESS: banner + commit banner on UART3;
   chip-id, RTC, PLL, DRAM-init polls all pass; **open blocker: boot0 jumps
   to 0x20 (failure path) after touching DMA @0x03002000 during DRAM init**.
   Details + fix history in `notes/boot0-progress.md`.
4. **M4 — OpenSBI.** boot0 loads the `melis-lzma` package from emulated
   SPI NOR into DRAM and jumps; OpenSBI banner appears on UART.
   (Blob layout verified: loads at 0x40000000, entry 0x40004000, melis
   payload ELF @+0x7e00 loads at SRAM 0x21000.)
5. **M5 — Melis kernel.** OpenSBI hands off to the embedded ELF payload;
   Melis banner/thread activity on UART. Spinor/NAND + partition mounting
   driven from the real flash image (stub only what is provably dead code).
6. **M6 — App/UI alive.** Melis app starts; input stubs (LRADC keys) answer;
   display pipeline (DE/TCON) implemented enough to expose the framebuffer.
7. **M7 — Screen rendering.** QEMU shows the radio's UI (either via a custom
   QEMU display device bound to the emulated TCON framebuffer, or exporting
   the framebuffer to a VNC/SDL surface). Static UI is success; animation is
   bonus.

Milestones M3–M5 are expected to be iterative: add MMIO stubs only as boot0/
OpenSBI/Melis prove they touch them; keep a running log of what was stubbed
and why in `notes/stubs.md`.

## 7. Debugging conventions

- Run QEMU with `-d guest_errors,unimp -D run/qemu.log` and `-serial stdio`.
- Prefer `-device loader` overrides over editing firmware when probing.
- Keep a copy of each successful `run/` config in `run/cmdline-<milestone>.sh`
  so any state is reproducible.
- When an address is first touched, note it in `notes/mmio.md` (address,
  size, who touched it, guess at peripheral) — this becomes our SoC map.

## 8. Repo state (as of the first bring-up session)

- `qemu/` branch `f133`, commit `256974b` — machine + devices build clean.
- Bring-up command: `run/cmdline-boot0.sh` (boot0 via `-bios`, flash via
  `-drive if=mtd`).
- `notes/boot0-progress.md` — fix history + current blocker (DMA in DRAM
  init; boot0 failure-jump to 0x20).
- gdb-on-QEMU (`-S -gdb tcp::1234` + gdb-multiarch) is the most effective
  analysis tool; capstone for static work; Ghidra MCP currently unavailable.

## 9. Out of scope (for now)

FM tuner, Bluetooth, WiFi/ssv6x5x, USB, audio output, DRAM training realism,
BROM emulation (we start at boot0), power/battery management.