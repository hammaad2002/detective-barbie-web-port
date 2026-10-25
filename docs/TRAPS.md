# BDETECT.EXE — recompiler lift coverage and traps

Reference executable: `work/ref/BDETECT.EXE` (PE32, SHA-256 prefix `b56d1981…`).

## How the toolchain runs

`recomp/src/cli.ts` exposes three commands:

| Command | Role |
|---------|------|
| `gen-c` | Discover functions (optional `--hints`), lift every function to IR, emit C under `--out`. Prints unsupported-instruction trap counts. |
| `metadata` | Same discovery as `gen-c`, writes address-only `ExeMetadata` JSON (`entries`, `noreturn`, `jumpTargets`). `--hints` optional. |
| `gen-wasm` | Loads metadata (`--meta`), calls `translateExe()` in `recomp/src/translate.ts`: rediscover with `speculate: false`, lift all functions, emit `game.wasm`. |

`translate.ts` does not decode game logic itself; it wires PE load, CFG discovery from metadata, per-function `liftFunction()`, and WASM encoding. Ghidra hints are not required for either path.

## What the lifter (`lift.ts`) generally supports

- **Integer GPR**: arithmetic, logic, shifts (including `shld`/`shrd`), bit tests, `cmpxchg`, `xadd`, flag ops, `bswap`, `leave`/`enter` (level 0 only).
- **Control flow**: calls, branches, tail calls, switch jump tables (invalid indices become runtime traps).
- **Stack**: 32-bit `push`/`pop`, `pushad`/`popad`, `pushf(d)`/`popf(d)`.
- **Segments**: `mov` and `les`/`lds`/`lfs`/`lgs`/`lss` for ES/DS/FS/GS/SS; **GS** loads trap.
- **Strings**: `movs*`/`stos*`/`cmps*`/`scas*` (including `rep`/`repe`/`repne`); **`rep lods`** traps.
- **x87 FPU**: common stack ops, `fild`/`fst*`, transcendentals via runtime helpers, `fsincos`, control-word save/load to memory.
- **SSE / MMX**: broad scalar and packed coverage (mov/compare/convert, many `p*` integer and `*ps`/`*pd` float ops, `shufps`, etc.); unimplemented SIMD mnemonics fall through to `no semantics for …`.
- **Misc**: `cpuid` (runtime helper), `stmxcsr`/`ldmxcsr`, `ud2`/`int3` as explicit traps, `emms`/`femms` as no-ops.

Anything that hits `trapHere()` or the default `no semantics for …` becomes an `unsupported …` IR trap; the C emitter still generates a function body that calls the runtime trap helper if that path is reached.

## Commands used on BDETECT.EXE

No Ghidra hints file exists yet; discovery uses the PE entry point plus speculative code-pointer scan (`gen-c`) or the metadata entry list plus callee closure (`gen-wasm`).

```bash
# Primary trap survey (lift + C emit)
node recomp/src/cli.ts gen-c work/ref/BDETECT.EXE --out /tmp/bdetect-gen-c

# WASM path (metadata written to allowed draft file, output under /tmp)
node recomp/src/cli.ts metadata work/ref/BDETECT.EXE --out metadata/b56d1981-draft.json
node recomp/src/cli.ts gen-wasm work/ref/BDETECT.EXE --meta metadata/b56d1981-draft.json --out /tmp/bdetect-wasm
```

Neither run crashed.

## Function counts

| Path | Discovery | Functions lifted | Notes |
|------|-----------|------------------|-------|
| `gen-c` | `speculate: true` | **1585** | Matches speculative scan in `cli.ts`. |
| `gen-wasm` | `speculate: false` + metadata closure | **1605** | Metadata lists **1585** entry VAs; closure adds **20** callees only reachable when seeding from the full entry set. |

## Unsupported-instruction traps (BDETECT)

CLI summary from `gen-c`:

```
unsupported-instruction traps: 6
```

Manual recount over the same discovery: **6** trap statements in **3** functions (`0x428730`, `0x428930`, `0x4289c0`). All sit in a small cluster near `0x428744`–`0x428a2f` (typical DOS/BIOS thunk style: port I/O, `int 1Ah`, timing, 16-bit stack ops).

### Trapping mnemonics (unsupported lifts only)

| Category | Instruction pattern | Count | Example VA | Lifter reason |
|----------|---------------------|------:|------------|---------------|
| Port I/O | `out imm8, al` | 1 | `0x00428744` | `no semantics for Out` |
| Port I/O | `in al, imm8` | 1 | `0x00428748` | `no semantics for In` |
| Software interrupt | `int imm8` (`int 1Ah`) | 1 | `0x0042875c` | `no semantics for Int` |
| Time stamp | `rdtsc` | 1 | `0x00428960` | `no semantics for Rdtsc` |
| Stack (16-bit) | `push ax` | 1 | `0x00428a0a` | `16-bit push` |
| Stack (16-bit) | `push cx` | 1 | `0x00428a2f` | `16-bit push` |

**FPU / SSE / MMX / string / segment**: no unsupported-instruction traps were reported for this executable under the current discovery set. (String `rep` blocks and x87/SSE paths in the main game code lift successfully; traps above are isolated to the thunk region.)

### Other IR traps (not counted in CLI “unsupported-instruction” line)

A separate pass found **51** additional trap *sites* in lifted IR, mostly **jump-table default** terminators (`switch index out of range at …`) and one **no-return call returned** terminator. These are control-flow safety nets, not missing decoders. CFG also records `int 1Ah` at `0x42875c` as a block terminator when decode treats it as ending a block.

## WASM translation

`gen-wasm` completed: **4.1 MB** module, **12** irreducible loops, discovery ~490 ms, translation ~403 ms.

## Metadata draft

`metadata/b56d1981-draft.json` was generated by the `metadata` command (format 1, full `entries` / `noreturn` / `jumpTargets`). It is **not** a Ghidra hints file; empty hints were not required because `--hints` is optional on all CLI commands.

## Minimal cause if a trap fires at runtime

Hitting any `unsupported …` site calls the runtime trap path with the embedded message (e.g. port I/O or `rdtsc` in the `0x4287xx` thunks). Fixing BDETECT playability likely means either stubbing those DOS services in the runtime or replacing/removing that thunk cluster—not extending the general SSE/MMX/string lifter for this binary.

## After lifter fixes

```bash
node recomp/src/cli.ts gen-c work/ref/BDETECT.EXE --out /tmp/bdetect-gen-c
```

```
unsupported-instruction traps: 0
```
