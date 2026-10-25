/**
 * Save backups. Writable sidecars (SavGame*.ar1–3, current.ar1–3, GameHist.ory, data\NewGame.*)
 * live in OPFS under `saves/` (mirroring paths the engine overlay uses). Export bundles them into a
 * ZIP; restore accepts the same layout, loose files, or a backup .zip.
 */
import { INSTALL_DIR } from "./cache.ts";
import { writeOpfsFile } from "./opfs-write.ts";
import { makeZip, readZip, type ZipEntry } from "./zip.ts";

/** OPFS directory for writable game sidecars (see docs/JOURNAL.md overlay design). */
export const SAVES_DIR = "saves";

const SAVE_BASENAME =
  /^(savgame\d+\.ar[1-3]|current\.ar[1-3]|gamehist\.ory|newgame\.(?:ar|db)[1-3])$/i;
const MAX_SAVE_BYTES = 8 << 20;

/** True when `base` is a save sidecar file name (case-insensitive). */
export function isSaveBasename(base: string): boolean {
  return SAVE_BASENAME.test(base);
}

/**
 * Map a path inside a backup ZIP (or a loose upload) to a relative path under {@link SAVES_DIR}
 * (`SavGame0.ar1`, `data/NewGame.ar1`, …), or null when it is not a save file.
 */
export function canonicalSaveRelPath(entryPath: string): string | null {
  const p = entryPath.replace(/\\/g, "/").replace(/^\/+/, "");
  if (!p || p.split("/").some((s) => s === "..")) return null;
  const parts = p.split("/").filter(Boolean);
  const base = parts[parts.length - 1] ?? "";
  if (!isSaveBasename(base)) return null;
  if (/^newgame\./i.test(base)) {
    if (parts.length === 1) return `data/${base}`;
    if (parts.length === 2 && /^data$/i.test(parts[0])) return `data/${base}`;
    return null;
  }
  return base;
}

/** ZIP member names for export (forward slashes, no `saves/` prefix). */
export function zipEntriesForExport(
  saves: { name: string; data: Uint8Array<ArrayBuffer>; date?: Date }[],
): ZipEntry[] {
  return saves.map((s) => ({
    name: s.name.replace(/\\/g, "/"),
    data: s.data,
    date: s.date,
  }));
}

/** Collect save files named in a ZIP central directory (pure; no I/O). */
export function entriesFromZipNames(
  entries: { name: string; data: Uint8Array<ArrayBuffer> }[],
): Map<string, Uint8Array<ArrayBuffer>> {
  const found = new Map<string, Uint8Array<ArrayBuffer>>();
  for (const e of entries) {
    const rel = canonicalSaveRelPath(e.name);
    if (rel) found.set(rel, e.data);
  }
  return found;
}

function savesPath(...parts: string[]): string[] {
  return [SAVES_DIR, ...parts.filter(Boolean)];
}

async function savesRoot(): Promise<FileSystemDirectoryHandle> {
  return (await navigator.storage.getDirectory()).getDirectoryHandle(SAVES_DIR, { create: true });
}

async function collect(
  dir: FileSystemDirectoryHandle,
  prefix: string,
  out: { name: string; data: Uint8Array<ArrayBuffer>; date: Date }[],
): Promise<void> {
  for await (const [name, handle] of (dir as unknown as { entries(): AsyncIterable<[string, FileSystemHandle]> }).entries()) {
    if (handle.kind === "directory") {
      await collect(handle as FileSystemDirectoryHandle, `${prefix}${name}/`, out);
      continue;
    }
    const rel = (prefix ? `${prefix}${name}` : name).replace(/\\/g, "/");
    const canonical = canonicalSaveRelPath(rel);
    if (!canonical) continue;
    try {
      const f = await (handle as FileSystemFileHandle).getFile();
      out.push({ name: canonical, data: new Uint8Array(await f.arrayBuffer()), date: new Date(f.lastModified) });
    } catch {
      // Removed while listing.
    }
  }
}

/** Save files currently in OPFS (overlay `saves/` tree, plus legacy install-root sidecars). */
export async function listSaves(): Promise<{ name: string; data: Uint8Array<ArrayBuffer>; date: Date }[]> {
  const out: { name: string; data: Uint8Array<ArrayBuffer>; date: Date }[] = [];
  const opfs = await navigator.storage.getDirectory();
  try {
    await collect(await opfs.getDirectoryHandle(SAVES_DIR), "", out);
  } catch {
    // No saves directory yet.
  }
  try {
    const install = await opfs.getDirectoryHandle(INSTALL_DIR);
    for await (const [name, handle] of (install as unknown as { entries(): AsyncIterable<[string, FileSystemHandle]> }).entries()) {
      if (handle.kind !== "file" || !isSaveBasename(name)) continue;
      try {
        const f = await (handle as FileSystemFileHandle).getFile();
        if (!out.some((s) => s.name.toLowerCase() === name.toLowerCase())) {
          out.push({ name, data: new Uint8Array(await f.arrayBuffer()), date: new Date(f.lastModified) });
        }
      } catch {
        // Removed while listing.
      }
    }
  } catch {
    // Not installed.
  }
  return out.sort((a, b) => a.name.localeCompare(b.name));
}

/** A ZIP of all save files, or null when there is nothing to back up. */
export async function exportSaves(): Promise<Blob | null> {
  const saves = await listSaves();
  if (!saves.length) return null;
  return new Blob([makeZip(zipEntriesForExport(saves))], { type: "application/zip" });
}

export interface ImportPlan {
  files: { name: string; data: Uint8Array<ArrayBuffer> }[];
  settings?: Uint8Array<ArrayBuffer>;
  overwrites: string[];
}

/** Validate chosen files before writing anything. */
export async function planImport(files: File[]): Promise<ImportPlan> {
  const found = new Map<string, Uint8Array<ArrayBuffer>>();
  for (const f of files) {
    if (/\.zip$/i.test(f.name)) {
      const entries = await readZip(new Uint8Array(await f.arrayBuffer()));
      for (const [rel, data] of entriesFromZipNames(entries)) found.set(rel, data);
    } else {
      const rel = canonicalSaveRelPath(f.name);
      if (!rel) {
        throw new Error(
          `${f.name} is not a Detective Barbie save (expected SavGame*.ar1–3, current.ar1–3, GameHist.ory, data/NewGame.*, or a backup .zip)`,
        );
      }
      found.set(rel, new Uint8Array(await f.arrayBuffer()));
    }
  }
  if (!found.size) throw new Error("no Detective Barbie save files found in the selection");
  for (const [name, data] of found) {
    if (data.length === 0 || data.length > MAX_SAVE_BYTES) throw new Error(`${name} has an unexpected size (${data.length} bytes)`);
  }
  const existing = new Set((await listSaves()).map((s) => s.name.toLowerCase()));
  const overwrites = [...found.keys()].filter((n) => existing.has(n.toLowerCase()));
  return {
    files: [...found].map(([name, data]) => ({ name, data })),
    overwrites,
  };
}

export async function applyImport(plan: ImportPlan): Promise<void> {
  await savesRoot();
  const existing = new Map<string, string>();
  try {
    const walk = async (dir: FileSystemDirectoryHandle, prefix: string) => {
      for await (const [name, handle] of (dir as unknown as { entries(): AsyncIterable<[string, FileSystemHandle]> }).entries()) {
        const rel = prefix ? `${prefix}${name}` : name;
        if (handle.kind === "directory") await walk(handle as FileSystemDirectoryHandle, `${rel}/`);
        else existing.set(rel.toLowerCase(), rel);
      }
    };
    await walk(await savesRoot(), "");
  } catch {
    // Empty tree.
  }
  for (const f of plan.files) {
    const target = existing.get(f.name.toLowerCase()) ?? f.name;
    await writeOpfsFile(savesPath(...target.split("/")), f.data);
  }
}
