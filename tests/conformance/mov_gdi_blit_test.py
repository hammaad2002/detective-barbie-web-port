#!/usr/bin/env python3
"""Sanity-check 8bpp DIB row pitch used by Mattel .MOV frames (matches ddraw gdi_blit_dib)."""
import struct
import sys


def main() -> int:
    path = sys.argv[1] if len(sys.argv) > 1 else "work/install/DATA/OPENA.MOV"
    with open(path, "rb") as f:
        hdr = f.read(1036)
    if len(hdr) < 12 or hdr[4:8] != b"DISP" or hdr[8:12] != b"pal0":
        print(f"FAIL: {path} missing DISP/pal0 header")
        return 1
    # Palette RGBQUADs start at byte 12 inside DISP chunk body (after pal0 tag).
    pal = hdr[12 : 12 + 256 * 4]
    if len(pal) < 256 * 4:
        print("FAIL: truncated palette in DISP header")
        return 1
    colors = set()
    for i in range(256):
        b, g, r, _ = pal[i * 4 : i * 4 + 4]
        colors.add((r, g, b))
    if len(colors) < 50:
        print(f"FAIL: palette has only {len(colors)} distinct RGB entries")
        return 1
    # First frame index bytes after 1036-byte header (typical Mattel layout).
    with open(path, "rb") as f:
        f.seek(1036)
        frame = f.read(64 * 48)
    if len(frame) < 64:
        print("FAIL: could not read frame bytes")
        return 1
    w = 64
    pitch = ((w + 3) // 4) * 4
    px_colors = set()
    for y in range(min(48, len(frame) // pitch)):
        row = frame[y * pitch : y * pitch + w]
        for idx in row:
            b, g, r, _ = pal[idx * 4 : idx * 4 + 4]
            px_colors.add((r, g, b))
    if len(px_colors) < 10:
        print(f"FAIL: first frame slice only {len(px_colors)} mapped colors")
        return 1
    print(f"OK: {path} palette={len(colors)} colors, frame slice={len(px_colors)} colors, pitch={pitch}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
