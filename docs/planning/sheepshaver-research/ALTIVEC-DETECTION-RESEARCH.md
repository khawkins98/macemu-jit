# AltiVec Detection & Validation under SheepShaver — research note

**Status:** ✅ Resolved 2026-06-07. Real-app AltiVec achieved (Fractal Carbon, `[JIT-COMPILED-MIX]
AltiVec=160`). This note captures the research that found the bug behind the false start, plus the
ranked options for hardening/validation. See ROADMAP §B5 and LEARNINGS (2026-06-07).

## The decisive finding (and the bug it caught)

A research subagent, cross-checking primary sources, caught that **`SS_FORCE_ALTIVEC` was writing the
wrong gestalt bit**:

- `gestaltPowerPCHasVectorInstructions` is a **bit NUMBER = 4** (Apple CarbonCore `Gestalt.h`,
  verified via WebFetch 2026-06-07) → vector mask = `1 << 4 = 0x10`.
- The force wrote `0x40` = `1 << 6` = `gestaltPowerPCHas64BitSupport`. It also tested `& 0x40` on
  readback, so the experiment self-confirmed a wrong bit.
- Consequence: Fractal Carbon tests bit 4, saw it clear, reported "(not detected)" — and the earlier
  "FC uses a non-gestalt probe / 0 AltiVec" conclusion was **confounded, not real**.

Fix `0x40` → `0x10`, re-run: FC detects AltiVec via `Gestalt('ppcf')`, takes its vector path, and the
AArch64 JIT compiles+runs the PPC AltiVec instructions (`AltiVec=160`, multiple AltiVec hot blocks at
~74M executions each). **FC DOES use gestalt bit 4; no NewWorld ROM port needed for AltiVec.**

`gestaltPowerPCProcessorFeatures` bit map (Apple Gestalt.h, for reference):

| bit | constant | mask |
|----|----------|------|
| 0 | gestaltPowerPCHasGraphicsInstructions | 0x01 |
| 1 | gestaltPowerPCHasSTFIWXInstruction | 0x02 |
| 2 | gestaltPowerPCHasSquareRootInstructions | 0x04 |
| 3 | gestaltPowerPCHasDCBAInstruction | 0x08 |
| **4** | **gestaltPowerPCHasVectorInstructions** | **0x10** |
| 5 | gestaltPowerPCHasDataStreams | 0x20 |
| 6 | gestaltPowerPCHas64BitSupport | 0x40 |
| 7 | gestaltPowerPCHasDCBTStreams | 0x80 |

> We set ONLY 0x10 (vector) for the experiment — deliberately not `0x3F`, to avoid advertising features
> the emulated 7400 doesn't have (e.g. bit 2 sqrt → apps could issue `fsqrt`, which the interpreter
> doesn't implement). A realistic 7400 `'ppcf'` value is a follow-up if we ship this as a real pref.

## Other corrections from the research

- **`mfmsr` returns `0xf072` → MSR[VEC] (0x02000000) is CLEAR**, not set. Both interp
  (`ppc-execute.cpp:1180`) and JIT (`ppc-jit.cpp:2719`) hard-code `0xf072`. The prior-session
  "mfmsr reads SET / 0x0200f072" note was wrong. Irrelevant to FC (gestalt path), but a latent
  correctness gap (a real 7400 sets VEC) — option B2 below.
- The exception-guarded "try one vector op" probe theory is **disfavored**: SheepShaver executes vector
  ops cleanly (no MSR[VEC] gate, no 0xF20 path), so such a probe would conclude PRESENT, not absent.

## Ranked options (for hardening / future work)

**Make detection real & shippable**
- **B1 (done, experimental):** register `'ppcf'` with the vector bit via `_NewGestalt $A3AD`
  (`SS_FORCE_ALTIVEC`). Decide whether to promote to a real opt-in pref. Caveat: Mac OS 8.6/9.0 in this
  OldWorld environment don't do VR context save/restore on task switch — weigh multitasking safety
  before making it default-on. (Single-app compute like FC is fine.)
- **B2 (cheap correctness):** fix `mfmsr` → `0x0200f072` (set MSR[VEC]); gate on `make test-jit` green.
  Real 7400 behavior; low priority since no observed consumer.

**Validate "AltiVec codegen runs" without a finicky app / without booting** (the user's question)
- **C2 / D (already exists):** `make test-jit` — ~349 interp-vs-JIT vectors incl. a dedicated AltiVec
  block — is the headless, no-boot validation of record. This is the answer to "confirm without booting."
- **C1 (low effort, no boot):** extend `rom-harness/` (already JITs arbitrary streams headlessly) with a
  crafted AltiVec instruction stream for a tighter end-to-end check in real ROM-addressed memory.
- **C3 (now achieved via FC):** a booted real app issuing AltiVec, verified by `[JIT-COMPILED-MIX]`.

## Sources
Apple CarbonCore `Gestalt.h` (gestaltPowerPCHasVectorInstructions = 4); Mac OS 8/9 Gestalt Manager
docs; Dauger Research AltiVec Fractal Carbon demo; PowerPC AltiVec PEM; verified in-tree against
`src/emul_op.cpp`, `ppc-execute.cpp:1180`, `ppc-jit.cpp:2719`, `main_unix.cpp:447`.
