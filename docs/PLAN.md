# Detective Barbie (PC, 1998) — web port plan

Play your own copy of *Detective Barbie* (PC, 1998, Mattel Media) in a desktop browser. The
repository ships **no game files and no game code**: each player supplies their own disc image
(`DETECTV1_1.ISO`), the browser extracts and streams assets into private storage, translates the
game's x86 executable (`BDETECT.EXE`) to WebAssembly on that machine, and executes the translated
code against a re-implementation of the Win32, DirectDraw, and DirectSound interfaces the game
uses.

The framework (`installer/`, `recomp/`, `runtime/`, `web/`, `tools/`) is forked from the author's
MIT-licensed web ports (`shrek2-team-action-web`). Everything below outlines the architecture,
target facts, and milestone summary for Detective Barbie. `docs/JOURNAL.md` records facts and
gotchas as they are established.

## 0. Target facts

| Item | Value |
|---|---|
| Disc | `DETECTV1_1.ISO`, volume `DETECTV1_1`, ISO 9660, 560 MB, 1,658 files |
| Game exe | `BDETECT.EXE` on disc root: PE32 GUI, 501 KB, base 0x400000, entry RVA 0x5f530, `.text` 0x6ae46 — unpacked, no DRM |
| Imports | 8 DLLs: `kernel32`, `user32`, `gdi32`, `comdlg32`, `advapi32`, `ddraw` (`DirectDrawCreate`), `dsound` (`DirectSoundCreate`), `winmm` (`timeGetTime`) |
| Graphics | 2D, 8-bit palette (640x480, 256 colors), DirectDraw (surfaces, palettes, blits, flips) + GDI DIBs |
| Audio | DirectSound (PCM WAV looping, position polling) + winmm (`timeGetTime`), 887 WAV files in `AUDIO/` + `AUDIO/DIALOGUE/` |
| Installer | InstallShield 3 (`SETUP.EXE`, `DATA1.CAB` 1.2 MB, `_SYS1.CAB`, `_USER1.CAB`, PKWARE DCL explode) |
| Storage / VFS | Read-only VFS serving `DATA/` and `AUDIO/` directly from ISO via range reads; writable saves in OPFS (`SAVGAME*`, `CURRENT.AR*`, `GAMEHIST.ORY`) |

## 1. Architecture

1. `installer/` — ISO 9660 reader + InstallShield 3 CAB extractor + ISO range-reading VFS provider into the Origin Private File System (OPFS).
2. `recomp/` — iced-x86 lifter translating `BDETECT.EXE` to WebAssembly with address-only hints in `metadata/<sha256>.json`.
3. `runtime/` — C++/Emscripten HLE for Win32, DirectDraw (8-bit paletted surfaces to canvas), and DirectSound audio mixer.
4. `web/` — Web application handling disc selection, OPFS storage, in-browser translation, canvas rendering, audio, and save management.
5. `tools/` — Build scripts, conformance tests, and release asset scans.

## 2. Milestones (status 2026-10-24)

### M0 — Bootstrap — **done**
- Framework initialized; D3D/GLES/FFmpeg/FMOD subsystems removed from this port.
- Reference `work/ref/BDETECT.EXE` analyzed (`docs/RECON.md`).
- Documentation, plan, and clean repo boundaries in place.

### M1 — Installer & VFS — **done**
- InstallShield 3 CAB reader (`installer/src/isc3.ts`) with PKWARE DCL explode.
- ISO 9660 range VFS; vitest installer/isc3/isovfs suites pass.
- CLI installs to `work/install` for native and smoke tests.

### M2 — Recompiler — **done**
- Full `gen-c` / `gen-wasm` for `BDETECT.EXE` (~2100 functions, zero lifter traps in reachable code).
- `metadata/b56d1981-draft.json` and conformance harness (`tests/conformance/`).

### M3 — Native runtime boot — **done**
- Win32 HLE for all 135 imports (`import_coverage`, `win32_extra_check.sh`).
- Native `detective` reaches title carnival; frame dumps via `DETECTIVE_DUMP_FRAME`.

### M4 — DirectDraw — **done (title / menus)**
- 8-bit paletted surfaces, flip, blit, palette, compose to RGBA.
- Native scripted menus to Team Barbie Detective screen; `ddraw_test` passes.
- Full in-game scenes: ongoing playtesting.

### M5 — DirectSound — **done (HLE + unit)**
- DirectSoundCreate, secondary buffers, mixer path; `dsound_test` passes.
- In-browser music/dialogue: smoke-tested on title; full case playthrough ongoing.

### M6 — Input & Saves — **partial**
- Mouse/keyboard via `DETECTIVE_INPUT` / web pointer events; cursor APIs implemented.
- OPFS saves, ZIP export/import UI on the page; full case save/reload cycle not yet signed off.

### M7 — Web build — **done (Play path)**
- Player engine (`detective_player`), in-browser translation worker, canvas + audio worklet.
- Headless `tools/web-smoke.mjs` reaches carnival title (~245 colours).

### M8 — Release — **in progress (this milestone)**
- `tools/build-release.sh`, `tools/deploy.sh`, `tools/scan-dist.mjs`, `tools/check-all.sh`.
- Cross-browser matrix and private-browsing UX: documented; broader QA ongoing.
- Production deploy via Cloudflare Pages (`tools/deploy.sh`).
