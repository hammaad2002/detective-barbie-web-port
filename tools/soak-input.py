#!/usr/bin/env python3
"""Generate DETECTIVE_INPUT soak segment: random-walk clicks + keys after K10 entry.

Usage: python3 tools/soak-input.py <seed> [start_frame] [end_frame] [step_frames]

Prints comma-separated events (no K10 prefix). Default: frame 1200..45000 every 50 frames.
"""
import random
import sys

WIDTH, HEIGHT = 640, 480
# Escape can WM_QUIT; include rarely after gameplay starts (caller passes start_frame>=3000).
KEYS_SAFE = ("Space", "Return")
KEYS_ALL = ("Space", "Return", "Escape")


def main() -> None:
    if len(sys.argv) < 2:
        print("usage: soak-input.py <seed> [start] [end] [step]", file=sys.stderr)
        sys.exit(2)
    seed = int(sys.argv[1])
    start = int(sys.argv[2]) if len(sys.argv) > 2 else 1200
    end = int(sys.argv[3]) if len(sys.argv) > 3 else 45000
    step = int(sys.argv[4]) if len(sys.argv) > 4 else 50

    rng = random.Random(seed)
    x = rng.randint(80, WIDTH - 80)
    y = rng.randint(80, HEIGHT - 80)
    parts: list[str] = []
    frame = start
    while frame <= end:
        # ~15% key, else click at random-walk position
        if rng.random() < 0.12:
            if frame >= 25000 and rng.random() < 0.15:
                parts.append(f"{frame}:key(Escape)")
            else:
                parts.append(f"{frame}:key({rng.choice(KEYS_SAFE)})")
        else:
            x = max(20, min(WIDTH - 20, x + rng.randint(-60, 60)))
            y = max(20, min(HEIGHT - 20, y + rng.randint(-50, 50)))
            parts.append(f"{frame}:click({x},{y})")
        frame += step + rng.randint(-5, 5)

    print(",".join(parts))


if __name__ == "__main__":
    main()
