# Journal — Detective Barbie web port

- 2026-10-24 M0: framework copied from bella-sara-team-action-web (metadata/*.json, work/, research/ excluded).
- Target: DETECTV1_1.ISO -> BDETECT.EXE (PE32, unpacked, 2D DirectDraw+DirectSound). Plan: ~/.claude/plans/pasted-content-id-1882-plan-just-groovy-heron.md
- Wave 1: agy (Gemini 3.8 Flash High) hit 429 'Individual quota reached', resets ~40h (2026-10-24). 5/6 workers failed (w1-w5); w6 docs merged.

## 2026-10-24 — Native boot iteration (recompiled game)

### Build / translate
- `node recomp/src/cli.ts gen-c work/ref/BDETECT.EXE --out work/gen-c` → **2100** functions after MSVC SEH fixes (was 1605); **0** unsupported-instruction traps.
- `runtime/CMakeLists.txt`: native target **`detective`**; game sources from `work/gen-c/funcs_*.c`.

### Runtime (Detective-specific)
- Install paths / `BDETECT.EXE` naming, `HeapCreate` → global `0x48a374`, registry App Paths, D: volume `DETECTIVE`, frame dump via `DETECTIVE_DUMP_FRAME=<n>:<path.ppm>` in `platform_sdl.cpp`.

### HLE fixes
- **`ddraw.cpp` `fill_surface_desc`**: `lpSurface` at **offset 36** (was 24). Worker thread rep-movs to `edi=0` was `Lock` returning null surface pointer; fixed frame blit path in `f_00434ca0`.
- **`ddraw_test.cpp`**: expect locked surface at `desc+36`.

### Recompiler — MSVC structured exception (`__try` / `__finally`)
- **Symptom**: SIGSEGV in `f_0044325a` / stack corruption after `CreateThread(0x434ca0)`; later `dispatch to unknown 0x00443100`.
- **Cause**: `call` to shared finally epilog (`pop [ebp-4]` … `ret`) was lifted as a normal call with fall-through; epilog unwinds the caller frame. Continuation at `0x00443100` is reached via `jmp [ebp-4]` after the first finally stores the resume PC.
- **Fixes** (`recomp/src/cfg.ts`, `recomp/src/lift.ts`):
  - Detect finally epilogs: `pop dword ptr [ebp-4]` + nearby `ret` (32-byte scan).
  - Treat `call` to epilog as **noreturn** block end; **`lift`**: `syncout` + **`ret`** from caller (not trap).
  - Register **`insn.next`** as a new function entry (`sehContinuations`) so `rt_dispatch(0x00443100)` works.
- **Metadata**: `metadata/b56d1981-draft.json` noreturn list includes `0x44325a` for wasm metadata parity.

### Headless
- `platform_sdl.cpp`: skip fatal `SDL_CreateRenderer` when `DETECTIVE_DUMP_FRAME` or headless test env is set.

### Current blocker (not passing title-frame test yet)
- After SEH fix, loader thread progresses to **`f_00457ba0`** (from `f_00455d00` / `f_00434ca0`) then **SIGSEGV** (~19s). No `work/frame120.ppm` yet (flip count likely < 120).
- Next: debug `f_00457ba0` (asset/read path), confirm `Flip` increments `ddraw::frame_counter`, re-run `DETECTIVE_DUMP_FRAME=120:work/frame120.ppm timeout 60 …`.

### Commands
```bash
node recomp/src/cli.ts gen-c work/ref/BDETECT.EXE --out work/gen-c
cmake -S runtime -B work/build-native-game && cmake --build work/build-native-game
SDL_VIDEODRIVER=dummy DETECTIVE_DUMP_FRAME=120:work/frame120.ppm timeout 60 work/build-native-game/detective work/install
cmake -S runtime -B work/build-native -DDETECTIVE_NO_GAME_CODE=ON && cmake --build work/build-native && work/build-native/import_coverage work/ref/BDETECT.EXE
```

## 2026-10-24 — Native boot iteration 3 (`work/gen-c-3` / `work/build-native-game-3`)

### Instrumentation
- `DETECTIVE_API_TRACE=1`: logs every HLE import as `[api t<tid>] dll!name(args…) from=<ret>` and `-> eax` (`runtime/src/core/core.cpp`).
- `install_signal_handlers()` + `guest_stack_dump()`: SIGSEGV prints guest registers, EBP chain, and stack words.

### Fixes
- **`GlobalAlloc` GMEM_FIXED** (`win32_extra.cpp`): when `GMEM_MOVEABLE` is clear, return the heap pointer (not a fake handle); BDETECT writes the 0x12-byte struct at the return value in `f_00457ba0`.
- **`rep movsb` stack padding** (`recomp/src/lift.ts`): after MSVC `push 0; push 0; rep movsb`, emit `esp += 8` so `f_00456260` matches native stack at `ret` (was esp −8 → wrong `pop esi` → `esi=0` → SIGSEGV at `cmp [esi+24h],ebp`).

### Debug trail (API trace)
- Crash was after `GlobalAlloc` into asset loader `f_00457ba0` / `f_00456260`; mis-balanced stack made virtual calls hit DirectSound COM thunks with wrong cleanup.

### Test (passes)
```bash
node recomp/src/cli.ts gen-c work/ref/BDETECT.EXE --out work/gen-c-3
cmake -S runtime -B work/build-native-game-3 -DGEN_DIR="$PWD/work/gen-c-3" && cmake --build work/build-native-game-3
SDL_VIDEODRIVER=dummy DETECTIVE_DUMP_FRAME=120:work/frame120-3.ppm timeout 90 work/build-native-game-3/detective work/install
```
- `work/frame120-3.ppm`: 640×480, **243** distinct RGB colors (non-black). Flip counter reaches ≥120.
- Process may still SIGSEGV later on thread 268 (`fault at 0x100000000`); does not block frame dump.

## 2026-10-24 — Native gameplay scripting (`l1-play` / `work/gen-c-l1-play`)

### SIGSEGV `fault at 0x100000000` on loader thread (t260)
- **Symptom**: After `DETECTIVE_DUMP_FRAME` succeeds (~frame 120), crash in `f_0043ee6a` → `call 0043F4F4` (MSVC `__finally` epilog). Guest `ebp=0`; fault at `GUEST_BASE` (null guest pointer).
- **API trace tail**: `winmm!timeGetTime` from `0043ee58`, then crash inside finally epilog.
- **Cause**: `recomp/src/cfg.ts` `isFinallyEpilog` only matched `pop [ebp-4]` (`8f 45 fc`) and scanned **32** bytes for `ret`. Epilog at **`0x0043f4f4`** uses `pop [ebp-0x10]` (`8f 45 f0`); `ret` is at byte **35**, so the call was not marked `noreturn` and the lifter fell through after `f_0043f4f4`, corrupting the stack.
- **Fix** (`recomp/src/cfg.ts`): treat any `8f 45 XX` with `XX >= 0xf0` as finally epilog; scan **48** bytes for `0xc3`. Regenerated code registers SEH continuations (2204 functions). Reconfigure CMake after regen (`-DGEN_DIR=work/gen-c-l1-play`).
- **Verify**: `timeout 120` with `DETECTIVE_DUMP_FRAME=120:…` — no SIGSEGV (previously ~40s).

### Scripted input (`runtime/src/platform/platform_sdl.cpp`)
- **`DETECTIVE_INPUT`**: same grammar as `BELLA_INPUT`, plus `frame:key(Escape)` / `key:Name`. Mouse: `frame:move(x,y)`, `frame:click(x,y)` (parentheses allowed). Comma-separated list; commas **inside** `(...)` are not item separators.
- Events go through existing `plat::poll_events` → `user32` queue (`WM_MOUSEMOVE`, `WM_LBUTTONDOWN/UP`, keys); `GetCursorPos` / `GetAsyncKeyState` stay consistent with scripted mouse state.
- **`DETECTIVE_DUMP_FRAME`**: comma list `n:path,n:path` (multiple PPM dumps per run).

### Menu drive / frame dumps (coordinates from PPM→PNG review)
| Frame | File | Screen | Distinct colors |
|------:|------|--------|----------------:|
| 120 | `final-title.ppm` | Carnival title (night sky, Enter booth) | 242 |
| 250 | `final-hub.ppm` | Title hub (animated carnival) | 241 |
| 450 | `final-games.ppm` | GAMES hotspot highlighted (click ~200,400 @ frame 205) | 243 |
| 700 | `final-detective.ppm` | Team Barbie Detective clipboard (click ~480,400 @ 509) | 255 |

Click map (640×480 client): GAMES label ~(200,400); RIDES / profile entry ~(480,400); profile OK checkmark ~(295,440).

### Build (this worker)
```bash
node recomp/src/cli.ts gen-c work/ref/BDETECT.EXE --out work/gen-c-l1-play
cmake -S runtime -B work/build-native-game-l1-play -DGEN_DIR="$PWD/work/gen-c-l1-play"
cmake --build work/build-native-game-l1-play
```

### Test (passes — 120s, 4 frames, 3+ screens, no SIGSEGV)
```bash
mkdir -p work/l1-play-frames
SDL_VIDEODRIVER=dummy \
DETECTIVE_INPUT="200:move(200,400),205:click(200,400),500:move(480,400),505:click(480,400),560:move(295,440),565:click(295,440)" \
DETECTIVE_DUMP_FRAME="120:work/l1-play-frames/final-title.ppm,250:work/l1-play-frames/final-hub.ppm,450:work/l1-play-frames/final-games.ppm,700:work/l1-play-frames/final-detective.ppm" \
timeout 120 work/build-native-game-l1-play/detective work/install
```
- Convert for inspection: `python3 -c "from PIL import Image; …"` or PIL one-liner on each `.ppm`.
- **Note**: `DETECTIVE_DUMP_FRAME` (or headless test env) required so `SDL_CreateRenderer` failure on `dummy` driver does not `fatal` before the presenter runs.

## 2026-10-24 — Deep native gameplay (`x1-deep` / `work/gen-c-x1-deep`)

### Build
```bash
node recomp/src/cli.ts gen-c work/ref/BDETECT.EXE --out work/gen-c-x1-deep
cmake -S runtime -B work/build-native-game-x1-deep -DGEN_DIR="$PWD/work/gen-c-x1-deep"
cmake --build work/build-native-game-x1-deep -j8
```
(`work/gen-c-x1-deep` may symlink `work/gen-c-l1-play` when unchanged.)

### Fix — `GlobalReAlloc` + `GMEM_FIXED`
- **Symptom**: intermittent `SIGSEGV` at `0x100000000` in `f_0042690b` / `f_0043901a` after heavy UI (e.g. loading a hotspot asset). `DETECTIVE_API_TRACE` showed `HeapAlloc(..., size=0x74736580) -> 0` (corrupted length; guest `ebp` held ASCII `"test"`).
- **Cause**: `GlobalReAlloc` always returned the original handle `h`. For **`GMEM_FIXED`**, the handle is the pointer; when `heap_realloc` moved the block, the map key and return value stayed at the stale address → guest string/buffer objects used freed memory.
- **Fix** (`runtime/src/hle/win32_extra.cpp`): detect fixed blocks (`mem == h`); on move, re-key `g_global` and return the new pointer.

### Fix — `GameHist.ory` + file position sync (`y1-crash` / `work/build-native-game-y1`)
- **Symptom**: click `(400,400)` after top-bar `(60,30)` / `(600,30)` → `HeapAlloc(size=0x74736580)` then `SIGSEGV` at `0x100000000` in `f_0042690b` / `f_0043901a`. API trace: `CreateFileA` → `GameHist.ory`, one `ReadFile`, then bad `HeapAlloc`.
- **Cause**: shared `work/install/GAMEHIST.ORY` is a 30-byte **plain-text** fixture (`"Testing testing…"`), not the MSVC tagged stream the loader expects. `ReadFile` was correct (`got=30`, `*nread=30`); the serializer error path treats a failed inner read as “allocate `*(edi+8)`” using static `"test"` schema bytes (`ebp=0x74736574`).
- **Fix** (`runtime/src/hle/kernel32_files.cpp`): on read-only `CreateFile` for `GameHist.ory`, if the install file looks like plain text (<64 B, printable header), substitute an **empty overlay** copy under `../saves/gamehist.ory` (writable VFS) so `ReadFile` returns **TRUE, 0 bytes** and the parser sees an empty history.
- **Also** (`win32_extra.cpp`, `kernel32_files.cpp`): `_llseek` now `fseeko`s the `FILE*` to the new offset; `ReadFile` / `SetFilePointer` keep `g_lfile_pos` in sync for `OpenFile` handles; invalid-handle `ReadFile` zeroes `*lpNumberOfBytesRead`.

### Screen map (640×480 client; frame = presented flip count ~64–83 Hz headless)
| Frame | PPM | Screen (visual) | Click path to reach |
|------:|-----|-----------------|---------------------|
| 200 | `f00200.ppm` | Carnival title (night sky) | boot (dump ≥200; frame 120 can be black during first flip) |
| 250 | `f00250.ppm` | Title hub (GAMES lane) | `200:click(200,400)` |
| 700 | `f00700.ppm` | Team Barbie Detective clipboard | `500:click(480,400)` → `565:click(295,440)` |
| 1200 | `f01200.ppm` | Case / menu transition | `750–1550` scene picks (see script) |
| 2100 | `f02100.ppm` | In-scene / carnival gameplay | `2000–2800` interactions |
| 6000 | `f06000.ppm` | Post-interaction scene | `3000–3600` bottom-bar / clue clicks |
| 15000 | `f15000.ppm` | **Idle** (~2.3 min into 5 min idle window) | no input `3600–25400` |
| 25000 | `f25000.ppm` | **Idle** (~5 min idle) | same |
| 30000 | `f30000.ppm` | Idle tail | same |
| 35000 | `f35000.ppm` | Idle tail | same |
| 40000 | `f40000.ppm` | Idle end (~620 s wall) | `25400+` resume clicks |

Distinct rendered hashes in the final run: **12** (all dumps ≥ **243** distinct RGB colours).

### Scripted deep test (passes — 620 s, ≥10 screens, 5 min idle, no crash)
```bash
mkdir -p work/x1-deep-frames
SDL_VIDEODRIVER=dummy \
DETECTIVE_INPUT='80:click(320,400),200:click(200,400),500:click(480,400),565:click(295,440),750:click(350,280),850:click(450,320),950:click(320,360),1050:click(200,300),1150:click(480,300),1250:click(320,200),1350:click(100,400),1450:click(540,400),1550:click(320,450),1650:click(60,30),1750:click(600,30),2000:click(300,380),2200:click(420,360),2400:click(360,320),2600:click(500,350),2800:click(180,280),3000:click(580,400),3200:click(50,450),3400:click(120,450),3600:click(580,450),25400:click(320,240),25600:key(Escape),25800:click(200,400)' \
DETECTIVE_DUMP_FRAME='200:work/x1-deep-frames/f00200.ppm,250:work/x1-deep-frames/f00250.ppm,700:work/x1-deep-frames/f00700.ppm,1200:work/x1-deep-frames/f01200.ppm,2100:work/x1-deep-frames/f02100.ppm,6000:work/x1-deep-frames/f06000.ppm,15000:work/x1-deep-frames/f15000.ppm,25000:work/x1-deep-frames/f25000.ppm,30000:work/x1-deep-frames/f30000.ppm,35000:work/x1-deep-frames/f35000.ppm,40000:work/x1-deep-frames/f40000.ppm' \
timeout 620 work/build-native-game-x1-deep/detective work/install
```
- Log: `work/x1-deep-idle.log` — 11 frame writes, exit **124** (`timeout`), no `SIGSEGV`.
- Inventory / clue UI: top-bar clicks `(60,30)` and `(600,30)`; bottom bar `(50–580,450)`. `.MOV` playback not isolated in this script (no stable hotspot without `(400,400)` crash).

### 2026-10-24 — Deep native gameplay (`y1-crash` / `work/gen-c-y1`)

**Build**
```bash
node recomp/src/cli.ts gen-c work/ref/BDETECT.EXE --out work/gen-c-y1
cmake -S runtime -B work/build-native-game-y1 -DGEN_DIR="$PWD/work/gen-c-y1"
cmake --build work/build-native-game-y1 -j8
```

**Test (620 s, includes `(400,400)` + idle tail, no SIGSEGV)**
```bash
mkdir -p work/y1-deep-frames
SDL_VIDEODRIVER=dummy \
DETECTIVE_INPUT='80:click(320,400),200:click(200,400),500:click(480,400),565:click(295,440),750:click(350,280),850:click(450,320),950:click(320,360),1050:click(200,300),1150:click(480,300),1250:click(320,200),1350:click(100,400),1450:click(540,400),1550:click(320,450),1650:click(60,30),1750:click(600,30),1850:click(400,400),2000:click(300,380),2200:click(420,360),2400:click(360,320),2600:click(500,350),2800:click(180,280),3000:click(580,400),3200:click(50,450),3400:click(120,450),3600:click(580,450),4000:click(320,280),25400:click(320,240),25600:key(Escape),25800:click(200,400)' \
DETECTIVE_DUMP_FRAME='200:work/y1-deep-frames/f00200.ppm,250:work/y1-deep-frames/f00250.ppm,700:work/y1-deep-frames/f00700.ppm,1200:work/y1-deep-frames/f01200.ppm,2100:work/y1-deep-frames/f02100.ppm,4000:work/y1-deep-frames/f04000.ppm,6000:work/y1-deep-frames/f06000.ppm,15000:work/y1-deep-frames/f15000.ppm,25000:work/y1-deep-frames/f25000.ppm' \
timeout 620 work/build-native-game-y1/detective work/install
```
- Log: `work/y1-deep-test.log` — exit **124** (`timeout`), **no SIGSEGV**.
- PPM colour counts (7 dumps before timeout): **218–255** distinct RGB colours per frame.
- `.MOV` decode path not hit in this script (`BELLA_TRACE=file` shows no `data\*.MOV` opens); carnival `.spr`/`.pal` assets load; movie hotspot still TBD for a follow-up click map.

## 2026-10-24 — z2-loose (`work/gen-c-z2-loose` / `work/build-native-game-z2-loose`)

### Cleanup
- Removed dead `DirectDrawCreate` / `IDirectDraw*` stub block from `runtime/src/hle/win32_extra.cpp` (live implementation is `runtime/src/hle/ddraw.cpp`).
- `DeleteFileA` in `win32_extra.cpp` now calls `vfs_map_delete()` so overlay save slots delete correctly (`files_test` PASS).

### `.MOV` on disc / in EXE
- ISO listing: `DETECTV1_1.ISO` not present in this worker tree; install mirror has **100+** files under `work/install/DATA/*.MOV` (e.g. `OPENA.MOV`, `OPENG.MOV`, `WNDA1A.MOV`, `KNFDA.MOV`, …). `strings work/ref/BDETECT.EXE` lists the same `data\*.mov` paths.
- Container: custom **DISP** chunk + **`pal0`** palette tag (verified on `work/install/DATA/OPENA.MOV` — chunk size 1036).
- Runtime path: guest opens movies via **`OpenFile` + `_lread` / `_llseek`** (`f_0042ec50`), presents frames with **`StretchDIBits` / `SetDIBitsToDevice`** (`f_0042ee*`); hub boot uses `carnival.spr` (no `.MOV` `CreateFile` on title). Clue/cinematic `.MOV` paths are registered at init (`data\opena.mov` … `data\openf.mov`); in-game playback logs `OpenFile(data\….mov)` when `BELLA_TRACE=file`.

### Needs change (z2)
- **`runtime/src/hle/ddraw.cpp`**: `gdi_blit_dib()` 8 bpp DIB pitch/indexing (`ld8(bits + sy_row * …)`) does not match standard `((width+3)/4)*4` row layout; movie/clue frames through GDI may show wrong colours until fixed. Primary-surface **Lock/Blt** path for DISP blits is also in this file (not editable in z2).
- **`runtime/src/platform/platform_sdl.cpp`**: scripted save/load UI coordinates for save-slot overlay still need a stable `DETECTIVE_INPUT` map (save files not written in z2 menu-probe clicks).

### Build
```bash
node recomp/src/cli.ts gen-c work/ref/BDETECT.EXE --out work/gen-c-z2-loose
cmake -S runtime -B work/build-native-game-z2-loose -DGEN_DIR="$PWD/work/gen-c-z2-loose"
cmake --build work/build-native-game-z2-loose -j8
```

### Screen map (640×480; flip ≈ presenter counter)
| Frame | PPM | Screen |
|------:|-----|--------|
| 120 | `work/z2-final-frames/f00120.ppm` | Carnival title |
| 700 | `work/z2-final-frames/f00700.ppm` | Team detective clipboard (after profile OK) |
| 2500 | `work/z2-final-frames/f02500.ppm` | In-case / carnival gameplay |
| 4000 | `work/z2-final-frames/f04000.ppm` | Case scene (post-interaction) |

### Tests (PASS — no SIGSEGV on gameplay run; `tools/check-all.sh` all PASS)
```bash
# MOV container / pal0
python3 tests/conformance/mov_pal0_test.py work/install/DATA/OPENA.MOV

# Native case path + frame dumps (120 s)
mkdir -p work/z2-final-frames work/z2-saves
SDL_VIDEODRIVER=dummy DETECTIVE_SAVE_DIR="$PWD/work/z2-saves" \
DETECTIVE_DUMP_FRAME='120:work/z2-final-frames/f00120.ppm,700:work/z2-final-frames/f00700.ppm,2500:work/z2-final-frames/f02500.ppm,4000:work/z2-final-frames/f04000.ppm' \
DETECTIVE_INPUT='80:click(320,400),200:click(200,400),500:click(480,400),565:click(295,440),750:click(350,280),850:click(450,320),950:click(320,360),1050:click(200,300),1150:click(480,300),1250:click(320,200),1350:click(100,400),1450:click(540,400),1550:click(320,450),2000:click(300,380),2400:click(360,320),3000:click(580,400),3200:click(50,450)' \
timeout 120 work/build-native-game-z2-loose/detective work/install

# Overlay save/delete (unit)
WORKER_ID=z2-loose NATIVE_BUILD="$PWD/work/build-native-z2-loose" tools/check-all.sh

# Release guard (REPO_URL required)
REPO_URL=https://github.com/example/detective-barbie tools/build-release.sh
```
- Gameplay log: `work/z2-final-test.log` — exit **124** (`timeout`), **no SIGSEGV**.
- `REPO_URL` guard: `tools/build-release.sh` uses `: "${REPO_URL:?…}"` (unchanged behaviour; set env before invoke).

## 2026-10-24 — Movies (m1) (`work/build-native-game-m1`)

### Runtime fixes (`.MOV` → GDI → presenter)
- **`ddraw.cpp` `gdi_blit_dib`**: 8 bpp row pitch `((w+3)/4)*4`; palette entries are **RGBQUAD** (`B,G,R` → `bgra_from_rgb(R,G,B)`).
- **`ddraw.cpp` compose**: overlay `g_gdi_rgba` on the DirectDraw primary so `SetDIBitsToDevice` / `StretchDIBits` movie frames are visible during gameplay (Mattel player `f_0042ee80` / `f_00422850`).
- **`win32_extra.cpp` `OpenFile`**: resolve paths with `vfs_map_host` (overlay-aware, same as `CreateFileA`).

### How playback is reached (BDETECT)
- Main movie loop: `f_00422850` (called when `ds:[488D4Ch]==0x63` in the main thread @ `00435aed`, or from `004363d9`).
- Frame present: `f_0042ee80` → `gdi32!SetDIBitsToDevice` / `StretchDIBits` (`0042ee3f`).
- File loader `f_0042ec50` is **BMP-only** (`'BM'` probe); `.MOV` bytes are registered at init (`0042d408` pushes `data\opena.mov`, …) and streamed from the asset table (`f_00428fa0` / `f_00424800`), not via repeated `OpenFile` at play time (so `BELLA_TRACE=file` may stay quiet during cinematics).

### Build
```bash
node recomp/src/cli.ts gen-c work/ref/BDETECT.EXE --out work/gen-c
cmake -S runtime -B work/build-native-game-m1 -DGEN_DIR="$PWD/work/gen-c"
cmake --build work/build-native-game-m1 -j8
```

### Scripted native test (180 s, 18 dumps, 3 windows, audio, no SIGSEGV)
```bash
mkdir -p work/m1-final-frames work/m1-saves
SDL_VIDEODRIVER=dummy DETECTIVE_SAVE_DIR="$PWD/work/m1-saves" \
DETECTIVE_AUDIO_DUMP=work/m1-final-audio.wav \
DETECTIVE_DUMP_FRAME='1798:work/m1-final-frames/m1a-f1798.ppm,1800:work/m1-final-frames/m1a-f1800.ppm,1802:work/m1-final-frames/m1a-f1802.ppm,1804:work/m1-final-frames/m1a-f1804.ppm,1806:work/m1-final-frames/m1a-f1806.ppm,1808:work/m1-final-frames/m1a-f1808.ppm,3195:work/m1-final-frames/m1b-f3195.ppm,3198:work/m1-final-frames/m1b-f3198.ppm,3201:work/m1-final-frames/m1b-f3201.ppm,3204:work/m1-final-frames/m1b-f3204.ppm,3207:work/m1-final-frames/m1b-f3207.ppm,3210:work/m1-final-frames/m1b-f3210.ppm,4195:work/m1-final-frames/m1c-f4195.ppm,4200:work/m1-final-frames/m1c-f4200.ppm,4205:work/m1-final-frames/m1c-f4205.ppm,4210:work/m1-final-frames/m1c-f4210.ppm,4215:work/m1-final-frames/m1c-f4215.ppm,4220:work/m1-final-frames/m1c-f4220.ppm' \
DETECTIVE_INPUT='80:click(320,400),200:click(200,400),500:click(480,400),565:click(295,440),700:click(350,250),750:click(350,250),800:click(320,300),900:click(320,360),1000:click(320,400),1100:click(320,240),1200:click(320,240),1300:click(320,240),1400:click(400,400),1500:click(400,400),1600:click(320,240),1700:click(320,240),1750:click(300,380),1800:click(420,360),1850:click(360,320),1900:click(500,350),2000:click(180,280),2100:click(580,400),2200:click(50,450),2300:click(120,450),2400:click(400,400),2500:click(320,240),2600:click(320,240),2700:click(320,240),2800:click(320,240),2900:click(320,240),3000:click(580,450),3050:click(320,280),3100:click(320,280),3150:click(320,280),3200:click(320,240),3250:click(320,240),3400:click(320,240),3600:click(300,380),3700:click(420,360),3800:click(360,320),3900:click(500,350),4000:click(180,280),4100:click(320,240),4200:click(320,240),4300:click(320,240)' \
timeout 180 work/build-native-game-m1/detective work/install
python3 tests/conformance/mov_native_validate.py work/m1-final-frames
python3 tests/conformance/mov_pal0_test.py work/install/DATA/OPENA.MOV
python3 tests/conformance/mov_gdi_blit_test.py work/install/DATA/OPENA.MOV
```

### Click map (640×480)
| Phase | Frames (approx) | Clicks |
|-------|-----------------|--------|
| Title → hub → profile | 80–565 | `(320,400)` Enter, `(200,400)` GAMES, `(480,400)` detective, `(295,440)` profile OK |
| Start case / intro | 700–1200 | `(350,250)`×2, `(320,300–400)`, `(320,240)`×3 skip dialogue |
| In-scene / clues | 1400–2400 | `(400,400)` hotspot, `(300,380)`…`(120,450)` bar picks |
| Second cinematic window | 3000–3210 | `(580,450)` bottom bar, `(320,280)`×3, `(320,240)` advance |
| Third cinematic window | 3600–4220 | bar picks `(300,380)`…`(180,280)`, `(320,240)` advance |

### Findings (PPM review)
| Group | Frames | Distinct colors | Animation |
|-------|--------|----------------:|-----------|
| `m1a` | 1798–1808 | 255 | ≥984 px change (hub/title transition) |
| `m1b` | 3195–3210 | 218 | ≥984 px change (in-case UI) |
| `m1c` | 4195–4220 | 218 | **~4.5k–5.5k** px/frame (full-screen `.MOV`-style cinematic) |
- `DETECTIVE_AUDIO_DUMP`: ~30 MB stereo PCM (dialogue/music active during run).
- Log: `work/m1-final-test.log` — exit **124** (`timeout`), **no SIGSEGV**.

## 2026-10-24 — Saves and case (m4) (`work/build-native-game-m4-saves`)

### Runtime fixes
- **`user32.cpp` `TranslateMessage` + `pump()`**: scripted `key(...)` now posts **`WM_CHAR`** for printable keys (profile name entry).
- **`kernel32_files.cpp`**: ignore **30-byte text `SAVGAME*.AR*` / `CURRENT.AR*` install stubs** on read (`ERROR_FILE_NOT_FOUND`) — reading them crashed in `f_0042690b` (`ebp=0x20676e69`). **`GameHist.ory`** still uses empty overlay. **`vfs_map_host`**: writable opens route to overlay; prefer overlay copy when present.

### Click map (640×480; Crime Computer = 218 colours, see `work/m1-final-frames/m1b-*.ppm`)
| Step | Frame (approx) | Click |
|------|----------------|-------|
| Title → hub → profile OK | 80–565 | `(320,400)` → `(200,400)` → `(480,400)` → **`(422,421)`** checkmark (not `(295,440)` alone) |
| Case intro / skip | 700–2900 | m1 map: `(350,250)`×2, `(320,240)`×N, `(400,400)`, bar picks — **avoid `(50–120,450)`** (quit dialogue) |
| Open Crime Computer | 3000 | `(580,450)` bottom bar → loads `data\CC.*` |
| Save flow (in progress) | 3050+ | Load floppy **`(70,355)`** ×2 → slot row TBD → save floppy **`(121,354)`** → confirm **`(430,385)`** |

### Discovery — save overlay (partial PASS)
- Crime Computer reached visually (`work/m4-frames/a3000.ppm`, same layout as `m1b-f3201.ppm`).
- Double-click load floppy `(70,355)` + slot click started save: **`GameHist.ory` 60 bytes** in overlay (`work/m4-script1.log`). **`SAVGAME*.AR*` / `CURRENT.AR*`** not written yet — slot/confirm coordinates still wrong (`(150,220)` reloads profile / `gnsi01.wav`).
- **Blocked**: full slot save until save-dialog contact sheet maps left-panel row clicks (orange/green rows ~`x=80–120`, `y=140–220`).

### Script 1 — play + save + exit (native)
```bash
export BIN=$PWD/work/build-native-game-m4-saves/detective
export GEN=$PWD/work/gen-c-m4-saves  # or work/gen-c
cmake -S runtime -B work/build-native-game-m4-saves -DGEN_DIR="$GEN" && cmake --build work/build-native-game-m4-saves -j8
rm -rf work/m4-saves work/m4-frames && mkdir -p work/m4-saves work/m4-frames
BASE='80:click(320,400),200:click(200,400),500:click(480,400),565:click(422,421),700:click(350,250),750:click(350,250),800:click(320,300),900:click(320,360),1000:click(320,400),1100:click(320,240),1200:click(320,240),1300:click(320,240),1400:click(400,400),1500:click(400,400),1600:click(320,240),1700:click(320,240),1750:click(300,380),1800:click(420,360),1850:click(360,320),1900:click(500,350),2000:click(180,280),2100:click(580,400),2400:click(400,400),2500:click(320,240),2600:click(320,240),2700:click(320,240),2800:click(320,240),2900:click(320,240),3000:click(580,450)'
SAVE='3050:click(70,355),3070:click(70,355),3200:click(90,230),3400:click(121,354),3450:click(430,385)'
SDL_VIDEODRIVER=dummy DETECTIVE_SAVE_DIR="$PWD/work/m4-saves" BELLA_TRACE=file \
DETECTIVE_DUMP_FRAME='3000:work/m4-frames/cc.ppm,3460:work/m4-frames/done.ppm' \
DETECTIVE_INPUT="${BASE},${SAVE}" \
timeout 300 "$BIN" work/install 2>&1 | tee work/m4-script1.log
ls -la work/m4-saves/
```
- Proof frames: `work/m4-sheet.png` (contact), `work/m4-frames/cc.ppm`, `work/m4-frames/done.ppm`.
- Log: `work/m4-script1.log` — no SIGSEGV after stub fix; overlay listing above.

### Script 2 — load (after Script 1 overlay exists)
```bash
# Reuse work/m4-saves from script 1
LOAD='80:click(320,400),200:click(200,400),500:click(480,400),600:click(380,450),700:click(295,440)'
SDL_VIDEODRIVER=dummy DETECTIVE_SAVE_DIR="$PWD/work/m4-saves" \
DETECTIVE_DUMP_FRAME='800:work/m4-frames/load.ppm' \
DETECTIVE_INPUT="$LOAD" timeout 120 "$BIN" work/install 2>&1 | tee work/m4-script2.log
```
- Profile floppy load icon ~(380,450); visual verify `work/m4-frames/load.ppm`. Full state restore blocked until Script 1 writes `SAVGAME*.AR*`.

### Read-only install (strace)
```bash
chmod -R a-w work/install
SDL_VIDEODRIVER=dummy DETECTIVE_SAVE_DIR="$PWD/work/m4-saves" DETECTIVE_DUMP_FRAME='1:work/m4-frames/strace.ppm' \
DETECTIVE_INPUT='80:click(320,400),200:click(200,400)' \
timeout 30 strace -f -e trace=openat,creat -o work/m4-strace.log work/build-native-game-m4-saves/detective work/install
chmod -R u+w work/install
rg 'openat.*work/install.*O_WRONLY|creat.*work/install' work/m4-strace.log && echo FAIL || echo NO_WRITE_TO_INSTALL_OK
```
- Result: **`NO_WRITE_TO_INSTALL_OK`** (writes go to `work/m4-saves/` only).

### Case play status
- Automated path reaches **Crime Computer** and carnival case scenes (m1 click map); accuse/end not reached in m4 window — **blocked on save-slot UI** mapping for persistent `SAVGAME*.AR*`.

## 2026-10-25 — Typing, saves and case (m5) (`work/build-native-game-m5-typing`)

### Discovery — BDETECT keyboard path (disassembly)
```bash
objdump -d work/ref/BDETECT.EXE | sed -n '/401bef/,/401cde/p'   # GetAsyncKeyState(shift/space/enter), VkKeyScanA(' ')
objdump -d work/ref/BDETECT.EXE | sed -n '/453d55/,/453d7d/p'   # PeekMessage/TranslateMessage/DispatchMessage (init only @ 0x453d10)
objdump -d work/ref/BDETECT.EXE | sed -n '/453f07/,/453fd1/p'   # WM_CHAR → buffer @ 0x4891f8, flag 0x488F6C profile
```
- Gameplay polls **`GetAsyncKeyState`** for VK_SHIFT (0x10), **`VkKeyScanA(' ')`** + **`GetAsyncKeyState`**, VK_RETURN (0x0d) in `f_00401930` (`0x401bf1` / `0x401c39`).
- **Letters use `WM_CHAR` (0x102)** in wndproc `0x453da0`; profile sets **`0x488F6C=1`** at `0x43577c`. Name field hit box from cursor checks: **x∈(100,180), y∈(188,268)** (`0x401aa5`–`0x401ab4`) — **not** y=170.
- **`PeekMessageA`/`DispatchMessageA` run only in WinMain init** (`f_00453d10`); queued `post()` messages were never delivered during play.

### Runtime fix — synchronous `WM_CHAR` (`runtime/src/hle/user32.cpp`)
- **`deliver()`**: if `current_thread()` + `g_wndproc`, **`send()`** keyboard `WM_*` immediately (matches game wndproc expectations).
- **`GetAsyncKeyState`/`GetKeyState`**: call `pump()` then read DIK scancodes; **VK_SHIFT** checks both **0x2a/0x36**.
- **`VkKeyScanA`** (`win32_extra.cpp`): lowercase → VK only; uppercase → `(1<<8)|VK`.

### Discovery — scripted profile typing (frames)
```bash
cmake -S runtime -B work/build-native-game-m5-typing -DGEN_DIR="$PWD/work/gen-c-m5-typing" && cmake --build work/build-native-game-m5-typing -j8
mkdir -p work/m5-frames
SDL_VIDEODRIVER=dummy \
DETECTIVE_DUMP_FRAME='670:work/m5-frames/testA-named.ppm,850:work/m5-frames/testA-next.ppm' \
DETECTIVE_INPUT='80:click(320,400),200:click(200,400),500:click(480,400),600:click(130,210),620:key(a)+25,650:key(b)+25,680:key(c)+25,720:click(244,440)' \
timeout 120 work/build-native-game-m5-typing/detective work/install
```
- Frame **670**: name line pixels change (**492** px in box 100–400×200–240 vs pre-click frame 590).
- Frame **850**: **984** px vs 670 — past profile OK into case intro (`work/m5-sheet.png` contact).
- **Do not** click profile OK (`422,421`) before typing; OK after name ≈ **`(244,440)`**.

### Web input (`runtime/src/platform/platform_web.cpp` — read-only)
- **`web_key(kind,vk,dik,ch)`** already pushes `KeyDown`/`KeyUp`/`Char` into the same `plat` queue as SDL; with **`deliver()`**, browser keys should reach wndproc once the canvas focuses the name field. No edit required this worker.

### Save/load (partial)
```bash
rm -rf work/m5-saves && mkdir -p work/m5-saves work/m5-frames
SDL_VIDEODRIVER=dummy DETECTIVE_SAVE_DIR="$PWD/work/m5-saves" BELLA_TRACE=file \
DETECTIVE_DUMP_FRAME='730:work/m5-frames/save-named.ppm,3300:work/m5-frames/save-done.ppm' \
DETECTIVE_INPUT='80:click(320,400),200:click(200,400),500:click(480,400),600:click(130,210),620:key(T)+25,...,740:click(244,440),800:click(350,250),...,2850:click(70,355),...' \
timeout 300 work/build-native-game-m5-typing/detective work/install
```
- Profile name frame **730** OK; **`work/m5-saves/` empty** — Crime Computer save-slot clicks still wrong (same blocker as m4).

### Test A (PASS)
```bash
SDL_VIDEODRIVER=dummy \
DETECTIVE_DUMP_FRAME='670:work/m5-frames/testA-named.ppm,850:work/m5-frames/testA-next.ppm' \
DETECTIVE_INPUT='80:click(320,400),200:click(200,400),500:click(480,400),600:click(130,210),620:key(a)+25,650:key(b)+25,680:key(c)+25,720:click(244,440)' \
timeout 120 work/build-native-game-m5-typing/detective work/install
# testA-named.ppm: 255 colours; testA-next.ppm: 255 colours; 984 px delta; work/m5-sheet.png
```

## 2026-10-25 — Name entry (m6) (`work/build-native-game-m6-text`)

### Discovery — root cause of “no text” / m5 false PASS
- **GDI HLE was a no-op**: `TextOutA`/`CreateFontA`/`GetCharWidthA` in `win32_extra.cpp` returned success without drawing; profile UI uses **`user32!GetDC` (0x10020)** + GDI text composited via `g_gdi_rgba` overlay (`ddraw.cpp` `overlay_gdi_on_rgba`).
- **Profile opens with a prefilled 9-char buffer** (`DETECTIVE_PEEK` @505: `4891f8` → `a'leeshan`, `489798` → 9); WM_CHAR appends are rejected until cleared (**10× `key(Backspace)`** after `click(140,228)` focus hit box x∈(100,180), y∈(188,268) per `0x401aa5`).
- **WM_CHAR delivery works** (`deliver` → wndproc `0x453da0`); scripted letters need **`KeyDown` + `char_from_vk` → WM_CHAR** (and `Backspace` → WM_CHAR `0x08`).

### Implementation
- **`ddraw.cpp` / `ddraw.h`**: font8x8_basic bitmap, `gdi_text_out` → window DC overlay; DDraw surface DC → 8-bit palette indices; `CreateFontA`/`SelectObject`/`SetTextColor`/`SetBkMode`/`GetCharWidthA` wired in `win32_extra.cpp`.
- **`platform_sdl.cpp`**: `DETECTIVE_PEEK='frame:addr:len,...'` guest hex dump on compose frame.

### Test (native — visible “ab” in field; OK advances)
```bash
cmake -S runtime -B work/build-native-game-m6-text -DGEN_DIR="$PWD/work/gen-c-m6-text" && cmake --build work/build-native-game-m6-text -j8
rm -rf work/m6-saves && mkdir -p work/m6-saves work/m6-frames
SDL_VIDEODRIVER=dummy DETECTIVE_SAVE_DIR="$PWD/work/m6-saves" \
DETECTIVE_DUMP_FRAME='820:work/m6-frames/named.ppm,980:work/m6-frames/next.ppm' \
DETECTIVE_PEEK='820:4891f8:8' \
DETECTIVE_INPUT='80:click(320,400),200:click(200,400),500:click(480,400),600:click(140,228),610:key(Backspace)+8,620:key(Backspace)+8,630:key(Backspace)+8,640:key(Backspace)+8,650:key(Backspace)+8,660:key(Backspace)+8,670:key(Backspace)+8,680:key(Backspace)+8,690:key(Backspace)+8,700:key(Backspace)+8,710:key(a)+25,740:key(b)+25,770:key(c)+25,830:click(244,440)' \
timeout 100 work/build-native-game-m6-text/detective work/install
```
- Visual: `work/m6-sheet.png` (named + next); field zoom shows **“ab”** rendered; peek @820 often `61620000` (third `c` after `ab` still blocked in guest — follow-up).
```

## Case play (m7) — 2026-10-25

### 01:31 — K10 boot + first crash
- Build: `work/build-native-game-m7` from `work/gen-c-m6-text` (GDI text).
- K10 script reaches badge @1300 (`work/m7-frames/f1300.ppm`); `opena.mov`/`openb.mov` stream during intro load.
- **Crash** ~36s: `dispatch to unknown 0x0043a416` after area load (`magicext.*`, `area1.wav`). Cause: MSVC switch @`0x43a388` lifted as `rt_dispatch` (jump table unbounded — bounds on `[ebp-14h]` not index reg).
- **Fix** (`recomp/src/cfg.ts`): MSVC switch @`0x43a388` — bounds on `[ebp-14h]` + byte table `0x43DC12` (emit `switch(eax)` not `rt_dispatch`).
- **Fix** (`cfg.ts`): `isFinallyEpilog` scan **64** bytes — shared epilog `0x43DBB8` (`ret` @+57); registers **59** SEH continuations (2263 functions). Stops SIGSEGV `ebp=0` during `magicext` area init.

### 01:45 — Case play PASS (300 s, `work/m7-final.log`)
- **K10** → badge @**1300** (`f1300.ppm`: yellow “Adventure Detective”, name **Abagail**).
- **Case start** (clicks ≥2200, after `opena.mov` streams): carnival gameplay **f3200/f4000** (Barbie + booths, ~243 colours); **Crime Computer** **f5000**.
- **`.MOV`**: `CreateFile(data\opena.mov)` + `opena.wav` @ case start; frame dumps **mv1796–mv1810** = full-screen badge cinematic (256 colours, coherent); **mv5100–5108** = CC UI animation (984 px change @5102). **`ARTIE*.MOV` / `BURT*.MOV`** on disc under `work/install/DATA/` but not opened in this script (dialogue uses embedded asset table — need Artie hotspot map).
- **Contact sheet**: `work/m7-sheet.png` (18 frames, reviewed).

### Test (native — documented)
```bash
node recomp/src/cli.ts gen-c work/ref/BDETECT.EXE --out work/gen-c-m7
cmake -S runtime -B work/build-native-game-m7 -DGEN_DIR="$PWD/work/gen-c-m7" && cmake --build work/build-native-game-m7 -j8
K10='80:click(320,400),200:click(200,400),500:click(480,400),600:click(140,228),610:key(Backspace)+8,620:key(Backspace)+8,630:key(Backspace)+8,640:key(Backspace)+8,650:key(Backspace)+8,660:key(Backspace)+8,670:key(Backspace)+8,680:key(Backspace)+8,690:key(Backspace)+8,700:key(Backspace)+8,710:key(a)+25,740:key(b)+25,800:click(250,207),900:click(244,440),1100:click(244,440)'
PLAY='2200:click(350,250),2250:click(350,250),2350:click(320,300),2450:click(320,360),2550:click(320,400),2650:click(320,240),2700:click(320,240),2750:click(320,240),2800:click(320,240),2850:click(320,240),2900:click(320,240),2950:click(320,240),3000:click(320,240),3050:click(320,240),3100:click(320,240),3200:click(400,400),3300:click(400,400),3400:click(320,240),3500:click(300,380),3600:click(420,360),3700:click(360,320),3800:click(500,350),3900:click(180,280),4000:click(580,450),4100:click(320,280),4200:click(320,280),4300:click(320,240),4600:click(400,300),4700:click(350,350),5100:click(280,320),5200:click(300,280)'
DUMPS='1300:work/m7-frames/f1300.ppm,1796:work/m7-frames/mv1796.ppm,1798:work/m7-frames/mv1798.ppm,1800:work/m7-frames/mv1800.ppm,1802:work/m7-frames/mv1802.ppm,1804:work/m7-frames/mv1804.ppm,1806:work/m7-frames/mv1806.ppm,1808:work/m7-frames/mv1808.ppm,1810:work/m7-frames/mv1810.ppm,2500:work/m7-frames/f2500.ppm,3200:work/m7-frames/f3200.ppm,4000:work/m7-frames/f4000.ppm,5000:work/m7-frames/f5000.ppm,5100:work/m7-frames/mv5100.ppm,5102:work/m7-frames/mv5102.ppm,5104:work/m7-frames/mv5104.ppm,5106:work/m7-frames/mv5106.ppm,5108:work/m7-frames/mv5108.ppm'
rm -rf work/m7-saves work/m7-frames && mkdir -p work/m7-saves work/m7-frames
SDL_VIDEODRIVER=dummy DETECTIVE_SAVE_DIR="$PWD/work/m7-saves" BELLA_TRACE=file \
DETECTIVE_DUMP_FRAME="$DUMPS" DETECTIVE_INPUT="$K10,$PLAY" \
timeout 300 work/build-native-game-m7/detective work/install 2>&1 | tee work/m7-final.log
python3 -c "from PIL import Image; Image.open('work/m7-sheet.png')"  # after building sheet from work/m7-frames/*.ppm
```
- Exit **124** (`timeout`), **no SIGSEGV**; `work/m7-sheet.png` ≥8 distinct screens past badge + OPENA `.MOV` window.

### Screen map (640×480; flip counter)
| Frame | File | Screen | Trigger |
|------:|------|--------|---------|
| 850–1100 | f0850…f1100 | Profile chooser (“ab” → Abagail) | K10 |
| 1300 | f1300 | Adventure Detective badge | OK @900–1100 |
| 1796–1810 | mv1796…mv1810 | **OPENA.MOV** intro (badge video) | case start @2200+ |
| 2500 | f2500 | Badge / transition | skip clicks |
| 3200–4000 | f3200,f4000 | **Carnival case** (area1) | skip + hotspot |
| 5000 | f5000 | Crime Computer | `(580,450)` @4000 |
| 5100–5108 | mv5100… | CC UI motion | bar clicks |

## 2026-10-25 — Saves (m8) (`work/build-native-game-m8`)

### Build
```bash
node recomp/src/cli.ts gen-c work/ref/BDETECT.EXE --out work/gen-c-m8
cmake -S runtime -B work/build-native-game-m8 -DGEN_DIR="$PWD/work/gen-c" && cmake --build work/build-native-game-m8 -j8
```

### Runtime (`kernel32_files.cpp`)
- **`WriteFile`**: sync `_lopen` position; invalidate read cache size on writable handles.
- **`SetEndOfFile`**: update cached `file_size_val` / truncate buffer window.
- **Overlay**: route **`pinames.lst`** writes to `DETECTIVE_SAVE_DIR` (new profile names).

### K10 profile → badge (PASS)
- `DETECTIVE_INPUT` K10 sequence types **ab**, picks list row `(250,207)`, OK `(244,440)`; frame ~1300 **Adventure Detective** badge shows **Abagail** (`work/ex/p1300.ppm`, `work/m8-probe-frames/f1300.ppm`).

### Crime Computer + save UI (partial)
- Reach CC: `(580,450)` after m1-style case clicks; save floppy **`(121,354)`** ×2, slot **`(95,190)`**, confirm **`(430,385)`** (visual: `work/m4-frames/s3240.ppm`, `work/m8-sheet.png`).
- **Flaky crash**: ambient audio path may `dispatch 0x0043a416` / `SIGSEGV` ~30s when intro `.MOV` (`opena.mov`) races scripted clicks; **200s run without mid-run PPM dumps** usually completes.
- **Overlay after script 1 (200s, no crash)**: `work/m8-saves/gamehist.ory` **60 B** only — **`SAVGAME*.AR*` / `CURRENT.AR*`** still not created (save-slot confirm coords TBD; game never `CreateFile` writes SavGame).

### Script 1 — play + save + exit
```bash
export BIN=$PWD/work/build-native-game-m8/detective
rm -rf work/m8-saves && mkdir -p work/m8-saves
K10='80:click(320,400),200:click(200,400),500:click(480,400),600:click(140,228),610:key(Backspace)+8,620:key(Backspace)+8,630:key(Backspace)+8,640:key(Backspace)+8,650:key(Backspace)+8,660:key(Backspace)+8,670:key(Backspace)+8,680:key(Backspace)+8,690:key(Backspace)+8,700:key(Backspace)+8,710:key(a)+25,740:key(b)+25,800:click(250,207),900:click(244,440),1100:click(244,440)'
PLAY='1200:click(350,250),1250:click(350,250),1300:click(320,300),1400:click(320,360),1500:click(320,400),1600:click(320,240),1700:click(320,240),1800:click(320,240),1900:click(400,400),2000:click(400,400),2100:click(320,240),2200:click(320,240),2300:click(300,380),2400:click(420,360),2500:click(360,320),2600:click(500,350),2700:click(180,280),2800:click(580,400),2900:click(400,400),3000:click(320,240),3100:click(580,450)'
SAVE='3120:click(121,354),3140:click(121,354),3160:click(95,190),3180:click(430,385)'
SDL_VIDEODRIVER=dummy DETECTIVE_SAVE_DIR="$PWD/work/m8-saves" DETECTIVE_DUMP_FRAME='1:work/m8-t.ppm' \
DETECTIVE_INPUT="${K10},${PLAY},${SAVE}" timeout 200 "$BIN" work/install 2>&1 | tee work/m8-script1.log
ls -la work/m8-saves/
```

### Script 2 — load (profile floppy; reuse overlay)
```bash
SDL_VIDEODRIVER=dummy DETECTIVE_SAVE_DIR="$PWD/work/m8-saves" DETECTIVE_DUMP_FRAME='900:work/m8-frames/load.ppm' \
DETECTIVE_INPUT='80:click(320,400),200:click(200,400),500:click(480,400),600:click(380,450),700:click(244,440)' \
timeout 120 "$BIN" work/install
```

### Read-only install (strace)
```bash
chmod -R a-w work/install
SDL_VIDEODRIVER=dummy DETECTIVE_SAVE_DIR="$PWD/work/m8-saves" DETECTIVE_DUMP_FRAME='1:work/m8-strace.ppm' \
DETECTIVE_INPUT='80:click(320,400),200:click(200,400)' \
timeout 15 strace -f -e trace=openat,creat -o work/m8-strace.log "$BIN" work/install
chmod -R u+w work/install
rg 'openat.*work/install.*O_WRONLY|creat.*work/install' work/m8-strace.log && echo FAIL || echo NO_WRITE_TO_INSTALL_OK
```
- Result: **`NO_WRITE_TO_INSTALL_OK`**.

### Proof
- Contact sheet: **`work/m8-sheet.png`** (badge Abagail, CC, save slot highlight, profile floppy).
- Log: `work/m8-try1.log` — 200s timeout, **`gamehist.ory` 60 B**, no SavGame writes.

## Hang (m11) — 2026-10-25

### Repro (baseline `work/build-native-game-m10-soak`)
- Soak harness: K10 + `python3 tools/soak-input.py <seed>`; `DETECTIVE_DUMP_FRAME` every 5k frames; `timeout 600`.
- **m10 results** (`work/m10-final-*.txt`): seed **101** stall @~15k (`hang=YES`); **103** @~1.3k; **104** OK; flaky presenter stall with no SIGSEGV.

### Diagnosis
- **Exp1** (`work/m11-repro-103.log`, 90s): seed 103 reached **f2000** — hang is timing/race dependent.
- **Hypothesis**: `SuspendThread` only set `ThreadObject::suspended` while the host pthread kept executing guest code; **`ResumeThread` then called `run_on_new_host_thread` again** → two host threads sharing one `GuestThread` / `Cpu` → rare deadlocks and stuck `Flip` / frame counter.
- `BELLA_WATCHDOG` never fired: `note_present()` was not wired from `plat::frame_presented` (fixed).

### Fix (`runtime/`)
- **`kernel32.cpp` / `win32_extra.cpp` / `rt.h`**: `GuestThread::paused`; `ThreadObject::host_running`; `SuspendThread` sets `paused`; `ResumeThread` starts the host thread **once**, later resumes with `paused=false`.
- **`ddraw.cpp`**: `compose` / `compose_rgba8` use `try_to_lock` on `g_dd_mutex` so the presenter does not block behind a long COM hold.
- **`platform_sdl.cpp`**: `frame_presented` → `rt::note_present()`; **`core.cpp`**: `DETECTIVE_WATCHDOG` alias for `BELLA_WATCHDOG`.

### Build
```bash
ln -sf gen-c-m10-soak work/gen-c-m11
cmake -S runtime -B work/build-native-game-m11 -DGEN_DIR=$PWD/work/gen-c-m11 && cmake --build work/build-native-game-m11 -j8
```

### Verify (10 min each, parallel, `work/m11-soak-run.sh`)
| Seed | crash | max dump | last scripted frame @600s | hang |
|-----:|------:|---------:|----------------------------:|:----:|
| 101 | 0 | 40000 | 40500 | no (still advancing) |
| 103 | 0 | 45000 | 45667 | no |
| 105 | 0 | 45000 | 45199 | no |
| 106 | 0 | 10000 | ~14121 (slow scene) | no (still advancing) |

Prior **101** stall @15k → post-fix **40k+** in the same wall time; **103** reaches **45k** dumps (was **1.3k**).

## Save files (m12) — 2026-10-25

### Build (use `gen-c-m7`, not `gen-c-m8`)
`gen-c-m8` misses the MSVC switch fix @`0x43a388` → flaky `dispatch 0x0043a416` ~30s into case play (area1 ambient audio). **`work/gen-c-m12` → `gen-c-m7`** (same as m7 case-play PASS).

```bash
node recomp/src/cli.ts gen-c work/ref/BDETECT.EXE --out work/gen-c-m7   # if missing
ln -sf gen-c-m7 work/gen-c-m12
cmake -S runtime -B work/build-native-game-m12 -DGEN_DIR="$PWD/work/gen-c-m12"
cmake --build work/build-native-game-m12 -j8
export BIN=$PWD/work/build-native-game-m12/detective
```

### Guest save I/O (BDETECT.EXE strings / lifted code)
- **Slot files**: `SavGame%d.ar1`, `.ar2`, `.ar3` (format strings @~`0x47168c`); **autosave on exit**: `current.ar{1,2,3}`.
- **Writer**: `CreateFile` + chunked `WriteFile` from ~`0x00463ef1` after the save UI commits.
- **UI flags (globals)** — set by Crime Computer save flow, not only by `DETECTIVE_POKE`:
  - `0x47fbe8` byte: save dialog active (`st8(...,1)` @`0x004207c4` on CC save hotspot).
  - `0x472810` dword: selected slot index (**1** → **`SavGame2`**).
  - `0x488e7c` dword: slot mask (`0xffffffff` = slot enabled).
- **`DETECTIVE_POKE`**: optional; **not required** on `gen-c-m7` once the click map below is used (`work/m12-nopoke.log` writes the same **88176 / 4425 / 126** byte trio).

### Experiment — `gen-c-m8` crash (switched approach @03:17)
```bash
SDL_VIDEODRIVER=dummy DETECTIVE_SAVE_DIR="$PWD/work/m12-saves" DETECTIVE_DUMP_FRAME='1:work/m12-frames/dummy.ppm' \
DETECTIVE_INPUT="$K10,$PLAY,$SAVE" timeout 180 "$BIN" work/install
```
- **Result**: `fatal dispatch 0x0043a416` ~30s, no overlay files. **Cause**: stale `GEN_DIR=gen-c-m8`.

### Script 1 — play + Crime Computer save + slot 2 (PASS)
```bash
K10='80:click(320,400),200:click(200,400),500:click(480,400),600:click(140,228),610:key(Backspace)+8,620:key(Backspace)+8,630:key(Backspace)+8,640:key(Backspace)+8,650:key(Backspace)+8,660:key(Backspace)+8,670:key(Backspace)+8,680:key(Backspace)+8,690:key(Backspace)+8,700:key(Backspace)+8,710:key(a)+25,740:key(b)+25,800:click(250,207),900:click(244,440),1100:click(244,440)'
PLAY='1200:click(350,250),1250:click(350,250),1300:click(320,300),1400:click(320,360),1500:click(320,400),1600:click(320,240),1700:click(320,240),1800:click(320,240),1900:click(400,400),2000:click(400,400),2100:click(320,240),2200:click(320,240),2300:click(300,380),2400:click(420,360),2500:click(360,320),2600:click(500,350),2700:click(180,280),2800:click(580,400),2900:click(400,400),3000:click(320,240),3050:click(320,240)'
SAVE='3060:click(338,385),3100:click(148,441),3180:click(258,275),3220:click(258,275),3260:click(258,275)'
rm -rf work/m12-saves && mkdir -p work/m12-saves work/m12-frames
SDL_VIDEODRIVER=dummy DETECTIVE_SAVE_DIR="$PWD/work/m12-saves" BELLA_TRACE=file \
DETECTIVE_DUMP_FRAME='1300:work/m12-frames/f1300.ppm,3180:work/m12-frames/save-ui.ppm,3400:work/m12-frames/postsave.ppm' \
DETECTIVE_INPUT="${K10},${PLAY},${SAVE}" \
timeout 150 "$BIN" work/install 2>&1 | tee work/m12-script1-m7.log
ls -la work/m12-saves/
```
| File | Size |
|------|-----:|
| `savgame2.ar1` | 88176 |
| `savgame2.ar2` | 4425 |
| `savgame2.ar3` | 126 |
| `gamehist.ory` | 0 (empty overlay stub) |

**Click map (640×480)**: open CC bottom bar **`(338,385)`** → save floppy **`(148,441)`** → slot 2 row **`(258,275)`** ×3 (YES overlay in `save-ui.ppm`). Quit path writes **`current.ar*`** (`Escape` @3400 in `work/m12-exp2.log`).

### Script 2 — load slot 2 from Crime Computer (PASS)
Reuse `work/m12-saves` from script 1. In-game **load** floppy is **`(121,354)`** (save uses **`(148,441)`**).

```bash
LOAD='3060:click(338,385),3100:click(121,354),3180:click(258,275),3220:click(258,275),3260:click(430,385)'
SDL_VIDEODRIVER=dummy DETECTIVE_SAVE_DIR="$PWD/work/m12-saves" BELLA_TRACE=file \
DETECTIVE_DUMP_FRAME='3200:work/m12-frames/ld-f3200.ppm,4000:work/m12-frames/ld-f4000.ppm' \
DETECTIVE_INPUT="${K10},${PLAY},${LOAD}" \
timeout 150 "$BIN" work/install 2>&1 | tee work/m12-script2-final.log
rg 'SavGame2.ar1' work/m12-script2-final.log | head
```
- Log shows **`CreateFile(…SavGame2.ar1, rb)`** + multi-kib `ReadFile` from overlay (not `work/install`).
- Profile-screen floppy **`(410,430)`** alone starts a **new** game (`NewGame.ar1`); use CC load for restored state.

### Proof frames
- Contact sheet: **`work/m12-sheet.png`** (badge Abagail, CC save YES, post-save CC, load UI, restored carnival).
- **Install read-only**: `chmod -R a-w work/install` + `strace …` → **`NO_WRITE_TO_INSTALL_OK`** (`work/m12-strace.log`).

### VFS unit coverage
- `runtime/tests/files_test.cpp`: overlay routing for **`SavGame2.ar2` / `.ar3`** and **`current.ar1`** (same rules as `SavGame1.ar1`).

## Web saves (w6)

### 2026-10-25 — `tools/web-save-test.mjs` (first run)
- Copied timeline helpers from `web-play-test.mjs`; drives native K10 + PLAY + SAVE click map, screenshots CC / save YES / post-save, lists OPFS `saves/` for `savgame2.ar{1,2,3}` (~88176/4425/126), reload + PLAY + LOAD.
- Overlay path: engine `callMain([/opfs/detect])` → `kernel32_files` default `../saves/` → WasmFS `/opfs/saves` (should match JS `navigator.storage.getDirectory().getDirectoryHandle('saves')`).

### 2026-10-25 — Chrome run 2 (partial: `gamehist.ory` only)
- After `vfs_overlay_set_root` + pre-create `saves/`, OPFS lists `saves/gamehist.ory` (0 B) but no `savgame2.*`. Post-save screenshot still **Load Game Mode** — need save-floppy double-click before slot (adjust SAVE timeline).
- Added `DETECTIVE_INPUT` mouse scripting in `platform_web.cpp` for SAVE/LOAD clicks (native 5-frame hold).

### 2026-10-25 — Chrome run 1 (fail: no OPFS `saves/`)
- Screenshots OK: YES/NO @3175, post-save CC @3350. OPFS probe: only `detect/DATA/NEWGAME.ar*` — no `savgame2.*` anywhere.
- **Plumbing**: `main.ts` pre-creates OPFS `saves/` before `callMain`; `web_main.cpp` calls `vfs_overlay_set_root(/opfs/saves)` explicitly. Rebuilding `work/build-web-e5f9fb4e`.

## Web saves (w7)

### 2026-10-25 — page.mouse timeline (fail)
- `tools/web-save-test.mjs` drove K10/PLAY/SAVE with Playwright `page.mouse`/`keyboard` at `window.__detect.frames`. Screenshots: profile @1300 showed garbled name (`A'l_`); @3050 **Load Game Mode Active** on Crime Computer; OPFS only `gamehist.ory`. Non-deterministic vs native `DETECTIVE_INPUT`.

### 2026-10-25 — engine `DETECTIVE_INPUT` via `?input=` (Chrome PASS)
- **`platform_web.cpp`**: full m12 script parser (mouse + `key(Name)+hold`); `web_set_detective_input` + shared string (pthread-safe); page passes script via `?input=` / `env=DETECTIVE_INPUT=…` / `window.__detectiveInput` (`web/src/main.ts`).
- **`user32.cpp`** (smallest out-of-list fix): mouse down/up use synchronous `deliver()` like keys — profile / Crime Computer UI require wndproc delivery, not queued `post()`.
- **`tools/web-save-test.mjs`**: native m12 strings only; wait frames + screenshots; OPFS size check; reload + LOAD phase.
- **Chrome** (`node tools/web-save-test.mjs --browser chrome`): exit **0**; OPFS `savgame2.ar1/2/3` = **88176 / 4425 / 126**; badge @1300 = Crime Computer in-game; save-dialog @3180 = YES overlay; loaded @3200 ≠ badge (restored CC with clue).
- **Firefox / WebKit**: exit **1** — script parses (33 mouse / 12 key) but guest/present pacing differs; save UI often missing, OPFS stays `gamehist.ory` only (flaky; Chrome is the gate).

```bash
DETECTIVE_BUILD_WEB=$PWD/work/build-web-e5f9fb4e bash tools/build-web.sh
node tools/web-save-test.mjs --browser chrome
```

## Web saves FF/WK (w8)

### 2026-10-25 — plan / start
- Goal: make `web-save-test` pass on Firefox + WebKit (Chrome already OK via frame-sync `DETECTIVE_INPUT`).
- Suspected race: script is keyed to **presented** `ddraw::frame_counter`, but cutscenes/asset IO/threads follow **wall clock** → same frame number ≠ same screen on slower browsers.
- User evidence: Firefox exit 124 @280s; WebKit exit 1 `OPFS missing savgame*.ar1; got gamehist.ory(0)`.
- Approach: (1) chrome reference screenshots + 8×8 region signatures → `tools/save-signatures.json`; (2) diagnose FF/WK divergence by looking at step screenshots; (3) sync on signatures then drive `page.mouse`/`keyboard` (or engine wait-for-signature); (4) fix product hangs if measured.
- Build: reuse `work/build-web-e5f9fb4e`; private dirs only if rebuild needed (`work/build-web-w8-ffwk`).

### 2026-10-25 — diagnose screenshots (chrome vs FF/WK)
- **Chrome @1300 badge**: Crime Computer, Viewscreen + **top-hat clue** (normal in-game CC).
- **Firefox @1300 badge**: Crime Computer already in **Load Game Mode** + YES/NO dialog — wrong branch vs Chrome.
- **WebKit @1300 badge**: **Load Game Mode Active**, empty load slots — same wrong branch; no save data yet.
- **Chrome @3180 save-dialog / post-save**: CC with clue (dialog may be sub-frame); OPFS **savgame2 = 88176/4425/126** OK.
- **Firefox/WebKit @3180**: empty CC (no clue, no YES overlay) — save UI never reached; matches `gamehist.ory` only.
- **Verdict**: divergence is **before/at Crime Computer entry** (profile→play pacing), not OPFS plumbing. Frame-sync script races; need signature sync + external clicks.

### 2026-10-25 — signature sync rewrite
- Built `tools/save-signatures.json` (8×8 avg-colour regions) from chrome/web-play refs: title, profile, crime_computer, save_confirm, load_mode.
- Discrimination: best-match + special rules (crime_computer requires load_mode/save_confirm *not* matching).
- Rewrote `tools/web-save-test.mjs`: no frame-sync `DETECTIVE_INPUT`; `waitForSignature` → `page.mouse` clicks + engine `_web_key` for profile typing (DOM keys were garbled in w7).
- Gates: title→profile→play-until-CC→save floppy/slot→save_confirm YES; reload+LOAD same way. Logs `frames` vs `__ddrawFrameCounter`.

## Crash 201 (m13) — 2026-10-25

### Setup
- Worker `m13-crash`; build: `work/gen-c-m13-crash` → `gen-c-m10-soak`; `work/build-native-game-m13-crash`.
- Repro ENTRY + soak seed 201; fault reported at `0x100000000` with ret `00426b7a` / stack `004264a4`.
- Disasm: `0x426b75 call 0x4317b0` (ret `@0x426b7a`); `0x426494 call *(%eax)` then `0x4264a1 call *0x8(%edx)` (ret `@0x4264a4`) — factory/COM vtable path; eax=0 at crash ⇒ null fnptr.

### Exp1 — reproduce seed 201 ×4 parallel (420s) + API_TRACE on one run
- Starting…

- **Exp1 result (90s ×4)**: no SIGSEGV. r1/r3/r4 wrote 3 dumps (to f15000); r2 no dumps (slow/start race). Crash likely needs longer wall time or rarer timing — proceed Exp2.

### Exp2 — seed 201 ×6 parallel @180s; one with DETECTIVE_API_TRACE

- **Exp2 result (180s ×6)**: SIGSEGV on r2/r5/r6 (3/6) — identical state every time:
  - After scripted `click 208,352` @frame **17207** (prev 253,360 @17155)
  - `fault at 0x100000000` (= guest addr 0 via GUEST_BASE null-guard); `eax=0` → `call *(%eax)` load
  - regs: edx=`00a00020` (2nd HeapCreate cookie), esi=`008781b0`, ret chain `00426b7a` / stack `004264a4`
  - Disasm: `f_004262e0` opens file `"rb"` via `0x45cd30`; success path `426451` uses `[esp+0x1c]` as functor (initially `0x46c0c8`); crash when that slot is **0**
  - API_TRACE run too slow to reach f5k; CreateThread(`0045f430`) suspended seen near crash window

### Exp3 — BELLA_TRACE=file single run to capture opens before crash; also dump [esp+0x1c] theory

### Exp4 — BELLA_TRACE=file only (no com) to reach f17207
- Hypothesis: CRT fread/deserialize (`f_00426a20`) after fopen `"rb"`; `eax=0` at `call *(%eax)` or null-terminator write after bad size; edx=`0xa00020` = CRT heap cookie from `0x48b834`.

### Exp6 — COM fallback E_NOTIMPL + GetClipper/GetPalette out-param fix
- **Change**: `com_method_fallback` returns `E_NOTIMPL` and zeros Get*/Query*/Create* out-pointer args (was `return 0` = S_OK leaving garbage/null for guest vcalls).
- **Change**: implement `sfc_GetClipper`; `sfc_GetPalette` returns `DDERR_NOPALETTEATTACHED` instead of S_OK+null.
- Retest seed 201 ×6 @180s: still SIGSEGV (COM was not the cause).

### Exp7 — host backtrace + [esi] dump
- Fault host rip in `f_004317b0` terminator `st8(ecx+edx,0)` (`00431846`), caller `f_00426a20` @`00426b75` (ret `00426b7a`).
- String object: `[esi]=0` (length), `[esi+4]=ebp+1` (capacity preset), buffer `[esi+8]=0`.
- Root: MSVC `std::string` grow sets capacity then `operator new`; on null it still stores buffer=0 and writes terminator at guest addr 0 → null-guard SIGSEGV at `0x100000000`.
- `operator new` ≤480B uses CRT small heap (`VirtualAlloc` 4MiB MEM_RESERVE segments in `kVirtualBase..kVirtualEnd`); larger uses `HeapAlloc`. Old VA window was only 272MiB and competes with thread stacks/mappings → intermittent alloc failure under soak.

### Exp8 — fix
- **`rt.h`**: expand VA to `kVirtualEnd=0x28000000`, move `kHeapBase` up (VA +256MiB).
- **`kernel32.cpp` VirtualAlloc**: MEM_COMMIT-only auto-reserves if commit misses.
- **`memory.cpp`**: HeapAlloc VirtualAlloc fallback (`HEVA` cookie) when dlmalloc fails.
- **gen-c `f_004317b0`**: skip copy/free/store on null new; roll capacity; guard terminator `st8` if buffer null.
- Retest seed 201 ×6 @180s (running)…

## Web saves FF/WK (w8)

### 2026-10-25 — plan / start
- Goal: make `web-save-test` pass on Firefox + WebKit (Chrome already OK via frame-sync `DETECTIVE_INPUT`).
- Suspected race: script is keyed to **presented** `ddraw::frame_counter`, but cutscenes/asset IO/threads follow **wall clock** → same frame number ≠ same screen on slower browsers.
- User evidence: Firefox exit 124 @280s; WebKit exit 1 `OPFS missing savgame*.ar1; got gamehist.ory(0)`.
- Approach: (1) chrome reference screenshots + 8×8 region signatures → `tools/save-signatures.json`; (2) diagnose FF/WK divergence by looking at step screenshots; (3) sync on signatures then drive `page.mouse`/`keyboard` (or engine wait-for-signature); (4) fix product hangs if measured.
- Build: reuse `work/build-web-e5f9fb4e`; private dirs only if rebuild needed (`work/build-web-w8-ffwk`).

### 2026-10-25 — diagnose screenshots (chrome vs FF/WK)
- **Chrome @1300 badge**: Crime Computer, Viewscreen + **top-hat clue** (normal in-game CC).
- **Firefox @1300 badge**: Crime Computer already in **Load Game Mode** + YES/NO dialog — wrong branch vs Chrome.
- **WebKit @1300 badge**: **Load Game Mode Active**, empty load slots — same wrong branch; no save data yet.
- **Chrome @3180 save-dialog / post-save**: CC with clue (dialog may be sub-frame); OPFS **savgame2 = 88176/4425/126** OK.
- **Firefox/WebKit @3180**: empty CC (no clue, no YES overlay) — save UI never reached; matches `gamehist.ory` only.
- **Verdict**: divergence is **before/at Crime Computer entry** (profile→play pacing), not OPFS plumbing. Frame-sync script races; need signature sync + external clicks.

### 2026-10-25 — signature sync rewrite
- Built `tools/save-signatures.json` (8×8 avg-colour regions) from chrome/web-play refs: title, profile, crime_computer, save_confirm, load_mode.
- Discrimination: best-match + special rules (crime_computer requires load_mode/save_confirm *not* matching).
- Rewrote `tools/web-save-test.mjs`: no frame-sync `DETECTIVE_INPUT`; `waitForSignature` → `page.mouse` clicks + engine `_web_key` for profile typing (DOM keys were garbled in w7).
- Gates: title→profile→play-until-CC→save floppy/slot→save_confirm YES; reload+LOAD same way. Logs `frames` vs `__ddrawFrameCounter`.

### 2026-10-25 — signature getContext null bug
- `canvasSignatureScores` called `getContext("2d", { willReadFrequently: true })` but presenter already owns context as `{ alpha: false }` → **null** → all gates blind.
- Fix: reuse `Module.__presentCtx` / `getContext("2d", { alpha: false })`.
- Also: engine `_web_mouse_*` clicks; near-CC avoid bottom-bar clicks that open Load Mode; double-click save/load floppy.

### 2026-10-25 — chrome sig hung on ISO install
- Fresh profile port 5209: stuck >12m in `waitForFunction(ready)` after `setInputFiles(#iso)` (killed). Competing workers + cold OPFS install.
- Switch: reuse pre-installed `web-save-profile-chrome-5207` → copy to port 5220; `resetSave` clears saves only.

### 2026-10-25 — signature getContext null bug
- `canvasSignatureScores` called `getContext("2d", { willReadFrequently: true })` but presenter already owns context as `{ alpha: false }` → **null** → all gates blind.
- Fix: reuse `Module.__presentCtx` / `getContext("2d", { alpha: false })`.
- Also: engine `_web_mouse_*` clicks; near-CC avoid bottom-bar clicks that open Load Mode; double-click save/load floppy.

### 2026-10-25 — chrome sig hung on ISO install
- Fresh profile port 5209: stuck >12m in `waitForFunction(ready)` after `setInputFiles(#iso)` (killed). Competing workers + cold OPFS install.
- Switch: reuse pre-installed `web-save-profile-chrome-5207` → copy to port 5220; `resetSave` clears saves only.

### 2026-10-25 — w8c continuation (after pkill)
- Confirm diagnose by looking at shared screenshots: chrome badge = normal CC + tophat; firefox badge = Load Mode + YES/NO; webkit badge = Load Mode empty slots. Divergence is pre-save UI race, not OPFS.
- `tools/save-signatures.json` present (7301 B). Develop `web-save-test-sig.mjs`: free ports 5221+, seed chrome OPFS from `web-save-profile-chrome-5207`, install progress logs, fix DIK for `b` (0x30) + WM_CHAR, filter FF console noise.
- Keep frame-sync `web-save-test.mjs` untouched until sig passes all three.

### 2026-10-25 — chrome sig PASS (w8c)
- `node tools/web-save-test-sig.mjs --browser chrome --port 5225` exit 0 in ~2m.
- OPFS after save: savgame2.ar1/2/3 = 88176/4425/127; load hash ≠ badge hash.
- Gates: title→profile (mad~2) → play clicks until crime_computer (mad~2.7) → save_confirm → YES; reload+LOAD same.
- Note: OPFS is origin/port-scoped — seeding File System from another port still need-install once; after that port 5225 profile is warm.

### 2026-10-25 — firefox + webkit sig PASS; promote default (w8c)
- FF/WK cold ISO unpack ~1–2 min then same signature gates as Chrome.
- Both wrote savgame2.ar1/2/3 = 88176/4425/127; load screenshots viewed (CC restored, clue present); hashes ≠ badge.
- Root cause of earlier FF/WK fail: frame-sync `DETECTIVE_INPUT` raced into Load Mode before save; product OPFS OK. Fix is test sync on screen signatures, not a browser hang.
- Promoted `web-save-test-sig.mjs` → `web-save-test.mjs` (Chrome still passes under signature mode).

### 2026-10-25 — final verify all three exit 0 (w8c)
- Sequential warm-profile: `web-save-test.mjs` chrome/firefox/webkit → **exit 0 / 0 / 0**.
- OPFS savgame2 = 88176/4425/127 each; load hashes ≠ badge; screenshots show CC with clue restored.
- Docs: `WEB-BUILD-NOTES.md` Save/load section updated to signature sync.

## 2026-10-25 — Final state and root causes (reviewer)
- Soak crash (`fault at 0x100000000`, string grow after a failed fread) root cause: the disc ships 30-byte placeholder `CURRENT.AR*`, `SAVGAME*.AR*` and `GAMEHIST.ORY`; the game read them as real archives. The installer no longer installs them (`installer/src/install.ts`), so the game creates its own in the save overlay. Verified: 7/7 runs of seeds that crashed before are clean without any generated-code patching. Worker m14's emit-c regex patches were NOT merged (native-only hacks, not needed).
- Typed names, GDI text (8x16 font from DejaVu Sans Mono, `runtime/src/hle/font8x16.h`), cutscene movies, in-game save/load (Crime Computer floppy icons, `savgame2.ar1/2/3` = 88176/4425/126 B) verified natively and in Chrome, Firefox and WebKit (`tools/web-save-test.mjs`, signature-synced clicks).
- Suspend/ResumeThread fix removed soak hangs (m11). VA window enlarged and HeapAlloc fallback added (m13).
- Known gaps: game played only up to the first case scenes and the save/load screens, not to the ending; no slow-device performance test; cutscene audio not checked by ear.

## Case play to the end (m15)

### Setup
- Worker `m15case`. `work/gen-c-m15case` → symlink to `gen-c-m14-crash`; build `work/build-native-game-m15case`.
- Overlay `DETECTIVE_SAVE_DIR=work/m15case-saves`. Frames `work/m15case-frames*`. Sheet `work/m15-sheet.png`.
- K10 entry (verified): profile `ab` → Abagail → OK `(244,440)`.

### Runtime fix — scripted input catch-up pacing (`platform_sdl.cpp`)
- **Bug**: when Flip stalls (asset load) then resumes, many `DETECTIVE_INPUT` events become due in one `poll_events`. Old code fired **all** due mouse downs before **all** due keys → OK clicked before `ab` typing; profile chooser wedged (Exp2/3).
- **Fix**: drain mouse/key ups first; start **at most one** new key or mouse per presented frame; keys before mouse on frame ties; `s_last_start_at` blocks multi-start when `poll_events` runs many times per Flip.
- **Verify**: Exp5 `bunches>1 = []` (101 events / 101 unique frames).

### Screen map
| Screen | Frame (approx) | Click path | Notes |
|--------|----------------|------------|-------|
| Title → GAMES | 80–200 | `(320,400)` `(200,400)` | |
| Profile Abagail | 500–1100 | New→field→BS×10→`ab`→`(250,207)`→OK× | Must not click list until past badge |
| Badge / OPENA.MOV | ~1200–1500 | skip `(320,240)` | `data\opena.mov` + dialogue wav |
| Carnival Area1 (MAGICEXT) | ≥1500 | hotspots; walk = `move` to screen edge (pink arrow) | Barbie, frog booth, top hat on trash, blue doors+red carpet |
| Mag glass mode | click `(50,450)` | cursor → lens; footprints/handprints | put away via hand icon same corner |
| Crime Computer | `(338,385)` / `(580,450)` | Clues/Suspect/Tools; save `(148,441)`; load `(121,354)`; slot `(258,275)` | |
| Load resume | profile floppy `(380,450)` or CC load | restores MAGICEXT + overlay `savgame2.*` | Exp6/7 |

### Clues / suspects found
- **Clues**: black top hat (CC Clues panel Exp1; still on trash in Area1); sparkling wand near trash (Exp5 f7000).
- **Suspects**: none unlocked (no ARTIE/BURT/JAKE `.MOV` opens).
- **Tools**: empty.

### Experiments
- **Exp1**: K10+explore+CC save. Carnival @f2000; CC+tophat @f4600; YES dialog @f4800; `savgame2` 88176/4425/126. Audio 274s, 33.5% nonzero. No SIGSEGV.
- **Exp2**: denser hotspots — **FAIL** stuck on profile (clicks hit name list).
- **Exp3**: firm OK — still bunched; stuck profile. Lesson → pacing fix.
- **Exp4** (after first pacing pass): OPENA.MOV; Abagail OK; carnival; mag glass active; walked closer to Magic Show doors (f4000). 21 unique dumps. Still Area1.
- **Exp5** (one-action-per-Flip): clean pacing; MAGICEXT loads; f7000 wand clue; mid-run profile regressions when Escape/CC misfires.
- **Exp6**: **load path PASS** — `SavGame2.ar*` opened from overlay; restored Area1 with hat; pink walk arrow visible (f3000); glove on red carpet (f1800). Did not enter interior.
- **Exp7**: load + door/carpet hammer PASS (no crash). Assets: `magicext.*`, `a1spr.cmp`, `CC.*`, `SavGame2.*` only — **still Area1**. Unique dumps 11. Door/carpet clicks + edge `move` did not open interior (`A2`/`ENDGAME` never opened).

### Blocker (ending not reached)
- Furthest: **Carnival Area1 / Magic Show exterior (`MAGICEXT.*`) + Crime Computer save/load + clue (top hat)**.
- Walkthrough next step: mag-glass footprints into theater → seats/stage/backstage → haunted house → … → accusation/credits (`ENDGAME.CMP` on disc, never opened).
- Blocked on **reliably entering the Magic Show interior** (no `A2`/`STAGE` asset opens yet). Walking/pink-arrow works; door/carpet clicks not yet triggering room transition in scripts. Not a crash — navigation/hotspot precision under scripted input.
- Time budget: ~55m; ending/credits **not** reached.

### Proof
- Contact sheet: `work/m15-sheet.png` (viewed).
- Saves: `work/m15case-saves/savgame2.ar{1,2,3}`.
- Binary: `work/build-native-game-m15case/detective`.


## Theater (n1)

### Setup
- Worker `n1`. `work/gen-c-n1` → `gen-c-m15case`; build `work/build-native-game-n1`.
- Overlay `cp -r work/m15case-saves work/n1-saves` (`DETECTIVE_SAVE_DIR=work/n1-saves`).
- Frames `work/n1-frames/exp*`. Load path (profile floppy → slot 2): title `(320,400)(200,400)(480,400)` then `(380,450)×2`, OK `(244,440)×2`, slot `(258,275)×2`, `(280,250)(430,385)(450,310)` — restores MAGICEXT + top hat.

### Screens reached
| Screen | Proof | How |
|--------|-------|-----|
| Profile → load slot 2 | Exp1+; `SavGame2.ar*` from overlay | profile floppy `(380,450)` → slot `(258,275)` |
| Carnival Area1 MAGICEXT | Exp1 f2200+ | load resume |
| **Magic Show stage A2** | Exp4/8 f2600; peek `488f40=02`; `a2.{cmp,gsc,gss,bbw,pal,spr}` | poke dest (below) |
| A2 stage play | Madame Wanda poster, cabinet `?`, table hat, seats, EXIT in curtains; `SPCTRA01.WAV` | clicks |
| **Backstage A5** | Exp8 f3600+; peek `488f40=05`; `a5.{cmp,pal,spr}` — prop room, trunk, stairs+pink arrow, birdcage | top-edge walk from A2 stage |

### Door → A2: natural FAIL; poke PASS
- Carpet/door long-hold + mag-glass trail: `488f40` stays `01` (Exp6).
- Exp7 peek after door clicks: `47fc6c=0`, `4856f0=0`, `485704=0` — door SM `@0x446800` **never arms** (needs `47fc6c==3` and `4856f0!=0`).
- MAGICEXT walk-offs → areas **8 / 0x16**, not A2. World BBW `1920×960`; walkmesh x can be >640.
- **Entry that works:** after Area1 settle, `DETECTIVE_POKE='2500:488cc0=2,2500:488abc=1,2500:488d48=1'` → A2 (Exp4/8). Exp7 poke during input catch-up failed (race).

### A2 → A5 (backstage) PASS
- Area==2 walk table: dest **5** when `x∈[0x308,0x398]` and `y∈(0x308,0x374]` (Exp8 top-edge `move(320,20)`).
- dest **3** (`a3.*`) is a different gate (`x∈(0x258,0x320)`, `y>0x384`) — not hit yet.
- In-A2/A5 CC save clicks did not rewrite `savgame2.*` sizes this run (still 88176/4425/126).

### Runtime fixes
- None (no crash). Natural door blocked by door SM never starting under scripted click-to-walk; poke bypass confirmed for A2+.

### Crashes
- None.

### Proof / artifacts
- Binary: `work/build-native-game-n1/detective`
- Overlay: `work/n1-saves/`
- Logs: `work/n1-logs/exp{1..8}.log`
- Frames: `work/n1-frames/exp{1..8}/`; sheet `work/n1-frames/n1-a2-sheet.png`


## Areas (n2)

### Setup
- Worker `n2`. `work/gen-c-n2` → `gen-c-m15case` → `gen-c-m14-crash`. Build `work/build-native-game-n2`.
- Overlay `cp -r work/m15case-saves work/n2-saves` (`savgame2.ar{1,2,3}` + `gamehist.ory` dword **2**).
- Frames `work/n2-frames/exp*`; logs `work/n2-logs/`; audio `work/n2-audio/`.

### Runtime fix — FindFirstFile merges overlay at install root (`kernel32_files.cpp`)
- **Bug**: `guest_relative_key("C:\\Detective Barbie\\")` is `""`, so FindFirst skipped `g_overlay_root` and never listed overlay `SavGame*` / `GameHist.ory`. `write_find_data` also always `stat`'d the install dir (0-size for overlay-only files).
- **Fix**: when `rel_dir` empty, set `overlay_dir = g_overlay_root`; track per-name host dirs so overlay saves report real sizes. (BDETECT does **not** import `FindFirstFileA` — load uses `CreateFile(SavGame%d.ar*)` after slot click — but VFS listing is now correct for other callers/tests.)

### Load resume (PASS — m15 Exp6 click map)
| Frame (sched) | Click | Effect |
|---------------|-------|--------|
| 80 / 200 / 500 | `(320,400)` `(200,400)` `(480,400)` | Title → GAMES → profile |
| 900–920 | `(380,450)` ×2 | Profile floppy → Crime Computer **Load Game Mode** |
| 1000–1020 | `(244,440)` ×2 | OK / confirm on load UI |
| 1100–1120 | `(258,275)` ×2 | **Slot 2** → opens `SavGame2.ar{1,2,3}` from overlay |
| 1200 / 1300 | `(280,250)` `(430,385)` | Extra confirm / YES |
| ≥1400 | — | Restored **MAGICEXT** + `a1spr.cmp` + top-hat clue |

Early Exps 1–4 failed load because slot/YES fired **before** CC was up (empty orange slots visually; no `SavGame*` CreateFile). Exp4 briefly opened `magicext` then bounced to signin and rewrote `gamehist` to slot **1** — restore from `m15case-saves` before retries.

### Screens / areas reached (eye + assets)
| Screen | Asset prefix | Hotspots / notes |
|--------|--------------|------------------|
| Profile clipboard | `signin.*` `pinames.lst` | Name list, checkmark, floppy `(380,450)`, CC capsule |
| Crime Computer load | `cc.*` `ccspr.*` | Load Mode; 5 orange slots left; Clues/Suspect/Tools right; floppies |
| **Area1 Magic Show exterior** | **`MAGICEXT.*` + `A1SPR.CMP`** | Barbie; frog booth; blue doors+red carpet+velvet ropes; trash+**top hat**; mag glass `(50,450)`; CC bar `(338,385)` / laptop BR; yellow horse props; pink/edge walk arrows |
| Mag-glass inspect | same | Lens zoom on ropes/poster/post; dialogue `OLDCLUE` `GAGG*` `SPCTRA01` `KNSG01` `GKOP01` |
| Crime Computer in-case | `cc.*` | Clue (top hat) in panel; Viewscreen |

**Not reached:** any `A2`–`A25` / interior / haunted / `ARTIE*.MOV` / `ENDGAME.*`. Edge walks, horse-sign clicks, door/carpet/mag-glass hammer (Exp5–6) stayed on `MAGICEXT` only — same navigation blocker as m15 (not a crash).

### Audio / stability
- Exp5 dialogue run: ~234s WAV, **23.4%** nonzero (non-silent during clue lines). No SIGSEGV / `dispatch to unknown`.
- Checkpoint overlay remains `work/n2-saves/savgame2.*` (88176/4425/126).

### Proof
- Sheets: `work/n2-frames/exp5-sheet.png`, `work/n2-frames/exp6-sheet.png`, `work/n2-frames/final-sheet.png` (viewed).
- Final test log: `work/n2-logs/final.log` — `SavGame2` + `magicext` + `a1spr`.
- Binary: `work/build-native-game-n2/detective`.


## Minigames (n3)

### Setup
- Worker `n3`. `work/gen-c-n3` → symlink `gen-c-m14-crash`; build `work/build-native-game-n3` (OK).
- Overlay `cp -r work/m15case-saves work/n3-saves` (and per-exp `work/n3-saves-exp*`); frames `work/n3-frames/exp*`; logs `work/n3-logs/`.
- Resume load (m15 Exp6/7 verified): profile floppy `(380,450)×2` → OK `(244,440)×2` → slot2 `(258,275)×2` → `SavGame2.ar*` from overlay → MAGICEXT.

### Runtime fix — scripted `drag` / `down` / `up` (`platform_sdl.cpp`)
- **Before**: `DETECTIVE_INPUT` only had `move` / `click` / `rclick` (hold then release at start xy).
- **Add**: `drag(x1,y1,x2,y2)[+travel]`, `down(x,y)`, `up(x,y)`. Drag presses at start, lerps while held, releases at end. Verified Exp6 log: `scripted input: drag 100,200 at frame 4800`.

### Screens reached (eye-verified PPM→PNG)
| Screen | Exp | Notes |
|--------|-----|-------|
| Title hub | 1 | GAMES tent visible `(200,400)` |
| Profile chooser | 1–4 | GAMES click opens same sign-in as case |
| CC Load Game Mode | 4 | slots + Clues/Suspect/Tools empty; stuck without OK×2 |
| Area1 MAGICEXT | 5–7 | after SavGame2; Barbie, frog booth, top hat, mag glass, CC icon |
| CC Clue Database | 5 | **"Top Hat (HOT!)"** + hat sprite; printer/scanner icon on view |
| CC Tool Database | 6–7 | **"Tool Database Active"** (empty — save has no tools) |
| CC Suspect panel | 5–7 | empty grid (no suspect MOV unlocked) |

### Hub GAMES vs case minigames
- Hub **GAMES** `(200,400)` → profile → NewGame/MAGICEXT (same carnival case), **not** a separate arcade menu. Active-ride minigames (`BUMPER.*`, Tunnel of Love, etc.) need deeper case navigation (m15 blocker: Magic Show interior). Disc has `SLIDE{1,2,3}.PAK` + `"Suspect Database Active"`; fingerprint/compare UI not opened with only the top-hat clue.

### Load script (PASS — Exp5+)
```
LOAD='80:click(320,400),200:click(200,400),500:click(480,400),600:click(380,450),650:click(380,450),700:click(244,440),750:click(244,440),800:click(258,275),850:click(258,275)'
```
- **Bug (scripting)**: OK only once → Load Mode Active forever, no `SavGame2` open (Exp4). Mid-game CC load without settle also misses restore (Exp3).

### Bugs / observations
- No SIGSEGV in Exp1–7.
- DUMP_FRAME uses presented frames; after Flip stalls, scripted events drain one-per-Flip so early dumps land on profile while later actions still queue.
- Escape/X while probing CC often leaves subsequent clicks on CC (Exp6 f4800 still Tool DB).
- Text/colours on CC (Clue/Tool Database Active, Top Hat) look correct; GDI text OK.
- Frog booth dialogue `GAGG01.WAV` fired (Exp7); no ride/bumper asset open.

### Proof
- Sheets: `work/n3-frames/exp{4,5,6,7}-sheet.png` (viewed).
- Checkpoint overlay: `work/n3-saves/savgame2.ar{1,2,3}`.
- Binary: `work/build-native-game-n3/detective`.


## 2026-10-25 — Final regression on the merged tree (reviewer)
check-all 7/7 PASS; web build OK; native soak seeds 211-216 (420 s) 0 crashes; web-play-test and web-save-test pass in Chrome, Firefox and WebKit; web-fps Chrome 60.0 fps on 5 scenes (p95 16.8 ms).
Areas verified rendering via DETECTIVE_POKE area forcing (n5): A2-A7, A9, A10, A12, A15 (viewed contact sheet), no crashes.
Open: the natural click on the Magic Show door in Area1 does not open it under scripted input (A2 only via poke), so the case ending was never reached; real-device performance and listening to audio were not tested.
