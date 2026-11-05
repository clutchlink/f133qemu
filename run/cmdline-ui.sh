#!/bin/sh
# F133 firmware with the experimental LCD output on localhost VNC :0 (5900).
# Build QEMU with VNC enabled; the flash overlay is temporary.
cd "$(dirname "$0")/.." || exit 1
exec qemu/build/qemu-system-riscv64 \
    -L qemu/pc-bios \
    -M f133 \
    -bios extract/boot0.bin \
    -drive if=mtd,format=raw,file=F133-20220107_HZ-BX-B5678-UI3-V1.9-20240419-1616.bin \
    -snapshot \
    -display none -vnc 127.0.0.1:0 \
    -serial stdio \
    "$@"
