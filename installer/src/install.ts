/**
 * Installs Detective Barbie from the player's own disc image: reads the ISO 9660 tree and writes
 * BDETECT.EXE, DATA/**, AUDIO/**, and selected root files to a sink (no MSI/CAB extraction).
 */
import { IsoVolume, type AsyncByteSource } from "./isovfs.ts";
import type { ByteSource, FileSink } from "./io.ts";
import { latin1 } from "./io.ts";

export const PRODUCT_NAME = "Detective Barbie";

const BDETECT_ISO_PATH = "BDETECT.EXE";
const BDETECT_SHA256 = "b56d1981369dc3166a710927e5673c807d25858bd33ccd0c6f04d7a329548bf3";

const CHUNK = 256 * 1024;

export interface InstallProgress {
  phase: "scan" | "extract" | "copy" | "done";
  done: number;
  total: number;
  path?: string;
}

export interface InstallOptions {
  /** Return false to skip a file (paths relative to install root). Default: everything. */
  include?: (installPath: string) => boolean;
  verify?: boolean;
  onProgress?: (p: InstallProgress) => void;
}

export interface InstallReport {
  volumeId: string;
  written: number;
  bytes: number;
  skipped: number;
  verified: number;
  mismatches: string[];
  registry: { root: number; key: string; name: string | null; value: string | null }[];
}

interface InstallFile {
  path: string;
  size: number;
}

function syncAsyncSource(iso: ByteSource): AsyncByteSource {
  return {
    size: iso.size,
    read: async (offset, length) => iso.read(offset, length),
  };
}

async function volumeId(src: AsyncByteSource): Promise<string> {
  const pvd = await src.read(16 * 2048, 2048);
  if (latin1(pvd, 1, 6) !== "CD001") return "unknown";
  return latin1(pvd, 40, 72).trim();
}

function includeRootFile(name: string): boolean {
  const u = name.toUpperCase();
  if (u === "BDETECT.EXE") return true;
  if (u === "LANG.DAT" || u === "OS.DAT" || u === "LAYOUT.BIN" || u === "README.TXT" || u === "OPENING.WAV") {
    return true;
  }
  // The disc ships 30-byte placeholder CURRENT.AR*, SAVGAME*.AR* and GAMEHIST.ORY; the game reads them as real
  // archives and crashes, so they are not installed (the game creates its own in the save overlay).
  return false;
}

async function walkDir(vol: IsoVolume, dir: string, out: InstallFile[]): Promise<void> {
  const entries = await vol.list(dir);
  for (const e of entries) {
    const rel = dir ? `${dir}/${e.name}` : e.name;
    if (e.isDir) await walkDir(vol, rel, out);
    else out.push({ path: rel.replace(/\\/g, "/"), size: e.size });
  }
}

async function listInstallFiles(vol: IsoVolume): Promise<InstallFile[]> {
  const out: InstallFile[] = [];
  const root = await vol.list("");
  for (const e of root) {
    const upper = e.name.toUpperCase();
    if (e.isDir) {
      if (upper === "DATA" || upper === "AUDIO") await walkDir(vol, e.name, out);
      continue;
    }
    if (includeRootFile(e.name)) out.push({ path: e.name.replace(/\\/g, "/"), size: e.size });
  }
  out.sort((a, b) => a.path.localeCompare(b.path));
  return out;
}

async function verifyBdetect(vol: IsoVolume): Promise<void> {
  const st = await vol.stat(BDETECT_ISO_PATH);
  if (!st || st.isDir) {
    throw new Error(
      `This disc image is not ${PRODUCT_NAME}: ${BDETECT_ISO_PATH} was not found. ` +
        "Use the retail Detective Barbie CD-ROM image (DETECTV1_1.ISO).",
    );
  }
  const { data } = await readAll(vol, BDETECT_ISO_PATH, st.size);
  if (sha256(data) !== BDETECT_SHA256) {
    throw new Error(
      `This disc image is not ${PRODUCT_NAME}: ${BDETECT_ISO_PATH} does not match the known retail build ` +
        `(expected sha256 ${BDETECT_SHA256}).`,
    );
  }
}

async function readAll(vol: IsoVolume, path: string, size: number): Promise<{ data: Uint8Array; crc: number }> {
  const out = new Uint8Array(size);
  let off = 0;
  let crc = 0xffffffff;
  while (off < size) {
    const n = Math.min(CHUNK, size - off);
    const chunk = await vol.read(path, off, n);
    if (chunk.length !== n) throw new Error(`${path}: short read at ${off} (+${n})`);
    out.set(chunk, off);
    crc = crc32Update(crc, chunk);
    off += n;
  }
  return { data: out, crc: crc32Finish(crc) };
}

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

function crc32Update(crc: number, chunk: Uint8Array): number {
  let c = crc;
  for (let i = 0; i < chunk.length; i++) c = CRC_TABLE[(c ^ chunk[i]) & 0xff] ^ (c >>> 8);
  return c >>> 0;
}

function crc32Finish(crc: number): number {
  return (crc ^ 0xffffffff) >>> 0;
}

export async function install(iso: ByteSource, sink: FileSink, opts: InstallOptions = {}): Promise<InstallReport> {
  const src = syncAsyncSource(iso);
  const vol = await IsoVolume.open(src);
  await verifyBdetect(vol);

  const include = opts.include ?? (() => true);
  const verify = opts.verify ?? true;
  const all = await listInstallFiles(vol);
  const wanted = all.filter((f) => include(f.path));
  const total = wanted.length;
  const report: InstallReport = {
    volumeId: await volumeId(src),
    written: 0,
    bytes: 0,
    skipped: all.length - wanted.length,
    verified: 0,
    mismatches: [],
    registry: [],
  };

  opts.onProgress?.({ phase: "scan", done: 0, total, path: undefined });

  for (const f of wanted) {
    const { data, crc } = await readAll(vol, f.path, f.size);
    if (data.length !== f.size) report.mismatches.push(`${f.path}: size ${data.length} != ${f.size}`);
    else if (verify) {
      const check = crc32Finish(crc32Update(0xffffffff, data));
      if (check !== crc) report.mismatches.push(`${f.path}: CRC32 stream mismatch`);
      else report.verified++;
    }
    await sink.writeFile(f.path, data);
    report.written++;
    report.bytes += data.length;
    opts.onProgress?.({ phase: "copy", done: report.written, total, path: f.path });
  }

  opts.onProgress?.({ phase: "done", done: report.written, total, path: undefined });
  return report;
}

/** SHA-256 (FIPS 180-4) over a Uint8Array, lowercase hex. Browser-safe (no Node crypto). */
function sha256(data: Uint8Array): string {
  const K = new Uint32Array([
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
  ]);
  const h = new Uint32Array([0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]);
  const w = new Uint32Array(64);
  const padded = new Uint8Array(((data.length + 9 + 63) >> 6) << 6);
  padded.set(data);
  padded[data.length] = 0x80;
  const dv = new DataView(padded.buffer);
  dv.setUint32(padded.length - 8, Math.floor((data.length * 8) / 2 ** 32));
  dv.setUint32(padded.length - 4, (data.length * 8) >>> 0);
  for (let off = 0; off < padded.length; off += 64) {
    for (let i = 0; i < 16; i++) w[i] = dv.getUint32(off + i * 4);
    for (let i = 16; i < 64; i++) {
      const s0 = ((w[i - 15] >>> 7) | (w[i - 15] << 25)) ^ ((w[i - 15] >>> 18) | (w[i - 15] << 14)) ^ (w[i - 15] >>> 3);
      const s1 = ((w[i - 2] >>> 17) | (w[i - 2] << 15)) ^ ((w[i - 2] >>> 19) | (w[i - 2] << 13)) ^ (w[i - 2] >>> 10);
      w[i] = (w[i - 16] + s0 + w[i - 7] + s1) >>> 0;
    }
    let [a, b, c, d, e, f, g, hh] = [h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]];
    for (let i = 0; i < 64; i++) {
      const S1 = ((e >>> 6) | (e << 26)) ^ ((e >>> 11) | (e << 21)) ^ ((e >>> 25) | (e << 7));
      const ch = (e & f) ^ (~e & g);
      const t1 = (hh + S1 + ch + K[i] + w[i]) >>> 0;
      const S0 = ((a >>> 2) | (a << 30)) ^ ((a >>> 13) | (a << 19)) ^ ((a >>> 22) | (a << 10));
      const maj = (a & b) ^ (a & c) ^ (b & c);
      const t2 = (S0 + maj) >>> 0;
      hh = g; g = f; f = e; e = (d + t1) >>> 0; d = c; c = b; b = a; a = (t1 + t2) >>> 0;
    }
    h[0] = (h[0] + a) >>> 0; h[1] = (h[1] + b) >>> 0; h[2] = (h[2] + c) >>> 0; h[3] = (h[3] + d) >>> 0;
    h[4] = (h[4] + e) >>> 0; h[5] = (h[5] + f) >>> 0; h[6] = (h[6] + g) >>> 0; h[7] = (h[7] + hh) >>> 0;
  }
  let out = "";
  for (let i = 0; i < 8; i++) {
    const word = h[i];
    for (let j = 0; j < 4; j++) out += ((word >>> (24 - j * 8)) & 0xff).toString(16).padStart(2, "0");
  }
  return out;
}
