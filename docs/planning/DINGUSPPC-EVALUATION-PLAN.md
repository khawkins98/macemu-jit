# Plan: DingusPPC comparative evaluation (Apple Silicon relevance)

> **Status:** 🟡 In progress — exploratory evaluation · **Created:** 2026-06-06 · **Updated:** 2026-06-06
> **Why this doc exists:** Track a disciplined comparison against DingusPPC so we can borrow useful ideas for SheepShaver/macOS-arm64 without derailing the current JIT roadmap.
> _Markers: ✅ done · 🟡 in progress · ⏸ blocked/deferred · ☐ todo. Finished an item? Flip its marker, bump **Updated**, and add a `CHANGELOG.md` entry (see [CONTRIBUTING](../../CONTRIBUTING.md) → "Documentation Lifecycle")._

---

## TL;DR

We are treating [dingusdev/dingusppc](https://github.com/dingusdev/dingusppc) as a **reference emulator**:
not a port target and not a code-import target. It has a solid interpreter/MMU modeling surface and
potentially strong ideas for correctness infrastructure.

> **Validated 2026-06-06 (web, primary source).** **License: GPL-3.0 — the original "similar GPL
> license family" was correct.** Verified against the repo's root `LICENSE` (GPLv3 verbatim) *and*
> GitHub's detected metadata (`spdx_id: GPL-3.0`). *(An earlier note in this doc briefly claimed
> "BSD-3-Clause"; that was a bad web-scrape — corrected. GPL-3.0 is the same copyleft family as our
> GPL SheepShaver, so idea-borrowing is unconstrained and code-level reuse is feasible under GPL terms
> — not "more permissive than us." Note the usual GPLv2-vs-GPLv3 nuance if code reuse is ever actually
> proposed; we treat this ideas-first regardless.)*
> It's a **pure C++ interpreter** (no JIT) for PowerPC Macs (NuBus/Old World, early New World, Pippin;
> Power Mac 6100/7500/G3-Beige most complete), with **real MMU translation, 601/FPU support, and CPU
> exceptions via setjmp/longjmp**, plus modeled hardware devices and a **CLI debugger** (show code;
> dump memory as hex/68K/PPC asm; view/edit NVRAM/registers/memory; step N). **This makes DingusPPC the
> single most directly relevant external reference for SheepShaver** — same guest ISA (PowerPC), and it
> actually *implements* the MMU/exception machinery SheepShaver stubs. If `MMU-NANOKERNEL-MP-PLAN.md` is
> ever pursued, DingusPPC is the concrete reference implementation to study (how it models translation
> + exceptions).

The intended output is a ranked matrix:
1. **Adopt now** (low risk, high leverage for this repo).
2. **Defer** (good ideas, but wrong timing for current goals).
3. **Avoid** (conflicts with SheepShaver's AArch64 JIT priorities).

---

## Scope and non-goals

**In scope**
- MMU/TLB modeling patterns and debugability concepts.
- Exception/timer/interrupt fidelity approaches that can harden verification.
- Test-harness and oracle discipline ideas (differential testing, artifact triage).
- Debugger/profiler tooling patterns (CLI debugger commands, disassembly flow, profile reporting).
- Any Apple Silicon-specific practices that improve correctness or maintainability.

**Out of scope**
- Replacing SheepShaver's execution core with an interpreter-first architecture.
- Direct code copy/import.
- Expanding current guest target scope (e.g., G5/MMU/MP) as part of this comparison alone.

---

## Work plan

### P0. Establish comparison baseline — ✅ done
- Confirmed current role: DingusPPC appears interpreter-centric with substantial MMU structures,
  while our active path is AArch64 JIT optimization/verification.
- Confirmed comparison framing: reference source for semantics and validation ergonomics.

### P1. Produce structured borrow/defer/avoid matrix — 🟡 in progress
Build a short matrix keyed to current priorities (A1/A2 verification, B-track JIT work):
- **Adopt now:** items that improve confidence without major runtime risk.
- **Defer:** items gated on later scope decisions (e.g., deeper supervisor fidelity).
- **Avoid for now:** items that would trade JIT throughput for architecture churn.
- Include a dedicated "debug tooling" lane so good CLI/profiler ideas do not get lost behind MMU/JIT discussions.

### P2. Convert matrix into bounded experiments — ☐ todo
Define 2-3 bounded experiments, each with:
- clear success criteria,
- rollback criteria,
- and where it lands in existing docs (`ROADMAP.md`, `OPTIMIZATION-PLAN.md`, `TESTING.md`).

### P3. Decide keep/defer at roadmap level — ☐ todo
After P2 results, update track priorities with explicit decisions:
- promote to active workstream,
- keep as parked design note,
- or close as "not aligned with current goals."

---

## Initial hypotheses (to validate)

- **Likely adopt-now:** harness/oracle discipline improvements and clearer MMU-model diagnostics.
- **Likely adopt-now (tooling):** lightweight debugger command flows (step/next/until/disas/mem/regs)
  and profile report hooks that make triage faster without changing execution semantics.
- **Likely defer:** deeper nanokernel/MMU/MP fidelity work unless/until D3 demand hardens.
- **Likely avoid-now:** interpreter-oriented core shifts that disrupt JIT momentum.

---

## Deliverables

1. A concise adopt/defer/avoid matrix with evidence links.
2. A short list of approved bounded experiments.
3. Roadmap updates reflecting accepted/rejected items.
