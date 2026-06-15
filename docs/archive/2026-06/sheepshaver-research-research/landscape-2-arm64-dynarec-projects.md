> **ARCHIVED 2026-06-12** — Moved to archive during the Reku pass.
> May contain outdated assumptions, resolved questions, or superseded plans.
> Current state: `docs/HANDOFF.md` · `docs/planning/ROADMAP.md`
> **Reason:** historical research
>

# Landscape 2: ARM64 JIT / Dynarec Projects (beyond Dolphin)

> **Status:** 📖 Reference / archive · **Created:** 2026-06-02 · **Updated:** 2026-06-04
> **Why this doc exists:** Landscape survey of ARM64 dynarec projects beyond Dolphin.
> _Markers: ✅ done · 🟡 in progress · ⏸ blocked/deferred · ☐ todo. Finished an item? Flip its marker, bump **Updated**, and add a `CHANGELOG.md` entry (see [CONTRIBUTING](../../../../CONTRIBUTING.md) → "Documentation Lifecycle")._


Survey of ARM64 dynamic-recompiler projects we had **not** covered in
`EMULATOR-RESEARCH-LEADS.md` (which focused on Dolphin, plus QEMU/RPCS3/Cemu/Xenia/PPSSPP
as contrasts). Goal: techniques and license-compatible code we could borrow for the
SheepShaver PPC→ARM64 JIT (`ppc-jit.cpp`). Compiled 2026-06-02.

SheepShaver is **GPLv2**. Borrowability test below is against that.

> **Verification note:** repo existence, license, and file paths were checked against the
> live GitHub repos / API on 2026-06-02 (file SPDX headers read directly where it mattered).
> Technique descriptions are from project docs/source and should still be re-read in-source
> before porting.

---

## Ranked by relevance to our PPC→ARM64 JIT

| Rank | Project | Guest | License | Borrowable? | Why it ranks here |
|------|---------|-------|---------|-------------|-------------------|
| 1 | **MAME PPC DRC** | PPC603/604/750 | **BSD-3-Clause** (per-file) | **Yes — vendorable** | Only non-Dolphin project with *PPC-specific* ARM64 dynarec in permissively-licensed C++. Closest guest match we have. |
| 2 | **oaknut** | (emitter only) | **MIT** | **Yes — vendorable** | Modern header-only AArch64 emitter; cleaner candidate than Dolphin's `Arm64Emitter` to supplement our hand-written emitter. `DualCodeBlock` = a W^X technique. |
| 3 | **dynarmic** | ARM (A32/A64) | **0BSD** | **Yes — vendorable** | Most polished standalone ARM64-host JIT lib. `reg_alloc.cpp`, `fastmem.h`, `DualCodeBlock` W^X via oaknut. Reference for a *register cache* (the thing our memory-resident GPRs lack). |
| 4 | **Box64** | x86_64 | **MIT** | **Yes — vendorable** | Mature deferred-flag state machine on ARM64 NZCV + dynablock chaining. Flag model is the most directly applicable idea (our XER CA / CR work). |
| 5 | **FEX-Emu** | x86 / x86_64 | **MIT** | Yes, but heavy | IR-based; `RedundantFlagCalculationElimination` pass is the canonical "deferred flags as an IR pass" design. Architecture-doc value > code value for us. |
| 6 | **Ryujinx ARMeilleure** | ARM64/ARM32 | MIT (C#) | Study-only (C#) | LSRA register allocator + host-mapped "fastmem" memory model. Concepts only — C#, can't link. |
| 7 | **Rosetta 2** | x86_64 | Proprietary | Study-only | What Apple Silicon HW gives translators: NZCV PF/AF extension, TSO mode. Informs what we *can't* match in pure software. |

---

## 1. MAME PowerPC DRC + ARM64 backend  — TOP LEAD

- **Repo:** github.com/mamedev/mame
- **License:** repo-level shows `NOASSERTION`, but the two files that matter both carry
  `// license:BSD-3-Clause` SPDX headers (verified): `drcbearm64.cpp` (copyright windyfairy,
  Vas Crabb) and `powerpc/ppcdrc.cpp` (copyright Aaron Giles). BSD-3-Clause is
  **GPLv2-compatible → vendorable** into SheepShaver (with attribution retained).
- **ARM64 relevance:** MAME's UML (Universal Machine Language) IR has a real **ARM64 DRC
  backend** (`src/devices/cpu/drcbearm64.cpp` / `.h`), and the PowerPC core
  (`src/devices/cpu/powerpc/`) targets PPC603/604/750 — **our exact CPU family** — through
  that backend via `ppcdrc.cpp` + `ppcfe.cpp` (front-end decode) + `ppccom.cpp` (shared state).
- **Files worth study:**
  1. `src/devices/cpu/powerpc/ppcdrc.cpp` — how a PPC603/604 is lowered to UML, incl. CR/XER
     update semantics, SPR access, and which ops punt. **Correctness + structure oracle that
     is closer to us than Dolphin** (Dolphin is 750-only Gekko; MAME covers 603/604 too).
  2. `src/devices/cpu/drcbearm64.cpp` — UML→ARM64 emission: register mapping, flag lowering,
     branch/block-exit emission. A second, independent, BSD-licensed ARM64 emitter+lowering.
  3. `src/devices/cpu/powerpc/ppcfe.cpp` — front-end: per-instruction flag/register read-write
     descriptors. This is exactly the dependency metadata a deferred-flag pass needs.
- **What we could take:** (a) `ppcdrc.cpp` as a *PPC-semantics cross-check* alongside QEMU,
  closer to our guest than Dolphin; (b) the `ppcfe.cpp` read/write descriptor model as a
  blueprint if we ever add a flag-liveness analysis to drop redundant CR0/XER writes;
  (c) BSD license means snippets of `drcbearm64.cpp` encodings are copy-able, not just
  readable. **Verdict: study first (ppcdrc + ppcfe), vendor opportunistically.**

## 2. oaknut — modern header-only AArch64 emitter

- **Repo:** github.com/merryhime/oaknut · **License:** **MIT** (vendorable).
- **ARM64 relevance:** Header-only C++20 AArch64 assembler. `CodeGenerator` / `VectorCodeGenerator`
  classes, `code.MOV(...)`, `code.RET()` style. Covers ARMv8.0–8.3 incl. FP/SIMD (no SVE).
- **Files/techniques:**
  1. `include/oaknut/oaknut.hpp` — the emitter. Self-contained, no Dolphin-style dependency
     tail (`CodeBlock.h`, `BitSet.h`, `Assert.h`…) that made cribbing `Arm64Emitter` costly.
  2. `include/oaknut/code_block.hpp` + `dual_code_block.hpp` — **`DualCodeBlock` maps the same
     physical code twice: one RW view, one RX view.** Emit through RW, execute through RX — no
     per-emit `pthread_jit_write_protect_np` toggling at all.
  3. `VectorCodeGenerator` → emit to `std::vector<u32>` (offline/test, no exec mapping).
- **What we could take:** Two distinct wins. (a) If we ever want to retire hand-rolled encoders
  in `ppc-jit.cpp`, oaknut is a cleaner drop-in than Dolphin's `Arm64Emitter` (MIT, no tail).
  (b) **`DualCodeBlock` is a real alternative to our W^X toggle dance.** macOS `MAP_JIT`
  historically resisted dual RW/RX mapping of one region, so this needs a spike to confirm it
  works under `MAP_JIT` on Apple Silicon — but if it does, it removes the toggle entirely
  (supersedes the "nesting counter" idea from Lead 2). **Verdict: spike DualCodeBlock under
  MAP_JIT; adopt emitter only if we decide to stop hand-encoding.**

## 3. dynarmic — standalone ARM-guest JIT library

- **Repo:** github.com/merryhime/dynarmic moved; canonical mirror **github.com/yuzu-mirror/dynarmic**.
  · **License:** **0BSD** (verified) — maximally permissive, vendorable.
- **ARM64 relevance:** Library JIT used by Citra/yuzu. Has a full **ARM64 host backend** under
  `src/dynarmic/backend/arm64/` that **emits via oaknut** (vendored in `externals/oaknut`).
- **Files/techniques:**
  1. `src/dynarmic/backend/arm64/reg_alloc.cpp` / `.h` — a compact, modern **register
     allocator** over an IR. Our JIT keeps all guest GPRs memory-resident (no reg cache); this
     is the reference design for the single biggest structural upgrade available to us, and the
     prerequisite the Dolphin CR/carry leads were blocked on.
  2. `src/dynarmic/backend/arm64/fastmem.h` + `a64_address_space.cpp` — host-MMU fastmem with
     SIGSEGV backpatching; cross-checks Dolphin Lead 6 with a smaller, license-clean codebase.
  3. `src/dynarmic/backend/arm64/emit_arm64.cpp` — IR-op → oaknut emission dispatch; a clean
     example of "small IR + emitter" architecture.
- **What we could take:** `reg_alloc.cpp` as the model for adding a per-block register cache
  (0BSD = copy freely). It also validates oaknut as production-grade. **Verdict: primary
  reference for register allocation; pairs with oaknut adoption.**

## 4. Box64 — x86_64→ARM64, deferred-flag state machine

- **Repo:** github.com/ptitSeb/box64 · **License:** **MIT** (verified) — vendorable.
- **ARM64 relevance:** Userspace x86_64→ARM64 (also RISC-V/LoongArch). Dynarec under
  `src/dynarec/arm64/`; blocks ("dynablocks") in `src/dynarec/dynablock.c`, chaining via
  `dynarec_arm64_jmpnext.c` / `arm64_next.S`.
- **Technique — deferred/lazy flags (the borrowable idea):** Box64 does **not** compute x86
  flags eagerly. It stores operation metadata (`op1`/`op2`/`res` via `UFLAG_OP12()` macros) and
  a per-instruction state (`status_none` / `status_set` / `status_unk` / `status_none_pending`).
  Flags are reconstructed by `const_updateflags_arm64` **only when a later instruction actually
  reads them** (`READFLAGS`/`GRABFLAGS`). Crucially it also tracks `need_nat_flags` /
  `before_nat_flags` — when the consumer is itself a flag-condition, it leaves the result in the
  **host ARM64 NZCV register** and skips materializing x86 flags entirely (`CFINV`/`MRS/MSR nzcv`
  to fix up carry polarity). Files: `dynarec_arm64_helper.h` (macros), `dynarec_arm64_functions.c`.
- **What we could take:** This is the production-grade version of Dolphin Lead 3 (lazy carry)
  generalized across all flags, and it's MIT. The *liveness-driven* "only set NZCV if a later
  in-block op consumes it" model maps directly onto our XER CA chains and CR0 record-form
  pressure. **Verdict: the most directly applicable flag-handling reference; study the
  `status_*` state machine before attempting our own lazy-CR0/CA within blocks.**

## 5. FEX-Emu — IR-based x86→ARM64

- **Repo:** github.com/FEX-Emu/FEX · **License:** **MIT** (verified).
- **ARM64 relevance:** Mature x86/x86_64→ARM64. IR-centric. Layout (from `docs/SourceOutline.md`):
  IR in `FEXCore/Source/Interface/IR/` (defined via `IR.json`); ARM64 backend in
  `FEXCore/Source/Interface/Core/JIT/` (`JIT.cpp` = "arm64 splatter backend" glue, plus
  `ALUOps.cpp`/`VectorOps.cpp`/`MemoryOps.cpp`); RA in
  `FEXCore/Source/Interface/IR/Passes/RegisterAllocationPass.cpp`.
- **Technique:** `FEXCore/Source/Interface/IR/Passes/RedundantFlagCalculationElimination.cpp`
  — the canonical "deferred flags as a dead-code-elimination IR pass": flag computations whose
  results are never read before being overwritten are deleted. Flag generation lives in
  `OpcodeDispatcher/Flags.cpp`.
- **What we could take:** Mostly **architectural**. We have no IR, so FEX's passes aren't
  drop-in. Value: the documented proof that the right home for flag-elimination is an IR pass,
  not ad-hoc emit-time checks — relevant if we ever grow even a tiny per-block IR. **Verdict:
  read `docs/` + RedundantFlagCalculationElimination for design rationale; not a code source
  for our hand-written, IR-less emitter.**

## 6. Ryujinx ARMeilleure — ARM64-guest JIT (C#)

- **Repo:** archived (Ryujinx/Ryujinx gone; mirrors at yuzu-mirror, code.relms.dev, etc.).
  · **License:** MIT (project) — but **C#, so study-only** (can't link into a C++ emulator).
- **ARM64 relevance:** Translates ARM64/ARM32 guest → x86-64 **or** ARM64 host. Pipeline:
  Decode → IR → SSA/opt → **LSRA register allocation** → codegen
  (`ARMeilleure/CodeGen/RegisterAllocators/`, `CodeGen/Arm64/`).
- **Techniques:** (a) classic **linear-scan register allocator** with live intervals — a second,
  well-documented RA reference alongside dynarmic's; (b) **host-mapped / "hardware-accelerated"
  memory**: guest address space mapped via host MMU + signal handlers so guest loads/stores are
  bare host accesses (same family as our DIRECT_ADDRESSING + Dolphin fastmem).
- **What we could take:** Concepts only. The LSRA live-interval writeup is a readable companion
  to dynarmic's `reg_alloc.cpp` if we pursue a register cache. **Verdict: conceptual reference
  for RA and fastmem; no code path (C#).**

## 7. Rosetta 2 — Apple's x86_64→ARM64 (proprietary)

- **License:** Proprietary. **Study-only**, public reverse-engineering writeups
  (FFRI Project Champollion parts 1–2; dougallj "Why is Rosetta 2 fast?").
- **ARM64 relevance / techniques:** AOT-dominant (translate whole text segment up front; JIT
  only for runtime-generated code). Speed comes largely from **hardware** we also run on:
  (a) the M-series **NZCV PF/AF extension** — when enabled, `ADDS`/`SUBS`/`CMP` compute x86
  parity and adjust flags into NZCV bits 26/27, making full x86 flag emulation free;
  (b) **TSO (Total Store Ordering) mode** — hardware x86 memory ordering, no fences needed.
- **What we could take:** No code. The actionable insight: **PF/AF aren't our problem (PPC has
  no parity/adjust flag)**, and PPC is already weakly-ordered like ARM, so TSO is irrelevant to
  us. Rosetta's headline tricks mostly **don't transfer** to a PPC guest — useful to know so we
  don't chase them. The transferable lesson is structural: **AOT for the static text segment**
  (Mac OS ROM + app code is largely static) could cut our JIT-residency problem (the open issue
  in EMULATOR-RESEARCH-LEADS post-findings item 9). **Verdict: study-only; one strategic idea
  (AOT static segments) worth keeping for the residency problem.**

---

## Top-5 actionable leads (ranked)

1. **Study MAME `ppcdrc.cpp` + `ppcfe.cpp` (BSD-3-Clause).** Closest-guest, vendorable PPC
   dynarec. Use as a second correctness oracle (covers 603/604, not just 750) and as the
   blueprint for read/write flag descriptors if we add flag-liveness. *Effort: low (reading);
   medium (if we adopt descriptors). Risk: low.*

2. **Adopt Box64's deferred-flag state machine model for our XER CA / CR0 within-block work
   (MIT).** Its `status_*` + "leave result in host NZCV when the consumer is a condition"
   design is exactly what Dolphin Lead 3 gestured at, in shippable form. Study
   `dynarec_arm64_helper.h` before writing our own. *Effort: medium. Risk: medium (exit/exception
   flushing must be airtight) — gate behind the open `adde` fix.*

3. **Spike oaknut `DualCodeBlock` under macOS `MAP_JIT` (MIT).** If a dual RW/RX mapping of one
   code region is permitted on Apple Silicon, it eliminates W^X toggling entirely — superseding
   the nesting-counter idea. *Effort: low (spike). Risk: low; may simply not be allowed under
   MAP_JIT — find out cheaply.*

4. **Use dynarmic `reg_alloc.cpp` (0BSD) as the reference design for a per-block register
   cache.** Our all-memory-resident GPR model is the root reason the Dolphin CR/carry
   optimizations were blocked. A register cache is the unlock; dynarmic is the cleanest,
   most-permissive model, and it pairs with oaknut. *Effort: high. Risk: high — do after the JIT
   is otherwise stable and residency is fixed.*

5. **Keep FEX `RedundantFlagCalculationElimination` + ARMeilleure LSRA as design references
   only.** No code path (IR-based / C#), but they justify *where* flag-elimination and RA
   belong if we ever grow a minimal per-block IR. Revisit only if leads 2 and 4 prove the
   ad-hoc approach has hit its ceiling. *Effort: reading only. Risk: none.*

> **Cross-cutting note:** the four borrowable non-PPC projects (oaknut, dynarmic, Box64, FEX)
> are *technique* references — their guest is ARM or x86, not PPC. **MAME is the only one
> offering PPC-specific borrowable code.** Rosetta 2 mostly tells us which of its tricks
> *don't* apply to a PPC guest. Relevance ranking above weights PPC-specificity over license
> permissiveness, then license over maturity.
