#!/usr/bin/env python3
"""Boot the flash image and check app startup and nonempty LCD scanout.

Run from any directory: python3 tools/smoke_ui.py [timeout-seconds]
Artifacts are written under run/. The flash uses a temporary snapshot.
"""
import json
import pathlib
import socket
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
RUN = ROOT / "run"
FLASH = ROOT / "F133-20220107_HZ-BX-B5678-UI3-V1.9-20240419-1616.bin"


def main():
    timeout = int(sys.argv[1]) if len(sys.argv) > 1 else 120
    RUN.mkdir(exist_ok=True)
    image = RUN / "smoke-ui.ppm"
    with tempfile.TemporaryDirectory(prefix="f133-smoke-") as tmp:
        sockpath = pathlib.Path(tmp) / "qmp.sock"
        serial = pathlib.Path(tmp) / "serial.log"
        command = [str(ROOT / "qemu/build/qemu-system-riscv64"), "-M", "f133",
                   "-bios", str(ROOT / "extract/boot0.bin"), "-drive",
                   f"if=mtd,format=raw,file={FLASH}", "-snapshot", "-display",
                   "none", "-serial", f"file:{serial}", "-qmp",
                   f"unix:{sockpath},server=on,wait=off"]
        with (RUN / "smoke-ui-qemu.log").open("w") as log:
            proc = subprocess.Popen(command, cwd=ROOT, stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + timeout
                while time.monotonic() < deadline:
                    if proc.poll() is not None:
                        raise RuntimeError(f"QEMU exited with {proc.returncode}")
                    text = serial.read_bytes() if serial.exists() else b""
                    if b"First Render" in text and b"desktop.mod Installed!" in text:
                        break
                    time.sleep(0.5)
                else:
                    raise RuntimeError("timed out waiting for app rendering")
                with socket.socket(socket.AF_UNIX) as client:
                    client.settimeout(10)
                    client.connect(str(sockpath))
                    qmp = client.makefile("rwb", buffering=0)
                    json.loads(qmp.readline())

                    def execute(name, arguments=None):
                        request = {"execute": name}
                        if arguments is not None:
                            request["arguments"] = arguments
                        qmp.write(json.dumps(request).encode() + b"\n")
                        while True:
                            reply = json.loads(qmp.readline())
                            if "error" in reply:
                                raise RuntimeError(str(reply["error"]))
                            if "return" in reply:
                                return

                    execute("qmp_capabilities")
                    execute("screendump", {"filename": str(image)})
                data = image.read_bytes()
                header = b"P6\n1024 600\n255\n"
                if (not data.startswith(header) or
                        len(data) != len(header) + 1024 * 600 * 3 or
                        len(set(data[len(header):])) < 4):
                    raise RuntimeError("unexpected or blank screenshot")
                print(f"PASS: app rendered; screenshot {image}")
            finally:
                proc.terminate()
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()


if __name__ == "__main__":
    main()
