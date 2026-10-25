# Detective Barbie — in your browser (bring your own disc)

Play your own copy of *Detective Barbie* (PC, 1998, Mattel Media) in a desktop browser.

This repository and the site built from it contain **no game files and no game code**. Each player
chooses their own disc image (`DETECTV1_1.ISO`); the browser unpacks it into private storage and
translates the game's x86 program (`BDETECT.EXE`) to WebAssembly on the player's machine. The
translated code runs on a re-implementation of the Windows, DirectDraw, and DirectSound interfaces
the game uses.

See `docs/PLAN.md` for architecture and milestones, and `docs/JOURNAL.md` for the engineering log.

Not affiliated with or endorsed by Mattel Media or Mattel, Inc.

## Playing

1. Open the site in a **normal** (non-private) desktop browser window.
2. Choose your **Detective Barbie CD** disc image (for example `DETECTV1_1.ISO`). The installer
   unpacks InstallShield cabinets into the browser's private storage (about 560 MB, one time).
3. Click **Play**. The page translates `BDETECT.EXE` locally (cached in private storage) and starts
   the game in a 640×480 canvas.
4. Use the mouse to point and click through menus and scenes. Press **F1** (or **Controls** on the
   HUD) for the full control list.
5. **Saved games** live only in this browser. Use **Back up saves** / **Restore saves** to export or
   import a ZIP (compatible with a Windows install's save files).

The retail disc must contain `BDETECT.EXE` on the root; images without it are refused with an
explanation.

## Browsers

Tested on current desktop **Chrome**, **Edge**, **Firefox**, and **Safari** with private storage
enabled (not private/incognito mode). You need roughly **600 MB** of storage quota for the unpacked
game. First-time install is typically under a minute in Chromium and Safari; Firefox may take about
two minutes.

Headless smoke (developers): `node tools/web-smoke.mjs --iso DETECTV1_1.ISO`.

## Controls

| Input | Action |
|-------|--------|
| Mouse | Point and click (game cursor) |
| **F1** | Controls help overlay |
| **Fullscreen** (HUD) | Enter / leave fullscreen |
| **Esc** | In-game back / menu (see Settings for browser Esc substitute) |
| Keyboard | Game-specific keys (see in-game help after **F1**) |

In **Settings**, pick a **Substitute Esc key** when the browser reserves Esc outside fullscreen
(Chrome and Edge pass Esc through in fullscreen).

## Building

Requirements: **Node.js 24+**, **Emscripten 6.0.12** (`work/tools/emsdk`), and for native debug
**SDL2** plus a C++ toolchain.

```sh
npm install
tools/build-web.sh                       # player engine -> web/public/engine/
cd web && npx vite                       # dev server at http://localhost:5173
```

Installer smoke (local disc image):

```sh
node installer/src/cli.ts DETECTV1_1.ISO work/install
node tools/web-smoke.mjs --install-only
```

Native recompiled game (HLE work; see `docs/JOURNAL.md`):

```sh
node recomp/src/cli.ts gen-c work/ref/BDETECT.EXE --out work/gen-c-<id>
cmake -S runtime -B work/build-native-game-<id> -DGEN_DIR="$PWD/work/gen-c-<id>"
cmake --build work/build-native-game-<id>
SDL_VIDEODRIVER=dummy DETECTIVE_DUMP_FRAME=120:work/frame120.ppm timeout 90 \
  work/build-native-game-<id>/detective work/install
```

Production bundle and release guards (`REPO_URL` is substituted into built pages):

```sh
REPO_URL=https://github.com/<owner>/<repo> tools/build-release.sh
```

Deploy to Cloudflare Pages (after `npx wrangler login`):

```sh
tools/deploy.sh    # PROJECT=detective-barbie by default
```

CI-style checks:

```sh
bash tools/check-all.sh
npx tsc -p tsconfig.json --noEmit
npm test
```

`work/` and `research/` are git-ignored: game files and derived data never enter the repository or
the shipped site (`tools/scan-dist.mjs` enforces this on `web/dist`).

## Licenses

Repository code is under the **MIT License** (`LICENSE`). Third-party components are listed in
`THIRD_PARTY.md`. The deployed site includes `licenses.html` with notices for iced-x86, Emscripten,
dlmalloc, and Vite.
