#!/usr/bin/env python3
"""Validate DETECTIVE_DUMP_FRAME PPMs from native movie smoke (m1-final-frames)."""
import hashlib
import os
import sys
from PIL import Image


def group_stats(files: list[str]) -> tuple[int, int, int, int]:
    hashes = []
    colors = []
    px_diff = 0
    for fn in files:
        im = Image.open(fn).convert("RGB")
        colors.append(len(set(im.getdata())))
        hashes.append(hashlib.sha256(im.tobytes()).hexdigest())
    trans = sum(1 for i in range(1, len(hashes)) if hashes[i] != hashes[i - 1])
    for i in range(1, len(files)):
        a = Image.open(files[i - 1]).convert("RGB").tobytes()
        b = Image.open(files[i]).convert("RGB").tobytes()
        px_diff += sum(1 for j in range(0, len(a), 3) if a[j : j + 3] != b[j : j + 3])
    return min(colors), len(set(hashes)), trans, px_diff


def main() -> int:
    base = sys.argv[1] if len(sys.argv) > 1 else "work/m1-final-frames"
    groups = {g: [] for g in ("m1a", "m1b", "m1c")}
    for fn in sorted(os.listdir(base)):
        if not fn.endswith(".ppm"):
            continue
        g = fn.split("-", 1)[0]
        if g in groups:
            groups[g].append(os.path.join(base, fn))
    fail = False
    all_hashes: set[str] = set()
    for g, files in groups.items():
        if len(files) < 6:
            print(f"FAIL {g}: need >=6 frames, got {len(files)}")
            fail = True
            continue
        mc, uh, tr, pd = group_stats(files)
        if mc <= 50:
            print(f"FAIL {g}: only {mc} distinct colors")
            fail = True
        if tr < 1 and pd < 500:
            print(f"FAIL {g}: no visible animation (trans={tr} px_diff={pd})")
            fail = True
        for fn in files:
            all_hashes.add(hashlib.sha256(open(fn, "rb").read()).hexdigest())
        print(f"OK {g}: frames={len(files)} colors>={mc} unique_hashes={uh} anim_trans={tr}")
    if len(all_hashes) < 3:
        print(f"FAIL: need >=3 distinct frames across groups, got {len(all_hashes)}")
        fail = True
    if fail:
        return 1
    print(f"OK: mov native validate ({len(all_hashes)} distinct frame blobs)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
