#!/usr/bin/env node
// Headless presentation FPS gate (20 s per scene, Chrome software GL caveat).
//   node tools/web-fps.mjs [--browser chrome] [--port 5174] [--seconds 20]
import { spawn } from "node:child_process";
import { existsSync, mkdirSync } from "node:fs";
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
const browserName = browserArgs(args).browser;
const measureSec = Number(opt("seconds", "20"));
const isoArg = opt("iso", "DETECTV1_1.ISO");

const SCENES = [
  { name: "title", setup: async () => {} },
  {
    name: "hub",
    setup: async (page) => {
      await waitFrames(page, 200);
      const pt = await gamePoint(page, 200, 400);
      await page.mouse.move(pt.x, pt.y);
      await page.mouse.down({ button: "left" });
      await page.mouse.up({ button: "left" });
      await waitFrames(page, 260);
    },
  },
  {
    name: "hub-animated",
    setup: async (page) => {
      await waitFrames(page, 200);
      const pt = await gamePoint(page, 200, 400);
      await page.mouse.move(pt.x, pt.y);
      await page.mouse.click(pt.x, pt.y);
      await waitFrames(page, 300);
    },
  },
];

const GAME_W = 640;
const GAME_H = 480;


async function runEvents(page, events) {
  for (const ev of events) {
    await waitFrames(page, ev.frame);
    if (ev.key) {
      await page.keyboard.press(ev.key, { delay: 120 });
      continue;
    }
    const pt = await gamePoint(page, ev.x, ev.y);
    await page.mouse.move(pt.x, pt.y);
    if (ev.click) {
      await page.mouse.down({ button: "left" });
      await page.mouse.up({ button: "left" });
    }
  }
}

const ENTRY = [
  { frame: 500, x: 480, y: 400, click: true },
  { frame: 605, x: 140, y: 228, click: true },
  ...[0, 1, 2, 3, 4, 5, 6, 7, 8, 9].map((i) => ({ frame: 620 + 10 * i, key: "Backspace" })),
  { frame: 710, key: "a" },
  { frame: 740, key: "b" },
  { frame: 805, x: 250, y: 207, click: true },
  { frame: 905, x: 244, y: 440, click: true },
  { frame: 1105, x: 244, y: 440, click: true },
];
const PLAY = [[1400, 320, 240], [1500, 320, 240], [1600, 320, 240], [1700, 320, 240], [1800, 320, 240], [1900, 400, 400], [2100, 320, 240], [2200, 320, 240], [2300, 300, 380], [2400, 420, 360], [2500, 360, 320], [2600, 500, 350], [2700, 180, 280], [2800, 580, 400], [3000, 320, 240]].map(([frame, x, y]) => ({ frame, x, y, click: true }));

SCENES.push(
  // Profile (name entry) screen: a menu screen with the game's own cursor.
  { name: "profile", setup: async (page) => { await runEvents(page, ENTRY.slice(0, 1)); await waitFrames(page, 590); } },
  { name: "cutscene", setup: async (page) => { await runEvents(page, ENTRY.slice(1)); await waitFrames(page, 1350); } },
  { name: "in-game", setup: async (page) => { await runEvents(page, PLAY); await waitFrames(page, 3200); } },
  // Barbie walking while the area scrolls: measured by walkSmoothness() below.
  { name: "walk", setup: async () => {}, walk: true },
);

// Hold the mouse at a screen edge so Barbie walks and the area scrolls, and measure how far the
// picture moves between consecutive animation frames (best horizontal match of two pixel rows).
// Motion drawn only at the game's 16 Hz logic rate shows up as mostly-zero shifts with jumps.
async function walkSmoothness(page) {
  const result = { moving: 0, frames: 0, zeroGaps: 0, meanShift: 0, shiftSd: 0 };
  for (const edgeX of [637, 2, 637, 2]) {
    const pt = await gamePoint(page, edgeX, 240);
    await page.mouse.move(pt.x, pt.y);
    await page.mouse.down({ button: "left" });
    const shifts = await page.evaluate(
      () =>
        new Promise((resolve) => {
          const c = document.getElementById("canvas");
          const ctx = c.getContext("2d", { willReadFrequently: true });
          const rows = [60, 120, 180];
          let prev = null, t0 = 0;
          const out = [];
          const grab = () => rows.map((y) => ctx.getImageData(0, Math.round((y * c.height) / 480), c.width, 1).data);
          const step = (t) => {
            if (!t0) t0 = t;
            const cur = grab();
            if (prev) {
              let best = 0, bestErr = Infinity;
              const scale = c.width / 640;
              for (let s = -24; s <= 24; s++) {
                const off = Math.round(s * scale);
                let err = 0;
                for (let r = 0; r < rows.length; r++) {
                  const a = prev[r], b = cur[r];
                  for (let x = 40 * scale; x < c.width - 40 * scale; x += 2) {
                    const i = x * 4, j = (x + off) * 4;
                    err += Math.abs(a[i] - b[j]) + Math.abs(a[i + 1] - b[j + 1]) + Math.abs(a[i + 2] - b[j + 2]);
                  }
                }
                if (err < bestErr) (bestErr = err), (best = s);
              }
              out.push(best);
            }
            prev = cur;
            if (t - t0 < 1500) requestAnimationFrame(step);
            else resolve(out);
          };
          requestAnimationFrame(step);
        }),
    );
    await page.mouse.up({ button: "left" });
    await page.waitForTimeout(400);
    // Only the stretch where the picture is scrolling counts (from the first to the last move).
    const first = shifts.findIndex((s) => s !== 0);
    let last = -1;
    for (let i = shifts.length - 1; i >= 0; i--) if (shifts[i] !== 0) { last = i; break; }
    if (first < 0) continue;
    const span = shifts.slice(first, last + 1);
    result.frames += span.length;
    for (const s of span) if (s === 0) result.zeroGaps++;
    const moving = span.filter((s) => s !== 0).map(Math.abs);
    result.moving += moving.length;
    result.shifts = (result.shifts ?? []).concat(span);
  }
  const mv = (result.shifts ?? []).filter((s) => s !== 0).map(Math.abs);
  if (mv.length) {
    result.meanShift = mv.reduce((a, b) => a + b, 0) / mv.length;
    result.shiftSd = Math.sqrt(mv.reduce((a, b) => a + (b - result.meanShift) ** 2, 0) / mv.length);
  }
  return result;
}

function resolveIso(path) {
  const candidates = [path, join(ROOT, path)];
  for (const c of candidates) if (existsSync(c)) return resolve(c);
  throw new Error(`disc image not found: ${path}`);
}

const portFree = (p) =>
  new Promise((resolvePort) => {
    const s = net.createServer();
    s.once("error", () => resolvePort(false));
    s.listen(p, "127.0.0.1", () => s.close(() => resolvePort(true)));
  });

let port = Number(opt("port", "5174"));
if (!(await portFree(port))) {
  for (let p = 5174; p < 5200; p++) {
    if (await portFree(p)) {
      port = p;
      break;
    }
  }
}

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

async function waitFrames(page, target, timeout = 180_000) {
  await page.waitForFunction((f) => (window.__detect?.frames ?? 0) >= f, target, { timeout, polling: 100 });
}

async function bootGame(page, isoPath) {
  await page.goto(`http://localhost:${port}/?noCapture`);
  await page.waitForLoadState("domcontentloaded");
  await page.waitForFunction(
    () => ["need-install", "ready", "error"].includes(window.__detect?.state ?? ""),
    null,
    { timeout: 60_000 },
  );
  const boot = await page.evaluate(() => window.__detect?.state);
  if (boot === "error") throw new Error("shell error on load");
  if (boot === "need-install") {
    await page.setInputFiles("#iso", isoPath);
    await page.waitForFunction(() => window.__detect?.state === "ready", null, { timeout: 900_000, polling: 500 });
  }
  await page.evaluate(() => {
    try {
      localStorage.setItem("detect.controlsSeen", "1");
    } catch {
      // ignore
    }
  });
  await page.click("#play");
  await page.waitForFunction(() => window.__detect?.state === "running", null, { timeout: Number(process.env.DETECT_BOOT_TIMEOUT_MS ?? 180_000) });
  await page.waitForFunction(() => (window.__detect?.frames ?? 0) > 60, null, { timeout: Number(process.env.DETECT_BOOT_TIMEOUT_MS ?? 180_000) });
}

// Module.__gameStats = [flips, primaryBlts, cursorPolls, sleeps, locks] (runtime/src/web/web_main.cpp).
const gameStats = (page) =>
  page.evaluate(() => {
    const g = globalThis.__detectiveModule?.__gameStats ?? [0, 0, 0, 0, 0];
    return { polls: g[2], locks: g[4], t: performance.now() };
  });

async function measureScene(page, scene) {
  await page.evaluate(() => window.__perf?.reset());
  await scene.setup(page);
  if (scene.walk) {
    const w = await walkSmoothness(page);
    console.log(
      `walk: ${w.frames} frames while scrolling, ${w.zeroGaps} without motion (${((100 * w.zeroGaps) / Math.max(1, w.frames)).toFixed(0)}%), ` +
        `shift per frame ${w.meanShift.toFixed(1)} px (sd ${w.shiftSd.toFixed(1)})`,
    );
    if (process.env.DETECT_WALK_SHIFTS) console.log(`walk shifts: ${(w.shifts ?? []).join(" ")}`);
  }
  const a = await gameStats(page);
  await new Promise((r) => setTimeout(r, measureSec * 1000));
  const b = await gameStats(page);
  const snap = await page.evaluate(() => window.__perf?.snapshot());
  // What the player sees while moving the mouse: circle the cursor every animation frame for 4 s
  // and count canvas updates whose content changed (the game draws its own cursor into its frames).
  const visibleFps = await page.evaluate(
    () =>
      new Promise((resolve) => {
        const mod = globalThis.__detectiveModule;
        const c = document.getElementById("canvas");
        const ctx = c.getContext("2d");
        let last = "", changes = 0, t0 = 0, px = 320, py = 240;
        const step = (t) => {
          if (!t0) t0 = t;
          const a = (t - t0) / 400;
          const x = Math.round(320 + 120 * Math.cos(a)), y = Math.round(260 + 80 * Math.sin(a));
          mod?._web_mouse_move(x, y, x - px, y - py);
          px = x;
          py = y;
          const d = ctx.getImageData(0, 0, c.width, c.height).data;
          let h = 0;
          for (let i = 0; i < d.length; i += 4 * 7) h = (h * 31 + d[i] + d[i + 1] * 3) | 0;
          const key = String(h);
          if (key !== last) changes++, (last = key);
          if (t - t0 < 4000) requestAnimationFrame(step);
          else resolve(changes / ((t - t0) / 1000));
        };
        requestAnimationFrame(step);
      }),
  );
  if (snap) snap.visibleFps = visibleFps;
  if (process.env.DETECT_SHOTS && scene.name === "profile") {
    for (const [i, [x, y]] of [[150, 150], [450, 330], [300, 250]].entries()) {
      await page.evaluate(([x, y]) => globalThis.__detectiveModule?._web_mouse_move(x, y, 0, 0), [x, y]);
      for (const ms of [50, 250, 1000]) {
        await page.waitForTimeout(ms === 50 ? 50 : ms - (ms === 250 ? 50 : 250));
        await page.locator("#canvas").screenshot({ path: `work/shot-${i}-${ms}.png` });
      }
    }
  }
  const dt = (b.t - a.t) / 1000;
  // The game's own frame rate: it locks its frame buffer once per rendered frame.
  if (snap) snap.gameFps = (b.locks - a.locks) / dt;
  if (snap) snap.cursorPolls = (b.polls - a.polls) / dt;
  return snap;
}

function fmtRow(cols) {
  return cols.map((c) => String(c).padEnd(14)).join("");
}

const isoPath = resolveIso(isoArg);
mkdirSync(join(ROOT, "work"), { recursive: true });

const vite = spawn("npx", ["vite", "--config", "vite.config.ts", "--port", String(port), "--strictPort"], {
  cwd: join(ROOT, "web"),
  stdio: ["ignore", "pipe", "pipe"],
  env: { ...process.env, DETECTIVE_ISO: isoPath },
});

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
  throw new Error("vite did not start");
}

let exitCode = 1;
const rows = [];
try {
  await waitForServer();
  const profile = join(ROOT, `work/web-fps-profile-${browserName}-${port}`);
  const ctx = await launch({ ...browserArgs(args), port, profile });
  const page =
    ctx.pages().length === 1 && ctx.pages()[0].url() === "about:blank" ? ctx.pages()[0] : await ctx.newPage();
  if (process.env.DETECT_CPU_THROTTLE && browserName === "chrome") {
    // Slow-device approximation: CDP CPU throttling (x-times slower), applies to the page and its workers best-effort.
    const cdp = await ctx.newCDPSession(page);
    await cdp.send("Emulation.setCPUThrottlingRate", { rate: Number(process.env.DETECT_CPU_THROTTLE) });
    console.log(`cpu throttle x${process.env.DETECT_CPU_THROTTLE}`);
  }
  const errors = [];
  page.on("pageerror", (e) => errors.push(e.message));

  await bootGame(page, isoPath);
  if (!(await page.evaluate(() => crossOriginIsolated))) throw new Error("missing COOP/COEP");

  console.log(`web-fps: ${measureSec}s per scene, browser=${browserName} (headless ≈ software GL)`);
  console.log(fmtRow(["scene", "presentFps", "rafFps", "p95Ms", "composeMs", "uploadMs", "visibleFps", "cursorPolls/s", "locks/s"]));

  for (const scene of SCENES) {
    const snap = await measureScene(page, scene);
    if (!snap) throw new Error("window.__perf missing");
    const pass = snap.presentFps >= 58 && snap.rafP95Ms <= 20;
    rows.push({ scene: scene.name, ...snap, pass });
    console.log(
      fmtRow([
        scene.name,
        snap.presentFps.toFixed(1),
        snap.rafFps.toFixed(1),
        snap.rafP95Ms.toFixed(2),
        snap.composeMs.toFixed(2),
        snap.uploadMs.toFixed(2),
        snap.visibleFps.toFixed(1),
        snap.cursorPolls.toFixed(1),
        snap.gameFps.toFixed(0),
      ]) + (pass ? "" : " FAIL"),
    );
    if (!pass) throw new Error(`${scene.name}: presentFps=${snap.presentFps.toFixed(1)} p95=${snap.rafP95Ms.toFixed(2)}ms`);
  }

  if (errors.length) throw new Error(errors.join(" | "));
  await ctx.close();
  exitCode = 0;
} catch (err) {
  console.error(err);
} finally {
  vite.kill("SIGTERM");
}
process.exit(exitCode);
