#!/usr/bin/env node
// Headless mouse interaction check (Detective Barbie is point-and-click; no gamepad path).
//   node tools/web-pad-test.mjs [--port 5174] [--browser chrome|firefox|webkit] [--headed]
import { browserArgs, launch } from "./browsers.mjs";

const args = process.argv.slice(2);
const opt = (name, dflt) => {
  const i = args.indexOf(`--${name}`);
  return i >= 0 ? args[i + 1] : dflt;
};
const port = opt("port", "5174");
const base = `http://localhost:${port}/`;
let failures = 0;
const check = (ok, what, detail = "") => {
  console.log(`${ok ? "PASS" : "FAIL"} ${what}${detail ? ` — ${detail}` : ""}`);
  if (!ok) failures++;
};

const ctx = await launch({ ...browserArgs(args), port });
const page = ctx.pages()[0] ?? (await ctx.newPage());
page.on("pageerror", (e) => console.log(`pageerror: ${e.message}`));

await page.goto(base);
await page.waitForFunction(() => window.__detect?.state === "ready" || window.__detect?.state === "need-install", null, { timeout: 60_000 });
const state = await page.evaluate(() => window.__detect?.state);
if (state === "need-install") {
  console.log("SKIP mouse play test — install the game first (tools/web-smoke.mjs --install-only)");
} else {
  await page.goto(`${base}?autoplay`);
  await page.waitForFunction(() => window.__detect?.state === "running" || window.__detect?.state === "error", null, { timeout: 120_000 }).catch(() => {});
  await page.mouse.move(640, 480);
  await page.mouse.click(640, 480);
  const locked = await page.waitForFunction(() => document.pointerLockElement?.id === "canvas", null, { timeout: 5_000 }).then(() => true, () => false);
  check(locked, "clicking the canvas locks the pointer for mouse look / clicks");
}

await ctx.close();
console.log(failures ? `${failures} check(s) failed` : "all checks passed");
process.exit(failures ? 1 : 0);
