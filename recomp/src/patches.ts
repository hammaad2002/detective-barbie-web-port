// Code patches applied to the player's own BDETECT.EXE at translation time (never shipped as game
// bytes: each entry only names an address and the instruction bytes expected there).
//
// Frame-rate patches: the game caps its walk loop at 62 ms (16 fps) and its menu/screen loop at
// 66 ms (15 fps), and draws its own mouse cursor once per loop. Game logic in both loops is
// scheduled by timeGetTime (objects wake at "now + delay"; walking moves per elapsed ms), so
// lowering the cap to 10 ms gives a fresh frame for every 60 Hz refresh without changing game speed (verified:
// identical walk distance per second and identical intro timing; see docs/PERF-NOTES.md).
// The ride minigames and the movie player keep their own pacing (their logic is per-frame).

export interface CodePatch {
  va: number;
  expect: number[];
  replace: number[];
  why: string;
}

export const CODE_PATCHES: CodePatch[] = [
  { va: 0x43656a, expect: [0x83, 0xf8, 0x3e], replace: [0x83, 0xf8, 0x0a], why: "walk loop: cmp elapsed,62 -> 10" },
  { va: 0x43656f, expect: [0xba, 0x3e, 0, 0, 0], replace: [0xba, 0x0a, 0, 0, 0], why: "walk loop: Sleep(62-elapsed) -> 10" },
  { va: 0x42334e, expect: [0x83, 0xf8, 0x42], replace: [0x83, 0xf8, 0x05], why: "screen loop: cmp elapsed,66 -> 5" },
  { va: 0x423353, expect: [0xba, 0x42, 0, 0, 0], replace: [0xba, 0x05, 0, 0, 0], why: "screen loop: Sleep(66-elapsed) -> 5" },
  // Menu-screen cursor object (0x43e790) re-arms itself 30 ms ahead, so the pointer moved at ~33 Hz.
  // It only reads the mouse and hit-tests buttons (no animation steps), so 10 ms is safe.
  { va: 0x43ec5b, expect: [0x83, 0xc0, 0x1e], replace: [0x83, 0xc0, 0x05], why: "menu cursor: wake every 30 ms -> 5" },
];

/** Apply every patch whose expected bytes are present; other executables are left untouched. */
export function applyCodePatches(mapped: Uint8Array, imageBase: number): number {
  let applied = 0;
  for (const p of CODE_PATCHES) {
    const o = p.va - imageBase;
    if (o < 0 || o + p.expect.length > mapped.length) continue;
    if (!p.expect.every((b, i) => mapped[o + i] === b)) continue;
    mapped.set(p.replace, o);
    applied++;
  }
  return applied;
}
