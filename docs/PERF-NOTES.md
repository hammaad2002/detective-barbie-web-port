# Performance notes — web 60 FPS (z1-fps)

## Native pacing (BDETECT.EXE)

The game paces animation with **DirectDraw `Flip`** (bumps a flip counter), **`WaitForVerticalBlank`** (no-op on our HLE except web), **`timeGetTime` / `GetTickCount`** (wall-clock ms from process start), and occasional **`Sleep(1)`** on worker threads around mutex handoffs. Native headless dummy SDL runs **~64–83 presenter loops/s** but the game only flips when its logic advances (~**15–30** visible animation updates/s on title/hub).

## Before (pre z1-fps)

| Metric | title | hub | animated hub |
|--------|------:|----:|-------------:|
| Presentation FPS (`__detect.frames` / s) | ~25–35 | ~25–35 | ~25–35 |
| rAF p95 frame time | ~35–50 ms | ~35–50 ms | ~35–50 ms |
| Compose + upload | per-frame `vector` alloc, JS BGRA swap loop + `createImageData` | same | same |
| Guest flip pacing | uncapped; `Sleep` used `sleep_for` on pthreads | same | same |

Headless Chrome uses **software GL**; numbers below are from that environment (GPU browser builds are typically faster).

## After (z1-fps)

Changes: **`compose_rgba8`** with 8-bit palette LUT into a fixed **640×480** buffer; **`present_rgba8`** reuses one `ImageData` + `subarray` memcpy; **`Flip` → `present_wait` + 60 Hz `emscripten_sleep`**; **`Sleep` / audio feeder** use **`emscripten_sleep`**; build **`-O3 -msimd128`** + optional **`wasm-opt -O3 --simd128`**.

Run `node tools/web-fps.mjs --browser chrome` for the current table (gate: **≥58** avg presentation FPS, **p95 ≤20 ms** over **20 s** on three scenes).

## Commands

```bash
DETECTIVE_BUILD_WEB=$PWD/work/build-web-z1-fps tools/build-web.sh
node tools/web-fps.mjs --browser chrome
node tools/web-play-test.mjs --browser chrome
```

## Verified 2026-10-24 (merged build, headless Chrome on an RTX 3060)
`node tools/web-fps.mjs --browser chrome`: presentation 60.0 fps and rAF 59.9–60.0 fps with p95 frame time 16.75 ms on title, hub and hub-animated; compose 0.3–0.4 ms, upload 0.22 ms per frame. The `guestFlipFps` column is not a real game frame rate (it counts DirectDraw counter bumps, thousands per second); ignore it.
Bug found while merging: the offscreen canvas was never resized to the frame size, so only the top-left 300x150 pixels were shown (45,000 non-black pixels, 143 colours). Fixed in `platform_web.cpp` `present_rgba8`. `web_render_tick` now composes every animation frame because the game also draws through Lock/Unlock and GDI without bumping the frame counter. `WaitForVerticalBlank` is a no-op again.
Chrome, Firefox and WebKit all pass `tools/web-play-test.mjs --skip-saves` with 241-255 colours on 4 distinct screens. `tools/web-audio-test.mjs` now requires a real time-domain peak (0.26 measured in Chromium, ~20 s after Play), not just ring-buffer movement.
Not verified: a real GPU-less machine, 60 fps in Firefox/WebKit (only functional passes), `.MOV` cinematics playback, and the in-game save-slot screens (click map missing).

## Other browsers and software rendering (measured 2026-10-24, same machine, 20 s per scene)
| Browser | title / hub / hub-animated present fps | p95 frame ms |
|---|---|---|
| Firefox (headless) | 60.0 / 60.0 / 60.0 | 17.14 |
| WebKit (headless) | 61.9 / 62.2 / 62.3 (display runs ~62 Hz) | 16.14 |
| Chrome, GPU disabled (`DETECT_SOFTWARE=1 node tools/web-fps.mjs --browser chrome --angle swiftshader`) | 60.0 / 60.0 / 60.0 | 16.74 |
The machine has 20 CPU threads; a slow single-core device was not measured. The 2D canvas presenter costs ~0.3-0.5 ms compose + 0.2-0.7 ms upload per frame.

## Final verification (2026-10-25, headless Chrome/Firefox/WebKit, RTX 3060, 20 threads)
- `node tools/web-fps.mjs --browser chrome`: 60.0 fps presentation, p95 16.75-16.77 ms on title, hub, hub-animated, the opening cutscene (a real `.MOV` video) and the carnival in-game scene. Firefox and WebKit measured 60 / ~62 fps on the first three scenes earlier. A slow single-core device was not tested.
- The game's own logic is not frame-locked to 60 Hz: `guestFlipFps` is a DirectDraw counter, not a frame rate.

## Final-build re-measure (2026-10-25)
| Browser / condition | title, hub, hub-animated, cutscene, in-game present fps | p95 ms |
|---|---|---|
| Firefox headless | 60.0 on all 5 scenes | 17.14-17.16 |
| WebKit headless | 62.2-62.3 on all 5 scenes (display ~62 Hz) | 16.14 |
| Chrome, CPU throttled x4 (CDP `DETECT_CPU_THROTTLE=4`) | 60.0 on all 5 | 17.13-17.19 |
| Chrome, CPU throttled x6 (`DETECT_BOOT_TIMEOUT_MS=500000 DETECT_CPU_THROTTLE=6`) | 60.0 on all 5 | 17.40-17.49 |
At x6 the first in-browser translation took much longer (the default 180 s boot wait was exceeded); later starts use the OPFS cache. CPU throttling is an approximation of a slow device, not a real one.
Cutscene audio (native mixer dump `DETECTIVE_AUDIO_DUMP`, entry script, 93 s): silence during load, speech-like sections from ~12 s, then loud music/ambience; clipping above 0.95 full scale in 0.05% of samples. Not listened to by ear.
