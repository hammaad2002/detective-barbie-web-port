/**
 * InstallShield 3 cabinet reader (.cab with magic "ISc(" / 0x28635349, or legacy .Z magic
 * 0x8c655d13). Detective Barbie's DATA1.CAB stores payloads as u16-length DEFLATE blocks
 * (the same layout unshield uses for major version 5 cabinets).
 */
import { dclExplode } from "./blast.ts";
import { cstr, u16, u32 } from "./io.ts";

const SIG_ISC = 0x28635349; // "ISc("
const SIG_IS3Z = 0x8c655d13; // InstallShield 3 .Z archive
const FLAG_COMPRESSED = 4;
const FLAG_INVALID = 8;

export interface Isc3Entry {
  name: string;
  dir: string;
  size: number;
}

interface FileRec {
  name: string;
  dir: string;
  size: number;
  compressedSize: number;
  flags: number;
  dataOffset: number;
}

export class Isc3Cabinet {
  private readonly bytes: Uint8Array;
  private readonly files: FileRec[] = [];

  constructor(bytes: Uint8Array) {
    this.bytes = bytes;
    const sig = u32(bytes, 0);
    if (sig !== SIG_ISC && sig !== SIG_IS3Z) {
      throw new Error(`not an InstallShield 3 cabinet (signature 0x${sig.toString(16)})`);
    }
    if (sig === SIG_IS3Z) throw new Error("InstallShield 3 .Z archives are not supported here (use ISc( .cab)");
    this.parseIscCabinet();
  }

  list(): Isc3Entry[] {
    return this.files.map((f) => ({ name: f.name, dir: f.dir, size: f.size }));
  }

  extract(name: string): Uint8Array {
    const want = name.toLowerCase();
    const f = this.files.find((x) => x.name.toLowerCase() === want);
    if (!f) throw new Error(`file not found in cabinet: ${name}`);
    const comp = this.bytes.subarray(f.dataOffset, f.dataOffset + f.compressedSize);
    if (f.flags & FLAG_COMPRESSED) return decompressPayload(comp, f.size);
    if (comp.length !== f.size) throw new Error(`${name}: stored size ${comp.length} != ${f.size}`);
    return comp.slice();
  }

  private parseIscCabinet(): void {
    const descOff = u32(this.bytes, 12);
    const stringBase = descOff + u32(this.bytes, descOff + 0x0c);
    let p = descOff + 0x0c;
    const fileTableOff = u32(this.bytes, p);
    p += 4 + 4 + 4 + 4;
    const directoryCount = u32(this.bytes, p);
    p += 4 + 8;
    const fileCount = u32(this.bytes, p);
    const ftBase = stringBase;
    const table = new Uint32Array(directoryCount + fileCount);
    for (let i = 0; i < table.length; i++) table[i] = u32(this.bytes, ftBase + i * 4);

    const dirs: string[] = [];
    for (let i = 0; i < directoryCount; i++) {
      dirs.push(cstr(this.bytes, ftBase + table[i]));
    }

    for (let fi = 0; fi < fileCount; fi++) {
      const fd = stringBase + table[directoryCount + fi];
      let q = fd;
      const nameOff = u32(this.bytes, q);
      q += 4;
      const dirIndex = u16(this.bytes, q);
      q += 4;
      const flags = u16(this.bytes, q);
      q += 2;
      const size = u32(this.bytes, q);
      q += 4;
      const compressedSize = u32(this.bytes, q);
      q += 4 + 0x14;
      const dataOffset = u32(this.bytes, q);
      if (flags & FLAG_INVALID || nameOff === 0 || dataOffset === 0) continue;
      const name = cstr(this.bytes, stringBase + nameOff);
      const dir = dirIndex < dirs.length ? dirs[dirIndex] : "";
      this.files.push({ name, dir, size, compressedSize, flags, dataOffset });
    }
  }
}

function decompressPayload(comp: Uint8Array, expected: number): Uint8Array {
  if (comp.length >= 2 && comp[1] >= 4 && comp[1] <= 6 && comp[0] <= 1) {
    try {
      const got = dclExplode(comp);
      if (got.length === expected) return got;
    } catch {
      /* fall through to DEFLATE blocks */
    }
  }
  const out = new Uint8Array(expected);
  let w = 0;
  let pos = 0;
  while (pos < comp.length) {
    if (pos + 2 > comp.length) break;
    const blockLen = u16(comp, pos);
    pos += 2;
    if (blockLen === 0) throw new Error("InstallShield cabinet: zero-length compressed block");
    const block = comp.subarray(pos, pos + blockLen);
    pos += blockLen;
    w = inflateDeflateChunk(block, out, w);
  }
  if (w !== expected) {
    throw new Error(`InstallShield cabinet: decompressed ${w} bytes, expected ${expected}`);
  }
  return out;
}

/** One InstallShield u16-block payload: a complete raw DEFLATE stream (zlib -15). */
function inflateDeflateChunk(block: Uint8Array, out: Uint8Array, start: number): number {
  const src = new Uint8Array(block.length + 1);
  src.set(block);
  const br = new IscBitReader(src);
  let o = start;
  const cap = out.length;
  for (;;) {
    const bfinal = br.readBit();
    const btype = br.readBits(2);
    if (btype === 0) {
      br.alignToByte();
      const len = br.readBits(16);
      const nlen = br.readBits(16);
      if ((len ^ 0xffff) !== nlen) throw new Error("InstallShield DEFLATE: stored block length check failed");
      const from = br.alignedPos;
      if (from + len > src.length) throw new Error("InstallShield DEFLATE: stored block overruns source");
      if (o + len > cap) throw new Error("InstallShield DEFLATE: output buffer too small");
      out.set(src.subarray(from, from + len), o);
      o += len;
      br.skipBytes(len);
    } else if (btype === 1) {
      o = iscCopyBlock(br, out, o, cap, ISC_FIXED_LIT, ISC_FIXED_DIST);
    } else if (btype === 2) {
      const hlit = br.readBits(5) + 257;
      const hdist = br.readBits(5) + 1;
      const hclen = br.readBits(4) + 4;
      const cl = new Array<number>(19).fill(0);
      for (let i = 0; i < hclen; i++) cl[ISC_CODELEN_ORDER[i]] = br.readBits(3);
      const clTable = iscBuild(cl);
      const lens = new Array<number>(hlit + hdist);
      let i = 0;
      while (i < lens.length) {
        const sym = iscDecodeSym(br, clTable);
        if (sym < 16) lens[i++] = sym;
        else {
          let run: number;
          let value: number;
          if (sym === 16) {
            if (i === 0) throw new Error("InstallShield DEFLATE: repeat with no previous length");
            run = br.readBits(2) + 3;
            value = lens[i - 1];
          } else if (sym === 17) {
            run = br.readBits(3) + 3;
            value = 0;
          } else {
            run = br.readBits(7) + 11;
            value = 0;
          }
          if (i + run > lens.length) throw new Error("InstallShield DEFLATE: length run overruns table");
          lens.fill(value, i, i + run);
          i += run;
        }
      }
      if (lens[256] === 0) throw new Error("InstallShield DEFLATE: missing end-of-block code");
      o = iscCopyBlock(br, out, o, cap, iscBuild(lens.slice(0, hlit)), iscBuild(lens.slice(hlit)));
    } else throw new Error("InstallShield DEFLATE: invalid block type");
    if (bfinal) break;
  }
  return o;
}

const ISC_LENGTH_BASE = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258];
const ISC_LENGTH_EXTRA = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0];
const ISC_DIST_BASE = [1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577];
const ISC_DIST_EXTRA = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13];
const ISC_CODELEN_ORDER = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15];

interface IscHuffman {
  counts: number[];
  symbols: number[];
}

const ISC_FIXED_LIT_LENS: number[] = [];
for (let i = 0; i < 144; i++) ISC_FIXED_LIT_LENS.push(8);
for (let i = 144; i < 256; i++) ISC_FIXED_LIT_LENS.push(9);
for (let i = 256; i < 280; i++) ISC_FIXED_LIT_LENS.push(7);
for (let i = 280; i < 288; i++) ISC_FIXED_LIT_LENS.push(8);
const ISC_FIXED_LIT = iscBuild(ISC_FIXED_LIT_LENS);
const ISC_FIXED_DIST = iscBuild(new Array<number>(30).fill(5), true);

function iscBuild(lengths: number[], allowIncomplete = false): IscHuffman {
  const counts = new Array<number>(16).fill(0);
  let max = 0;
  for (const l of lengths) {
    counts[l]++;
    if (l > max) max = l;
  }
  let left = 1;
  for (let len = 1; len <= 15; len++) {
    left = (left << 1) - counts[len];
    if (left < 0) throw new Error("InstallShield DEFLATE: over-subscribed Huffman code");
  }
  if (left > 0 && max !== 1 && !allowIncomplete) throw new Error("InstallShield DEFLATE: incomplete Huffman code");
  const offs = new Array<number>(17);
  offs[1] = 0;
  for (let len = 1; len < 16; len++) offs[len + 1] = offs[len] + counts[len];
  const symbols = new Array<number>(lengths.filter((l) => l > 0).length);
  for (let sym = 0; sym < lengths.length; sym++) if (lengths[sym] > 0) symbols[offs[lengths[sym]]++] = sym;
  return { counts, symbols };
}

class IscBitReader {
  private pos = 0;
  private buf = 0;
  private cnt = 0;
  private readonly src: Uint8Array;

  constructor(src: Uint8Array) {
    this.src = src;
  }

  readBit(): number {
    if (this.cnt === 0) {
      if (this.pos >= this.src.length) throw new Error("InstallShield DEFLATE: unexpected end of data");
      this.buf = this.src[this.pos++];
      this.cnt = 8;
    }
    const bit = this.buf & 1;
    this.buf >>>= 1;
    this.cnt--;
    return bit;
  }

  readBits(n: number): number {
    let v = 0;
    for (let i = 0; i < n; i++) v |= this.readBit() << i;
    return v;
  }

  skipBytes(n: number): void {
    if (this.cnt !== 0) throw new Error("InstallShield DEFLATE: internal alignment error");
    this.pos += n;
  }

  get alignedPos(): number {
    return this.pos;
  }

  alignToByte(): void {
    this.cnt = 0;
  }
}

function iscDecodeSym(br: IscBitReader, h: IscHuffman): number {
  let code = 0;
  let first = 0;
  let index = 0;
  for (let len = 1; len <= 15; len++) {
    code |= br.readBit();
    const count = h.counts[len];
    if (code - first < count) return h.symbols[index + (code - first)];
    index += count;
    first = (first + count) << 1;
    code <<= 1;
  }
  throw new Error("InstallShield DEFLATE: invalid Huffman code");
}

function iscCopyBlock(br: IscBitReader, out: Uint8Array, pos: number, cap: number, lit: IscHuffman, dist: IscHuffman): number {
  for (;;) {
    const sym = iscDecodeSym(br, lit);
    if (sym === 256) return pos;
    if (sym < 256) {
      if (pos >= cap) throw new Error("InstallShield DEFLATE: output buffer too small");
      out[pos++] = sym;
      continue;
    }
    const li = sym - 257;
    if (li >= 29) throw new Error("InstallShield DEFLATE: invalid length symbol");
    const len = ISC_LENGTH_BASE[li] + br.readBits(ISC_LENGTH_EXTRA[li]);
    if (pos + len > cap) throw new Error("InstallShield DEFLATE: match overruns output buffer");
    const dsym = iscDecodeSym(br, dist);
    if (dsym >= 30) throw new Error("InstallShield DEFLATE: invalid distance symbol");
    const distVal = ISC_DIST_BASE[dsym] + br.readBits(ISC_DIST_EXTRA[dsym]);
    let cur = pos;
    for (let i = 0; i < len; i++) {
      out[cur] = cur - distVal >= 0 ? out[cur - distVal] : 0;
      cur++;
    }
    pos = cur;
  }
}
