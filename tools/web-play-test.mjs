#!/usr/bin/env node
// Scripted in-browser play-through (same click map as native DETECTIVE_INPUT in docs/JOURNAL.md).
//   node tools/web-play-test.mjs [--iso DETECTV1_1.ISO] [--port 5174] [--browser chrome|firefox|webkit]
//                            [--angle vulkan|gl|swiftshader] [--headed] [--skip-saves] [--quota-test]
// Starts Vite, installs if needed, clicks Play, drives the title → hub → games → detective path with
// page.mouse at canvas-scaled 640×480 coordinates, writes work/web-play-*.png (≥4 distinct screens),
// and checks colours, screenshot hashes, and console/page errors. Optional saves backup/restore UI test.
import { createHash } from "node:crypto";
import { execFileSync } from "node:child_process";
import { spawn } from "node:child_process";
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
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
const skipSaves = args.includes("--skip-saves");
const quotaTest = args.includes("--quota-test");
const isoArg = opt("iso", "DETECTV1_1.ISO");
const browserName = browserArgs(args).browser;

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

const TIMELINE = [
  { frame: 120, type: "shot", label: "title" },
  { frame: 200, type: "move", x: 200, y: 400 },
  { frame: 205, type: "click", x: 200, y: 400 },
  { frame: 250, type: "shot", label: "hub" },
  { frame: 450, type: "shot", label: "games" },
  { frame: 500, type: "move", x: 480, y: 400 },
  { frame: 505, type: "click", x: 480, y: 400 },
  { frame: 590, type: "shot", label: "profile" },
  { frame: 600, type: "move", x: 140, y: 228 },
  { frame: 605, type: "click", x: 140, y: 228 },
  { frame: 610, type: "key", key: "Backspace" },
  { frame: 620, type: "key", key: "Backspace" },
  { frame: 630, type: "key", key: "Backspace" },
  { frame: 640, type: "key", key: "Backspace" },
  { frame: 650, type: "key", key: "Backspace" },
  { frame: 660, type: "key", key: "Backspace" },
  { frame: 670, type: "key", key: "Backspace" },
  { frame: 680, type: "key", key: "Backspace" },
  { frame: 690, type: "key", key: "Backspace" },
  { frame: 700, type: "key", key: "Backspace" },
  { frame: 710, type: "key", key: "a" },
  { frame: 740, type: "key", key: "b" },
  { frame: 800, type: "move", x: 250, y: 207 },
  { frame: 805, type: "click", x: 250, y: 207 },
  { frame: 850, type: "shot", label: "typed" },
  { frame: 900, type: "move", x: 244, y: 440 },
  { frame: 905, type: "click", x: 244, y: 440 },
  { frame: 1100, type: "move", x: 244, y: 440 },
  { frame: 1105, type: "click", x: 244, y: 440 },
  { frame: 1300, type: "shot", label: "badge" },
  { frame: 1400, type: "shot", label: "movie1" },
  { frame: 1500, type: "shot", label: "movie2" },
  { frame: 1650, type: "shot", label: "movie3" },
];

function canvasStats(page) {
  return page.evaluate(() => {
    const c = document.getElementById("canvas");
    if (!c || !(c instanceof HTMLCanvasElement) || c.width < 2) return { distinctColors: 0 };
    const ctx = c.getContext("2d");
    if (!ctx) return { distinctColors: 0 };
    const { data } = ctx.getImageData(0, 0, c.width, c.height);
    const colors = new Set();
    for (let i = 0; i < data.length; i += 4) colors.add((data[i] << 16) | (data[i + 1] << 8) | data[i + 2]);
    return { distinctColors: colors.size };
  });
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

async function runTimeline(page, shots, lastHashRef) {
  for (const ev of TIMELINE) {
    await waitFrames(page, ev.frame);
    if (ev.type === "shot") {
      const path = join(outDir, `web-play-${ev.label}.png`);
      const png = await page.locator("#canvas").screenshot({ type: "png" });
      writeFileSync(path, png);
      const hash = sha(png);
      const { distinctColors } = await canvasStats(page);
      console.log(`capture ${ev.label} @${ev.frame}: ${path} colors=${distinctColors} hash=${hash}`);
      if (distinctColors <= 50) throw new Error(`${ev.label}: expected >50 distinct colors, got ${distinctColors}`);
      if (hash === lastHashRef.value && lastHashRef.value) throw new Error(`${ev.label}: screenshot hash matches previous`);
      lastHashRef.value = hash;
      shots.push({ label: ev.label, frame: ev.frame, path, distinctColors, hash });
      continue;
    }
    if (ev.type === "key") {
      await page.keyboard.press(ev.key, { delay: 120 });
      continue;
    }
    const pt = await gamePoint(page, ev.x, ev.y);
    await page.mouse.move(pt.x, pt.y);
    if (ev.type === "click") {
      await page.mouse.down({ button: "left" });
      await page.mouse.up({ button: "left" });
    }
  }
}

const sha = (buf) => createHash("sha256").update(buf).digest("hex").slice(0, 16);

let exitCode = 1;
try {
  await waitForServer();
  const profile = join(ROOT, `work/web-play-profile-${browserName}-${port}`);
  const ctx = await launch({ ...browserArgs(args), port, profile, extra: { acceptDownloads: true } });
  const page = (ctx.pages().length === 1 && ctx.pages()[0].url() === "about:blank")
    ? ctx.pages()[0]
    : (await ctx.newPage());

  const consoleErrors = [];
  const pageErrors = [];
  page.on("console", (m) => {
    if (m.type() === "error") consoleErrors.push(m.text());
  });
  page.on("pageerror", (e) => pageErrors.push(e.message));

  const playUrl = `http://localhost:${port}/?noCapture&timing`;
  await page.goto(playUrl);
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
  const playClick0 = Date.now();
  await page.click("#play");
  await page.waitForFunction(() => window.__detect?.state === "running", null, { timeout: 180_000 });
  await page.waitForFunction(() => (window.__detect?.frames ?? 0) > 30, null, { timeout: 180_000 });
  let playStartFirstMs = await page.evaluate(() => window.__detect?.playStartMs);
  if (playStartFirstMs == null) playStartFirstMs = Date.now() - playClick0;
  console.log(`play start (1st, OPFS cached wasm): ${playStartFirstMs} ms`);

  const isolated = await page.evaluate(() => crossOriginIsolated);
  if (!isolated) throw new Error("page is not cross-origin isolated (missing COOP/COEP)");

  const shots = [];
  const lastHashRef = { value: "" };
  await runTimeline(page, shots, lastHashRef);

  const uniqueHashes = new Set(shots.map((s) => s.hash));
  if (uniqueHashes.size < 4) throw new Error(`need 4 distinct screenshot hashes among timed captures, got ${uniqueHashes.size}`);

  if (pageErrors.length) throw new Error(`page errors: ${pageErrors.join(" | ")}`);
  const badConsole = consoleErrors.filter((t) => !/favicon|Failed to load resource.*favicon/i.test(t));
  if (badConsole.length) throw new Error(`console errors: ${badConsole.join(" | ")}`);

  let playStartSecondMs = null;
  if (!skipSaves) {
    await page.reload();
    await page.waitForFunction(() => window.__detect?.state === "ready", null, { timeout: 120_000 });
    const bytes = Array.from({ length: 30 }, (_, i) => 65 + (i % 26)); // synthetic save payload (the disc stubs are not installed)
    await page.evaluate(async (data) => {
      const root = await navigator.storage.getDirectory();
      const saves = await root.getDirectoryHandle("saves", { create: true });
      const w = await (await saves.getFileHandle("SAVGAME0.AR1", { create: true })).createWritable();
      await w.write(new Uint8Array(data));
      await w.close();
    }, bytes);
    await page.reload();
    await page.waitForFunction(() => window.__detect?.state === "ready", null, { timeout: 120_000 });

    const [download] = await Promise.all([page.waitForEvent("download"), page.click("#save-export")]);
    const tmp = join(outDir, "web-play-saves");
    rmSync(tmp, { recursive: true, force: true });
    mkdirSync(tmp, { recursive: true });
    const zipPathEarly = join(tmp, download.suggestedFilename());
    await download.saveAs(zipPathEarly);
    console.log(`saves UI: backed up to ${zipPathEarly}`);

    await page.reload();
    await page.waitForFunction(() => window.__detect?.state === "ready", null, { timeout: 120_000 });
    await page.evaluate(() => {
      try {
        localStorage.setItem("detect.controlsSeen", "1");
      } catch {
        // ignore
      }
    });
    const playClick1 = Date.now();
    await page.click("#play");
    await page.waitForFunction(() => window.__detect?.state === "running", null, { timeout: 180_000 });
    playStartSecondMs = await page.evaluate(() => window.__detect?.playStartMs);
    if (playStartSecondMs == null) playStartSecondMs = Date.now() - playClick1;
    console.log(`play start (2nd after reload, cached translate): ${playStartSecondMs} ms`);

    await page.reload();
    await page.waitForFunction(() => window.__detect?.state === "ready", null, { timeout: 120_000 });
    const beforeReload = await page.evaluate(async () => {
      const saves = await (await navigator.storage.getDirectory()).getDirectoryHandle("saves");
      const f = await (await saves.getFileHandle("SAVGAME0.AR1")).getFile();
      return f.size;
    });
    await page.reload();
    await page.waitForFunction(() => window.__detect?.state === "ready", null, { timeout: 120_000 });
    const afterReload = await page.evaluate(async () => {
      const saves = await (await navigator.storage.getDirectory()).getDirectoryHandle("saves");
      const f = await (await saves.getFileHandle("SAVGAME0.AR1")).getFile();
      return f.size;
    });
    if (beforeReload !== afterReload || beforeReload !== bytes.length) {
      throw new Error(`save did not survive reload (${beforeReload} → ${afterReload})`);
    }
    console.log(`saves OPFS: SAVGAME0.AR1 ${afterReload} bytes survives reload`);

    const zipPath = zipPathEarly;
    await page.evaluate(async () => {
      const saves = await (await navigator.storage.getDirectory()).getDirectoryHandle("saves");
      await saves.removeEntry("SAVGAME0.AR1");
    });
    await page.reload();
    await page.waitForFunction(() => window.__detect?.state === "ready", null, { timeout: 120_000 });
    await page.setInputFiles("#save-import", zipPath);
    // The overwrite confirmation only appears when the saves directory already holds files.
    const confirmShown = await page.locator("#save-confirm").waitFor({ state: "visible", timeout: 5_000 }).then(() => true, () => false);
    if (confirmShown) await page.click("#save-confirm-yes");
    await page.waitForFunction(
      () => /Restored/i.test(document.getElementById("save-status")?.textContent ?? ""),
      null,
      { timeout: 15_000 },
    );
    const restored = await page.evaluate(async () => {
      const saves = await (await navigator.storage.getDirectory()).getDirectoryHandle("saves");
      const f = await (await saves.getFileHandle("SAVGAME0.AR1")).getFile();
      return [...new Uint8Array(await f.arrayBuffer())];
    });
    const listing = JSON.parse(
      execFileSync("python3", [
        "-I",
        "-c",
        "import sys,zipfile,hashlib,json;z=zipfile.ZipFile(sys.argv[1]);print(json.dumps({i.filename:hashlib.sha256(z.read(i)).hexdigest()[:16] for i in z.infolist()}))",
        zipPath,
      ]).toString(),
    );
    const zipHash = listing["SAVGAME0.AR1"] ?? listing["savgame0.ar1"];
    if (sha(Buffer.from(restored)) !== zipHash) throw new Error("restore from UI did not match backup ZIP");
    console.log("saves UI: backup and restore OK");
  }

  console.log(
    JSON.stringify(
      {
        browser: browserName,
        port,
        iso: isoPath,
        captures: shots,
        playStartMs: { first: playStartFirstMs, second: playStartSecondMs },
        frames: await page.evaluate(() => window.__detect?.frames),
      },
      null,
      2,
    ),
  );

  if (quotaTest) {
    const quotaProfile = join(ROOT, `work/web-quota-profile-${browserName}-${port}`);
    rmSync(quotaProfile, { recursive: true, force: true });
    const qctx = await launch({ ...browserArgs(args), port, profile: quotaProfile });
    const qpage = qctx.pages()[0] ?? (await qctx.newPage());
    await qpage.addInitScript(() => {
      const est = navigator.storage.estimate.bind(navigator.storage);
      navigator.storage.estimate = async () => {
        const real = await est();
        return { quota: 100 * 1048576, usage: 95 * 1048576, usageDetails: real.usageDetails };
      };
    });
    await qpage.goto(`http://localhost:${port}/`);
    await qpage.waitForFunction(() => window.__detect?.state === "need-install", null, { timeout: 60_000 });
    await qpage.setInputFiles("#iso", isoPath);
    await qpage.waitForFunction(
      () => window.__detect?.state === "error" || /600 MB/.test(document.getElementById("log")?.textContent ?? ""),
      null,
      { timeout: 120_000 },
    );
    const quotaMsg = await qpage.evaluate(() => document.getElementById("log")?.textContent ?? "");
    if (!/600 MB|store \d+ MB/.test(quotaMsg)) {
      throw new Error(`low-quota install did not show expected message: ${quotaMsg.slice(0, 200)}`);
    }
    console.log(`quota test: low estimate blocks install — ${quotaMsg.split("\n")[0]}`);
    await qctx.close();
  }

  await ctx.close();
  exitCode = 0;
} catch (err) {
  console.error(err);
  exitCode = 1;
} finally {
  stopVite();
}
process.exit(exitCode);
