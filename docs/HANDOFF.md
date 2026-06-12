# Project Handoff — resume entry point

> **Status: PAUSED 2026-06-12** · Resume: read this doc, then `docs/AGENT-CONTEXT.md`.
> For session logs: `docs/archive/2026-06/LEARNINGS-2026-06.md`.

## Resume prompt

> Read `docs/HANDOFF.md` then `docs/AGENT-CONTEXT.md` (authoritative frontier + constants).
> Then read `docs/planning/ROADMAP.md` §Machine Layer milestones.
> Process: `docs/MILESTONE-WORKFLOW.md`. Never push without being asked.

## Current state

- **M8** (NK-level interrupt consumption) — shipped, gated-off-green (`SS_NW_IRQ_CONSUME`).
- **M9** (VIA-IFR) — stalled. Infrastructure committed; root cause of boot stall unresolved.

### M9 blocker: what we know

With `SS_NW_VIA_IFR=1 SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1`:
- 68k starts (`[DR68K] first instruction` fires), then stalls: dec_expiries=5, fired=1 (vs 7222/112 baseline)
- OP_IRQ_NW at 0x5000ed08 **never fires** — stall is upstream of the handler
- Without VIA_IFR: healthy baseline (dec_expiries=7222, fired=112, OP_IRQ_NW not installed)
- 0x5000ed08 is also never reached without the patch — SS_NW_IRQ_CONSUME consumes interrupts at NK level; the 68k interrupt handler only fires when the NK delivers to 68k

### What's been shipped (this session)

- **OP_IRQ_NW handler** (`emul_op.cpp` case OP_IRQ_NW): frame-aware return — detects 68020 interrupt exception frame vs JSR return address from `[A7]` high byte (`>= 0x40` = JSR, pops 4-byte ret addr, sets `r->pc`). Harness 353/353.
- **M68kRegisters.pc** (`main.h`, `sheepshaver_glue.cpp`): new `pc` field; initialized from `gpr(24)` in `execute_emul_op`, written back after `EmulOp` — allows EMUL_OP handlers to redirect 68k PC on return.
- **rom_patches.cpp**: `via_nw901_int` now installs `OP_IRQ_NW` instead of `OP_IRQ`.

### Root cause: still unknown — two suspects

`SS_NW_VIA_IFR=1` enables TWO things:
1. **Trampoline tp[25-26]**: writes `[KDP+0x67c]` = `0x68ff6084` (NK cold-init phase)
2. **ROM patch**: replaces 8 bytes at `0x5000ed08` with `OP_IRQ_NW + rte + nop + nop`

The stall could be caused by either. RECON §8b declared "ROM patch is sole cause" but that was before we verified OP_IRQ_NW never fires. **Next step: isolate.**

### Next-step isolation probe (2 boots)

**Boot A — trampoline only, no ROM patch (add a `SS_NW_VIA_IFR_NOROM=1` bypass or comment out the `via_nw901_int` block temporarily):**
If stall persists → trampoline is the cause.  
If healthy → ROM patch is the cause.

**Quick path (no code change): run with `SS_NW_VIA_IFR=1` but check what's at 0x5000ed08 post-boot to confirm the patch applied, then do a separate boot that skips just the ROM patch by probing an address that's only hit if the patched bytes ran.**

The cleaner approach: add `SS_NW_VIA_IFR_ROM_PATCH=1` as a separate gate for the `via_nw901_int` block, so trampoline and patch can be tested independently.

### Verification criteria (unchanged)

- `SS_PROBE_68K=0x5000ed08:5` fires (confirms 68k reached the handler)
- With `SS_NW_VIA_IFR=1`: dec_expiries recovers to ~2393
- Harness 353/353 throughout

## Standing operational notes

- Branch `macos-arm64`; never push unprompted.
- Parallel boots via `SheepShaver/tools/ss-slot-boot.sh` only (never global pkill).
- Cleanup: `SheepShaver/tools/ss-reap.sh`.
- Queued ideas: `docs/planning/BACKLOG.md`.
