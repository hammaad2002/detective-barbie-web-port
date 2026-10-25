import { describe, expect, it } from "vitest";
import { existsSync } from "node:fs";
import { open, stat } from "node:fs/promises";
import { IsoVolume } from "../installer/src/isovfs.ts";

const ISO_CANDIDATES = [
  "DETECTV1_1.ISO",
];

function findIsoPath(): string | undefined {
  for (const p of ISO_CANDIDATES) {
    if (existsSync(p)) return p;
  }
  return undefined;
}

class NodeAsyncFileSource {
  readonly size: number;
  private readonly fd: Awaited<ReturnType<typeof open>>;

  private constructor(fd: Awaited<ReturnType<typeof open>>, size: number) {
    this.fd = fd;
    this.size = size;
  }

  static async open(path: string): Promise<NodeAsyncFileSource> {
    const st = await stat(path);
    const fd = await open(path, "r");
    return new NodeAsyncFileSource(fd, st.size);
  }

  async read(offset: number, length: number): Promise<Uint8Array> {
    if (!Number.isInteger(offset) || !Number.isInteger(length) || offset < 0 || length < 0 || offset + length > this.size) {
      throw new RangeError(`read [${offset}, +${length}) outside source of size ${this.size}`);
    }
    const buf = Buffer.alloc(length);
    let done = 0;
    while (done < length) {
      const { bytesRead } = await this.fd.read(buf, done, length - done, offset + done);
      if (bytesRead === 0) throw new Error(`short read at ${offset + done}`);
      done += bytesRead;
    }
    return new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength);
  }

  async close(): Promise<void> {
    await this.fd.close();
  }
}

describe("IsoVolume on DETECTV1_1.ISO", () => {
  const isoPath = findIsoPath();

  it.skipIf(!isoPath)("lists root and reads BDETECT.EXE lazily", async () => {
    const src = await NodeAsyncFileSource.open(isoPath!);
    try {
      const vol = await IsoVolume.open(src);
      const root = await vol.list("/");
      expect(root.some((e) => e.name.toUpperCase() === "BDETECT.EXE" && !e.isDir)).toBe(true);

      const st = await vol.stat("/BDETECT.EXE");
      expect(st).toEqual({ size: 501248, isDir: false });

      const head = await vol.read("/BDETECT.EXE", 0, 2);
      expect(head[0]).toBe(0x4d);
      expect(head[1]).toBe(0x5a);

      // Span a 2048-byte ISO sector boundary within the file (offset 2040, length 16).
      const cross = await vol.read("/BDETECT.EXE", 2040, 16);
      expect(cross.length).toBe(16);
      const same = await vol.read("/bdetect.exe;1", 2040, 16);
      expect(cross).toEqual(same);
    } finally {
      await src.close();
    }
  });
});
