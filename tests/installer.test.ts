import { existsSync, mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { describe, expect, it } from "vitest";
import { install } from "../installer/src/install.ts";
import { DirSink, NodeFileSource } from "../installer/src/node.ts";

const ISO_CANDIDATES = [
  "/home/hammad/repos/detective-barbie-web-port/DETECTV1_1.ISO",
  "DETECTV1_1.ISO",
];

const EXPECTED_FILE_COUNT = 1349;

function findIsoPath(): string | undefined {
  for (const p of ISO_CANDIDATES) {
    if (existsSync(p)) return p;
  }
  return undefined;
}

describe("Detective Barbie ISO install", () => {
  const isoPath = findIsoPath();

  it.skipIf(!isoPath)("copies the game tree with size/CRC verification", async () => {
    const outDir = mkdtempSync(join(tmpdir(), "db-install-"));
    try {
      const src = new NodeFileSource(isoPath!);
      const report = await install(src, new DirSink(outDir), { verify: true });
      src.close();
      expect(report.mismatches).toEqual([]);
      expect(report.written).toBe(EXPECTED_FILE_COUNT);
      expect(report.verified).toBe(EXPECTED_FILE_COUNT);
      expect(report.skipped).toBe(0);
    } finally {
      rmSync(outDir, { recursive: true, force: true });
    }
  });
});
