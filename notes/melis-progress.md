# Melis bring-up observations

Reproduced with the current uncommitted QEMU tree, after `ninja -C qemu/build
qemu-system-riscv64`, using `run/cmdline-boot0.sh -d guest_errors,unimp`.
The run was stopped after 35 seconds. This executes boot0 and reads the real
SPI NOR image; no loader overrides were used.

- The serial output reaches `sbi2ekernel` and `scheduler startup`. This
  supersedes the OpenSBI fault described in `opensbi.md`, although its exact
  resolution has not been isolated.
- Melis initializes storage, starts a display thread, and reports `nor flash
  init ok`, but `nor_transfer` fails with -6 on a 16384-byte read at flash
  offset 0xc000. `register_blk_device` consequently reports GPT discovery
  failure. Investigate SPI/DMA completion and data handling before assuming a
  filesystem problem; no specific cause has been verified yet.
- A later timer-thread load page fault reports `sepc=0x4005a432`,
  `stval=0x10`, with backtrace `0x4005ad44`, `0x400581ca`, `0x4005897e`,
  `0x4005d704`, `0x40059a4e`. The relationship, if any, to the storage
  failure is unknown.
- QEMU logs repeated PLIC invalid register accesses at offset 0x2098, as well
  as unimplemented display and I2C accesses. These have not been diagnosed.
- The SPI model initializes an IRQ output but currently never drives it;
  whether the failing transfer relies on that interrupt remains to be tested.

## Storage progress

The Melis 256-byte quad-read transfer enabled DMA queue-complete IRQ bit 2,
but the DMA model only set package-complete bit 1. Latching both at the end
of a descriptor chain, fixing IRQ enable register reads and per-bank masking,
eliminated the transfer timeout, exposing an invalid GPT instead.

The SPI trace showed `BURST_CNT=0x105`, `XMIT_CNT=4`, and quad BC2 single
count 5: command and address are followed by a dummy byte. Discarding that
fifth byte for the observed quad mode fixed the shifted data. A 40-second
normal boot subsequently discovered ROOTFS and UDISK, mounting them as minfs
on D and FAT on E respectively. The timer-thread page fault still occurs;
these changes do not establish its cause. Quad BC2 behavior beyond the
observed read remains to be validated.

## SD card detect and application startup

The timer-thread fault at `0x4005a432` is on the SD/MMC initialization path,
after the emulation falsely reports a card inserted. The firmware config's
`detect_pin` record at 0x2d88 points to a six-word GPIO value at 0xa604:
port 6, pin 6 (PF6). Matching Melis driver source identifies detection mode
2 as GPIO interrupt and uses active-low present value 0. The low GPIO stub
therefore looked like an inserted card. A minimal register-backed PIO model
now reports PF6 high while not configured as output, modeling an empty socket.
The guest reports `present:0`; the previous timer faults no longer appeared
in the observed runs, and `d:\apps\desktop.mod` loads and enters `app_entry`.
This does not emulate insertion or the SD controller.

## First LCD output

The guest only programmed DE mixer layers after LCD vertical-blank interrupts
were supplied. TCON LCD at 0x05461000 now has register storage and an
approximate 17 ms vblank, routed to PLIC source 106 (D1 peripheral 90).
TCON interrupt enable/status handling follows the matching public Melis
`de_lcd.c`; the timing is not panel-accurate. Mixer 0 register storage and
an experimental QEMU console scan out the first RGB888 UI layer: observed
registers 0x05103000/04/0c/10 contain attribute, size, stride, and address;
the app programs 1024x600 with a 3072-byte stride. The scanout does not yet
implement other planes, blending, scaling, rotation or format conversion
besides RGB888.

After about 70 seconds a QEMU `screendump` showed the touch-calibration
crosshair over the wallpaper (`run/f133-screen.png`). This is actual guest
framebuffer content, not a complete radio UI; the app entered touch setup.
Touch input was added subsequently, as described below. `run/cmdline-ui.sh` starts the same boot
with VNC on localhost port 5900. In this build VNC was enabled with
`meson configure qemu/build -Dvnc=enabled` followed by a ninja rebuild.
The original flash is protected by QEMU `-snapshot` on both run scripts.
VNC startup was verified by the RFB 3.8 handshake on 127.0.0.1:5900.
`python3 tools/smoke_ui.py` performs a headless boot regression, waits for
desktop installation and a first render, and checks a 1024x600 nonuniform
screendump. It does not check pixel-perfect output or exercise touch.

## Experimental touchscreen

The firmware uses a GT911 at I2C3 address 0x5d. Its `ctp_para` config
specifies 1024x600 and PG7 as the interrupt pin. I2C3 at 0x02502c00 uses
QEMU's Allwinner I2C model, routed to PLIC source 28. Its F133 variant
self-clears soft reset and preserves bus enable, as required by the Melis
engine-mode driver. The GT911 implementation identifies as 911 and provides
the registers observed during initialization and single-contact reporting.
QEMU absolute pointer and left-button events produce reports; GPIO PG7
interrupt status/enable at 0x020002d4/0x020002d0 route to PLIC source 95.

On QMP input tests Melis reads the contact and release and the UI changes
from Radio to Main after tapping Home. `make smoke-touch` repeats this test;
`make ui` exposes the input through the VNC viewer. GT911 report interrupts
are retried every 20 ms until the guest clears the report as a bring-up
approximation. Multitouch, controller timing, accurate GPIO electrical
behavior, and all GT911 registers are not implemented. Rapid event sequences
may overwrite an unacknowledged report. Flash writes still exhibit errors in
some runs; the snapshot protects the original image, but persistent settings
are not established by this test.

The Ghidra MCP backend was unavailable during this run. Raw reproduction logs
were written to `/tmp/opencode/f133-current-serial.log` and
`/tmp/opencode/f133-current.log` (temporary, not durable project artifacts).
