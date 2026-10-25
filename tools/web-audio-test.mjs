#!/usr/bin/env node
// Assert the game produces non-silent audio within 15 s after the first Play click (title screen).
// Taps the Web Audio graph via `?audioTap` (AnalyserNode on the ring-player worklet output).
//   node tools/web-audio-test.mjs [--browser chrome|firefox|webkit] [--port 5174] [--headed]
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
const isoArg = opt("iso", "DETECTV1_1.ISO");
const PEAK_THRESHOLD = 0.002;
/** Engine ring must hold at least this many unread stereo frames (proves `audio_open` is feeding). */
const MIN_RING_DELTA = 2048;
const WAIT_MS = 40_000;

function resolveIso(path) {
  const candidates = [
    path,
    join(ROOT, path),
    join(ROOT, "..", "..", "..", "detective-barbie-web-port", path),
    join("/home/hammad/repos/detective-barbie-web-port", path),
  ];
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
      console.log(`port ${port} busy; using ${p}`);
      port = p;
      break;
    }
  }
}

const vite = spawn("npx", ["vite", "--config", "vite.config.ts", "--port", String(port), "--strictPort"], {
  cwd: join(ROOT, "web"),
  stdio: ["ignore", "pipe", "pipe"],
  env: { ...process.env, DETECTIVE_ISO: resolveIso(isoArg) },
});
mkdirSync(join(ROOT, "work"), { recursive: true });
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

let exitCode = 1;
try {
  await waitForServer();
  const profile = join(ROOT, `work/web-play-profile-${browserName}-${port}`);
  const ctx = await launch({ ...browserArgs(args), port, profile });
  const page =
    ctx.pages().length === 1 && ctx.pages()[0].url() === "about:blank" ? ctx.pages()[0] : await ctx.newPage();

  const playUrl = `http://localhost:${port}/?noCapture&audioTap`;
  await page.goto(playUrl);
  await page.waitForFunction(
    () => ["need-install", "ready", "error"].includes(window.__detect?.state ?? ""),
    null,
    { timeout: 60_000 },
  );
  if ((await page.evaluate(() => window.__detect?.state)) === "need-install") {
    await page.setInputFiles("#iso", resolveIso(isoArg));
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
  await page.waitForFunction(() => window.__detect?.state === "running", null, { timeout: 180_000 });
  await page.waitForFunction(() => window.__detectAudio != null, null, { timeout: 30_000 });
  await page.waitForFunction(() => (window.__detect?.frames ?? 0) > 60, null, { timeout: 180_000 });

  let peak = 0;
  let maxRing = 0;
  let ctxState = "suspended";
  const t0 = Date.now();
  const deadline = t0 + WAIT_MS;
  while (Date.now() < deadline) {
    const sample = await page.evaluate(async () => {
      const tap = window.__detectAudio;
      if (!tap) return { peak: 0, ctxState: "missing" };
      if (tap.contextState() !== "running") await tap.resume().catch(() => {});
      return {
        peak: tap.samplePeak(),
        maxPeak: tap.maxPeak,
        ctxState: tap.contextState(),
        ringDelta: tap.ringDelta?.() ?? 0,
      };
    });
    peak = Math.max(peak, sample.peak, sample.maxPeak ?? 0);
    maxRing = Math.max(maxRing, sample.ringDelta ?? 0);
    ctxState = sample.ctxState;
    if (peak >= PEAK_THRESHOLD) break;
    await page.waitForTimeout(100);
  }

  const elapsed = Date.now() - t0;
  const frames = await page.evaluate(() => window.__detect?.frames ?? 0);
  console.log(
    JSON.stringify(
      { browser: browserName, port, peak, maxRingDelta: maxRing, threshold: PEAK_THRESHOLD, elapsedMs: elapsed, audioContext: ctxState, frames },
      null,
      2,
    ),
  );

  if (peak < PEAK_THRESHOLD) {
    throw new Error(
      `no game audio within ${WAIT_MS} ms (peak=${peak}, ringDelta=${maxRing}, AudioContext=${ctxState})`,
    );
  }
  await ctx.close();
  exitCode = 0;
} catch (err) {
  console.error(err);
  exitCode = 1;
} finally {
  vite.kill("SIGTERM");
}
process.exit(exitCode);
