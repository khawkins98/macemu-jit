# Project Handoff — resume entry point

> **Status: PAUSED 2026-06-12** · Resume: read this doc, then `docs/AGENT-CONTEXT.md`.
> For historical session logs: `docs/archive/2026-06/LEARNINGS-2026-06.md`.

## Resume prompt

> Read `docs/HANDOFF.md` then `docs/AGENT-CONTEXT.md` (authoritative frontier + constants).
> Where we are: M8 slot-4 consumption shipped gated-off-green (`SS_NW_IRQ_CONSUME`).
> Active: **M9 VIA-IFR** — the via_nw901_int patch (OP_IRQ+rte @0x5000ed08) stalls the
> 68k boot. Start with Probe 1 below. Process: `docs/MILESTONE-WORKFLOW.md`. Never push
> without being asked.

## Active frontier: M9 VIA-IFR boot stall

**State (session 4):** `SS_NW_VIA_IFR=1` reduces dec_expiries from 2393 to 6. The ROM
patch is the sole cause. `SS_PROBE_68K=0x5000ed08` never fires with the patch on.

**Probable cause:** 0x5000ed08 is entered via JSR during early 68k init, not only via
interrupt. Our `rte` after `OP_IRQ` corrupts the caller's stack (pops an interrupt frame
where a normal return was expected).

**Fix candidate:** replace `rte` with frame-aware return — detect interrupt vs JSR from
the SR format word on the stack.

## Next-step probes (start here — 2 minutes total)

**Probe 1 — confirm the unpatched handler fires at all:**
```bash
SheepShaver/tools/ss-slot-boot.sh --label via-no-patch-probe \
  --env 'SS_NW_IRQ_CONSUME=1' --env 'SS_NW_PIC=1' \
  --env 'SS_PROBE_68K=0x5000ed08:5' --timeout 25
```
Expected: `[PROBE68K] MATCH` lines with d3=1 (interrupt path).
If probe NEVER fires without the patch → wrong vector at 0x64 or EMUL_OP_IRQ dispatch broken.

**Probe 2 — if probe 1 fires, identify JSR vs interrupt callers:**
```bash
SheepShaver/tools/ss-slot-boot.sh --label via-no-patch-callers \
  --env 'SS_NW_IRQ_CONSUME=1' --env 'SS_NW_PIC=1' \
  --env 'SS_PROBE_68K=0x5000ed08:10' --timeout 25
```
If early matches have SP pointing to a normal return frame → JSR caller confirmed.

## Open questions

| # | Question | Status |
|---|----------|--------|
| 1 | What sets `$0d94` at tick time? | OPEN |
| 2 | What handler is at 0x64 in our boot? (probe says 0x5000ed08 — differs from QEMU's 0x5000ec50) | OPEN |
| 3 | What does the `$6e4` vector chain expect on dismissal? | OPEN |
| 4 | SC#1 r0=0x0d (M8 residue) — explained or fixed before `SS_NW_IRQ_CONSUME` flip | OPEN |

## Verification criteria

- `SS_PROBE_68K=0x5000ed08:5` fires with d3=1
- With `SS_NW_VIA_IFR=1`: dec_expiries recovers to ~2393 (matching baseline)
- Boot advances past PROGRAM#5 park
- Harness 353/353 throughout

## Operational

- Branch `macos-arm64`; never push unprompted.
- Parallel boots via `SheepShaver/tools/ss-slot-boot.sh` only (never global pkill).
- Cleanup: `SheepShaver/tools/ss-reap.sh`.
- Queued ideas and future backlog: `docs/planning/BACKLOG.md`.
