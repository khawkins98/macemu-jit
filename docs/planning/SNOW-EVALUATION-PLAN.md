# Plan: Snow comparative evaluation (classic-mac emulation + debugger tooling)

> **Status:** 🟡 In progress — exploratory evaluation · **Created:** 2026-06-06 · **Updated:** 2026-06-06
> **Why this doc exists:** Track a disciplined comparison against Snow so we can selectively adopt useful emulation and debugging ideas without derailing SheepShaver priorities.
> _Markers: ✅ done · 🟡 in progress · ⏸ blocked/deferred · ☐ todo. Finished an item? Flip its marker, bump **Updated**, and add a `CHANGELOG.md` entry (see [CONTRIBUTING](../../CONTRIBUTING.md) → "Documentation Lifecycle")._

---

## TL;DR

We treat [twvd/snow](https://github.com/twvd/snow) as a **reference emulator** with strong
hardware-fidelity and debugging UX ideas, not as a port target.

> **Validated 2026-06-06 (web).** Snow is a **68K-only** Macintosh emulator (Mac 128K/512K/Plus/SE/
> Classic/Macintosh II era; 68000/68020/68030 + FPU/PMMU), **Rust + egui**, **MIT-licensed**, and
> explicitly **hardware-level** ("emulate at the hardware level… as opposed to emulators that patch
> the ROM or intercept system calls" — the deliberate *opposite* of SheepShaver/BasiliskII's
> paravirtualization). Consequences for us:
> - **Snow's *emulation* lessons apply to BasiliskII (68K) only — never to SheepShaver's PowerPC
>   path.** Don't let "hardware fidelity" leak into SheepShaver design discussions; for us it's a
>   *contrast* (the road we deliberately didn't take — see `MMU-NANOKERNEL-MP-PLAN.md`), not a model.
> - **Only Snow's *debugger/observability UX* transfers to SheepShaver** — and it's the highest-value
>   thing here (see "Build/debug presentation" below). Its debugger chrome is confirmed: breakpoints
>   (execution/bus-access/system-trap/exception/interrupt forms), watchpoints, single-step,
>   disassembly, register/memory viewers+editing, instruction-history export, system-trap history,
>   peripheral inspection — all in a live egui GUI.

Snow is especially relevant to:
1. **BasiliskII/68K resurrection planning** (if we reopen that track) — *emulation* fidelity, and
2. **debug/triage tooling + build-debug presentation** for SheepShaver validation — *UX only*.

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

## Build/debug presentation — should we adopt Snow-style "chrome"? (recommendation, 2026-06-06)

**The question:** Snow launches with rich live chrome (registers, disassembly, memory, trap/interrupt
history, watchpoints) so you see machine state on the fly. Should we adapt how *our* build/debug
environment is presented?

**Direction: yes — but build an *observability inspector*, not a Snow-style step-debugger, and host
it in Silicon Sheep, not the emulator core.** *(Now folded into the Silicon Sheep plan as a tracked
feature — see [`DESKTOP_INTEGRATION_PLAN.md`](DESKTOP_INTEGRATION_PLAN.md) → "Developer Inspector /
Debug Chrome". This section is the rationale; that is the home.)* Reasoning:

- **We already have the data, not the presentation.** SheepShaver emits a rich heartbeat (per-region
  block rates `jNK/jDR/jRAM`, `comp` count, `j2i` transitions, `rss`, `cpu%`, a warning matrix), a
  trace ring, HOT-PC sampling, `SS_JIT_WATCH_ADDR` watchpoints, and `jit-analyze.py` — plus live JIT
  stats already pushed to the **SDL window title**. The gap is that it's stderr logs + offline Python,
  not a live interactive view. Closing *that* gap is cheap and high-leverage.
- **A full Snow-style debugger is the wrong target for us.** Snow is a hardware-level 68K emulator
  where single-step/disassemble/edit-memory is the natural debugging model. Our debugging is
  *differential* (SS_JIT_VERIFY, the harness, `jit-diff-sweep`) over a JIT + paravirtualized PPC —
  a bespoke step-debugger over JITed blocks is expensive and lower-value. What pays off is **live
  observability**: hot blocks (from the B1 profiler), per-region execution mix, fallback/`j2i` rates,
  interrupt/spcflags state, watchpoint hits, HOT-PC. Borrow Snow's *answer to "which surfaces matter"*,
  not its architecture.
- **Host it in Silicon Sheep (Tauri), not the emulator.** Rich chrome in a web/Tauri panel is far
  cheaper than a native egui debugger, Silicon Sheep is already the dev/power-user shell, and it keeps
  debugger-UI churn **out of the emulator core** (which the Adopt/Defer/Avoid table above correctly
  says to avoid before A-track gates settle). The data path: emulator diagnostics + B1 profiler →
  structured stream → a Silicon Sheep "Inspector" tab.
- **This is profiling-first, not a detour.** The recommended first technical step is still **B1, the
  execution-weighted profiler** — the Inspector is simply its natural front-end. Snow/Dingus are the
  ergonomics references for the panel's layout; B1 + existing diagnostics are the data layer.

**Sequence:** B1 profiler (data) → a minimal live view (extend the window-title stats, or a tiny
stderr→Silicon Sheep bridge) → an Inspector tab in Silicon Sheep that grows as the profiler/diagnostics
mature. No emulator-core debugger work. **Tracked under ROADMAP A1 (verification tooling) feeding
Track C (Silicon Sheep).**

## Early recommendations

1. Use Snow as a **debugger-ergonomics reference** now — for *which surfaces matter*, not its core.
2. Keep Snow's hardware-fidelity strategy as a **design comparator/contrast**, not a near-term mandate
   (and note it's 68K-only, so even that contrast lands on BasiliskII, not SheepShaver's PPC path).
3. **Unify the tooling crosswalk:** Snow (S1), DingusPPC (P1), and Infinite Mac all propose a
   debug-tooling matrix — these should feed **one** matrix, not three, converging on the Inspector
   above. Owner: whoever picks up B1.
