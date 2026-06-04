# SheepShaver macOS arm64 Changelog

Changes specific to the `macos-arm64` branch (fork of kanjitalk755/macemu).

## 2026-06-04

### Networking

- **VDE virtual networking** (backport of upstream `06d8bc02`): SheepShaver can now use
  a VDE switch for Ethernet. The destination VDE link is configured directly in the
  `ether` pref via a new `vde:` prefix (e.g.
  `--ether 'vde:cmd://ssh root@server vde_plug tap://tap0'`), so it persists with the
  rest of the prefs. Two correctness fixes in the shared `ether_unix.cpp` send path:
  outgoing packets now send the actual frame length (was `sizeof(packet)`, which
  appended trailing garbage), and the infinite `do {} while (len < 0)` send-retry was
  replaced with a proper `excessCollsns` error return. SheepShaver's `configure` gains
  `--with-vdeplug` (default yes) and an `AC_CHECK_LIB(vdeplug, vde_close)` probe that
  defines `HAVE_LIBVDEPLUG` and links `-lvdeplug` when the library is present (Homebrew
  `vde`, header `libvdeplug.h`). The bare `vde` ether pref (no destination) still works.
  Boot/packet-flow on real hardware is unverified by this change.

### JIT Correctness

- **AltiVec `vsel` fix**: `vsel` (vector select) emitted ARM64 `BSL` with its two
  source operands swapped — it computed `(vC & vA) | (vB & ~vC)` instead of PPC's
  `(vB & vC) | (vA & ~vC)`, returning vA wherever the mask bit was set. This would
  silently corrupt any AltiVec software that uses `vsel` (the emulator advertises a
  G4, so AltiVec is live). One-token operand swap; caught and regression-tested by
  a new differential vector.

- **AltiVec element-order bug — `vspltb`/`vsplth` FIXED, multiplies pending**:
  byte/halfword splats and the even/odd byte multiplies (`vmuloub`/`vmuleub`)
  selected the WRONG element. Root cause: the VR is stored in the interpreter's
  `ev_mixed` byte order (`byte_element(i)=(i&~3)+(3-(i&3))`, `half_element`
  similarly — bytes reversed *within* each word, word order preserved), and
  `emit_load_vr` (ppc-jit.cpp:779) loads it raw via `LDR Q`; the JIT then indexed
  NEON lanes with the raw PPC element. **Fixed** `vspltb`/`vsplth` by applying the
  `ev_mixed` remap to the DUP index (verified across indices 0/3/15 and 0/3/7).
  `vspltw` was already correct (word order preserved).

  **Blast radius mapped (differential probes, 2026-06-04):** the same ev_mixed
  mismatch breaks most sub-word-rearranging ops — CONFIRMED broken:
  `vmrghb`/`vmrglb`/`vmrghw`/`vmrglw` (merges), `vpkuhum` (pack), and the even/odd
  multiplies (`vmuloub`/`vmuleub`). CONFIRMED correct: `vsldoi` (shift), splats
  (now fixed), and the element-symmetric arith/logical/compare ops. The multiplies
  have an ADDITIONAL bug: `vmuloub` (case 8) emits `MUL.8B` (a non-widening byte
  multiply) instead of `UMULL.8H` — the comment lied. **Fix strategy** for the rest:
  a systematic option is `emit_load_vr`/`emit_store_vr` doing `LDR Q`+`REV32.16B` /
  `REV32.16B`+`STR Q` (converts ev_mixed↔natural so raw lanes work, but adds 2 ops
  per AltiVec op, requires reverting the splat remap, and re-verifying every op +
  a boot); the multiplies need that PLUS the correct widening instruction. Deep
  Phase-2 work. Repro vectors are in `gen-altivec-vectors.py`.

- **Harness integrity preflight added**: the SheepShaver `jit-test/run.sh` had NO
  self-validation (unlike BasiliskII's). Added a preflight that aborts on a
  malformed/missing/duplicate-name vector before running. On its first run it
  caught real pre-existing bugs — three **duplicate vector names** (`crand_basic`,
  `mcrf_basic`, `orc_basic`) where the second `T_` definition shadowed the first,
  so one vector of each pair never ran (silent lost coverage); fixed by renaming
  the shadowed ones (recovers the lost tests). Duplicate-hex vectors are warned
  (redundant, non-fatal). A true **vacuousness** guard (catching a vector whose
  result hides in an FPR/VR/memory) is still deferred — it needs a sentinel/
  mutation redesign; the gen-*-vectors.py generators are the practical defense.

### Testing & Benchmarking

- **18 real FP-arithmetic test vectors**: the pre-existing `fp_*` vectors were
  *vacuous* — they ended at `stfd` and never loaded the result into a GPR, but the
  harness REGDUMP captures GPRs, not FPRs, so a wrong FP result was invisible and
  the JIT-vs-interpreter diff passed trivially. FP arithmetic was effectively
  untested. The new vectors load the result back into a GPR (fadd/fsub/fmul/fdiv,
  the fma family, frsp/fctiwz/fneg/fabs/fmr, and single-precision forms). Generated
  by `jit-test/gen-fp-vectors.py` (documented, reproducible).

- **AltiVec test coverage was almost entirely fake**: an initial 15-vector batch
  (14 vacuous, doubled-XO no-ops) *and* all 12 pre-existing `vec_*` vectors were
  found vacuous — confirmed empirically (result GPRs read 0 in both modes). The
  cycle-1 "13/207 AltiVec covered" was illusory. Replaced with 13 correctly-encoded,
  verified-non-vacuous vectors (vsel, vspltw, the vand/vor/vxor/vnor/vadduwm/
  vsubuwm/vmaxsw/vminsw/vcmpequw arith-logical-compare set), via the documented
  `jit-test/gen-altivec-vectors.py`. Three rounds of vacuous/masking vectors slipped
  the harness — see the "no integrity guard" note in JIT Correctness below.

- **`make harness-count`**: single source of truth for the harness vector count,
  derived from `jit-test/run.sh` (the count had drifted across several docs). The
  *gate* references in the testing docs (TESTING.md, OPTIMIZATION-PLAN.md,
  CLAUDE.md, CONTRIBUTING.md) were de-hardcoded to reference it; dated historical
  snapshots in session logs and baseline tables are intentionally left as-is.

- **FP microbench kernels**: `make bench` gains `fp-add`/`fp-fma`. They measure the
  FPR store/load round-trip (the JIT has no FP register allocator), not raw FP-unit
  latency — useful as the baseline an FP register allocator would improve against.

### Documentation

- **Paranoia FP conformance**: concrete manual run steps documented in TESTING.md,
  with the honest caveat that automation needs a guest binary + boot.

- **IMPROVEMENT-CYCLE-1.md**: prioritized, collision-aware improvement plan from a
  multi-agent audit (read-only auditors → adversarial verification → synthesis).

## 2026-06-03

### Emulator Features

- **Clean shutdown**: Special > Shut Down now exits the host process cleanly,
  running all cleanup handlers (JIT miss report, cache free, SDL teardown).
  Previously the process hung with a black screen after the guest powered off.

- **Restart**: Deferred — requires deeper ROM reset state management. Special >
  Restart still has no effect (same as upstream).

- **Prefs: K/M/G suffixes**: Integer prefs now accept human-readable sizes
  (e.g., `ramsize 256M`). Also supports `0x` hex prefix via `strtol` base 0.
  Shared by both BasiliskII and SheepShaver (symlink).

- **Prefs: comments**: `#` and `;` line comments were already supported by the
  parser but undocumented. Now noted in CLAUDE.md and prefs examples.

- **Networking**: `ether slirp` provides outbound NAT networking (web, FTP)
  with built-in DHCP. No host configuration required.

- **JIT code cache sizing**: Default increased from 64 MB to 256 MB, eliminating
  recompilation churn during boot and app launch (previously 2+ full flushes per
  session).  Configurable via `jitcachesize` pref or `SS_JIT_CACHE_KB` env var.

- **Startup prefs readout**: SheepShaver now prints each loaded pref value on
  startup, showing what configuration is active.

### JIT Performance

- **Register allocator (RA)**: Re-enabled and fully converted. All 32-bit GPR
  accesses go through `ra_load`/`ra_store`, operating directly on ARM64
  x21-x28 registers — no MOV bounce through temporaries. Benchmark Mix
  improved **+15.6%** (548 → 634), Dhrystones **+9.4%**, CPU **+2.2%**.

- **TBZ bclr optimization**: Mixed Mode guard on function returns uses single
  `TBZ` instruction instead of `AND` + `CBZ` (1 instruction saved per `bclr`).

- **Atomic spcflags (P0d)**: Replaced spinlock-based spcflags with
  `std::atomic`, eliminating lock contention between the 60 Hz VBL timer
  and the JIT dispatch loop. CPU score 65.2 (new high).

- **Trailing MOV elimination (P0f)**: Carry, overflow, and immediate-carry ops
  (subfc, addc, addco, subfco, addo, subfo, nego, addze, subfze, addic, addic.,
  subfic) now compute ADDS/SUBS directly into the RA destination register,
  eliminating a redundant MOV per instruction.  Mix 638 (new high).

- **RA eviction test**: New `lmw_stmw_wide` harness vector exercises 12 live
  GPRs, forcing mid-block RA eviction. Harness now 236/236.

- **JIT miss report via atexit**: The opcode coverage histogram now prints
  reliably on process exit (via `atexit` hook), regardless of how the guest
  shuts down. Includes session wall-clock time.

- **JIT coverage**: 98.4% of compiled instructions run natively. The remaining
  1.6% (opc=19: bcctr/isync) accounts for 98.5% of all misses — making native
  bcctr the single highest-impact remaining optimization.

### JIT Correctness

- **RA flush on bclr bail path**: Added `ra_flush_all()` before the Mixed Mode
  bail epilogue — prevents dirty cached GPR values from being lost when bclr
  bails to the interpreter mid-block.

- **RA ordering constraint**: `ra_load` for source operands must precede
  `ra_store` for the destination. Otherwise `ra_store` allocates without
  loading the old value, and a subsequent `ra_load` for the same GPR (when
  rD == rA) returns uninitialized data.

- **gpr64 coherence warning**: Documented that `emit_load_gpr64` /
  `emit_store_gpr64` bypass the RA cache for the low word. Safe today (PPC64
  ops unreachable from 32-bit guests), must be fixed before G5 support.

### Testing & Benchmarking

- **`make test-jit`**: New target that runs the harness in JIT equivalence mode
  (`SS_HARNESS_MODE=jit`), actually testing JIT codegen against the interpreter.
  The old `make test-opcodes` only tested interpreter determinism.

- **`make bench` (jit-bench)**: Microbenchmark for fast A/B testing of codegen
  changes — reports ns/insn for targeted kernels (carry-chain, rc1/CR0, ALU).
  Supports `--save-baseline` / `--compare` for differential timing. No boot needed.

- **TESTING.md maintenance contract**: Freshness rules, per-change checklist,
  harness mode awareness. Prevents test/doc rot.

### Debug Output

- **Disk driver**: Suppressed per-poll DiskStatus flood for csDriverGestaltCode
  (status 43). Replaced two-line output with single-line format showing the
  four-char gestalt selector: `csDriverGestaltCode 43: 'flus'`.

### Documentation

- **OPTIMIZATION-PLAN.md**: Updated with completed items (RA, TBZ), new
  entries (lazy CR0, trailing-MOV elimination, cross-block register pinning,
  indirect bclr chaining), post-RA benchmark baseline, and miss-data-driven
  reprioritization of P2 (native bcctr).

- **USER-HANDBOOK.md**: New user guide covering prefs reference, networking
  setup, environment variables, benchmarking, and troubleshooting.

- **CHANGELOG.md**: This file — tracks user-visible changes per session.
