> **ARCHIVED 2026-06-12** — Moved to archive during the Reku pass.
> May contain outdated assumptions, resolved questions, or superseded plans.
> Current state: `docs/HANDOFF.md` · `docs/planning/ROADMAP.md`
> **Reason:** historical research
>

# Research Directory Index

> **Status:** 📖 Index · **Created:** 2026-06-02 · **Updated:** 2026-06-04
> **Why this doc exists:** Index of the SheepShaver JIT research corpus (leads, landscapes, strategic studies).
> _Markers: ✅ done · 🟡 in progress · ⏸ blocked/deferred · ☐ todo. Finished an item? Flip its marker, bump **Updated**, and add a `CHANGELOG.md` entry (see [CONTRIBUTING](../../../../CONTRIBUTING.md) → "Documentation Lifecycle")._


Output of the 2026-06-02 research effort (Dolphin/PPC-JIT leads, landscape surveys,
implementation prep). **Start here.**

## The three documents that matter day-to-day

| Doc | Role | Status |
|-----|------|--------|
| [`IMPLEMENTATION-BACKLOG.md`](IMPLEMENTATION-BACKLOG.md) | **Source of truth for work items.** Tiers A (correctness) / B (cheap wins) / C (strategic), each with code designs, test vectors, verification steps. Updated as findings supersede each other. | ✅ Live — keep current |
| [`RESEARCH-HANDOFF.md`](RESEARCH-HANDOFF.md) | Operational instructions for an implementation agent: pre-flight checks, ground rules, phase ordering, reporting protocol. | ✅ Live |
| [`../EMULATOR-RESEARCH-LEADS.md`](../EMULATOR-RESEARCH-LEADS.md) | Research narrative — what was investigated and why verdicts came out as they did. Superseded sections are marked inline. | 📖 Narrative/archive |

Related plans outside this directory:
[`../COMPATIBILITY-TESTING-PLAN.md`](../COMPATIBILITY-TESTING-PLAN.md) (how we'll measure
correctness vs the x86 JIT and real apps) ·
[`SheepShaver/docs/AARCH64_JIT_GOLDEN_WORKLOADS.md`](../../../../SheepShaver/docs/AARCH64_JIT_GOLDEN_WORKLOADS.md) (canonical test
gate) · [`LEARNINGS.md`](../../../../LEARNINGS.md) and [`CHANGELOG.md`](../../../../CHANGELOG.md)
(the JIT work's durable findings + change log — the dated session handoff docs were retired 2026-06-04).

## Reference studies (read when working the related backlog item)

### Strategic analyses (Tier C backlog items)

| Doc | Backlog item | Headline finding |
|-----|-------------|------------------|
| [`c1-residency-root-cause.md`](c1-residency-root-cause.md) | **C1** | Dual-cache trap: interpreter never consults JIT cache once warm. Fix = gate restructure, not AOT. |
| [`c2-mame-ppc-drc-study.md`](c2-mame-ppc-drc-study.md) | **C2** | MAME PPC DRC is BSD-3-Clause and confirms our bug fixes; wholesale adoption not viable. |
| [`b5-c3-video-implementation-prep.md`](b5-c3-video-implementation-prep.md) | **B5, C3** | Hardware cursor + QuickDraw accel already implemented in-tree; work is enable/extend. |
| [`c4-wx-dual-mapping-spike.md`](c4-wx-dual-mapping-spike.md) | **C4** | Empirical YES: dual-mapping eliminates W^X toggling, ~27% faster. Runnable test in `../../../../SheepShaver/spikes/wx-dual-mapping/`. |
| [`c5-background-compilation-survey.md`](c5-background-compilation-survey.md) | **C5** | Cemu's compile-on-miss model is the design to copy; Ryujinx call-counter bolt-on. |
| [`c5-background-compilation-feasibility.md`](c5-background-compilation-feasibility.md) | **C5** | Thread-safety audit: 2-4 days of work after C1+C4; races inventoried. |

### Implementation prep (Tier B backlog items)

| Doc | Backlog item | Deliverable |
|-----|-------------|-------------|
| [`b2-logical-imm-prep.md`](b2-logical-imm-prep.md) | **B2** | Tested encoder ready to integrate: [`SheepShaver/src/kpx_cpu/src/cpu/jit/aarch64/ppc-logical-imm.hpp`](../../../../SheepShaver/src/kpx_cpu/src/cpu/jit/aarch64/ppc-logical-imm.hpp) + test in `../../../../SheepShaver/spikes/logical-imm/`. |

### Dolphin deep-dives (first research round — verdicts now in the backlog)

| Doc | Verdict |
|-----|---------|
| [`lead-1-cr-64bit-representation.md`](lead-1-cr-64bit-representation.md) | Defer (needs CR register cache); do the cheap `emit_update_cr0` cleanup instead (B1) |
| [`lead-2-wx-nesting-counter.md`](lead-2-wx-nesting-counter.md) | Downgraded to hygiene (B3) — likely superseded entirely by C4 dual-mapping |
| [`lead-3-lazy-carry.md`](lead-3-lazy-carry.md) | Not yet — found the `adde` bug (A1); revisit after C1 + profiling |
| [`lead-4-oe-form-punt.md`](lead-4-oe-form-punt.md) | **Rejected** — OE forms are hot for us; found the `mullwo` bug (A3) |
| [`lead-5-arm64-emitter.md`](lead-5-arm64-emitter.md) | Reference only; spawned B2 (LogicalImm) |
| [`lead-6-fastmem-backpatch.md`](lead-6-fastmem-backpatch.md) | **Closed** — already satisfied by DIRECT_ADDRESSING |
| [`lead-7-dispatch-linking.md`](lead-7-dispatch-linking.md) | BRK tripwire yes (B4); found chain-pool bug (A4) + the residency data that led to C1 |

### Landscape surveys (second research round)

| Doc | Contents |
|-----|----------|
| [`landscape-2-arm64-dynarec-projects.md`](landscape-2-arm64-dynarec-projects.md) | MAME / oaknut / dynarmic / Box64 / FEX / Ryujinx / Rosetta 2 — licenses, techniques, what's liftable |
| [`landscape-3-classic-mac-video-accel.md`](landscape-3-classic-mac-video-accel.md) | Video acceleration architectures: framebuffer vs silicon emulation vs paravirtual driver |

## Supersession map (what overrides what)

```
EMULATOR-RESEARCH-LEADS.md (original leads + ordering)
  ⮡ superseded by per-lead deep dives (lead-1 … lead-7)
      ⮡ verdicts consolidated into IMPLEMENTATION-BACKLOG.md
landscape-2 "Rosetta AOT fixes residency" idea
  ⮡ REFUTED by c1-residency-root-cause.md (dual-cache trap; AOT deferred into C5)
lead-2 W^X nesting counter (B3)
  ⮡ likely superseded by c4-wx-dual-mapping-spike.md (pending user decision)
Backlog line numbers for ppc-jit.cpp / ppc-cpu.cpp
  ⮡ drift as the JIT agent lands work — trust case labels / function names over line numbers
```

## Conventions

- New research docs go in this directory, named `<backlog-item>-<slug>.md` (e.g.
  `c1-…`, `b2-…`) or `landscape-N-<slug>.md` for surveys.
- When a finding changes a backlog item, **update `IMPLEMENTATION-BACKLOG.md` in the same
  commit** — the backlog must never lag the research.
- When a conclusion is later refuted, don't delete it — mark it inline (see the Rosetta-AOT
  example in the leads doc) so future readers understand why direction changed.
