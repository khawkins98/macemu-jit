# Project Handoff — resume entry point

> **Status: M15 COMPLETE 2026-06-14 — FORGE verdict verified** · The `SS_NW_IRQ_CONSUME`
> consumption path dead-ends: across **510 consumption boots** the 68k level-1 handler
> `0x5000ec50` is reached **0/510** (EXT edge re-fires into CGRP fallback `0x50325fd0`, not
> NK slot-4 service `0x50314660`); NK routing structs stay **frozen-zero across obs=1e9**
> (zero struct-populating writes); `SC#1=0x0d` is a PIC-off artifact (absent under
> `SS_NW_PIC=1`). Two independent pillars; verdict does NOT rest on a single chain.
> **Misroute-why diagnostic COMPLETE (2026-06-14): STRUCTURAL.** EXT IS delivered correctly to
> the published NK EXT entry `0x50314880`; the misroute is DOWNSTREAM, in the NK dispatcher.
> See `docs/planning/M15-FINDINGS-consumption-recon.md` "Addendum — misroute-why diagnostic".
>
> **M16 COMPLETE (2026-06-14) — DoD-3 NO-GO. NewWorld 9.x interrupt routing banked as
> forge-class; frontier pivots to COMPATIBILITY-PAYOFF.** Task-0 RE pinned the dead-end to the
> empty "CGRP" interrupt-group descriptor (`*(KDP-0x338)=0x68ffc1c0`): gate `[0x68ffc1e0]=1` needs
> ≥2, but the handler table is empty (`+0x38/+0x3c/+0x44`=0) and the service routine `0x503148e0`
> self-guards, so the one-word forge is SAFE but INERT. The milestone was re-scoped to **CGRP
> handler-table synthesis** (plan rev-4 / spec rev-2), then a pre-implementation red-team round
> (SHA `df627fe0`, 3 reviewers) fired the **pre-authorized early NO-GO**: the descriptor FORMAT is
> RE-tractable (`[entry+0]`=SRR0, `[entry+4]`=TOC, SRR1 from r19) but the **synthesis target value
> is ROM-absent** — `0x5000ec50` has **0 word-refs** in the 4 MB ROM (it is 68k code reached only
> via the DR emulator), and the `"CGRP"` tag is ROM-absent (runtime/disk-built by the IM-init that
> never runs). Scratch `0x68ff5000` is provably live (occupancy map); a populated table re-opens the
> M10 DR-reentry `0xDEADBEEF` crash class; CGRP is only the **first of N** frozen structs. The only
> surviving path — a host-owned NK-EXT-handler PPC stub — is materially larger and documented as the
> re-entry point if 9.x becomes a hard requirement. **RE banked (Q1–Q7):
> `docs/planning/M16-FINDINGS-oracle-forge.md`.** Plan/spec CLOSED (do-not-execute banners).
> **Next: M17 — host-owned NK interrupt stub (9.2 NewWorld is a HARD requirement, user 2026-06-14).**
> The M16 NO-GO closed the *cheap* CGRP-table seed, not the goal: M17 pursues the surviving path —
> synthesize a host-owned stub that manufactures the pending-interrupt state and performs the
> **sanctioned** PPC→68k cross (the `HandleInterrupt` MODE_EMUL_OP + `Execute68k` path that already
> exists in-tree, newworld-fenced at `glue.cpp:3519`) so the 68k DR dispatches the level-1
> autovector to `0x5000ec50` (confirmed real guest code). Success = advance ONE wall + capture the
> next (CGRP is first-of-N), NOT reach Finder. DRAFT spec+plan committed (`3a7826f7`), **red-team
> pending** (priority: Q0-B run-mode-at-EXT-dispatch is M17's likeliest early NO-GO). Spec/plan:
> `docs/superpowers/{specs,plans}/2026-06-14-m17-host-irq-stub*`. The M16 RE stays banked
> (`M16-FINDINGS` Q1–Q7). Compatibility-payoff (8.6–9.0.4 usability) is now a parallel/secondary
> track. Does NOT reopen the M15 FORGE verdict.
> Standing fact: ring tool path is **`tools/ring-walk.py`** (repo-root `tools/`, NOT
> `SheepShaver/tools/`).
> For session logs: `docs/archive/2026-06/LEARNINGS-2026-06.md` + `docs/archive/2026-06/HANDOFF-SESSIONS-M9-M12.md`.

### M15 verdict (FORGE — verified) and next-milestone entry

> M14 left NewWorld 9.x as a precisely-characterized wall (all paths collapse to forge
> class; DEC does NOT signal the DR; KDP+0x674=0, hnfo+0x14=NIL, hnfo+0x28=0 because IM
> init runs downstream of Cuda init). **M15 adjudicated the one open question** — is the
> consumption path one fixable divergence short of completing IM init, or does it need a
> host forge? **Answer: FORGE** (510/510 routing gap; structs frozen to obs=1e9; `0x0d`
> is an unreachable-config artifact). The verdict is settled.
>
> The next milestone's Task 0 is a SEPARATE forward-looking question — "is that dead-end
> repairable?" — entered as a **time-boxed misroute-why diagnostic** (why the EXT edge
> selects CGRP fallback `0x50325fd0` over NK slot-4 service `0x50314660`), using
> `tools/ring-walk.py` on a fresh `SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1 SS_DR_R24_RING=1` boot
> (NOT `SS_JIT_WATCH_DUMPS=0`). Decision rule: obvious wrong branch / one-line condition →
> fix it and run real init (forge unnecessary); structural/not-obvious within the box →
> STOP and fall through to **oracle-first forge (M14 §7 step 5)** — extract correct
> `hnfo+0x14` / `hnfo+0x28` / `KDP+0x674` from a working paravirtual or QEMU mac99 boot,
> then seed. This does NOT reopen the FORGE verdict.

## HEADLINE — the M13 correction + M14 reconciliation

> **DEC does NOT signal the DR. EXT (Cuda) delivery to 68k does NOT.**
> **ed0a entries are from HandleInterrupt MODE_EMUL_OP, not the NK DEC handler.**
>
> The ed0a entries were **misattributed to DEC autovector.** Full DEC handler RE (§7)
> proves: the NK DEC handler at 0x50313200 services timers, restores CR fully
> (`mtcrf 0xff, r13`), and returns via rfi — it NEVER dispatches to the DR. The 64/64
> ed0a entries come from `HandleInterrupt` `MODE_EMUL_OP` `Execute68k` (the host-side
> VBL path that runs during EMUL_OP callbacks). In `MODE_68K` (where Cuda init runs),
> HandleInterrupt only bumps Ticks — no CR injection, no autovector.
>
> **The chicken-and-egg, precisely located:**
> - `KDP+0x674` (CR mask) = **0** — never initialized (probed live)
> - `hnfo+0x14` (source table) = **NIL** — never populated
> - `hnfo+0x28` (pending bits) = **0** — never written
> - All three are populated by the Interrupt Manager init, which runs DOWNSTREAM of
>   Cuda init. Every viable fix path collapses to the M10-forge class (host-seeding
>   NK data structures). See M14-FINDINGS §7 for the precise resume experiment.
>
> **Standing corrections:**
> - The ed08→ed0a probe-granularity fix IS correct (always probe ed0a, never ed08)
> - DEC does NOT deliver to 68k via the NK handler — ed0a entries are MODE_EMUL_OP only
> - All paths (HLE, CR injection, CGRP forge, Execute68k) collapse to forge-class
> - Evidence: M14-FINDINGS §4a (smoke tests) + §7 (DEC RE + collapse analysis)

## Resume prompt

> Read `docs/HANDOFF.md` (this HEADLINE), then `docs/AGENT-CONTEXT.md`.
>
> **M15 is COMPLETE — NewWorld 9.x consumption path adjudicated FORGE (verified).** If
> resuming NewWorld 9.x, the next milestone enters at the **misroute-first time-boxed
> gate** (Task 0: why EXT edge → CGRP fallback `0x50325fd0` not NK slot-4 `0x50314660`,
> via `tools/ring-walk.py`), falling through to **oracle-first forge (M14 §7 step 5)** if
> the misroute is structural/non-obvious. Read
> `docs/planning/M15-FINDINGS-consumption-recon.md` "Verdict (Task 4)" — don't re-derive.
> This does NOT reopen the verdict.
>
> **Current focus: COMPATIBILITY-PAYOFF** — make the already-booting Mac OS 8.6–9.0.4
> genuinely usable. The project boots to Finder with full native JIT on arm64. The
> highest-value work is: CopyBits HLE, idle-skip, perf optimization, app compatibility,
> and the Silicon Sheep launcher (Track C).
>
> Process: `docs/MILESTONE-WORKFLOW.md`. Never push without being asked. Never global pkill —
> slot boots only via `SheepShaver/tools/ss-slot-boot.sh`.

---

## Current state (2026-06-14 end-of-session)

- **M8** — shipped, gated-off-green (`SS_NW_IRQ_CONSUME`).
- **M9** — stall fixed (ROM patch removed). ⚠️ Its "68k handler delivery blocked" framing is the
  RETRACTED artifact (see HEADLINE) — delivery was working; the wall was downstream.
- **M10** — COMPLETE (2026-06-13). Gate `SS_M10_CGRP` — **reverted in the M13 close-out (2026-06-14)**:
  the forged CGRP table targeted the non-problem (native delivery already works) and crashed
  (0xDEADBEEF). Retained only as a negative-result record in M13-FINDINGS / LEARNINGS.
- **M13** — COMPLETE (2026-06-14). Produced the **artifact retraction**: native interrupt delivery
  confirmed working (ed0a 8/8 baseline, genuine vector-$64 frame); the dead `SS_NW_DR_AUTOVEC` and
  `SS_M10_CGRP` mechanisms reverted. See `docs/planning/M13-FINDINGS-interrupt-delivery.md`.
- **M14** — COMPLETE, PARKED (2026-06-14). 9 smoke tests + watchpoint + uncapped probe +
  DEC handler RE + ed0a misattribution discovery. Timer delivery correct at VIA layer.
  **DEC does NOT signal the DR** — ed0a entries come from HandleInterrupt MODE_EMUL_OP
  Execute68k, not the NK DEC handler (which restores CR fully and returns without
  dispatching to the DR). KDP+0x674=0, hnfo+0x14=NIL, hnfo+0x28=0 — all because IM init
  runs downstream of Cuda init. All viable paths collapse to the M10-forge class (seed
  uninitialized NK routing infra from host). Parked with precise resume experiment in §7.
  See `docs/planning/M14-FINDINGS-cuda-delivery.md`.
- **M15** — COMPLETE (2026-06-14). Consumption-recon: adjudicated the one open M14 question
  (is the `SS_NW_IRQ_CONSUME` path one fixable divergence short of completing IM init, or
  forge?). **Verdict: FORGE — verified.** Two independent pillars (510/510 routing gap:
  68k L1 handler `0x5000ec50` reached 0/510, EXT re-fires into CGRP fallback `0x50325fd0`;
  `SC#1=0x0d` is a PIC-off artifact) + struct-watch pivot (NK routing structs frozen-zero
  to obs=1e9, 0 populating writes). Doc-only milestone; harness score=100 unchanged. Next
  = misroute-first time-boxed gate → oracle-first forge fallback.
  See `docs/planning/M15-FINDINGS-consumption-recon.md`.
- **M11a** — COMPLETE (2026-06-13). Frame-PC stability: static RE confirmed r24 is never clobbered by NK (see LEARNINGS 2026-06-13 M11a). 3/3 × 90s acceptance runs: probe match=1/5, no SIGSEGV. No code change.
- **M11** — COMPLETE (2026-06-13). Aperture at 0x81000000 (vm_mac_acquire_fixed 16MB), MMIO_APERTURE non-hull, SDL the_buffer → aperture, OF video node (640×480×32, "cofb"), T-F6 (13/13). [FB-DIRTY]=0 expected (boot exits 0.3s). Harness 353/353. Next: M12.
- **M12** — PARTIAL (2026-06-13). Wave0+Wave1 landed (`ddbd8d79`, `348544cd`); boot stable
  30s+ but `irq_fired=0`, `[FB-DIRTY]=0`. Pixel gate FAIL; frontier captured → M13.

### Session history (relocated)

The full M9 root-cause analysis, the M10/M11 "what shipped" tables, and the session-3 /
session-4 probe campaigns now live in **`docs/archive/2026-06/HANDOFF-SESSIONS-M9-M12.md`**.
The **current, verified** interrupt-delivery diagnosis (which reframes several of those
findings — e.g. the M10 `0x5000ed08` probe-match is now a falsified negative result) is
**`docs/planning/M13-FINDINGS-interrupt-delivery.md`**. Read M13-FINDINGS, not the session log,
for the live picture.

---

## Standing operational notes

- Branch `macos-arm64`; never push unprompted.
- Parallel boots via `SheepShaver/tools/ss-slot-boot.sh` only (never global pkill).
- Cleanup: `SheepShaver/tools/ss-reap.sh`.
- Queued ideas: `docs/planning/BACKLOG.md`.
