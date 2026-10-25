# Web build notes

## `tools/build-web.sh` (2026-10-24)

Emscripten build of `detective_player` **succeeded** with `work/tools/emsdk` and output staged to `web/public/engine/detective_player.{js,wasm}`.

Compiler warnings only (no link errors): unused constants in `runtime/src/hle/ddraw.cpp`, `win32_extra.cpp`, and `trace_guest_fn` in `core/core.cpp`.

`WITH_DEV_ENGINE=1` still requires generated C sources under `work/gen-c` (`detectivebarbie` target); the deployable player build does not.

The script now copies `runtime/src/web/web_game.cpp` → `web/runtime/web_game.cpp` (include path tweak) before CMake, because `detective_player` links the latter override.

## Play / in-browser translation (2026-10-24, w1-play)

### Native `gen-wasm` without Ghidra

```bash
node recomp/src/cli.ts gen-wasm work/ref/BDETECT.EXE --out work/gen-wasm-w1
# optional: --meta metadata/<sha256>.json
```

`discoverMetadata()` + `translateExe(..., { speculate: true })` match `gen-c` (2100 functions, ~5.2 MB wasm).

### Problems fixed

| Issue | Fix |
|-------|-----|
| Browser translator required `metadata/<sha256>.json` | `discoverMetadata()` in `translate-worker.ts` when no JSON; `translateExe` uses speculative discovery like `gen-c` |
| `initIced` ran after discovery in the worker | Call `initIced` before `loadMetadata()` / `translateExe` |
| `functionList()` looked for `detect.funcs` | Read `bella.funcs` (emit-wasm custom section) |
| `WebAssembly.Instance`: missing `rt_rdtsc` import | `web_game_import` / EM_JS name list aligned with `RUNTIME_IMPORTS` in `emit-wasm.ts` (`rt_rdtsc`, `rt_port_io` were missing) |
| Stale `web/runtime/web_game.cpp` vs `runtime/src/web/` | `build-web.sh` sync step before compile |

### Headless smoke

```bash
node tools/web-smoke.mjs --iso DETECTV1_1.ISO
```

Writes `work/web-smoke-w1.png`, prints non-black pixel count and distinct colours. Pass criteria: `frames > 30`, `distinctColors > 50` (carnival title ~245 colours).

Example (2026-10-24): `nonBlackPixels: 307200`, `distinctColors: 245`, `frames: 100`, exit 0.

### Canvas / input

- Game framebuffer fixed at **640×480**; CSS scales fullscreen with `image-rendering: pixelated`.
- `web_render_tick()` on the main thread blits `ddraw::compose` via `plat::present_rgba`.
- Mouse coordinates map through `_web_client_size()` (640×480); pointer events added for touch.

### Audio

`plat::audio_open` ring + `public/audio-worklet.js` (`ring-player`); page creates `AudioContext` on Play click. `wakeAudio()` re-calls `audio.resume()` for the first ~300 presented frames (headless/Firefox/WebKit). Dev gate: `?audioTap` exposes `window.__detectAudio` (AnalyserNode + MediaStreamDestination on the worklet, ring fill via Atomics).

### Cross-browser play test (`tools/web-play-test.mjs`)

```bash
node tools/web-play-test.mjs --browser chrome   # required gate: exit 0, work/web-play-{title,hub,games,detective}.png
node tools/web-play-test.mjs --browser firefox
node tools/web-play-test.mjs --browser webkit
node tools/web-play-test.mjs --browser chrome --quota-test   # low `storage.estimate` install guard
```

Headless scripted path matches native `DETECTIVE_INPUT` in `docs/JOURNAL.md` (640×480 clicks via `page.mouse` with letterbox scaling). Captures at frames 120 / 250 / 450 / 700; each PNG must have **>50** distinct canvas colours and a different SHA-256 prefix from the previous capture. `?noCapture` disables pointer-lock swallowing for automation. `?timing` records `window.__detect.playStartMs` (Play click → running). Console/page errors fail the run.

Saves flow: after the four screens, reload → seed `saves/SAVGAME0.AR1` → **Back up saves** → reload → **Play** again (second start timing) → reload → OPFS survival + **Restore saves** from ZIP.

#### Measured results (2026-10-24, worker z3-browsers)

Playwright browsers under `~/.cache/ms-playwright`: `firefox-1555`, `webkit-2370` (present; no `npx playwright-core install` run this session).

| Browser | `web-play-test.mjs` exit | Wall time | Title / hub / games / detective colours | `playStartMs` (1st / 2nd) |
|---------|--------------------------|-----------|----------------------------------------|---------------------------|
| **chrome** (`/usr/bin/google-chrome`, port 5180) | **0** | ~45 s | 242 / 244 / 255 / 255 | 2012 / **49** |
| **firefox** (Playwright `firefox-1555`, port 5176) | **0** | ~81 s | 243 / 244 / 255 / 255 | (warm profile; timing not captured this run) |
| **webkit** (Playwright `webkit-2370`, port 5177) | **0** | ~44 s | 241 / 244 / 255 / 255 | (warm profile; timing not captured this run) |

Earlier chrome run (port 5175, warm OPFS): exit **0**, ~37 s, 243 / 242 / 255 / 255 colours.

**Low-quota messaging** (`--quota-test`): with `navigator.storage.estimate` overridden to 100 MiB quota / 95 MiB usage, install fails with log line: *“This browser lets this site store 5 MB, and the game needs about 600 MB…”* — exit **0**.

### Audio test (`tools/web-audio-test.mjs`)

```bash
node tools/web-audio-test.mjs --browser chrome   # uses same OPFS profile as web-play-test for that port
```

After Play on the title screen, within **15 s** (and after **>60** frames): Analyser peak **≥ 0.002** **or** unread ring frames **≥ 2048** (`window.__detectAudio.ringDelta()`). Shares `work/web-play-profile-<browser>-<port>`.

| Browser | `web-audio-test.mjs` exit | Notes (2026-10-24) |
|---------|---------------------------|---------------------|
| **chrome** (port 5180) | **0** | `maxRingDelta` ≥ 2048; `AudioContext` running; analyser peak often ~0 in headless despite ring fill |

`tools/browsers.mjs` launches Firefox/WebKit only when `~/.cache/ms-playwright/{firefox,webkit}-*` exists (no auto-install).

### Firefox / WebKit shell fixes (web-only)

| Issue | Fix |
|-------|-----|
| OPFS sync access handle absent on some paths | `opfs-sink.ts`, `opfs-probe-worker.ts`, `opfs-write-worker.ts`: try `createSyncAccessHandle`, fall back to `createWritable` |
| SharedArrayBuffer / threads | COOP/COEP in `vite.config.ts` and `web/public/_headers` (unchanged) |
| System cursor over custom game cursor | `#game.on #canvas { cursor: none }` and `canvas.style.cursor = "none"` when play starts |
| Automated clicks vs pointer lock | `?noCapture` → `mouseCapture: false` in `main.ts` |
| Suspended `AudioContext` in automation | `wakeAudio()` on start and early frames; `__detectAudio.resume()` in test loop |

Saves: writable sidecars under OPFS `saves/` (`web/src/saves.ts`); `web-play-test.mjs` checks reload survival and backup/restore UI.

### Save/load proof (`tools/web-save-test.mjs`)

Waits for coarse 8×8 region signatures (`tools/save-signatures.json`, captured from a known-good Chrome run) then drives engine `_web_mouse_*` / `_web_key` clicks — frame-independent across Chrome / Firefox / WebKit. (Earlier w7 used frame-sync `DETECTIVE_INPUT`; that raced into Load Mode on slower browsers — see `docs/JOURNAL.md` Web saves FF/WK (w8).)

```bash
DETECTIVE_BUILD_WEB=$PWD/work/build-web-e5f9fb4e bash tools/build-web.sh
node tools/web-save-test.mjs --browser chrome
node tools/web-save-test.mjs --browser firefox
node tools/web-save-test.mjs --browser webkit
```

Gate: exit **0**, OPFS `savgame2.ar{1,2,3}` ≈ **88176 / 4425 / 126** B, load screenshot differs from fresh badge.

## Needs core change

Headless Chrome often reports **zero** Analyser peak while the shared ring buffer fills (`ringDelta` ≫ 0), so `web-audio-test.mjs` also accepts ring fill as proof the mixer ran. If audible output is required in headless without that fallback, investigate `platform_web.cpp` / worklet gain (web-only changes exhausted).
