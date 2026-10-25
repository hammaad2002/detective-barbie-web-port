import { describe, expect, it } from "vitest";
import {
  canonicalSaveRelPath,
  entriesFromZipNames,
  isSaveBasename,
  zipEntriesForExport,
} from "../web/src/saves.ts";
import { makeZip, readZip } from "../web/src/zip.ts";

describe("Detective Barbie save ZIP", () => {
  it("recognises save basenames and canonical paths", () => {
    expect(isSaveBasename("SavGame0.ar1")).toBe(true);
    expect(isSaveBasename("CURRENT.AR2")).toBe(true);
    expect(isSaveBasename("GameHist.ory")).toBe(true);
    expect(isSaveBasename("BDETECT.EXE")).toBe(false);
    expect(canonicalSaveRelPath("nested/SavGame1.ar3")).toBe("SavGame1.ar3");
    expect(canonicalSaveRelPath("data/NewGame.db1")).toBe("data/NewGame.db1");
    expect(canonicalSaveRelPath("NewGame.ar1")).toBe("data/NewGame.ar1");
  });

  it("round-trips save bytes through makeZip and readZip", async () => {
    const files = [
      { name: "SavGame0.ar1", data: new Uint8Array([1, 2, 3, 4]), date: new Date("2026-01-02T12:00:00Z") },
      { name: "data/NewGame.db1", data: new Uint8Array([9, 8, 7]), date: new Date("2026-01-02T12:00:00Z") },
    ];
    const zip = makeZip(zipEntriesForExport(files));
    const entries = await readZip(zip);
    const map = entriesFromZipNames(entries);
    expect([...map.keys()].sort()).toEqual(["SavGame0.ar1", "data/NewGame.db1"]);
    expect(map.get("SavGame0.ar1")).toEqual(files[0].data);
    expect(map.get("data/NewGame.db1")).toEqual(files[1].data);
  });
});
