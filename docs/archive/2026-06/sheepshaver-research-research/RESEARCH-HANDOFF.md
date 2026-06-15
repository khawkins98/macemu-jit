> **ARCHIVED 2026-06-12** — Moved to archive during the Reku pass.
> May contain outdated assumptions, resolved questions, or superseded plans.
> Current state: `docs/HANDOFF.md` · `docs/planning/ROADMAP.md`
> **Reason:** historical research
>

# Research Handoff — Implementation Instructions

> **Status:** 📖 Reference / archive · **Created:** 2026-06-02 · **Updated:** 2026-06-04
> **Why this doc exists:** Operational instructions for an implementation agent picking up the research backlog.
> _Markers: ✅ done · 🟡 in progress · ⏸ blocked/deferred · ☐ todo. Finished an item? Flip its marker, bump **Updated**, and add a `CHANGELOG.md` entry (see [CONTRIBUTING](../../../../CONTRIBUTING.md) → "Documentation Lifecycle")._


**Audience:** the implementation agent picking up the 2026-06-02 research output.
**Repo:** `/Users/khawkins/Documents/git/macemu-jit`, branch `macos-arm64`.
**Read this file first, then `IMPLEMENTATION-BACKLOG.md` (same directory) for full item details.**

---

## 0. Before you touch anything

1. Run `git status` and `git log --oneline -10`. As of this handoff (commit `e91710cb`) there
   was **in-flight, uncommitted JIT work** in:
   - `SheepShaver/src/kpx_cpu/src/cpu/jit/aarch64/ppc-jit.cpp`
   - `SheepShaver/src/kpx_cpu/src/cpu/jit/aarch64/ppc-jit.h`
   - `SheepShaver/src/kpx_cpu/src/cpu/ppc/ppc-cpu.cpp`
   - `SheepShaver/jit-test/run.sh`

   If those changes are still uncommitted: **stop and ask the user** whether that work is
   abandoned, in progress, or yours to finish. Do not assume.
   If they've been committed: read the commits — they likely implemented some Tier A items
   already (the in-flight work included OE-form inlining and chain-site changes). **Cross off
   anything already done.** All line numbers in the research docs predate those commits.

2. Read `LEARNINGS.md` (repo root) — required by project convention at the start of every session.

3. Establish the test baseline before changing anything:
   ```bash
   cd SheepShaver && make build-ss && make test-opcodes
   # Expected: score=100 (209+ vectors). If it isn't 100, stop and report — do not "fix forward".
   ```

## 1. Ground rules

- **Every JIT change is gated on `make test-opcodes` staying at score 100.** Run it after
  every item, not just at the end.
- **Boot verification** for anything touching block compilation, dispatch, or epilogues:
  `make run-jit` (VNC port 5999), confirm boot to desktop, then `make kill`.
- One item per commit, message format: `fix:`/`feat:`/`perf:` + what + which backlog item.
- Follow `docs/planning/JIT-STYLE-DECISION.md`: immediate architectural writeback, no lazy state across
  block boundaries. Several research items were *rejected* for violating this — don't
  reintroduce them (see backlog "Rejected leads" table).
- If a research doc's line numbers don't match the file, trust the `case` labels / function
  names given alongside them. The docs were written to survive drift.

## 2. Work items, in order

Full details for every item: `IMPLEMENTATION-BACKLOG.md`. Summary and sequence:

### Phase 1 — Correctness (Tier A) — do first, smallest possible diffs

| # | Item | Where | Status check first |
|---|------|-------|--------------------|
| A1 | `adde` carry-out fix (CMP + ADCS) | ppc-jit.cpp case 138 | May already be fixed by in-flight work |
| A2 | `subfe` carry-out fix (same pattern) | ppc-jit.cpp case 136 | Same |
| A3 | `mullwo` → punt to interpreter | ppc-jit.cpp case 715 alias | Same |
| A4 | Chain-site pool exhaustion counter | ppc-jit.cpp `record_chain_site()` | Same |

- A1/A2: ready-to-apply code is in the backlog, **independently confirmed by MAME's
  implementation** (`c2-mame-ppc-drc-study.md` §carry lowering — same CMP/ADCS shape).
- A1/A2 each come with a new harness vector (in the backlog) that **must fail before the fix
  and pass after**. Add the vector first, watch it fail, then fix. If it doesn't fail before
  the fix, the bug was already fixed by the in-flight work — skip the item.
- A3: also add a comment pointing to `divwo` as the precedent for punting.

### Phase 2 — JIT residency (Tier C1) — the highest-impact item

This was the strategic finding of the whole research effort. Read
`c1-residency-root-cause.md` in full before starting.

1. **Probe first (R1):** add the read-only `jit_bc_lookup` counter at the interpreter's
   cache-hit transition (ppc-cpu.cpp `skip_jit:` loop, the `find()`-succeeds path). Boot,
   read the counter (lldb or env-gated stderr print). Expected: a large number, confirming
   compiled JIT blocks are being ignored.
2. **Then the fix:** when a complete JIT block exists for the next PC, `break` out of the
   interpreter inner loop back to `pdi_execute:` so the JIT gate runs. Keep the probe counter
   in place to verify it drops to ~zero.
3. **Verify:** harness 100, boot to desktop, and a T2-style check (sample or counter) showing
   sustained execution inside the JIT code cache. Record before/after in LEARNINGS.md.
4. **Do not** build ROM-AOT machinery — the analysis showed it's the wrong fix shape for this
   problem. It's deferred, not part of this phase.

### Phase 3 — Cheap wins (Tier B) — independent of each other, any order

| # | Item | Prep available |
|---|------|----------------|
| B1 | `emit_update_cr0` cleanup (CSET/BFI, ~18→~8 insns) | Design in `lead-1-*.md` |
| B2 | Logical-immediate `rlwinm`/`rlwimi`/`rlwnm` masks | **Encoder already written and tested**: `#include "ppc-logical-imm.hpp"`. Integration plan keyed to case labels in `b2-logical-imm-prep.md`. ⚠ AND-immediate base opcode is `0x12000000`, NOT the reg-reg `0x0A000000`. Keep reg-reg fallback for unencodable masks. |
| B3 | W^X toggle hoist in `patch_chain_sites()` | Design in `lead-2-*.md` Option A. ⚠ `sys_icache_invalidate` must still cover every patched address. **Consider skipping in favor of C4 below.** |
| B4 | Debug-gated BRK tripwire on invalidated blocks | Design in `lead-7-*.md` |
| B5(a) | Hardware cursor, native SDL window | **One line**: flip `hardcursor` default (prefs_items.cpp:64) |
| B5(b) | Hardware cursor over VNC | ~60-100 lines, plan in `b5-c3-video-implementation-prep.md` |

### Phase 4 — Strategic (Tier C) — needs a user decision before starting

| # | Item | Decision needed |
|---|------|-----------------|
| C4 | Dual-mapping W^X (replaces all toggling, ~27% faster emission) | Spike proved it works (`c4-wx-dual-mapping-spike.md`, runnable test in `spikes/wx-dual-mapping/`). But adoption touches every emit site — ask the user whether to do it now or after residency work stabilizes. If yes, it **supersedes B3**. |
| C5 | Background compilation on a worker thread (fixes JIT boot-time stalls — boot is >180s with JIT vs ~10s interpreter) | Full design ready: `c5-background-compilation-survey.md` (Cemu compile-on-miss model + Ryujinx call counter) + `c5-background-compilation-feasibility.md` (race inventory, ARM64 publication rules). **Hard prerequisites: C1 then C4.** ~2-4 days after those land. |
| C2 | MAME DRC code lifting (FP, flag-liveness design) | Only after A+B+C1 are done. Study: `c2-mame-ppc-drc-study.md`. |
| C3 | Widen QuickDraw accel (more transfer modes, ScrollRect) | Independent of JIT; needs gfxaccel testing infrastructure first. Plan: `b5-c3-video-implementation-prep.md`. |

## 3. What NOT to do

From the backlog's "Rejected / closed leads" — these were investigated and rejected;
don't re-propose them:

- ❌ Punt OE-form arithmetic to the interpreter (Mac OS's 68K emulator makes them hot)
- ❌ Fastmem/SIGSEGV backpatching (nothing to backpatch; DIRECT_ADDRESSING already optimal)
- ❌ Vendor Dolphin's Arm64Emitter wholesale (use our tested `ppc-logical-imm.hpp` instead)
- ❌ 64-bit CR representation / lazy carry / lazy CR0 (blocked or unprofitable without a
  register cache; revisit only after C1 + profiling)
- ❌ Emulate ATI video silicon (unproven everywhere, GPL-3.0 hazard)

## 4. Reporting

When you finish a phase:
1. Update `JIT-STATUS.md` (pass/fail status) and add non-obvious findings to `LEARNINGS.md`.
2. Update the item's entry in `IMPLEMENTATION-BACKLOG.md`: mark **DONE (commit hash)** or
   document why it was skipped/blocked.
3. Commit and push (`git push origin macos-arm64`).
4. If you found new bugs along the way, add them to the backlog as new Tier A items rather
   than fixing them opportunistically — keep diffs reviewable.

## 5. Reference index

| Doc | Use it for |
|-----|-----------|
| `IMPLEMENTATION-BACKLOG.md` | Full item details, code designs, test vectors |
| `c1-residency-root-cause.md` | Phase 2: control flow, probes, fix design |
| `c2-mame-ppc-drc-study.md` | Second opinion on carry/overflow semantics; MAME as oracle |
| `c4-wx-dual-mapping-spike.md` | Phase 4 decision: empirical dual-mapping results |
| `b2-logical-imm-prep.md` | Phase 3 B2: integration plan for the tested encoder |
| `b5-c3-video-implementation-prep.md` | Phase 3 B5 / Phase 4 C3: video file:line map |
| `lead-1` … `lead-7-*.md` | Dolphin deep-dives backing Tier B designs |
| `landscape-2`, `landscape-3` | Broader context; license table for external code |
| `../EMULATOR-RESEARCH-LEADS.md` | The full research narrative + verdicts |
| QEMU `target/ppc/translate.c` | Correctness oracle when a vector disagrees with the ISA manual |
