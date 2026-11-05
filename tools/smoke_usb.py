#!/usr/bin/env python3
"""Boot a temporary FAT32 USB disk and check the guest mounts it."""
import pathlib
import subprocess
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
FLASH = ROOT / "F133-20220107_HZ-BX-B5678-UI3-V1.9-20240419-1616.bin"


def main():
    run = ROOT / "run"
    run.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="f133-usb-") as tmp:
        image = pathlib.Path(tmp) / "disk.img"
        with image.open("wb") as f:
            f.truncate(128 * 1024 * 1024)
        subprocess.run(["mkfs.fat", "-F", "32", "-n", "F133USB", str(image)],
                       check=True, stdout=subprocess.DEVNULL)
        log = run / "smoke-usb-serial.log"
        command = [str(ROOT / "qemu/build/qemu-system-riscv64"), "-M", "f133",
                   "-bios", str(ROOT / "extract/boot0.bin"), "-drive",
                   f"if=mtd,format=raw,file={FLASH}", "-drive",
                   f"if=none,id=usbdisk,format=raw,file={image}", "-device",
                   "usb-storage,drive=usbdisk", "-snapshot", "-display", "none",
                   "-serial", f"file:{log}"]
        with (run / "smoke-usb-qemu.log").open("w") as qemu_log:
            proc = subprocess.Popen(command, cwd=ROOT, stdout=qemu_log,
                                    stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 120
                while time.monotonic() < deadline:
                    if proc.poll() is not None:
                        raise RuntimeError(f"QEMU exited: {proc.returncode}")
                    data = log.read_bytes() if log.exists() else b""
                    if (b"fat,mount on F." in data and
                            b"SCSI_DISK_100" in data and b"First Render" in data):
                        print("PASS: USB disk enumerated, FAT mounted on F:, UI rendered")
                        return
                    if b"assertion failed" in data:
                        raise RuntimeError("guest assertion; see " + str(log))
                    time.sleep(.25)
                raise RuntimeError("USB mount timed out; see " + str(log))
            finally:
                proc.terminate()
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()


if __name__ == "__main__":
    main()
