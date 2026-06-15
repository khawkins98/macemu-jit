# M13 Diagnostic #2 — why the NK consumes EXT but never delivers to the 68k handler

**Date:** 2026-06-13. **Type:** RE-only fan-out (3 threads). **Predecessor context:**
`docs/planning/2026-06-13-m13-taskA-bakeoff.md` (full mechanism map + the 4 falsified injection
approaches) and `LEARNINGS.md` top entry (read both first).

## Premise (CONFIRMED by diagnostic #1, 2026-06-13)

Interrupt delivery to the **68k handler at `0x5000ED08`** is the real blocker for the M9→M13
pixel/QuickDraw push. Injection-OFF baseline (`SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1 SS_M11_FB=1`):
- Scheduler healthy (dec_expiries=67393) BUT `0x5000ED08` **never fires** (PROBE68K armed, 0 matches).
- The 68k world spins in a polling/wait loop (HOT-PC at DR interpreter `0x50468ae4`, r24 cycling
  scattered ROM 68k PCs) → pre-System dead-end at ~15s ([ALARM] model-rejection/hang).
- **`irq_fired` is MISLEADING** — it counts EXT consumed *at the NK* (g_exc_consume_stats.fired,
  deferred-edge at PPC `0x50325fd0` / `0x50318014`), NOT delivery to the 68k handler. The real
  delivery signal is `0x5000ed08` running (PROBE68K match).
- CGRP is the ONLY thing that makes `0x5000ED08` fire (on→handler runs but crashes on DR re-entry;
  off→never runs). Hand-injection to complete delivery is architecturally dead (falsified 4×).

**The question:** why does the NK *consume* the EXT but never *route* it to the 68k handler — and
what would make it do so natively?

## Banked facts (verified this session — re-verify addresses before relying)

- 68k handler: `0x5000ED08`. DR is a recompiler: steady-state 68k runs in cache `0x17fa0000+`;
  DR_WARM `0x5046e9d8` is a cold trampoline; DR interpreter hot point `0x50468ae4`.
- NK consume path PCs: `0x50325fd0`, `0x50318014` (deferred-edge fire). NK EXT body: `0x50314880`
  (IN the 4 MB ROM dump `/Users/Shared/macemu/dumps/rom901.bin` @ base 0x50000000 → disassemblable).
- glue.cpp comment flags: the EXT body's frontier handler is the `[KDP+0x5b0]` fallback
  (`0x50325f00`) because the **registered-handler table is NOT installed (W2L-1)**.
- ECB=`0x68fff000`; 68k regfile save area (DR PPC-resume ctx) `0x68fff740`; A7 slot `0x68fff50c`.
- CGRP structs (when `SS_M10_CGRP=1`): base `0x68ffc1c0`, TABLE `0x68ffc210`, DESC `0x68ffc260`,
  STUB `0x68ffc268`. NK EXT body checks user-mode (PR bit) before the CGRP route.
- Tools: `SS_PROBE_PC` (PPC block-entry), `SS_PROBE_68K` (DR 68k-PC dispatch), `SS_DUMP_ROM`,
  the QEMU differential rig (`SheepShaver/tools/qemu-rig.sh` — behavioral oracle, MMIO caveats).

## Threads (one subagent each — RE ONLY, NO code fixes)

**Thread 1 — NK EXT routing.** RE the NK EXT body (`0x50314880`, in ROM) + the consume path
(`0x50325fd0`/`0x50318014`). Trace exactly where an EXT goes: which condition sends it to the
`[KDP+0x5b0]` fallback (`0x50325f00`) vs. a route that would reach the 68k vector. What is the
registered-handler table, where does it live, what installs it, and why is it "not installed" (W2L-1)?
Deliver: the decision tree the NK uses for EXT, and the precise condition/state that currently
prevents 68k-handler delivery.

**Thread 2 — 68k-world readiness.** What must the 68k side have set up to *receive* a level interrupt
at `0x5000ED08` — the 68k interrupt vector/handler install, the VIA IFR/IER state the handler checks,
the autovector level. Is the spin loop (DR interpreter, r24 cycling 0x5000010a/0x50038a2c/0x5006621c)
a known wait-for-tick loop? Identify (probe r24 + disassemble those ROM 68k PCs) what event/flag the
68k world is polling for. Deliver: what the 68k world is waiting on and what state it needs before an
interrupt would do anything useful.

**Thread 3 — working-delivery oracle.** Establish what a CORRECT NK→68k interrupt delivery looks like,
as a reference. Use the QEMU 9.0.1 rig (behavioral oracle) and/or RE of the NK CGRP/EXT completion
path to characterize: on a healthy boot, how does the NK hand a level interrupt to the 68k world (what
it saves, what PC it resumes, the round trip through `0x5000ED08` and back). Deliver: the reference
shape of a working delivery, to compare against our broken one. (Caveats: QEMU MacIO at 0x80000000 not
0xF3000000; behavioral only, never an address oracle — see LEARNINGS 2026-06-12.)

## Shared rules (BINDING)
- RE ONLY. No source fixes. If you write throwaway instrumentation, keep it gated under `SS_M10_CGRP`
  and revert it; baseline must stay byte-identical, harness 353/353.
- NEVER pkill/kill-by-name. Diagnostic boots ONLY via `SheepShaver/tools/ss-slot-boot.sh`. Concurrent
  boots in different slots are safe; clean stale leases with `ss-reap.sh`.
- Fresh worktree: bootstrap per the bake-off plan's "Worktree build bootstrap" (autogen+configure with
  ccache CC/CXX, then `make build-ss`) only if you need to build for instrumentation; pure static RE +
  probes against the existing binary may not need a rebuild.
- Re-verify every address by probe/disasm before relying on it (DR region is RAM-resident; 9.0.1 NW
  diagnostic config; values may be config-specific).

## Deliverable (each agent's final message)
A structured RE finding: (1) the question answered (or precisely where blocked), (2) evidence
(probe/disasm with addresses + tags), (3) the concrete implication for "make the NK deliver to
0x5000ED08 natively", (4) honest confidence/caveats. No code.
</content>
