/**
 * PKWare Data Compression Library (DCL) "implode" decompressor.
 *
 * TypeScript port of Mark Adler's blast.c (zlib contrib/blast), which implements the same
 * algorithm as PKWare's explode(). InstallShield 3 `.Z` archives store file payloads in this
 * format; some cabinet variants use it as well.
 */

const MAXBITS = 13;
const MAXWIN = 4096;

const LITLEN = new Uint8Array([
  11, 124, 8, 7, 28, 7, 188, 13, 76, 4, 10, 8, 12, 10, 12, 10, 8, 23, 8,
  9, 7, 6, 7, 8, 7, 6, 55, 8, 23, 24, 12, 11, 7, 9, 11, 12, 6, 7, 22, 5,
  7, 24, 6, 11, 9, 6, 7, 22, 7, 11, 38, 7, 9, 8, 25, 11, 8, 11, 9, 12,
  8, 12, 5, 38, 5, 38, 5, 11, 7, 5, 6, 21, 6, 10, 53, 8, 7, 24, 10, 27,
  44, 253, 253, 253, 252, 252, 252, 13, 12, 45, 12, 45, 12, 61, 12, 45,
  44, 173,
]);
const LENLEN = new Uint8Array([2, 35, 36, 53, 38, 23]);
const DISTLEN = new Uint8Array([2, 20, 53, 230, 247, 151, 248]);
const LEN_BASE = [3, 2, 4, 5, 6, 7, 8, 9, 10, 12, 16, 24, 40, 72, 136, 264];
const LEN_EXTRA = [0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8];

interface Huffman {
  count: Int16Array;
  symbol: Int16Array;
}

class BlastInput {
  pos = 0;
  bitbuf = 0;
  bitcnt = 0;
  readonly src: Uint8Array;

  constructor(src: Uint8Array) {
    this.src = src;
  }

  bits(need: number): number {
    let val = this.bitbuf;
    while (this.bitcnt < need) {
      if (this.pos >= this.src.length) throw new BlastError(2);
      val |= this.src[this.pos++] << this.bitcnt;
      this.bitcnt += 8;
    }
    this.bitbuf = val >> need;
    this.bitcnt -= need;
    return val & ((1 << need) - 1);
  }
}

class BlastError extends Error {
  readonly code: number;

  constructor(code: number) {
    super(`blast failed with code ${code}`);
    this.code = code;
  }
}

function construct(rep: Uint8Array): Huffman {
  const length = new Int16Array(256);
  let symbol = 0;
  let ri = 0;
  while (ri < rep.length) {
    const byte = rep[ri++];
    let left = (byte >> 4) + 1;
    const len = byte & 15;
    do {
      length[symbol++] = len;
    } while (--left);
  }
  const n = symbol;
  const count = new Int16Array(MAXBITS + 1);
  for (let i = 0; i < n; i++) count[length[i]]++;
  if (count[0] === n) {
    return { count, symbol: new Int16Array(0) };
  }
  let left = 1;
  for (let len = 1; len <= MAXBITS; len++) {
    left = (left << 1) - count[len];
    if (left < 0) throw new BlastError(-9);
  }
  const offs = new Int16Array(MAXBITS + 2);
  offs[1] = 0;
  for (let len = 1; len < MAXBITS; len++) offs[len + 1] = offs[len] + count[len];
  const sym = new Int16Array(n);
  for (symbol = 0; symbol < n; symbol++) {
    if (length[symbol] !== 0) sym[offs[length[symbol]]++] = symbol;
  }
  return { count, symbol: sym };
}

function decode(s: BlastInput, h: Huffman): number {
  let bitbuf = s.bitbuf;
  let left = s.bitcnt;
  let code = 0;
  let first = 0;
  let index = 0;
  let len = 1;
  let ci = 1;
  for (;;) {
    while (left--) {
      code |= (bitbuf & 1) ^ 1;
      bitbuf >>= 1;
      const cnt = h.count[ci];
      if (code < first + cnt) {
        s.bitbuf = bitbuf;
        s.bitcnt = (s.bitcnt - len) & 7;
        return h.symbol[index + (code - first)];
      }
      index += cnt;
      first += cnt;
      first <<= 1;
      code <<= 1;
      len++;
      ci++;
    }
    left = MAXBITS + 1 - len;
    if (left === 0) break;
    if (s.pos >= s.src.length) throw new BlastError(2);
    bitbuf = s.src[s.pos++];
    s.bitcnt += 8;
    if (left > 8) left = 8;
    left--;
  }
  return -9;
}

let tables: { lit: Huffman; len: Huffman; dist: Huffman } | undefined;

function tablesOnce(): { lit: Huffman; len: Huffman; dist: Huffman } {
  if (!tables) {
    tables = { lit: construct(LITLEN), len: construct(LENLEN), dist: construct(DISTLEN) };
  }
  return tables;
}

/** Decompress one PKWare DCL implode stream. */
export function dclExplode(src: Uint8Array): Uint8Array {
  const s = new BlastInput(src);
  const { lit: litcode, len: lencode, dist: distcode } = tablesOnce();
  const out = new Uint8Array(MAXWIN);
  const chunks: Uint8Array[] = [];
  let next = 0;
  let first = true;

  const flush = (): void => {
    if (next > 0) {
      chunks.push(out.slice(0, next));
      next = 0;
    }
  };

  const lit = s.bits(8);
  if (lit > 1) throw new BlastError(-1);
  const dict = s.bits(8);
  if (dict < 4 || dict > 6) throw new BlastError(-2);

  for (;;) {
    if (s.bits(1)) {
      const sym = decode(s, lencode);
      let len = LEN_BASE[sym] + s.bits(LEN_EXTRA[sym]);
      if (len === 519) break;
      const dsym = len === 2 ? 2 : dict;
      let dist = decode(s, distcode) << dsym;
      dist += s.bits(dsym);
      dist++;
      if (first && dist > next) throw new BlastError(-3);
      do {
        let copy = MAXWIN;
        let from = next - dist;
        if (next < dist) {
          from += copy;
          copy = dist;
        }
        copy -= next;
        if (copy > len) copy = len;
        len -= copy;
        next += copy;
        let to = next - copy;
        let f = from;
        while (copy--) out[to++] = out[f++];
        if (next === MAXWIN) {
          flush();
          first = false;
        }
      } while (len !== 0);
    } else {
      const symbol = lit ? decode(s, litcode) : s.bits(8);
      out[next++] = symbol;
      if (next === MAXWIN) {
        flush();
        first = false;
      }
    }
  }
  flush();
  const total = chunks.reduce((a, c) => a + c.length, 0);
  const result = new Uint8Array(total);
  let w = 0;
  for (const c of chunks) {
    result.set(c, w);
    w += c.length;
  }
  return result;
}

/** Alias for Mark Adler's `blast()` entry point. */
export const blast = dclExplode;
