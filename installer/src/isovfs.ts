/**
 * Lazy ISO 9660 volume access over an async byte range source (browser / Node).
 * Parsing rules match {@link ./iso9660.ts}; only requested byte ranges are read.
 */
import { latin1, u32 } from "./io.ts";

const SECTOR = 2048;

export interface AsyncByteSource {
  readonly size: number;
  read(offset: number, length: number): Promise<Uint8Array>;
}

interface IsoEntry {
  name: string;
  isDir: boolean;
  /** Byte offset of file data or directory listing within the image. */
  offset: number;
  size: number;
}

function normDirPath(dir: string): string {
  return dir.replace(/^[\\/]+|[\\/]+$/g, "");
}

export class IsoVolume {
  private readonly src: AsyncByteSource;
  private readonly root: IsoEntry;
  private readonly joliet: boolean;
  private readonly dirCache = new Map<number, IsoEntry[]>();

  private constructor(src: AsyncByteSource, root: IsoEntry, joliet: boolean) {
    this.src = src;
    this.root = root;
    this.joliet = joliet;
  }

  static async open(src: AsyncByteSource): Promise<IsoVolume> {
    let pvd: Uint8Array | undefined;
    let svd: Uint8Array | undefined;
    for (let sector = 16; sector < 64; sector++) {
      const vd = await src.read(sector * SECTOR, SECTOR);
      if (latin1(vd, 1, 6) !== "CD001") throw new Error(`not an ISO 9660 image (sector ${sector})`);
      const type = vd[0];
      if (type === 255) break;
      if (type === 1 && !pvd) pvd = vd;
      if (type === 2 && vd[88] === 0x25 && vd[89] === 0x2f && [0x40, 0x43, 0x45].includes(vd[90])) svd = vd;
    }
    if (!pvd) throw new Error("ISO 9660 primary volume descriptor not found");
    const joliet = svd !== undefined;
    const vd = svd ?? pvd;
    const vol = new IsoVolume(src, { name: "", isDir: true, offset: 0, size: 0 }, joliet);
    const root = vol.parseRecord(vd, 156);
    if (!root) throw new Error("ISO 9660 root directory record missing");
    vol.root.offset = root.offset;
    vol.root.size = root.size;
    return vol;
  }

  async list(dir: string): Promise<{ name: string; size: number; isDir: boolean }[]> {
    const path = normDirPath(dir);
    const node = path === "" ? this.root : await this.lookup(path);
    if (!node || !node.isDir) throw new Error(`not a directory: ${dir}`);
    const entries = await this.readDir(node);
    return entries.map((e) => ({ name: e.name, size: e.size, isDir: e.isDir }));
  }

  async stat(path: string): Promise<{ size: number; isDir: boolean } | null> {
    const e = await this.lookup(normDirPath(path));
    if (!e) return null;
    return { size: e.size, isDir: e.isDir };
  }

  async read(path: string, off: number, len: number): Promise<Uint8Array> {
    const e = await this.lookup(normDirPath(path));
    if (!e || e.isDir) throw new Error(`file not found in ISO: ${path}`);
    if (!Number.isInteger(off) || !Number.isInteger(len) || off < 0 || len < 0 || off + len > e.size) {
      throw new RangeError(`read [${off}, +${len}) outside file of size ${e.size}`);
    }
    return this.src.read(e.offset + off, len);
  }

  private async lookup(path: string): Promise<IsoEntry | undefined> {
    let cur = this.root;
    for (const part of path.split(/[\\/]+/).filter(Boolean)) {
      if (!cur.isDir) return undefined;
      const want = part.replace(/;\d+$/i, "").toLowerCase();
      const next = (await this.readDir(cur)).find((e) => e.name.toLowerCase() === want);
      if (!next) return undefined;
      cur = next;
    }
    return cur;
  }

  private async readDir(dir: IsoEntry): Promise<IsoEntry[]> {
    const cached = this.dirCache.get(dir.offset);
    if (cached) return cached;
    const data = await this.src.read(dir.offset, dir.size);
    const out: IsoEntry[] = [];
    let o = 0;
    while (o < data.length) {
      const len = data[o];
      if (len === 0) {
        o = (Math.floor(o / SECTOR) + 1) * SECTOR;
        continue;
      }
      const e = this.parseRecord(data, o);
      if (e) out.push(e);
      o += len;
    }
    this.dirCache.set(dir.offset, out);
    return out;
  }

  private parseRecord(b: Uint8Array, o: number): IsoEntry | undefined {
    const extent = u32(b, o + 2);
    const size = u32(b, o + 10);
    const flags = b[o + 25];
    const nameLen = b[o + 32];
    const nameStart = o + 33;
    if (nameLen === 1 && (b[nameStart] === 0 || b[nameStart] === 1)) {
      return o === 156 ? { name: "", isDir: true, offset: extent * SECTOR, size } : undefined;
    }
    let name: string;
    if (this.joliet) {
      name = "";
      for (let i = 0; i + 1 < nameLen; i += 2) name += String.fromCharCode((b[nameStart + i] << 8) | b[nameStart + i + 1]);
    } else {
      name = latin1(b, nameStart, nameStart + nameLen);
    }
    name = name.replace(/;\d+$/, "");
    if (!(flags & 2) && name.endsWith(".")) name = name.slice(0, -1);
    if (flags & 0x80) throw new Error(`multi-extent ISO files are not supported: ${name}`);
    return { name, isDir: (flags & 2) !== 0, offset: extent * SECTOR, size };
  }
}
