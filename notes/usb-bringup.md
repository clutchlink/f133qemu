# USB mass-storage bring-up

The firmware starts EHCI host drivers for USB0 and USB1. The upstream Linux
`sunxi-d1s-t113.dtsi` describes EHCI1 at 0x04200000 (peripheral IRQ 33,
PLIC source 49) and EHCI0 at 0x04101000 (peripheral IRQ 30, source 46).
USB1 PHY registers start at 0x04200800. Both regions remain stubs in the
normal machine.

An experiment used QEMU's `aw-h3-ehci-usb` at USB1, IRQ 49, and a
`usb-storage` device backed by a 128 MiB FAT32 filesystem image. With the
storage connected at boot, the guest asserted in `rt_sem_control` before UI
startup. Connecting it through QMP after `First Render` printed
`ehci_irq: highspeed device connect`, then produced the same assertion.
Ghidra's backtrace resolves 0x40081024 as `usb_hcd_poll_rh_status`, calling
`usb_hcd_giveback_urb` (0x40081612) and callback 0x4007e38e, which invokes a
semaphore control operation via 0x400bb1c0. The invalid semaphore's origin
and whether the cause is a guest race or inaccurate controller behavior remain
unresolved. No enumeration or FAT mount was observed. Logs from the experiment
were written to `/tmp/opencode/usb-test.log` and `/tmp/opencode/usb-hot.log`.

## Working USB1 mass-storage experiment

The assertion was avoided by exposing one EHCI port, rather than the six
provided by QEMU's generic H3 model. The F133 subclass sets `portnr = 1`.
Control transfers initially timed out: EHCI's async list was programmed at
`0xc0279040`, where the open-bus fallback returned `0xf1f0f1f0`. An experimental
priority alias maps `0xc0000000` onto DRAM at `0x40000000`. This is a pragmatic
translation for the firmware's DMA addresses, not a verified model of the
hardware bus; the alias is visible to the entire system, not only EHCI.

With this alias, the guest enumerates QEMU `usb-storage` (46f4:0001), registers
`SCSI_DISK_100`, and mounts a 128 MiB raw FAT32 image at `F:`. The UI also
renders. `make smoke-usb` checks these messages on a disposable image; it does
not verify guest file contents or writes. `-snapshot` discards writes to both
the firmware and USB disk. USB1 PHY behavior, USB0, OHCI, low/full-speed
devices and accurate DMA translation are not implemented or verified.
