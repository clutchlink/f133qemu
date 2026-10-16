#!/usr/bin/env python3
"""Parse the GPT partition table found at flash offset 0xc200 (LBA 97).

Prints the GPT header fields and the partition entries (name, start, end)
so we know how boot0/melis address flash regions.
"""
import os
import struct
import uuid
import sys

FW = os.path.join(os.path.dirname(__file__), "..",
                  "F133-20220107_HZ-BX-B5678-UI3-V1.9-20240419-1616.bin")
GPT_OFF = 0xc200


def main():
    fw = open(FW, "rb").read()
    hdr = fw[GPT_OFF:GPT_OFF + 512]
    if hdr[:8] != b"EFI PART":
        print("no GPT at 0xc200")
        return 1
    (rev, hdr_size, crc, _r, cur_lba, bak_lba, first_usable, last_usable,
     guid, pe_lba, nent, ent_size, ent_crc) = struct.unpack(
        "<IIII QQQQ 16s Q III", hdr[8:92])
    print(f"GPT @0x{GPT_OFF:x} rev={rev} hdr_size={hdr_size}")
    print(f"  current_lba={cur_lba} backup_lba={bak_lba} "
          f"first_usable={first_usable} last_usable={last_usable}")
    print(f"  partition_entry_lba={pe_lba} num_entries={nent} entry_size={ent_size}")
    sec = 512
    for i in range(nent):
        e = fw[GPT_OFF + (pe_lba - cur_lba) * sec + i * ent_size:][:ent_size]
        tguid = e[:16]
        pstart, pend, _attr = struct.unpack("<QQQ", e[32:56])
        name = e[56:128].decode("utf-16-le").rstrip("\0")
        if not name and tguid == b"\0" * 16:
            continue
        tg = uuid.UUID(bytes_le=tguid)
        print(f"  [{i:2}] name={name!r:16} start_lba={pstart} end_lba={pend} "
              f"size=0x{(pend - pstart + 1) * sec:x} @0x{pstart * sec:x} type={tg}")


if __name__ == "__main__":
    sys.exit(main())