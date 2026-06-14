> **ARCHIVED 2026-06-14** — Moved to archive during the Reku doc-archive sweep.
> May contain outdated assumptions, resolved questions, or superseded plans.
> Current state: `docs/HANDOFF.md` · `docs/planning/ROADMAP.md`
> **Reason:** M8→M17 forge arc CLOSED (banked NO-GO); superseded by Operation NewSheep. Verdict banked in the paired `docs/planning/M1x-FINDINGS-*.md`.

# M15 — Consumption-path recon: "where does the SR interrupt stick?"

> **Type:** bounded recon milestone (verdict-producing, no production behavior change).
> **Created:** 2026-06-14 · **Status:** design approved, pre-plan.
> **Parent strategy:** `docs/planning/MACHINE-LAYER-PLAN.md` (rev 4) — this is the
> recon that decides how to break the M14 wall (the one link short of a 9.2 boot).
> **Predecessor findings:** `docs/planning/M14-FINDINGS-cuda-delivery.md` §4a/§4b/§7.

---

## 0. Why this milestone exists

The Machine Layer (M0–M13) is the "full hardware virtualization stack for the boot"
already built: MMIO bus + Mach-fault decoder, virtual clock + event scheduler, real
device models (SCC 8530, VIA 6522, Cuda, OpenPIC, ADB), a real exception core
(sc/rfi/SRR0-1/DEC delivery), MixedMode 68k↔PPC switch, NK syscall surface, FE1F
service surface, first host→guest interrupt delivery. It is parked **one link short**
of booting Mac OS 9.2: the Cuda SR interrupt reaches the NK but is never *consumed* by
the real 68k Cuda handler, so the Interrupt Manager init that would populate the NK
routing structures (`KDP+0x674`, `hnfo+0x14`, `hnfo+0x28`) never runs — leaving every
M14 fix path collapsed to "forge those structures from the host" (the M10-CGRP class,
0xDEADBEEF crash risk).

M14 characterized the **native** delivery path to a dead end (NK fallback handler at
`0x50325f00` clears the PIC source and returns without setting `cr2lt`/forwarding to the
DR). It did **not** re-examine the **M8 consumption path** (`SS_NW_IRQ_CONSUME=1`, the
slot-4 round trip), which was fenced off this session. That path already works
mechanically end-to-end env-on but its default flip was refused over a deterministic
`SC#1=0x0d` divergence and a RED retirement check.

**The open uncertainty this milestone closes:** is the consumption path *one divergence*
short of completing IM init (→ option 1, "make real init run," forge unnecessary), or
does it have *deeper* gaps (→ option 2, informed forge from an oracle, the bounded
fallback)? We will not commit months to either without evidence.

## 1. Goal and the verdict it produces

A single bounded recon. It changes no production behavior. It answers one question with
ring/probe evidence and emits a forge-vs-real recommendation.

**Definition of done** — a committed verdict doc
`docs/planning/M15-FINDINGS-consumption-recon.md` stating:
1. The exact stall point of the Cuda SR interrupt in the **consumption** path
   (`SS_NW_IRQ_CONSUME=1` + Smoke-H), with ring/probe evidence, relative to the 68k Cuda
   handler running IM init.
2. Whether the `SC#1=0x0d` divergence is the **sole** remaining gap or one of several.
3. Whether *anyone* writes the NK routing structs (`hnfo+0x00/+0x14/+0x28`, `KDP+0x674`)
   when consumption is on (a single write event flips the verdict toward "real").
4. The forge-vs-real recommendation with reasoning, and the concrete next-milestone
   handoff:
   - verdict **real** → the option-1 task list (push the real interrupt through to the
     handler so IM init populates the structs itself);
   - verdict **forge** → the precise oracle-extraction plan for option 2 (capture correct
     `KDP+0x674`/`hnfo+0x14`/`hnfo+0x28` values from a working paravirtual/QEMU boot).

**Decision rule (set with Ken, 2026-06-14):** consumption reaches the 68k handler AND
the struct begins populating → **real (option 1)**. Consumption stalls before the
handler, OR the struct stays empty even with the round-trip firing → **forge (option 2)**.

## 2. Scope

**In scope**
- Boot under the slot protocol with `SS_NW_IRQ_CONSUME=1` + Smoke-H timer delivery.
- Trace the Cuda SR interrupt through the consumption path waypoints (post latch
  `0x68fff070` → staging → drain → slot-4 `twi` → 68k level-1 handler).
- Characterize the `SC#1=0x0d` divergence: on the consumption critical path, or beside it?
- Watch the NK routing structs across the consumption boot.
- Env-gated trace-only code is permitted (e.g. a new `SS_M15_*` diagnostic gate) **only**
  if existing instruments can't pin the stall; it must be paravirtual-byte-identical.

**Explicitly OUT** (YAGNI / single-purpose)
- No forge implementation.
- No consumption-path *fixes* — this milestone observes, it does not repair.
- No oracle-value extraction (that is the **first task of the option-2 milestone**, only
  if the verdict is forge — per the "trace only, verdict first" scoping decision).

## 3. Method

Known chain (proven prior sessions), so the recon starts where the map ends:

```
VIA timer → IFR_SR ✓ → PIC edge ✓ → NK EXT handler 0x50314880 ✓
          → fallback 0x50325f00 ✓ → SWALLOWS (clears source, returns, no cr2lt)   ← native dead end
```

The recon's target is the **M8 consumption path** (`SS_NW_IRQ_CONSUME=1`), a *different*
route from the native one above.

**Recon boots** (all via `SheepShaver/tools/ss-slot-boot.sh`, never global pkill;
hold instrument sets constant across A/B per the timing-sensitivity rule):

1. **Consumption baseline** — `SS_NW_IRQ_CONSUME=1` + Smoke-H, with `SS_DR_R24_RING=1`
   and probes on the slot-4 round-trip waypoints. Capture where the SR interrupt stalls
   relative to the 68k Cuda handler completing IM init.
2. **`SC#1=0x0d` characterization** — pin what the first syscall's `0x0d` selector
   divergence is and whether it sits on the consumption critical path.
3. **Struct-population watch** — `SS_JIT_TRACE_RING=1` + `SS_JIT_WATCH_ADDR` on
   `hnfo+0x00/+0x14/+0x28` (`0x68ff4f00`-region) and `KDP+0x674` (`0x68fff674`) across the
   consumption boot. Use the span form + `[WATCH-SAMPLE]` to prove frozen-vs-moving
   (value-identical writes are watch-blind — pair with a counter/adjacent discriminator).

**Instrument caveats (load-bearing):** `SS_PROBE_68K` is exact-match and
nested-execute-blind — a "never fires" is UNPROVEN until ring-confirmed (this exact class
of false negative is what produced the retracted M9→M13 keystone). The all-on boot is
~50/50 non-deterministic between a clean park and a post-EXT frontier crash — classify by
durable markers (`[DR68K] first instruction`, `EXT delivered #1`, struct-write events),
never by crash presence. Ring dumps are read with `tools/ring-walk.py`, not by eye.

**Bound:** a small fixed number of boots (target ≤ ~6); budgets are caps,
partial-findings-beat-stalling. If the stall point is pinned and the struct-write question
answered, stop and write the verdict — even if the `SC#1=0x0d` root cause is only
partially characterized (note the residual as the option-N milestone's Task 0).

## 4. Gates

Standing non-regression contract stays green (guaranteed structurally — the recon is
read-only / env-gated, paravirtual untouched):
- `cd SheepShaver && make test-jit` → 353/353 (authoritative; batch for inner loop).
- Paravirtual `make e2e` unaffected (no production path changed).
- `make nw-northstar` report-only observe line at close (non-deterministic, not a gate).

If any trace-only code is added, the commit states the structural-inertness argument and
shows a gated-off byte-identical A/B per AGENT-CONTEXT "Gate tiers."

## 5. Process

`docs/MILESTONE-WORKFLOW.md` is binding: plan on the house template → Task-0 recon with
blocking-answer tables → env-gated (here: read-only) execution → verdict close-out.
Branch `macos-arm64`; never push unprompted. Slot boots only.
