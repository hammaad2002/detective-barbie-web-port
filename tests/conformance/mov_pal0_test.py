#!/usr/bin/env python3
"""Verify Mattel .MOV container has DISP + pal0 palette chunk (used by BDETECT movie player)."""
import struct
import sys


def main() -> int:
    path = sys.argv[1] if len(sys.argv) > 1 else "work/install/DATA/OPENA.MOV"
    with open(path, "rb") as f:
        data = f.read(64)
    if len(data) < 12:
        print(f"FAIL: {path} too small")
        return 1
    size = struct.unpack_from("<I", data, 0)[0]
    tag = data[4:8]
    if tag != b"DISP":
        print(f"FAIL: expected DISP tag, got {tag!r}")
        return 1
    pal = data[8:12]
    if pal != b"pal0":
        print(f"FAIL: expected pal0 palette tag, got {pal!r}")
        return 1
    if size < 8:
        print(f"FAIL: bad DISP chunk size {size}")
        return 1
    print(f"OK: {path} DISP/pal0 header (chunk size={size})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
