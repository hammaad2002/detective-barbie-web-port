#!/usr/bin/env node
// In-browser save/load via signature sync (tools/save-signatures.json) + engine clicks/keys.
// Frame-independent; works across Chrome / Firefox / WebKit (see docs/JOURNAL.md Web saves w8).
//   node tools/web-save-test.mjs [--iso DETECTV1_1.ISO] [--port 5225] [--browser chrome|firefox|webkit]
// Dev copy of the same flow: tools/web-save-test-sig.mjs
import { createHash } from "node:crypto";
import { spawn } from "node:child_process";
import { cpSync, existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import net from "node:net";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { browserArgs, launch } from "./browsers.mjs";

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const args = process.argv.slice(2);
const opt = (name, dflt) => {
  const i = args.indexOf(`--${name}`);
  return i >= 0 ? args[i + 1] : dflt;
};
const isoArg = opt("iso", "DETECTV1_1.ISO");
const browserName = browserArgs(args).browser;

const portFree = (p) =>
  new Promise((resolvePort) => {
    const s = net.createServer();
    s.once("error", () => resolvePort(false));
    s.listen(p, "127.0.0.1", () => s.close(() => resolvePort(true)));
  });

// Shared worker farm saturates 5181–5220; pick a free port up through 5350.
// Search upward from the requested port so parallel runs don't all collapse onto 5221.
let port = Number(opt("port", "5225"));
if (!(await portFree(port))) {
  let found = 0;
  for (let p = port + 1; p < 5350; p++) {
    if (await portFree(p)) {
      console.log(`port ${port} busy; using ${p}`);
      port = p;
      found = 1;
      break;
    }
  }
  if (!found) throw new Error(`no free port in ${port + 1}–5349`);
}

function resolveIso(path) {
  const candidates = [
    path,
    join(ROOT, path),
    join(ROOT, "..", "..", "..", "detective-barbie-web-port", path),
  ];
  for (const c of candidates) if (existsSync(c)) return resolve(c);
  throw new Error(`disc image not found: ${path}`);
}

const isoPath = resolveIso(isoArg);
const outDir = join(ROOT, "work");
mkdirSync(outDir, { recursive: true });

const SIG_PATH = join(ROOT, "tools/save-signatures.json");
const SIGS = JSON.parse(readFileSync(SIG_PATH, "utf8"));
const SIG_TOL = SIGS.tol ?? 18;
const SIG_GRID = SIGS.grid ?? 8;

const vite = spawn("npx", ["vite", "--config", "vite.config.ts", "--port", String(port), "--strictPort"], {
  cwd: join(ROOT, "web"),
  stdio: ["ignore", "pipe", "pipe"],
  env: { ...process.env, DETECTIVE_ISO: isoPath },
});
let viteLog = "";
vite.stdout?.on("data", (d) => (viteLog += d));
vite.stderr?.on("data", (d) => (viteLog += d));

async function waitForServer() {
  const deadline = Date.now() + 60_000;
  while (Date.now() < deadline) {
    try {
      const res = await fetch(`http://localhost:${port}/`);
      if (res.ok) return;
    } catch {
      // retry
    }
    await new Promise((r) => setTimeout(r, 300));
  }
  throw new Error(`vite did not start on port ${port}\n${viteLog}`);
}

function stopVite() {
  vite.kill("SIGTERM");
}

const GAME_W = 640;
const GAME_H = 480;
const EXPECT_SIZES = { ar1: 88176, ar2: 4425, ar3: 126 };
const SIZE_TOL = 64;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// Click maps from native m12 (docs/JOURNAL.md), driven after signature gates — not frame clocks.
const TITLE_CLICKS = [
  [320, 400],
  [200, 400],
  [480, 400],
];
const PROFILE_SETUP = { focus: [140, 228], check: [250, 207], ok: [244, 440] };
const PLAY_CLICKS = [
  [350, 250],
  [350, 250],
  [320, 300],
  [320, 360],
  [320, 400],
  [320, 240],
  [320, 240],
  [320, 240],
  [400, 400],
  [400, 400],
  [320, 240],
  [320, 240],
  [300, 380],
  [420, 360],
  [360, 320],
  [500, 350],
  [180, 280],
  [580, 400],
  [400, 400],
  [320, 240],
  [320, 240],
];
const SAVE_CLICKS = {
  floppy: [338, 385],
  slot: [148, 441],
  yes: [258, 275],
};
const LOAD_CLICKS = {
  floppy: [338, 385],
  slot: [121, 354],
  yes: [258, 275],
  exit: [430, 385],
};

async function gamePoint(page, gx, gy) {
  return page.evaluate(
    ({ gx, gy, cw, ch }) => {
      const canvas = document.getElementById("canvas");
      const r = canvas.getBoundingClientRect();
      const iw = Math.min(r.width, (r.height * cw) / ch);
      const ih = Math.min(r.height, (r.width * ch) / cw);
      const ix = r.left + (r.width - iw) / 2;
      const iy = r.top + (r.height - ih) / 2;
      return { x: ix + (gx / cw) * iw, y: iy + (gy / ch) * ih };
    },
    { gx, gy, cw: GAME_W, ch: GAME_H },
  );
}

/** Engine-direct click (avoids DOM/pointer-lock scaling races across browsers). */
async function clickGame(page, gx, gy, holdMs = 200) {
  const x = Math.round(gx);
  const y = Math.round(gy);
  await page.evaluate(
    ({ x, y }) => {
      const m = globalThis.__detectiveModule;
      if (!m?._web_mouse_button) throw new Error("no __detectiveModule._web_mouse_button");
      m._web_mouse_move(x, y, 0, 0);
      m._web_mouse_button(0, 1, x, y);
    },
    { x, y },
  );
  await sleep(holdMs);
  await page.evaluate(
    ({ x, y }) => {
      globalThis.__detectiveModule._web_mouse_button(0, 0, x, y);
    },
    { x, y },
  );
}

/** Engine-direct key (more reliable than DOM for profile typing across browsers). */
async function engineKey(page, name, holdMs) {
  // DIK for letters: a=0x1e, b=0x30 (not sequential). WM_CHAR comes from user32 KeyDown→char_from_vk.
  const map = {
    Backspace: [0x08, 0x0e, 0],
    a: [0x41, 0x1e, 0x61],
    b: [0x42, 0x30, 0x62],
  };
  const pair = map[name];
  if (!pair) throw new Error(`unknown engine key ${name}`);
  const [vk, dik, ch] = pair;
  await page.evaluate(
    ({ vk, dik, ch }) => {
      const m = globalThis.__detectiveModule;
      if (!m?._web_key) throw new Error("no __detectiveModule._web_key");
      m._web_key(0, vk, dik, 0);
      if (ch) m._web_key(2, 0, 0, ch);
    },
    { vk, dik, ch },
  );
  await sleep(holdMs);
  await page.evaluate(
    ({ vk, dik }) => {
      globalThis.__detectiveModule._web_key(1, vk, dik, 0);
    },
    { vk, dik },
  );
}

async function canvasSignatureScores(page) {
  return page.evaluate(
    ({ screens, grid }) => {
      const c = document.getElementById("canvas");
      if (!c || !(c instanceof HTMLCanvasElement)) return null;
      // Must reuse the presenter's context options ({ alpha: false }); a different
      // getContext options bag returns null and would break all signature gates.
      const mod = globalThis.__detectiveModule;
      const ctx = mod?.__presentCtx || c.getContext("2d", { alpha: false });
      if (!ctx) return null;
      const scores = {};
      for (const [name, screen] of Object.entries(screens)) {
        let total = 0;
        let n = 0;
        for (const reg of screen.regions) {
          const [x, y, w, h] = reg.box;
          const { data } = ctx.getImageData(x, y, w, h);
          // Box-average into grid×grid, then MAD vs reference rgb.
          const cellW = w / grid;
          const cellH = h / grid;
          const avg = new Array(grid * grid * 3).fill(0);
          const cnt = new Array(grid * grid).fill(0);
          for (let py = 0; py < h; py++) {
            for (let px = 0; px < w; px++) {
              const cx = Math.min(grid - 1, Math.floor(px / cellW));
              const cy = Math.min(grid - 1, Math.floor(py / cellH));
              const ci = cy * grid + cx;
              const di = (py * w + px) * 4;
              avg[ci * 3] += data[di];
              avg[ci * 3 + 1] += data[di + 1];
              avg[ci * 3 + 2] += data[di + 2];
              cnt[ci]++;
            }
          }
          const ref = reg.rgb;
          for (let i = 0; i < grid * grid; i++) {
            const d = Math.max(1, cnt[i]);
            for (let ch = 0; ch < 3; ch++) {
              const v = avg[i * 3 + ch] / d;
              total += Math.abs(v - ref[i * 3 + ch]);
              n++;
            }
          }
        }
        scores[name] = n ? total / n : 999;
      }
      return scores;
    },
    { screens: SIGS.screens, grid: SIG_GRID },
  );
}

function screenMatches(name, scores, tol = SIG_TOL) {
  if (!scores || scores[name] == null) return false;
  if (name === "crime_computer") {
    return scores.crime_computer <= tol && scores.load_mode > tol && (scores.save_confirm ?? 999) > tol;
  }
  if (name === "save_confirm") return scores.save_confirm <= tol;
  if (name === "load_mode") return scores.load_mode <= tol && scores.save_confirm > tol;
  const best = Object.entries(scores).sort((a, b) => a[1] - b[1])[0]?.[0];
  return scores[name] <= tol && best === name;
}

async function waitForSignature(page, name, { timeout = 180_000, poll = 250, label = name } = {}) {
  const deadline = Date.now() + timeout;
  let last = null;
  while (Date.now() < deadline) {
    const scores = await canvasSignatureScores(page);
    last = scores;
    if (screenMatches(name, scores)) {
      console.log(
        `sig ${label}: matched ${name} mad=${scores[name].toFixed(1)} frames=${await page.evaluate(() => window.__detect?.frames)} ddraw=${await page.evaluate(() => globalThis.__detectiveModule?.__ddrawFrameCounter ?? 0)}`,
      );
      return scores;
    }
    await sleep(poll);
  }
  const shot = join(outDir, `web-save-sigfail-${browserName}-${label}.png`);
  try {
    writeFileSync(shot, await page.locator("#canvas").screenshot({ type: "png" }));
  } catch {
    // ignore
  }
  throw new Error(`timeout waiting for signature '${name}' (${label}); last=${JSON.stringify(last)}; shot=${shot}`);
}

async function captureNow(page, path, label) {
  const png = await page.locator("#canvas").screenshot({ type: "png" });
  writeFileSync(path, png);
  const scores = await canvasSignatureScores(page);
  const frames = await page.evaluate(() => ({
    frames: window.__detect?.frames,
    ddraw: globalThis.__detectiveModule?.__ddrawFrameCounter ?? 0,
  }));
  console.log(`capture ${label}: ${path} frames=${JSON.stringify(frames)} best=${scores ? Object.entries(scores).sort((a, b) => a[1] - b[1])[0] : "?"}`);
  return png;
}

async function listOpfsSaves(page) {
  return page.evaluate(async () => {
    async function walk(dir, prefix = "") {
      const out = [];
      for await (const [name, handle] of dir.entries()) {
        const p = prefix ? `${prefix}/${name}` : name;
        if (handle.kind === "directory") out.push(...(await walk(handle, p)));
        else {
          const f = await handle.getFile();
          out.push({ name: p, size: f.size });
        }
      }
      return out;
    }
    const root = await navigator.storage.getDirectory();
    const all = await walk(root);
    const inSaves = all.filter((f) => /^saves\//i.test(f.name) || /^opfs\/saves\//i.test(f.name));
    if (inSaves.length) return inSaves.map((f) => ({ name: f.name.replace(/^(?:opfs\/)?saves\//i, ""), size: f.size }));
    return all
      .filter((f) => /savgame\d+\.ar[123]/i.test(f.name))
      .map((f) => ({ name: f.name.split("/").pop(), size: f.size }));
  });
}

function assertSavgame2(files) {
  const byLower = new Map(files.map((f) => [f.name.toLowerCase(), f]));
  const slot = [2, 3, 1, 0].find((n) => byLower.has(`savgame${n}.ar1`));
  if (slot === undefined) {
    throw new Error(`OPFS missing savgame*.ar1; got ${files.map((x) => `${x.name}(${x.size})`).join(", ")}`);
  }
  for (const ext of ["ar1", "ar2", "ar3"]) {
    const key = `savgame${slot}.${ext}`;
    const f = byLower.get(key);
    if (!f) throw new Error(`OPFS missing ${key}`);
    const exp = EXPECT_SIZES[ext];
    if (Math.abs(f.size - exp) > SIZE_TOL) {
      throw new Error(`${key}: expected ~${exp} bytes, got ${f.size}`);
    }
  }
  console.log(`verified savgame${slot}.ar{1,2,3} sizes`);
}

async function bootToRunning(page, isoPath, { resetSave = false } = {}) {
  const q = ["noCapture"];
  if (resetSave) q.push("resetSave");
  const playUrl = `http://localhost:${port}/?${q.join("&")}`;
  console.log(`boot goto ${playUrl}`);
  await page.goto(playUrl);
  await page.waitForLoadState("domcontentloaded");
  await page.waitForFunction(
    () => ["need-install", "ready", "error"].includes(window.__detect?.state ?? ""),
    null,
    { timeout: 60_000 },
  );
  const boot = await page.evaluate(() => window.__detect?.state);
  console.log(`boot state=${boot}`);
  if (boot === "error") {
    const msg = await page.evaluate(() => window.__detect?.status ?? "");
    throw new Error(`shell error on load: ${msg}`);
  }
  if (boot === "need-install") {
    console.log(`boot installing ISO ${isoPath}`);
    await page.setInputFiles("#iso", isoPath);
    const installDeadline = Date.now() + 900_000;
    let last = "";
    while (Date.now() < installDeadline) {
      const st = await page.evaluate(() => ({
        state: window.__detect?.state,
        status: document.getElementById("status")?.textContent ?? "",
      }));
      if (st.state === "ready") break;
      if (st.state === "error") throw new Error(`install failed: ${st.status}`);
      const line = `${st.state}|${st.status}`;
      if (line !== last) {
        console.log(`boot install… ${line}`);
        last = line;
      } else {
        // heartbeat every ~15s so we can tell install is alive
        const elapsed = Math.floor((Date.now() - (installDeadline - 900_000)) / 1000);
        if (elapsed % 15 === 0) console.log(`boot install still ${line} t=${elapsed}s`);
      }
      await sleep(1000);
    }
    const after = await page.evaluate(() => window.__detect?.state);
    if (after !== "ready") throw new Error(`ISO install timed out; state=${after}`);
    console.log("boot install ready");
  }
  await page.evaluate(() => {
    try {
      localStorage.setItem("detect.controlsSeen", "1");
    } catch {
      // ignore
    }
  });
  console.log("boot click play");
  await page.click("#play");
  await page.waitForFunction(() => window.__detect?.state === "running", null, { timeout: 300_000 });
  console.log("boot running");
  await page.waitForFunction(() => (window.__detect?.frames ?? 0) > 20, null, { timeout: 180_000 });
  console.log(`boot frames=${await page.evaluate(() => window.__detect?.frames)}`);
  await page.locator("#canvas").click({ position: { x: 10, y: 10 } }).catch(() => {});
}

/** Prefer a pre-installed Chrome OPFS profile so we skip the 10+ min ISO unpack. */
function ensureProfileDir(browser, portNum) {
  const dest = join(ROOT, `work/web-save-profile-${browser}-${portNum}`);
  const explicit = opt("profile", "");
  if (explicit) {
    if (!existsSync(dest)) {
      console.log(`seeding profile from --profile ${explicit} → ${dest}`);
      cpSync(explicit, dest, { recursive: true });
    }
    return dest;
  }
  if (browser === "chrome" && !existsSync(join(dest, "Default"))) {
    const seeds = [
      join(ROOT, "work/web-save-profile-chrome-5207"),
      join(ROOT, "work/web-save-profile-chrome-5208"),
      join(ROOT, "work/chrome-profile-5174"),
    ];
    for (const seed of seeds) {
      if (!existsSync(join(seed, "Default", "File System"))) continue;
      console.log(`seeding chrome OPFS profile from ${seed} → ${dest}`);
      cpSync(seed, dest, { recursive: true });
      for (const lock of ["SingletonLock", "SingletonSocket", "SingletonCookie"]) {
        try {
          rmSync(join(dest, lock), { force: true });
        } catch {
          // ignore
        }
      }
      break;
    }
  }
  mkdirSync(dest, { recursive: true });
  return dest;
}

async function runTitleToProfile(page) {
  console.log("gate: title");
  // Title may already be up; also accept brief black/loading by retrying clicks.
  const deadline = Date.now() + 120_000;
  while (Date.now() < deadline) {
    const scores = await canvasSignatureScores(page);
    if (screenMatches("profile", scores)) {
      console.log("already on profile");
      return;
    }
    if (screenMatches("title", scores) || (scores && scores.title < 40)) {
      for (const [x, y] of TITLE_CLICKS) {
        await clickGame(page, x, y);
        await sleep(700);
        if (screenMatches("profile", await canvasSignatureScores(page))) return;
      }
    } else {
      // Still loading / cutscene — nudge center
      await clickGame(page, 320, 400);
      await sleep(500);
    }
    if (screenMatches("profile", await canvasSignatureScores(page))) return;
  }
  await waitForSignature(page, "profile", { timeout: 60_000, label: "title-to-profile" });
}

async function runProfile(page) {
  await waitForSignature(page, "profile", { timeout: 120_000, label: "profile" });
  await clickGame(page, ...PROFILE_SETUP.focus);
  await sleep(200);
  for (let i = 0; i < 10; i++) {
    await engineKey(page, "Backspace", 60);
    await sleep(40);
  }
  await engineKey(page, "a", 350);
  await sleep(80);
  await engineKey(page, "b", 350);
  await sleep(200);
  await clickGame(page, ...PROFILE_SETUP.check);
  await sleep(400);
  await clickGame(page, ...PROFILE_SETUP.ok);
  await sleep(600);
  await clickGame(page, ...PROFILE_SETUP.ok);
  await sleep(400);
}

async function runPlayUntilCrimeComputer(page) {
  console.log("gate: play until crime_computer");
  const deadline = Date.now() + 300_000;
  let i = 0;
  while (Date.now() < deadline) {
    const scores = await canvasSignatureScores(page);
    if (screenMatches("crime_computer", scores)) {
      console.log(`reached crime_computer after ${i} play clicks`);
      return;
    }
    // Avoid deepening load mode if we accidentally opened it
    if (screenMatches("load_mode", scores) || screenMatches("save_confirm", scores)) {
      await clickGame(page, 500, 80); // try close (X)
      await sleep(300);
      await clickGame(page, 430, 385);
      await sleep(400);
      i++;
      continue;
    }
    // Near CC (low crime_computer score): only click viewscreen — bottom bar/floppies
    // open Load Mode and are what raced FF/WK on the frame-sync script.
    let x, y;
    if (scores && scores.crime_computer < 45) {
      x = 200;
      y = 200;
    } else {
      [x, y] = PLAY_CLICKS[i % PLAY_CLICKS.length];
    }
    await clickGame(page, x, y);
    i++;
    await sleep(450);
    if (i % 8 === 0) {
      const f = await page.evaluate(() => ({
        frames: window.__detect?.frames,
        ddraw: globalThis.__detectiveModule?.__ddrawFrameCounter ?? 0,
      }));
      const best = scores ? Object.entries(scores).sort((a, b) => a[1] - b[1])[0] : null;
      console.log(`play progress clicks=${i} best=${best} ${JSON.stringify(f)}`);
    }
  }
  throw new Error("timeout waiting to reach crime_computer");
}

async function runSave(page) {
  await waitForSignature(page, "crime_computer", { timeout: 60_000, label: "pre-save-cc" });
  // m12: open bottom bar → save floppy → YES×3 (258,275 is slot2/YES overlay)
  await clickGame(page, ...SAVE_CLICKS.floppy);
  await sleep(600);
  await clickGame(page, ...SAVE_CLICKS.slot);
  await sleep(400);
  await clickGame(page, ...SAVE_CLICKS.slot); // double-click floppy (w6/w7 needed this)
  await sleep(500);
  await waitForSignature(page, "save_confirm", { timeout: 90_000, label: "save-yes" });
  for (let n = 0; n < 3; n++) {
    await clickGame(page, ...SAVE_CLICKS.yes);
    await sleep(350);
  }
  await sleep(800);
}

async function runLoad(page) {
  await waitForSignature(page, "crime_computer", { timeout: 180_000, label: "pre-load-cc" });
  await clickGame(page, ...LOAD_CLICKS.floppy);
  await sleep(600);
  await clickGame(page, ...LOAD_CLICKS.slot);
  await sleep(400);
  await clickGame(page, ...LOAD_CLICKS.slot);
  await sleep(500);
  const deadline = Date.now() + 30_000;
  while (Date.now() < deadline) {
    const scores = await canvasSignatureScores(page);
    if (screenMatches("save_confirm", scores)) {
      await clickGame(page, ...LOAD_CLICKS.yes);
      await sleep(350);
      await clickGame(page, ...LOAD_CLICKS.yes);
      break;
    }
    if (screenMatches("load_mode", scores)) {
      // click first occupied slot then YES
      await clickGame(page, 150, 160);
      await sleep(400);
    } else {
      break;
    }
  }
  await sleep(400);
  await clickGame(page, ...LOAD_CLICKS.exit);
  await sleep(600);
}

const sha = (buf) => createHash("sha256").update(buf).digest("hex").slice(0, 16);

let exitCode = 1;
try {
  await waitForServer();
  const profile = ensureProfileDir(browserName, port);
  console.log(`profile=${profile}`);
  const ctx = await launch({ ...browserArgs(args), port, profile, extra: { acceptDownloads: true } });
  const page =
    ctx.pages().length === 1 && ctx.pages()[0].url() === "about:blank" ? ctx.pages()[0] : await ctx.newPage();

  const consoleErrors = [];
  const pageErrors = [];
  page.on("console", (m) => {
    if (m.type() === "error") consoleErrors.push(m.text());
  });
  page.on("pageerror", (e) => pageErrors.push(e.message));

  console.log(`=== phase 1: signature-sync SAVE (${browserName}) ===`);
  await bootToRunning(page, isoPath, { resetSave: true });
  await runTitleToProfile(page);
  await runProfile(page);
  await runPlayUntilCrimeComputer(page);

  const badgePath = join(outDir, `web-save-save-badge-${browserName}.png`);
  await captureNow(page, badgePath, "badge");

  await runSave(page);
  await captureNow(page, join(outDir, `web-save-save-dialog-${browserName}.png`), "save-dialog");
  const postSavePath = join(outDir, `web-save-save-post-save-${browserName}.png`);
  await captureNow(page, postSavePath, "post-save");

  const opfsAfterSave = await listOpfsSaves(page);
  console.log("OPFS saves/ after in-game save:");
  for (const f of opfsAfterSave) console.log(`  ${f.name}  ${f.size}`);
  assertSavgame2(opfsAfterSave);

  const postSaveHash = sha(readFileSync(postSavePath));
  const badgeHash = sha(readFileSync(badgePath));

  console.log(`=== phase 2: reload + signature-sync LOAD (${browserName}) ===`);
  await bootToRunning(page, isoPath, { resetSave: false });
  await runTitleToProfile(page);
  await runProfile(page);
  await runPlayUntilCrimeComputer(page);
  await runLoad(page);

  const loadedPath = join(outDir, `web-save-load-loaded-${browserName}.png`);
  await captureNow(page, loadedPath, "loaded");
  const loadedHash = sha(readFileSync(loadedPath));
  if (loadedHash === badgeHash) {
    throw new Error("loaded screenshot hash matches fresh badge (load may not have restored state)");
  }

  const opfsFinal = await listOpfsSaves(page);
  assertSavgame2(opfsFinal);

  // Firefox/WebKit emit noisy services.settings / wallpaper / favicon errors in headless.
  const badConsole = consoleErrors.filter(
    (t) =>
      !/favicon|Failed to load resource|services\.settings|wallpaper|nimbus|EmptyDatabaseError|Could not find .* in cache|Unable to find the attachment|Could not download new icon|ServerInfoError|JSON\.parse: unexpected end/i.test(
        t,
      ),
  );
  if (pageErrors.length) throw new Error(`page errors: ${pageErrors.join(" | ")}`);
  if (badConsole.length) throw new Error(`console errors: ${badConsole.join(" | ")}`);

  console.log(
    JSON.stringify(
      {
        browser: browserName,
        port,
        mode: "signature-sync",
        opfsAfterSave,
        opfsFinal,
        hashes: { badge: badgeHash, postSave: postSaveHash, loaded: loadedHash },
        frames: await page.evaluate(() => window.__detect?.frames),
        ddraw: await page.evaluate(() => globalThis.__detectiveModule?.__ddrawFrameCounter ?? 0),
      },
      null,
      2,
    ),
  );

  await ctx.close();
  exitCode = 0;
} catch (err) {
  console.error(err);
  exitCode = 1;
} finally {
  stopVite();
}
process.exit(exitCode);
