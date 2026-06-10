# Tactical Bug Fix & Feature Plan

> **Status:** ⏸ Partially superseded · **Created:** 2026-06-08 · **Updated:** 2026-06-10
> **Why this doc exists:** Compiled tactical bug fix suggestions and strategic optimizations for immediate and mid-term focus.
>
> **⚠️ Note (2026-06-10):** Pillar 1 ("Break the OS Ceiling: jump68k handoff") is superseded
> by the **[Machine Layer](MACHINE-LAYER-PLAN.md)** — the 9.0.1 ROM now boots via proper
> device models (M0+M1 complete), not HLE shim porting. Pillars 2–5 (CopyBits HLE,
> BasiliskII build, perf optimization, documentation) remain independently valid.
> _Markers: ✅ done · 🟡 in progress · ⏸ blocked/deferred · ☐ todo. Finished an item? Flip its marker, bump **Updated**, and add a `CHANGELOG.md` entry (see [CONTRIBUTING](../../CONTRIBUTING.md) → "Documentation Lifecycle")._

This document aligns prioritized tactical fixes with the project's five core strategic recommendations.

---

## The Five Strategic Pillars (High Priority)

### ☐ 1. Break the OS Ceiling: Prioritize the `jump68k` Handoff
- **Scope:** Redirect the New World parcels PPC→68K handoff to the emulator's entry point.
- **Files to watch:**
    - `SheepShaver/src/rom_patches.cpp`: Locate the `jump68k` entry point in the parcels ROM.
    - `SheepShaver/src/emul_op.cpp`: Verify the `Execute68kTrap` and handoff logic.
- **Technical Guidance:** The nanokernel has reached instruction 128 but stalls at the 68K transition. This is the **forcing function** for supervisor-level SPRG and MMU state (SDR1/HTAB).
- **ROI:** Unlocks Mac OS 9.1/9.2.2 support.

### ☐ 2. Attack the "Worst Layer": `CopyBits` HLE
- **Scope:** Implement Selective HLE for `CopyBits` and `BlockMoveData`.
- **Files to watch:**
    - `src/gfxaccel.cpp`: Hook `_CopyBits` (trap `0xA8EC`).
    - `src/emul_op.cpp`: Handle the NativeOp transition.
- **Technical Guidance:** Currently, `CopyBits` runs through a triple-layer stack (PPC JIT → 68K DR Emulator → PPC JIT). A native NEON/ARM64 blitter hook provides the largest perceived performance win.
- **ROI:** Massive throughput boost for graphics-heavy workloads.

### ☐ 3. Restore the 68K Lineage: BasiliskII Interpreter Build
- **Scope:** Restore a functional **Interpreter-only** build on macOS arm64.
- **Files to watch:**
    - `BasiliskII/src/Unix/main_unix.cpp`: Fix Linux-isms in signal handling and memory mapping.
    - `BasiliskII/src/Unix/configure.ac`: Ensure `--disable-jit` produces a clean, functional binary.
- **Technical Guidance:** 68K is fast enough on M-series chips without a JIT. This restores the 68K lineage (System 7.x) without the complexity of a JIT port.
- **ROI:** Reclaims "full classic Mac history" identity for the repo.

### ☐ 4. Close the Safety Loop: AltiVec Context Switching
- **Scope:** Implement proper VR (Vector Register) context save/restore.
- **Files to watch:**
    - `src/kpx_cpu/src/cpu/ppc/ppc-cpu.cpp`: Update exception/interrupt handlers to spill/fill VR0-VR31.
    - `src/kpx_cpu/src/cpu/ppc/ppc-execute.cpp`: Enable `MSR[VEC]` (bit 0x02000000) modeling.
- **Technical Guidance:** Required for safe multitasking. Without this, concurrent AltiVec apps will corrupt each other's state.
- **ROI:** Makes AltiVec production-ready and removes the `altivec` opt-in requirement.

### ☐ 5. Unified Verification: JSONL "Test Session" Schema
- **Scope:** Implement the Unified Test Session schema for all diagnostics.
- **Files to watch:**
    - `tools/jit-analyze.py`: Update to consume JSONL.
    - `src/kpx_cpu/src/cpu/jit/aarch64/ppc-jit.cpp`: Add JSONL emitters for JIT stats/divergences.
- **Technical Guidance:** Centralize divergences, benchmarks, and profiles into a single run-stamped record. Enables automated "refereeing" of bugs via `SS_TEST_HEX`.
- **ROI:** Dramatically reduces triage time and human-in-the-loop overhead.

---

## Tactical & Technical Debt

### ☐ 6. Complete MMU Modeling (Prerequisite for Pillar 1)
- **Scope:** SDR1/HTAB page-table init.
- **Guidance:** See `docs/planning/HANDOFF-NEWWORLD-SUPERVISOR-MMU.md`. Add `sdr1` to `PPCRegs` and stop dropping `mtspr SDR1` writes.

### ☐ 7. Improve FP/AltiVec Test Coverage
- **Scope:**
    - Regenerate arithmetic word-op vectors with distinct AND carry-inducing operands.
    - Add variable-shift family (where NEON ≠ AltiVec 1:1).
- **ROI:** Catches silent codegen bugs before they ship.

### ☐ 8. Phase 4 Optimizations (Strategic)
- **Scope:** Per-block overhead ceiling (prologue reduction), Cross-block pinning, P-VRA.
- **Gate:** Must pass Phase-2 benchmarks (regression check).

### ☐ 9. Complete Vacuousness Guard
- **Scope:** Deep tier — sentinel/mutation harness for memory-only results.
- **Status:** Shallow tier (NOP check) is already live.

---

## Further Investigations: Suspect Code & Coherence Audit

### ☐ 12. GPR64 RA Coherence Hole
- **Scope:** `emit_load_gpr64` / `emit_store_gpr64` (used by PPC64 doubleword ops like `ld`, `std`, `sld`) read/write the `PPCRegs` struct directly, bypassing the Register Allocator's low-word cache.
- **Risk:** High (if PPC64 code runs). A 32-bit op leaves a dirty low word in a host register; a subsequent 64-bit read sees the stale struct word.
- **Action:** Route the low-word through `ra_load`/`ra_store` once a PPC64/G5 path is active.
- **Files:** `src/kpx_cpu/src/cpu/jit/aarch64/ppc-jit.cpp` (~L823).

### ☐ 13. Vestigial PPC64 FP Conversions (`fctid`, `fcfid`)
- **Scope:** The JIT implements `fctid`, `fctidz`, and `fcfid` (64-bit conversions), but the interpreter treats them as illegal instructions on the default 7400 CPU.
- **Risk:** Divergence. If a guest issues these, the JIT will execute a "fake" 64-bit conversion while the interpreter will crash/illegal-op.
- **Action:** Either finish real 64-bit/G5 emulation or make these JIT cases return `false` to match the interpreter's illegal-op behavior.
- **Files:** `src/kpx_cpu/src/cpu/jit/aarch64/ppc-jit.cpp` (~L4517).

### ☐ 14. `unsigned long` & Pointer Casting Audit
- **Scope:** Widespread use of `unsigned long` (64-bit on macOS) for `uint32` and pointers, often combined with explicit `(uint32)` casts.
- **Risk:** Truncation bugs or mismanaged `NATMEM_OFFSET` arithmetic.
- **Action:** Audit `vm.hpp` and `sigsegv.cpp` for potential 64-to-32-bit truncation in address calculations.
- **Technical Note:** The project relies on `host = 0x400000000000 + guest`. Ensure all guest addresses are explicitly promoted to `uint64_t` before addition.

### ☐ 15. SDR1 "0xdead001f" Sentinel Propagation
- **Scope:** The nanokernel uses SDR1 to derive KDP and HTAB.
- **Risk:** If any code dereferences the `0xdead0000` base without our specific `ignoresegv` or Trampoline backing, it will stall.
- **Action:** Replace all `0xdead001f` sentinels with real, RAM-backed register logic (Tracked in Pillar 1/Item 6).
