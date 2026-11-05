#!/usr/bin/env python3
"""Initialize and safely prepare the pinned QEMU submodule for F133."""
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
QEMU = ROOT / "qemu"
REVISION = "4fc49f46dc95d4a27de2509e7fceb2931e91faeb"
PATCH = ROOT / "qemu.patch"
LINKS = {
    "hw/riscv/f133.c": "f133.c",
    "hw/ssi/sunxi_spi.c": "sunxi_spi.c",
    "hw/dma/sunxi_dma.c": "sunxi_dma.c",
    "include/hw/riscv/f133.h": "f133.h",
    "include/hw/ssi/sunxi_spi.h": "sunxi_spi.h",
    "include/hw/dma/sunxi_dma.h": "sunxi_dma.h",
}


def git(*args, check=True):
    return subprocess.run(["git", *args], cwd=ROOT, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          check=check)


def main():
    if not (QEMU / ".git").exists():
        if QEMU.exists() and any(QEMU.iterdir()):
            raise RuntimeError("qemu/ is not an initialized submodule and is not empty")
        subprocess.run(["git", "submodule", "update", "--init", "--", "qemu"],
                       cwd=ROOT, check=True)

    actual = git("-C", str(QEMU), "rev-parse", "HEAD").stdout.strip()
    if actual != REVISION:
        raise RuntimeError(f"qemu/ is at {actual}, expected {REVISION}; "
                           "resolve its state manually before setup")

    # Check all paths before changing anything. Never replace existing work.
    for target, name in LINKS.items():
        dest = QEMU / target
        source = ROOT / "src" / name
        if not source.is_file():
            raise RuntimeError(f"missing source file: {source}")
        if os.path.lexists(dest) and (not dest.is_symlink() or
                                     dest.resolve() != source.resolve()):
            raise RuntimeError(f"refusing to replace existing path: {dest}")

    forward = git("-C", str(QEMU), "apply", "--check", str(PATCH), check=False)
    reverse = git("-C", str(QEMU), "apply", "--reverse", "--check",
                  str(PATCH), check=False)
    if forward.returncode == 0:
        subprocess.run(["git", "-C", str(QEMU), "apply", str(PATCH)], check=True)
        print("Applied qemu.patch")
    elif reverse.returncode == 0:
        print("qemu.patch already applied")
    else:
        raise RuntimeError("qemu.patch is neither cleanly applicable nor already "
                           f"applied; inspect local changes:\n{forward.stderr}")

    for target, name in LINKS.items():
        dest = QEMU / target
        if not os.path.lexists(dest):
            source = ROOT / "src" / name
            dest.symlink_to(os.path.relpath(source, dest.parent))
            print(f"Linked {target} -> src/{name}")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"QEMU setup failed: {exc}", file=sys.stderr)
        sys.exit(1)
