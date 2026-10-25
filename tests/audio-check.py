#!/usr/bin/env python3
"""Validate a DETECTIVE_AUDIO_DUMP WAV: non-silent, low clipping, minimum duration."""
import math
import struct
import sys
import wave


def analyze(path: str, min_seconds: float = 30.0, clip_frac_max: float = 0.05):
    with wave.open(path, "rb") as w:
        ch = w.getnchannels()
        rate = w.getframerate()
        width = w.getsampwidth()
        nframes = w.getnframes()
        raw = w.readframes(nframes)

    if ch != 2 or width != 2:
        raise SystemExit(f"expected 16-bit stereo, got {width * 8}-bit {ch}ch")

    duration = nframes / float(rate)
    if duration < min_seconds:
        raise SystemExit(f"duration {duration:.1f}s < {min_seconds}s")

    clip = 0
    sum_sq = 0.0
    n = 0
    for i in range(0, len(raw), 2):
        v = struct.unpack_from("<h", raw, i)[0]
        x = v / 32768.0
        sum_sq += x * x
        n += 1
        if abs(v) >= 32760:
            clip += 1

    rms = math.sqrt(sum_sq / max(n, 1))
    clip_frac = clip / max(n, 1)

    print(f"path={path} rate={rate} duration={duration:.2f}s rms={rms:.6f} clip_frac={clip_frac:.4f}")

    if rms <= 0.0:
        raise SystemExit("RMS is zero (silent dump)")
    if clip_frac > clip_frac_max:
        raise SystemExit(f"clipping {clip_frac * 100:.2f}% > {clip_frac_max * 100:.0f}%")

    print("AUDIO CHECK PASS")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <path.wav>", file=sys.stderr)
        raise SystemExit(2)
    raise SystemExit(analyze(sys.argv[1]))
