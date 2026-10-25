#!/usr/bin/env node
// Headless browser smoke test for the web shell (local development only).
//   node tools/web-smoke.mjs [--install-only] [--iso DETECTV1_1.ISO] [--port 5174] [--seconds 120]
//                            [--browser chrome|firefox|webkit] [--angle vulkan|gl|swiftshader] [--headed]
// Starts the Vite dev server, opens Chromium via playwright-core (see tools/browsers.mjs),
// installs from the chosen disc image through the file input, optionally clicks Play, and writes
// work/web-smoke-w1.png plus JSON stats (non-black pixel count and distinct colours on the canvas).
import { spawn } from "node:child_process";
import { existsSync, mkdirSync, writeFileSync } from "node:fs";
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
const installOnly = args.includes("--install-only");
const seconds = Number(opt("seconds", installOnly ? "900" : "900"));
const isoArg = opt("iso", "DETECTV1_1.ISO");

const portFree = (p) =>
  new Promise((resolve) => {
    const s = net.createServer();
    s.once("error", () => resolve(false));
    s.listen(p, "127.0.0.1", () => s.close(() => resolve(true)));
  });

let port = Number(opt("port", "5174"));
if (!(await portFree(port))) {
  for (let p = 5174; p < 5200; p++) {
    if (await portFree(p)) {
      console.log(`port ${port} busy; using ${p} for this smoke run`);
      port = p;
      break;
    }
  }
}

function resolveIso(path) {
  const candidates = [
    path,
    join(ROOT, path),
    join(ROOT, "..", "..", "..", "detective-barbie-web-port", path),
  ];
  for (const c of candidates) if (existsSync(c)) return resolve(c);
  throw new Error(`disc image not found: ${path} (tried ${candidates.join(", ")})`);
}

const isoPath = resolveIso(isoArg);
const outDir = join(ROOT, "work");
mkdirSync(outDir, { recursive: true });
const shotPath = join(outDir, "web-smoke-w1.png");

const vite = spawn("npx", ["vite", "--config", "vite.config.ts", "--port", port, "--strictPort"], {
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

function canvasStats(page) {
  return page.evaluate(() => {
    const c = document.getElementById("canvas");
    if (!c || !(c instanceof HTMLCanvasElement) || c.width < 2) return { nonBlackPixels: 0, distinctColors: 0 };
    const ctx = c.getContext("2d");
    if (!ctx) return { nonBlackPixels: 0, distinctColors: 0 };
    const { data, width, height } = ctx.getImageData(0, 0, c.width, c.height);
    let nonBlackPixels = 0;
    const colors = new Set();
    for (let i = 0; i < data.length; i += 4) {
      const r = data[i];
      const g = data[i + 1];
      const b = data[i + 2];
      if (r > 8 || g > 8 || b > 8) nonBlackPixels++;
      colors.add((r << 16) | (g << 8) | b);
    }
    return { nonBlackPixels, distinctColors: colors.size };
  });
}

let exitCode = 1;
try {
  await waitForServer();
  const ctx = await launch({ ...browserArgs(args), port, profile: join(ROOT, `work/web-smoke-profile-${port}`) });
  const page = (ctx.pages().length === 1 && ctx.pages()[0].url() === "about:blank")
    ? ctx.pages()[0]
    : (await ctx.newPage());
  const t0 = Date.now();
  const stamp = () => ((Date.now() - t0) / 1000).toFixed(1).padStart(6);
  page.on("console", (m) => console.log(`${stamp()} console.${m.type()}: ${m.text()}`));
  page.on("pageerror", (e) => console.log(`${stamp()} pageerror: ${e.message}\n${e.stack ?? ""}`));

  await page.goto(`http://localhost:${port}/`);
  await page.waitForLoadState("domcontentloaded");
  await page.waitForFunction(
    () => ["need-install", "ready", "error"].includes(window.__detect?.state ?? ""),
    null,
    { timeout: 60_000 },
  );
  const boot = await page.evaluate(() => ({ state: window.__detect?.state, log: document.getElementById("log")?.textContent ?? "" }));
  if (boot.state === "error") throw new Error(`shell error: ${boot.log}`);

  const needInstall = boot.state === "need-install";
  if (needInstall) {
    await page.setInputFiles("#iso", isoPath);
    await page.waitForFunction(
      () => {
        const p = document.getElementById("progress");
        const max = p && !p.hidden ? p.max : 0;
        const val = p && !p.hidden ? p.value : 0;
        return window.__detect?.state === "ready" || (max > 0 && val >= max);
      },
      null,
      { timeout: 900_000, polling: 500 },
    );
    await page.waitForFunction(() => window.__detect?.state === "ready", null, { timeout: 120_000 });
  }

  const opfs = await page.evaluate(async () => {
    let root;
    try {
      root = await (await navigator.storage.getDirectory()).getDirectoryHandle("detect");
    } catch {
      return { files: 0, bdetect: false, dataDir: false };
    }
    const names = [];
    for await (const [n, h] of root.entries()) if (h.kind === "file") names.push(n);
    let bdetect = false;
    try {
      await root.getFileHandle("BDETECT.EXE");
      bdetect = true;
    } catch {}
    let dataDir = false;
    try {
      await root.getDirectoryHandle("DATA");
      dataDir = true;
    } catch {}
    return { files: names.length, bdetect, dataDir };
  });
  if (!opfs.bdetect || !opfs.dataDir) throw new Error(`OPFS install incomplete: ${JSON.stringify(opfs)}`);

  let nonBlackPixels = 0;
  let distinctColors = 0;
  let frames = 0;
  let state = "ready";

  if (!installOnly) {
    await page.evaluate(() => {
      try {
        localStorage.setItem("detect.controlsSeen", "1");
      } catch {
        // ignore
      }
    });
    await page.click("#play");
    const deadline = Date.now() + seconds * 1000;
    while (Date.now() < deadline) {
      await page.waitForTimeout(2000);
      const s = await page.evaluate(() => ({
        state: window.__detect?.state ?? "loading",
        frames: window.__detect?.frames ?? 0,
      }));
      state = s.state;
      frames = s.frames;
      const pix = await canvasStats(page);
      console.log(
        `${stamp()} state: ${state} frames: ${frames} nonBlack: ${pix.nonBlackPixels} colors: ${pix.distinctColors}`,
      );
      if (state === "error") break;
      if (state === "running" && frames > 30 && pix.distinctColors > 50) break;
    }
    const png = await page.locator("#canvas").screenshot({ type: "png" });
    writeFileSync(shotPath, png);
    const pix = await canvasStats(page);
    nonBlackPixels = pix.nonBlackPixels;
    distinctColors = pix.distinctColors;
    console.log(`screenshot: ${shotPath}`);
    console.log(`nonBlackPixels: ${nonBlackPixels}`);
    console.log(`distinctColors: ${distinctColors}`);
  }

  const stats = {
    installOnly,
    iso: isoPath,
    opfs,
    state,
    frames,
    screenshot: installOnly ? null : shotPath,
    nonBlackPixels,
    distinctColors,
    seconds: (Date.now() - t0) / 1000,
  };
  console.log(JSON.stringify(stats, null, 2));
  await ctx.close();
  exitCode =
    opfs.bdetect &&
    opfs.dataDir &&
    (installOnly || (state !== "error" && frames > 30 && distinctColors > 50))
      ? 0
      : 1;
} catch (err) {
  console.error(err);
  exitCode = 1;
} finally {
  stopVite();
}
process.exit(exitCode);
