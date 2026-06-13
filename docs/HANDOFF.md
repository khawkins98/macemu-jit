# Project Handoff — resume entry point

> **Status: PAUSED 2026-06-13** · Resume: read this doc, then `docs/AGENT-CONTEXT.md`.
> For session logs: `docs/archive/2026-06/LEARNINGS-2026-06.md`.

## Resume prompt

> Read `docs/HANDOFF.md`, then `docs/AGENT-CONTEXT.md` (authoritative frontier + constants).
> Active work: M10+M11a COMPLETE — `SS_PROBE_68K=0x5000ed08` fires, 3/3 clean (no SIGSEGV).
> r24-stability confirmed by NK static RE; no code change needed (see LEARNINGS 2026-06-13 M11a).
> Next: M11 framebuffer — recon at `docs/planning/machine/FRAMEBUFFER-RECON.md`.
> Process: `docs/MILESTONE-WORKFLOW.md`. Never push without being asked.
> Never global pkill — slot boots only via `SheepShaver/tools/ss-slot-boot.sh`.

---

## Current state (2026-06-13 end-of-session)

- **M8** — shipped, gated-off-green (`SS_NW_IRQ_CONSUME`).
- **M9** — stall fixed (ROM patch removed); 68k handler delivery blocked by deeper issues (see §S4 findings).
- **M10** — COMPLETE (2026-06-13). `SS_PROBE_68K=0x5000ed08` fires. Gate: `SS_M10_CGRP=1`.
- **M11a** — COMPLETE (2026-06-13). Frame-PC stability: static RE confirmed r24 is never clobbered by NK (see LEARNINGS 2026-06-13 M11a). 3/3 × 90s acceptance runs: probe match=1/5, no SIGSEGV. No code change. Next: M11 framebuffer.

### What shipped this session

| Change | File(s) | Status |
|--------|---------|--------|
| `OP_IRQ_NW` moved to end of enum; append-only comment added | `emul_op.h` | ✅ committed `ab258361` |
| Stale WriteMacInt16([KDP+0x67c]) removed from OP_IRQ_NW; comment explains why NK owns that cell | `emul_op.cpp` | ✅ committed `db67aded` |
| tp[25-26] now **always nop** (was conditional on `SS_NW_VIA_IFR`); comment explains design | `rom_patches.cpp` | ✅ committed `db67aded` |
| `SS_NW_VIA_IFR_ROM_PATCH=0` isolation gate added | `rom_patches.cpp` | ✅ committed `db67aded` |
| `[PROGRESS]` atexit line — fires on every exit incl. SIGTERM | `main_unix.cpp`, `sheepshaver_glue.cpp`, `ppc-cpu.cpp` | ✅ committed `3b05ed8e` |
| enum append-only rule added to CONTRIBUTING change table | `CONTRIBUTING.md` | ✅ committed `bf35cc1c` |
| 8.6 boot regression fixed + verified clean | — | ✅ |

---

## M9 blocker: root cause isolated

### What changed since the last HANDOFF

The old HANDOFF listed "two suspects: trampoline tp[25-26] vs ROM patch." That isolation is now
**done by construction**: tp[25-26] are unconditionally nop in the current code regardless of
`SS_NW_VIA_IFR`. The only thing `SS_NW_VIA_IFR=1` now does is apply the ROM patch.

**Confirmed isolation:**
- `SS_NW_VIA_IFR=0` → tp=nop, no ROM patch → healthy
- `SS_NW_VIA_IFR=1 SS_NW_VIA_IFR_ROM_PATCH=0` → tp=nop, no ROM patch → healthy (same as above)
- `SS_NW_VIA_IFR=1` → tp=nop, ROM patch applied → **stall**

**The ROM patch is the sole cause.** No further isolation needed.

### What the ROM patch does

In `rom_patches.cpp` `patch_68k()`, when `SS_NW_VIA_IFR=1`:

```
Pattern found at 0x5000ed08:  48 e7 f0 f0  76 01 60 26
                               movem.l d0-d7/a0-a3,-(sp)  moveq #1,d3  bra.s +0x28
Replaced with:                 fe 79  4e 73  4e 71  4e 71
                               OP_IRQ_NW  rte  nop  nop
```

`fe79` = `M68K_EMUL_BREAK + OP_IRQ_NW` (= 0xfe43 + 54 = 0xfe79).

### What we know about the stall mechanism

- **OP_IRQ_NW never fires** — the 68k world never reaches 0x5000ed08 under the current gates
- The stall (`dec_expiries=5`) sets in *before* the 68k interrupt handler would ever run
- Therefore: the 8 replaced bytes must be **read as data** by PPC boot code before the DEC
  scheduler goes idle — the NK is likely scanning ROM for a known pattern

### [PROGRESS] signatures

Every `ss-slot-boot.sh` run (which sets `SS_TERM_DUMP=1`) now ends with:

```
[PROGRESS] program_max=N dr68k=N dec_expiries=N irq_fired=N
```

| State | Expected |
|-------|----------|
| **Stall** (SS_NW_VIA_IFR=1, full env-on) | `program_max=8 dr68k=1 dec_expiries=5 irq_fired=0` |
| **Healthy baseline** (full env-on, no VIA_IFR) | `program_max≥5 dr68k=1 dec_expiries≥40 irq_fired=0` |
| **Default newworld boot** (no extra env) | `program_max=8 dr68k=1 dec_expiries=5 irq_fired=0` — **this is expected; the newworld ROM is not expected to boot fully** |
| **M9 done** | `dec_expiries≥200 irq_fired≥1` |

> **Note (2026-06-12 session 3):** The "Stall" and "Healthy baseline" rows are measured with the
> full env-on cluster (`SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1`). The default newworld diagnostic boot
> (no extra env) parks at `dec_expiries=5` in ALL configs — this is not a regression, it is the
> NK going idle quickly without a real interrupt source. The stall/baseline distinction only
> manifests with the full env-on cluster active.

---

## Session 3 findings (2026-06-12) — probe campaign results

All probes used the full env-on cluster: `SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1`.
All boots used the absolute path `SheepShaver/tools/ss-slot-boot.sh` from the repo root.

### What we probed and learned

**Probe 1 — NK scheduler task-context cell `[0x68ffd584]` at 0x5032306c:**
- Stall: `[0x68ffd584]=0x68ffd57c` — non-zero (task context pointer present)
- Baseline: `[0x68ffd584]=0x68ffd57c` — **identical**
- ❌ **Hypothesis DISPROVED**: the task-context cell is NOT zero in the stall. The HANDOFF's
  "NK failed to initialize a task" theory is wrong.

**Probe 2 — Task deadline cells `[0x68ffd5b4]/[0x68ffd5b8]` at same PC:**
- Both boots: `0x7fffffff/0xffffffff` (int64_max = "no deadline" sentinel)
- ❌ The task deadline is identical — not the divergence point.

**Probe 3 — Linear mode, all visits to 0x5032306c:**
- **Stall boot: only 1 visit total.** NK reaches the scheduler once, parks DEC at `0x7fffffff`
  (because task deadline = int64_max → deadline too far → DEC armed to long park), and never
  returns (DEC fires ~every 85s at 25MHz, only 5 expiries in 20s boot).
- Baseline boot: 2000+ visits — real tasks with tight deadlines get created, DEC fires ~80Hz.

**Key finding:** OP_IRQ_NW never fires in the stall boot (no `[OP_IRQ_NW] JSR caller` log).
The 68k never reaches `0x5000ed08` even though the ROM patch is applied and the interrupt
delivery mechanism is running. The NK posts the interrupt but something prevents 68k delivery.

### Refined root cause hypothesis

With the ROM patch active, the NK's EXT delivery path (at `0x50314880`) does NOT deliver the
interrupt to the 68k. The NK should jump to the 68k interrupt vector at address `0x64`
(= `0x5000ed08`), but it doesn't — or it does and something causes immediate un-delivery.

The NK's EXT handler has a condition before 68k delivery that involves `[KDP+0x67c]`
(= a pointer to `ECB+0x70 = 0x68fff070`, the NK's interrupt-pending halfword). If this cell
is wrong, the NK may not complete delivery. The trampoline previously wrote a shadow address
here (causing corruption per commit `db67aded` comment), which is now fixed to nop. But the
stall persists — suggesting a different mechanism.

---

## Session 4 findings (2026-06-12) — full root cause analysis

### What was done

1. **ROM patch removed** (`patch_68k()` VIA_IFR block → comment-only). This fixes the stall.
   `SS_NW_VIA_IFR=1` is now a no-op. Harness 353/353 unchanged.

2. **SS_PROBE_68K=0x5000ed08 never fires**, even in baseline. 30s runs with `SS_NW_PIC=1
   SS_NW_IRQ_CONSUME=1`: probe armed but never matched. The 68k interrupt handler is NOT
   being called in any current boot configuration.

### Root cause chain — why 68k interrupt handler never fires

**Layer 1 — NK EXT handler PR-bit check (0x50314880):**
```
bl 0x50313d40          ← save context
rlwinm. r9, r11, 0, 0x10, 0x10   ← extract r11.bit16 = PR bit (user-mode flag)
beq 0x50313ab0         ← if PR=0 (kernel mode) → fallback, skip CGRP entirely
```
In our boot r11=0x0000000a at EXT handler entry → PR=0 → ALWAYS takes fallback.
The DR emulator runs in **kernel mode** (SS_M6A_USER_MSR=0 by default). External
interrupts fired in kernel mode bypass the CGRP 68k-delivery path entirely.

**Layer 2 — CGRP+0x20 not a counter (was misidentified):**
CGRP+0x20 = 0x00000001 in baseline (probe at NK scheduler visit=1). The NK init code
at 0x503115f8 writes `NK_base + 0x3da0 = 0x503143a0` to CGRP+0x20 — it's a **function
pointer**, not a "registered group count." Value 1 means the NK cold-start never ran
that init code for our specific CGRP instance. The `cmpwi r9, 2; blt` guard at
0x50314894 checks whether this pointer is valid (≥2 = non-null/non-error), but this
check is NEVER REACHED because the PR-bit check bails first.

**Layer 3 — CGRP delivery fields uninitialized:**
CGRP+0x38 (guard) = 0, CGRP+0x3c (TABLE_BASE) = 0, CGRP+0x40 (STACK_TABLE) = 0,
CGRP+0x44 (COUNT) = 0. The NK delivery function at 0x503148e0 would also bail on
CGRP+0x38==0. These are normally populated by Mac OS calling NK interrupt-registration
services during System startup — our boot stalls before that point.

### M10 prerequisites to get 68k handler to fire

1. **User-mode DR** — fix SS_M6A_USER_MSR or provide an equivalent mechanism so the
   DR emulator runs with MSR PR=1. This makes external interrupts fired during 68k
   execution take the CGRP path (r11.bit16=1) instead of the fallback.
   SS_M6A_USER_MSR is quarantined (zero-page slide crash) — needs proper fix.

2. **CGRP initialization** — after user-mode is working, populate CGRP:
   - CGRP+0x20 = valid function pointer (NK_base + 0x3da0 = 0x503143a0)
   - CGRP+0x38 = non-zero guard
   - CGRP+0x3c = TABLE_BASE (array of context descriptor pointers)
   - CGRP+0x40 = STACK_TABLE (array of stack pointers per interrupt group)
   - CGRP+0x44 = COUNT (≥10, since NK EXT handler posts source index 9)
   Each TABLE_BASE entry is a pointer to a 2-word descriptor [RFI_target, r2].
   RFI_target must be the 68k interrupt injection entry in the DR emulator (TBD).

### Revised acceptance criteria for M9 close-out

M9 is **partially complete**. The stall is fixed; the probe criterion requires M10 work:

| Criterion | Status |
|-----------|--------|
| Harness 353/353 | ✅ |
| dec_expiries≥200 irq_fired≥1 (baseline w/ full env) | ✅ ~1577 / 376 |
| ROM patch removed (stall root cause fixed) | ✅ committed (this session) |
| SS_PROBE_68K=0x5000ed08:5 fires | ❌ requires M10 (user-mode DR + CGRP init) |

---

## Standing operational notes

- Branch `macos-arm64`; never push unprompted.
- Parallel boots via `SheepShaver/tools/ss-slot-boot.sh` only (never global pkill).
- Cleanup: `SheepShaver/tools/ss-reap.sh`.
- Queued ideas: `docs/planning/BACKLOG.md`.
