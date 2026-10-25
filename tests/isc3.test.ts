import { existsSync } from "node:fs";
import { describe, expect, it } from "vitest";
import { Isc3Cabinet } from "../installer/src/isc3.ts";
import { IsoImage } from "../installer/src/iso9660.ts";
import { NodeFileSource } from "../installer/src/node.ts";

const ISO_CANDIDATES = [
  "DETECTV1_1.ISO",
];

const isoPath = ISO_CANDIDATES.find((p) => existsSync(p));

describe.skipIf(!isoPath)("InstallShield 3 DATA1.CAB", () => {
  it("inflates every listed file to its declared size", () => {
    const image = new IsoImage(new NodeFileSource(isoPath!));
    const entry = image.lookup("DATA1.CAB");
    expect(entry).toBeDefined();
    const cabBytes = image.open("DATA1.CAB").read(0, entry!.size);
    const cab = new Isc3Cabinet(cabBytes);
    const files = cab.list();
    expect(files.length).toBeGreaterThan(0);
    for (const f of files) {
      const data = cab.extract(f.name);
      expect(data.length).toBe(f.size);
    }
  });
});
