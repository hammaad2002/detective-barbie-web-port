/**
 * Lift coverage for instructions that rarely appear in Win32 game code but exist in BDETECT thunks.
 */
import { readFileSync } from "node:fs";
import { describe, expect, it } from "vitest";
import { Program } from "../../recomp/src/cfg.ts";
import { liftFunction } from "../../recomp/src/lift.ts";
import { PeImage } from "../../recomp/src/pe.ts";
import type { IrFunc, Stmt } from "../../recomp/src/ir.ts";

const EXE = "work/ref/BDETECT.EXE";

function unsupportedTraps(ir: IrFunc): Stmt[] {
  const out: Stmt[] = [];
  for (const b of ir.blocks.values()) {
    for (const s of b.stmts) {
      if (s.k === "trap" && s.msg.startsWith("unsupported")) out.push(s);
    }
  }
  return out;
}

function hasHelper(ir: IrFunc, fn: string): boolean {
  for (const b of ir.blocks.values()) {
    for (const s of b.stmts) {
      if (s.k === "helper" && s.fn === fn) return true;
    }
  }
  return false;
}

describe("BDETECT thunk lifter coverage", () => {
  const pe = new PeImage(new Uint8Array(readFileSync(EXE)));
  const prog = new Program(pe, {});
  prog.discover({ speculate: true });

  it("lifts CMOS/BIOS thunk 0x428730 without unsupported traps", () => {
    const f = prog.funcs.get(0x428730);
    expect(f).toBeDefined();
    const ir = liftFunction(prog, f!, () => -1);
    expect(unsupportedTraps(ir)).toEqual([]);
    expect(hasHelper(ir, "rt_port_io")).toBe(true);
  });

  it("lifts RDTSC helper 0x428930 without unsupported traps", () => {
    const f = prog.funcs.get(0x428930);
    expect(f).toBeDefined();
    const ir = liftFunction(prog, f!, () => -1);
    expect(unsupportedTraps(ir)).toEqual([]);
    expect(hasHelper(ir, "rt_rdtsc")).toBe(true);
  });

  it("lifts 16-bit push/pop flags thunk 0x4289c0 without unsupported traps", () => {
    const f = prog.funcs.get(0x4289c0);
    expect(f).toBeDefined();
    const ir = liftFunction(prog, f!, () => -1);
    expect(unsupportedTraps(ir)).toEqual([]);
  });
});
