#!/bin/sh
# F133 radio firmware emulation — M3/M4 bring-up command line.
# Boot0 runs from SRAM; the emulated SPI NOR (w25q128) holds the real flash
# image so boot0 can read the package/config from flash.
cd "$(dirname "$0")/.." || exit 1
exec qemu/build/qemu-system-riscv64 \
    -M f133 \
    -bios extract/boot0.bin \
    -drive if=mtd,format=raw,file=F133-20220107_HZ-BX-B5678-UI3-V1.9-20240419-1616.bin \
    -snapshot \
    -nographic \
    "$@"
