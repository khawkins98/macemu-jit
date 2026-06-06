# Plan: Snow comparative evaluation (classic-mac emulation + debugger tooling)

> **Status:** 🟡 In progress — exploratory evaluation · **Created:** 2026-06-06 · **Updated:** 2026-06-06
> **Why this doc exists:** Track a disciplined comparison against Snow so we can selectively adopt useful emulation and debugging ideas without derailing SheepShaver priorities.
> _Markers: ✅ done · 🟡 in progress · ⏸ blocked/deferred · ☐ todo. Finished an item? Flip its marker, bump **Updated**, and add a `CHANGELOG.md` entry (see [CONTRIBUTING](../../CONTRIBUTING.md) → "Documentation Lifecycle")._

---

## TL;DR

We treat [twvd/snow](https://github.com/twvd/snow) as a **reference emulator** with strong
hardware-fidelity and debugging UX ideas, not as a port target.

Snow is especially relevant to:
1. **BasiliskII/68K resurrection planning** (if we reopen that track), and
2. **debug/triage tooling quality** for SheepShaver validation.

---

## What looked immediately useful

From Snow's public docs/readme, the highest-signal ideas are:

- Rich debugger surfaces with explicit operator UX:
  breakpoints (including trap/interrupt/exception forms), watchpoints, disassembly, register/memory
  editing, instruction history export, system-trap history, and peripheral state inspection.
- Clear statement of debugging trace cost ("extra trace functionality impacts performance while open"),
  which is a good model for making diagnostic overhead explicit.
- Hardware-focused framing ("emulate at hardware level, avoid ROM patch/syscall intercept strategy"),
  which is useful as a methodology contrast for design decisions in this repo.

---

## Scope and non-goals

**In scope**
- Debugger and instrumentation ergonomics we can apply to SheepShaver test/triage workflows.
- Hardware-model discipline lessons relevant to BasiliskII planning and long-horizon fidelity work.
- Cross-pollination opportunities with DingusPPC debugger/profiler concepts.

**Out of scope**
- Porting Snow.
- Replacing current SheepShaver architecture with Snow's architecture.
- Expanding current product scope to support "everything Snow supports."

---

## Adopt / defer / avoid (initial pass)

### Adopt now (low risk, high leverage)
- Add/standardize lightweight, explicit-cost diagnostics in our tooling docs and scripts.
- Borrow debugger UX patterns for triage loops (history export, trap-focused views, quick watch setup).
- Tighten "debug mode vs normal mode" expectations in docs/tests so perf impact is not ambiguous.

### Defer (good ideas, wrong timing)
- Deep hardware-fidelity initiatives tied to full 68K platform breadth until D1 scope is active.
- Large debugger-UI feature work in the emulator core before A-track verification gates settle.

### Avoid for now
- Strategy shifts that trade current JIT throughput/roadmap momentum for broad architecture churn.

---

## Work plan

### S0. Baseline capture — ✅ done
- Snow feature set reviewed for emulation scope and debugging/tooling capabilities.
- Initial overlap mapped: debugger ergonomics + traceability tooling.

### S1. Debug-tooling crosswalk (Snow + Dingus + current repo) — 🟡 in progress
- Build a compact matrix:
  - current capability here,
  - Snow capability,
  - Dingus capability,
  - and the smallest actionable upgrade.

### S2. Propose bounded first moves — ☐ todo
- Pick 2-3 low-risk upgrades (docs + tooling first, core changes only if needed), each with:
  - success criteria,
  - rollback criteria,
  - ownership in ROADMAP/A-track docs.

### S3. D1 tie-in decision — ☐ todo
- If BasiliskII track D1 is reactivated, decide which Snow findings become concrete 68K tasks.

---

## Early recommendations

1. Use Snow as a **debugger-ergonomics reference** now.
2. Keep Snow's hardware-fidelity strategy as a **design comparator**, not a near-term mandate.
3. Fold Dingus CLI-debugger/profiler ideas into the same crosswalk so we get one coherent tooling plan.
