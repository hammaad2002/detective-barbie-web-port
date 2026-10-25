/// <reference lib="webworker" />
/**
 * Translates the installed `BDETECT.EXE` (from OPFS) into a WebAssembly module on the player's own
 * machine and caches it in OPFS as `detect/cache/<name>`. Messages out: progress / done / error.
 */
import { initIced } from "iced-x86";
import icedWasmUrl from "../../node_modules/iced-x86/iced_x86_bg.wasm?url";
import type { ExeMetadata } from "../../recomp/src/translate.ts";
import { discoverMetadata, translateExe } from "../../recomp/src/translate.ts";
import { INSTALL_DIR, cacheName, sha256 } from "./cache.ts";

// Optional offline metadata (addresses only); when missing, discovery matches native gen-c.
const METADATA = import.meta.glob("../../metadata/*.json", { query: "?url", import: "default", eager: true }) as Record<string, string>;

async function loadMetadata(exe: Uint8Array, sha: string): Promise<ExeMetadata> {
  const metaUrl = METADATA[`../../metadata/${sha}.json`];
  if (metaUrl) return (await (await fetch(metaUrl)).json()) as ExeMetadata;
  return discoverMetadata(exe, { name: "BDETECT.EXE", sha256: sha, size: exe.length });
}

self.onmessage = async () => {
  try {
    const opfs = await navigator.storage.getDirectory();
    const root = await opfs.getDirectoryHandle(INSTALL_DIR);
    const exe = new Uint8Array(await (await (await root.getFileHandle("BDETECT.EXE")).getFile()).arrayBuffer());
    const sha = await sha256(exe);
    initIced(new Uint8Array(await (await fetch(icedWasmUrl)).arrayBuffer()));
    const meta = await loadMetadata(exe, sha);
    const t0 = performance.now();
    let last = 0;
    const out = translateExe(exe, meta, (phase, done, total) => {
      const now = performance.now();
      if (now - last > 100) {
        last = now;
        postMessage({ type: "progress", phase, done, total });
      }
    });
    const cache = await root.getDirectoryHandle("cache", { create: true });
    const fh = await cache.getFileHandle(cacheName(sha), { create: true });
    const h = await (fh as unknown as { createSyncAccessHandle(): Promise<FileSystemSyncAccessHandle> }).createSyncAccessHandle();
    h.truncate(0);
    h.write(out.wasm, { at: 0 });
    h.flush();
    h.close();
    postMessage({
      type: "done",
      name: cacheName(sha),
      seconds: (performance.now() - t0) / 1000,
      bytes: out.wasm.length,
      functions: out.functions,
      irreducible: out.irreducible,
    });
  } catch (err) {
    postMessage({ type: "error", message: err instanceof Error ? `${err.message}\n${err.stack ?? ""}` : String(err) });
  }
};
