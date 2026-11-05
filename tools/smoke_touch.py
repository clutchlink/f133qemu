#!/usr/bin/env python3
"""Check that a QMP pointer tap on Home reaches the firmware UI."""
import json
import pathlib
import socket
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
FLASH = ROOT / "F133-20220107_HZ-BX-B5678-UI3-V1.9-20240419-1616.bin"


def wait_for(path, marker, timeout, proc):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"QEMU exited: {proc.returncode}")
        if path.exists() and marker in path.read_bytes():
            return
        time.sleep(0.25)
    raise RuntimeError(f"timed out waiting for {marker!r}")


def main():
    timeout = int(sys.argv[1]) if len(sys.argv) > 1 else 120
    run = ROOT / "run"
    run.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="f133-touch-") as tmp:
        tmp = pathlib.Path(tmp)
        serial, qmp_path = tmp / "serial.log", tmp / "qmp.sock"
        command = [str(ROOT / "qemu/build/qemu-system-riscv64"), "-M", "f133",
                   "-bios", str(ROOT / "extract/boot0.bin"), "-drive",
                   f"if=mtd,format=raw,file={FLASH}", "-snapshot", "-display",
                   "none", "-serial", f"file:{serial}", "-qmp",
                   f"unix:{qmp_path},server=on,wait=off"]
        with (run / "smoke-touch-qemu.log").open("w") as qemu_log:
            proc = subprocess.Popen(command, cwd=ROOT, stdout=qemu_log,
                                    stderr=qemu_log)
            try:
                wait_for(serial, b"First Render", timeout, proc)
                with socket.socket(socket.AF_UNIX) as client:
                    client.settimeout(10)
                    client.connect(str(qmp_path))
                    stream = client.makefile("rwb", buffering=0)
                    json.loads(stream.readline())

                    def execute(name, arguments=None):
                        request = {"execute": name}
                        if arguments is not None:
                            request["arguments"] = arguments
                        stream.write(json.dumps(request).encode() + b"\n")
                        while True:
                            reply = json.loads(stream.readline())
                            if "error" in reply:
                                raise RuntimeError(str(reply["error"]))
                            if "return" in reply:
                                return

                    execute("qmp_capabilities")
                    # Absolute coordinates use QEMU's 0..32767 range. This
                    # hits the Home icon near (50, 33) on the 1024x600 UI.
                    for down in (True, False):
                        execute("input-send-event", {"events": [
                            {"type": "abs", "data": {"axis": "x", "value": 1600}},
                            {"type": "abs", "data": {"axis": "y", "value": 1800}},
                            {"type": "btn", "data": {"button": "left", "down": down}},
                        ]})
                        time.sleep(1)
                    wait_for(serial, b"EnterUi Main", 30, proc)
                    execute("screendump", {"filename": str(run / "smoke-touch.ppm")})
                print("PASS: touch press and release navigated from Radio to Main")
            finally:
                if serial.exists():
                    (run / "smoke-touch-serial.log").write_bytes(serial.read_bytes())
                proc.terminate()
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()


if __name__ == "__main__":
    main()
