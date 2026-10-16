#!/usr/bin/env python3
"""Carve the F133 radio firmware into its constituent images.

Verified layout (see AGENTS.md section 2):
  0x000000  boot0 (eGON.BT0)                       <= 0x10000
  0x010000  sunxi-package table (2 entries)
            entry records: tag(4) + name + u32 rel_off, u32 size @ rec+0x44
            offsets relative to 0x10000
  entry 1: MIE;melis-lzma   -> LZMA-alone stream -> CHK "opensbi" image
  entry 2: IIE;melis-config -> raw config tree

Outputs into extract/:
  boot0.bin, boot0.header.txt
  package_table.txt
  opensbi_blob.bin     (decompressed melis-lzma)
  melis_config.bin     (raw config tree)
"""
import lzma
import os
import struct
import sys

FW = os.path.join(os.path.dirname(__file__), "..",
                  "F133-20220107_HZ-BX-B5678-UI3-V1.9-20240419-1616.bin")
OUT = os.path.join(os.path.dirname(__file__), "..", "extract")
PKG_BASE = 0x10000


def main():
    os.makedirs(OUT, exist_ok=True)
    fw = open(FW, "rb").read()
    print(f"firmware: {len(fw)} bytes (0x{len(fw):x})")

    # ---- boot0 ------------------------------------------------------------
    boot0 = fw[:PKG_BASE]
    with open(f"{OUT}/boot0.bin", "wb") as f:
        f.write(boot0)
    last = max(o for o in range(len(boot0)) if boot0[o] != 0)
    hdr = []
    hdr.append(f"boot0: carved 0x0..0x{PKG_BASE:x} -> boot0.bin (last nonzero 0x{last:x})")
    hdr.append(f"  0x00: {fw[0x00:0x10].hex(' ')}   (0x0: jal? / 0x4: eGON.BT0 magic)")
    for off in range(0x10, 0x40, 4):
        w = struct.unpack("<I", fw[off:off + 4])[0]
        hdr.append(f"  0x{off:02x}: 0x{w:08x}  {fw[off:off+4].hex(' ')}")
    hdr.append(f"  0x2c string: {fw[0x2c:0x38].split(chr(0).encode())[0].decode(errors='replace')}")
    open(f"{OUT}/boot0.header.txt", "w").write("\n".join(hdr) + "\n")
    print("\n".join(hdr))

    # ---- sunxi-package table ---------------------------------------------
    lines = ["sunxi-package table @ 0x10000:"]
    images = []
    off = PKG_BASE
    while off < 0x10400:
        tag = fw[off:off + 4]
        if tag in (b"MIE;", b"IIE;"):
            name = fw[off + 4:off + 0x44].split(b"\0")[0].decode(errors="replace")
            rel, size = struct.unpack("<II", fw[off + 0x44:off + 0x4c])
            fstart = PKG_BASE + rel
            blob = fw[fstart:fstart + size] if rel < len(fw) and size <= len(fw) else b""
            comp = bool(blob) and blob[0] < 0xe0
            lines.append(f"  {tag.decode()} {name!r}: rel=0x{rel:x} file=0x{fstart:x} "
                         f"size=0x{size:x} props_byte="
                         + (f"0x{blob[0]:02x}" if blob else "<oob>")
                         + f" ({'lzma-alone' if comp else 'raw?'})")
            if blob and name:
                images.append((tag.decode(), name, fstart, size, blob, comp))
            off += 0x48
        off += 4
    open(f"{OUT}/package_table.txt", "w").write("\n".join(lines) + "\n")
    print("\n".join(lines))

    # ---- extract each entry ----------------------------------------------
    for _, name, fstart, size, blob, comp in images:
        out, fn = blob, f"{OUT}/{name}.bin"
        if comp:
            try:
                out = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE).decompress(blob)
                print(f"  {name}: lzma 0x{size:x} -> {fn} (0x{len(out):x} bytes)")
            except lzma.LZMAError as e:
                print(f"  {name}: NOT lzma ({e}) — saving raw")
                comp = False
        else:
            print(f"  {name}: raw -> {fn} (0x{size:x} bytes)")
        with open(fn, "wb") as f:
            f.write(out)
        print(f"    head: {out[:16].hex(' ')}")


if __name__ == "__main__":
    sys.exit(main())