#!/usr/bin/env python3
"""Set the system libc heap size in OpenOrbis' crt1.o.

OpenOrbis ships crt1.o with `sceLibcHeapSize = -1` (the "default" heap, about
12.7 MiB on retail hardware). That default only works when a mini app inherits
the shell's heap. A big app gets its own libc heap from this symbol, and the
first calloc then faults in libSceLibcInternal.

The value is a local symbol in crt1.o's .data, so defining sceLibcHeapSize in
the app does not override it. Rewrite the .data section instead:

    +0x00  u32  sceLibcHeapExtendedAlloc = 1
    +0x04  u32  padding
    +0x08  u64  sceLibcHeapSize (bytes)
    +0x10  u32  sce_libc_heap_delayed_alloc = 0
    +0x14  u32  sce_libc_heap_extended_alloc = 0
    +0x18  zero padding to 0x28

Usage:
    python3 tools/patch_crt1.py <crt1.o> [--heap-mib 512]
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

CRT_DATA_SIZE = 0x28


def section_header(data: bytes, name: str) -> tuple[int, int, int]:
    if data[:4] != b"\x7fELF" or data[4] != 2:
        raise SystemExit("crt1.o is not a 64-bit ELF file")
    e_shoff, = struct.unpack_from("<Q", data, 0x28)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    if not e_shoff or not e_shnum:
        raise SystemExit("crt1.o has no section table")

    def header(index: int) -> tuple[int, int, int, int, int, int]:
        off = e_shoff + index * e_shentsize
        sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size = struct.unpack_from(
            "<IIQQQQ", data, off)
        return sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size

    _, _, _, _, str_off, str_size = header(e_shstrndx)
    strings = data[str_off:str_off + str_size]
    for index in range(e_shnum):
        sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size = header(index)
        end = strings.find(b"\0", sh_name)
        current = strings[sh_name:end].decode("ascii", "replace")
        if current == name:
            return sh_offset, sh_size, sh_flags
    raise SystemExit(f"section {name} not found")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("crt1", type=Path)
    parser.add_argument("--heap-mib", type=int, default=512)
    args = parser.parse_args()

    if args.heap_mib <= 0:
        print("heap size must be positive", file=sys.stderr)
        return 2

    data = bytearray(args.crt1.read_bytes())
    offset, size, flags = section_header(data, ".data")
    if size < CRT_DATA_SIZE:
        print(f".data is only {size:#x} bytes; expected at least {CRT_DATA_SIZE:#x}",
              file=sys.stderr)
        return 2

    heap = args.heap_mib * 1024 * 1024
    wanted = struct.pack("<IIQII", 1, 0, heap, 0, 0) + bytes(CRT_DATA_SIZE - 24)
    if data[offset:offset + CRT_DATA_SIZE] == wanted:
        print(f"crt1.o already requests a {args.heap_mib} MiB heap")
        return 0

    data[offset:offset + CRT_DATA_SIZE] = wanted
    args.crt1.write_bytes(data)
    print(f"crt1.o heap set to {args.heap_mib} MiB (section .data at {offset:#x})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
