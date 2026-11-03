// Code patches applied to the player's own BDETECT.EXE at translation time (never shipped as game
// bytes: each entry only names an address and the instruction bytes expected there).
//
// Frame-rate patches: the game caps its walk loop at 62 ms (16 fps) and its menu/screen loop at
// 66 ms (15 fps), and draws its own mouse cursor once per loop. Menu-screen objects wake at
// "now + delay", so the faster screen loop does not change their speed. In-game objects advance
// once per walk-loop pass, so the runtime keeps them on the original 62 ms steps and blends what it
// draws in between (runtime/src/hle/smooth.cpp). On the web both loops then wait for the display's
// next refresh instead of sleeping (kernel32 Sleep). See docs/PERF-NOTES.md.
// The ride minigames and the movie player keep their own pacing (their logic is per-frame).

export interface CodePatch {
  va: number;
  expect: number[];
  replace: number[];
  why: string;
}

/** `call rel32` placed at `at` that targets `to`. */
function call(at: number, to: number): number[] {
  const rel = (to - (at + 5)) >>> 0;
  return [0xe8, rel & 0xff, (rel >>> 8) & 0xff, (rel >>> 16) & 0xff, rel >>> 24];
}
/** `call [0x48c530]`: BDETECT.EXE's import slot for winmm!timeGetTime. */
const callTimeGetTime = [0xff, 0x15, 0x30, 0xc5, 0x48, 0x00];

export const CODE_PATCHES: CodePatch[] = [
  { va: 0x43656a, expect: [0x83, 0xf8, 0x3e], replace: [0x83, 0xf8, 0x0a], why: "walk loop: cmp elapsed,62 -> 10" },
  { va: 0x43656f, expect: [0xba, 0x3e, 0, 0, 0], replace: [0xba, 0x0a, 0, 0, 0], why: "walk loop: Sleep(62-elapsed) -> 10" },
  { va: 0x42334e, expect: [0x83, 0xf8, 0x42], replace: [0x83, 0xf8, 0x05], why: "screen loop: cmp elapsed,66 -> 5" },
  { va: 0x423353, expect: [0xba, 0x42, 0, 0, 0], replace: [0xba, 0x05, 0, 0, 0], why: "screen loop: Sleep(66-elapsed) -> 5" },
  // The walk loop places the camera from Barbie's position before the scheduler moves her. With
  // logic steps every 62 ms but frames every 10 ms, the camera caught up on in-between frames, so
  // she jittered one step ahead and back. The camera now reads a copy (kCameraPos in
  // runtime/src/core/rt.h) that timeGetTime refreshes only on step frames, as in the original.
  { va: 0x4364a9, expect: [0x66, 0xfc, 0x47, 0], replace: [0xf2, 0xff, 0x2e, 0], why: "walk camera: read y from kCameraPos+2" },
  { va: 0x4364b0, expect: [0x64, 0xfc, 0x47, 0], replace: [0xf0, 0xff, 0x2e, 0], why: "walk camera: read x from kCameraPos" },
  { va: 0x4364b9, expect: [0x68, 0xfc, 0x47, 0], replace: [0xf4, 0xff, 0x2e, 0], why: "walk camera: read z from kCameraPos+4" },
  // Menu-screen cursor object (0x43e790) re-arms itself 30 ms ahead, so the pointer moved at ~33 Hz.
  // It only reads the mouse and hit-tests buttons (no animation steps), so 10 ms is safe.
  { va: 0x43ec5b, expect: [0x83, 0xc0, 0x1e], replace: [0x83, 0xc0, 0x05], why: "menu cursor: wake every 30 ms -> 5" },
  // Smooth motion between the 62 ms logic steps (runtime/src/hle/smooth.cpp). The scheduler runs the
  // object scripts and then draws every object from its record; two stubs in alignment padding call
  // timeGetTime right before that draw (records get blended positions) and right after it (true
  // positions come back before any other code reads them). The runtime recognises the calls by
  // their return addresses (0x4247dc, 0x4247f7) and leaves every other caller of these paths alone.
  {
    va: 0x4247d1,
    expect: new Array(12).fill(0x90),
    replace: [...call(0x4247d1, 0x423fc0), ...callTimeGetTime, 0xc3],
    why: "stub: 0x423fc0, then timeGetTime (before-draw hook), ret",
  },
  {
    va: 0x4247f1,
    expect: new Array(12).fill(0x90),
    replace: [...callTimeGetTime, ...call(0x4247f7, 0x430fd0), 0xc3],
    why: "stub: timeGetTime (after-draw hook), then 0x430fd0, ret",
  },
  { va: 0x424ce7, expect: call(0x424ce7, 0x423fc0), replace: call(0x424ce7, 0x4247d1), why: "scheduler: before-draw stub" },
  { va: 0x436531, expect: call(0x436531, 0x430fd0), replace: call(0x436531, 0x4247f1), why: "walk loop: after-draw stub" },
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
