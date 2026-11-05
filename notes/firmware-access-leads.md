# Firmware access leads (surface-level Ghidra analysis)

Analyzed the existing `opensbi-chk.bin` raw image project at base 0x40000000;
these findings apply to this image, not necessarily similar devices. Addresses
are Ghidra addresses in the raw CHK image, not established runtime addresses.

- `FUN_400386b0` calls `FUN_400c0018` during initialization after opening the
  D partition and starting several modules. The latter creates a `tshell`
  thread at `FUN_400bfb20`. Whether its input is available on a physical UART
  on another device has not been established.
- The shell thread calls `FUN_400bf8b4` on `d:\startup.sh` unless a global
  panic-related flag is set. That function opens the file, reads lines, and
  passes non-comment lines to the shell dispatcher `FUN_400bf09a`. In this
  emulated image D is flash ROOTFS, not known to be removable media.
- `FUN_400c079e` implements `insmod`, passing its one argument to
  `esMODS_MInstall` at `FUN_4010444e`, then opening the installed module.
  `FUN_40103c86` opens the given path and recognizes ePDK module/executable
  formats, including an ELF path with a `MAGIC` section; the install path
  invokes a module callback. This supports module execution but does not prove
  removable-storage paths, module signing policy, or a compatible test module.
- `FUN_400c1710` implements hexdump. Positional arguments are input paths;
  `FUN_400c1484` opens each input and reads it. Critically, `-f` opens an output
  path with flags 0x601 and writes dumped bytes to it. Do not use `-f` for a
  read-only test; whether raw flash is exposed as a readable node is unknown.
- `FUN_400c0268` recognizes `reboot efex`: it writes `0x5aa5a55a` to
  `0x07090108` via `FUN_400548d8`, then calls the reboot routine. This is not
  proof of safe RAM-only recovery or readback; avoid on unbacked-up hardware.
- `FUN_400c09e0` loads `/mnt/D/mod/update.mod` and invokes different module
  operations for `-f` and an update file, followed by reboot. Its help states
  `-f` erases boot0; neither branch is a demonstrated read-only dump path.

## Removable media and Bluetooth

- `FUN_4004f5f4` registers an SD/MMC block device named `SDMMC-DISK` plus a
  card number. It does not itself establish execution of a file from the card.
- `FUN_4004f9b4` (`melis_part_init_bootcard`) reads SD sectors at 0xa000 and
  recognizes an eight-byte `softw411` signature, logging `PhoenixCard burn is
  mbr`, then registers partitions. The inspected function is partition setup,
  not evidence that an inserted card automatically boots or programs flash.
  Boot0 and other callers need separate analysis; PhoenixCard is not a safe
  dumping procedure based on this evidence.
- `FUN_40091cfc` (`UsbBlkDevReg`) registers a USB mass-storage host device;
  this is evidence for accessing an inserted USB disk, not for exporting the
  device's own flash. `FUN_4008dae8` and `FUN_4008e000` initialize USB gadget
  NCM and `mirror` functions, respectively. Neither establishes a dump or code
  execution interface, or that those modes are enabled on the target device.
- No Bluetooth dump, module-loading, or debug interface was identified in this
  kernel-image pass. Application modules and variant-specific firmware have not
  been analyzed.

### USB follow-up

- `FUN_40094a56` reads `usbc0` configuration including `usb_used`,
  `usb_port_type`, detection mode and ID/VBUS GPIOs. Port type selects a USB
  device, host, or hardware-role scanning thread. Thus the behavior of an
  individual connector depends on configuration and board wiring.
- `FUN_40094cac` switches the USB0 role; in device role it calls
  `FUN_4008e4ca`, which chooses NCM for mode 0 and mirror for mode 1. Neither
  path has been shown to expose a file transfer, flash access or shell.
- The raw flash filesystem metadata contains `desktop.mod`, `update.mod`,
  and `SetupUpdate*.data` names. Their presence alone does not show what the
  app searches for on USB. No automatic USB module/script execution is
  established.

### USB application and emulation follow-up

Using `dump_tool extract` from Gamadril/d1s-melis-tools v0.6.0 on the original
image, extracted the MinFS `apps/init.axf` to `/tmp/opencode/usb-probe/`.
SHA-256: `919980a3fd331c905bb0bde19e00809c6fd96ed0a50d37969a9411cce51decf5`.
The extracted app includes `CarApp_DiskIn`, `%s\\Update\\%s`,
`%s\\Update\\Config.ini`, `LTTF133.img`, and `LTTMcu.bin`. Ghidra at
`0xe9291144` and `0xe929245c` shows constructed paths under `Update`; the
decompiler mistakenly treats some calls as non-returning, so complete control
flow and variant behavior require further verification. Independent research:
https://github.com/Gamadril/d1s-melis-tools/blob/main/docs/INFO.md (describes
automatic update/config imports on a related, not identical, radio).
Disassembly after the incorrectly non-returning call at `0xe92924b0` shows
the app checks whether the constructed update path exists and proceeds to a
further check at `0xe928e4d2`; its full behavior has not been established.

In a snapshot-protected QEMU run, a disposable FAT32 USB1 image containing
root `startup.sh` and `autorun.sh`, both containing
`echo USB_AUTORUN_PROBE_7F31`, mounted as F: and produced
`CarApp_DiskIn F:,type 1`. No marker appeared during the 115-second boot.
This does not prove the files were read or exclude other triggers and names.
Do not use update filenames or Config.ini on an unbacked-up physical device.

A second 115-second snapshot-protected QEMU run added an empty, invalid
`Update/LTTF133.img` to the same test image. F: mounted and `CarApp_DiskIn`
appeared, but the serial log showed no update-file detection or execution.
This does not establish whether the file was checked, whether the empty file
was rejected silently, or whether a later/UI-triggered scan would act on it.

Hot-plugging the same image via QMP after `First Render` produced a different
result: the application logged `CarApp_DiskIn F:,type 1`, `UpdateApp
F:\Update\LTTF133.img` and `CarApi_CheckImage`, installed `update.mod`, logged
`no version section,can update`, entered `SetupUpdateApp`, then logged `begin
to update` and rejected the zero-byte file as `img is invalid`. The guest also
attempted NOR writes which the QEMU SPI model reported as failures. Their exact
relationship to the updater and config/cache operations is not established.
This is not a safe physical-device test; `-snapshot` protected both images.
The USB disk-in handler around `0xe928190c` calls `0xe9281872` on USB paths,
which reaches the update handler at `0xe929245c`, subject to conditions. The
boot-attached negative test was therefore misleading about this hotplug path.
The serial transcript is preserved in `run/usb-access-hotplug.log`.

Hot-plugging the benign script image after rendering also logged `UpdateApp
F:\Update\LTTF133.img` and `UpdateConfig F:` without those files present;
neither script marker appeared during the 20-second observation. Transcript:
`run/usb-access-scripts-hotplug.log`. This supports absence of those two
automatic script triggers under the observed conditions, not a general absence
of USB execution mechanisms.

An independent F133 tool `xfel_spi_nor` supports FEL `read <address> <length>
<file>` as well as destructive write/erase commands. This is a conditional USB
readout lead only if this particular board can be put into FEL without altering
flash and the appropriate USB device port is accessible; neither is confirmed,
and USB0 device mode is not emulated here. See the related-device documentation
linked above; do not assume its FEL entry procedure or wiring applies.
