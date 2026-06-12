# Project Handoff — resume entry point

> **Status: PAUSED 2026-06-12** · Resume: read this doc, then `docs/AGENT-CONTEXT.md`.
> For session logs: `docs/archive/2026-06/LEARNINGS-2026-06.md`.

## Resume prompt

> Read `docs/HANDOFF.md`, then `docs/AGENT-CONTEXT.md` (authoritative frontier + constants).
> Active work: M9 VIA-IFR stall — root cause is now isolated to the ROM patch alone (see §below).
> Process: `docs/MILESTONE-WORKFLOW.md`. Never push without being asked.
> Never global pkill — slot boots only via `SheepShaver/tools/ss-slot-boot.sh`.

---

## Current state (2026-06-12 end-of-session)

- **M8** — shipped, gated-off-green (`SS_NW_IRQ_CONSUME`).
- **M9** — infrastructure committed; stall root-cause **isolated to ROM patch** (see below).

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
| **Stall** (SS_NW_VIA_IFR=1) | `program_max=8 dr68k=1 dec_expiries=5 irq_fired=0` |
| **Healthy baseline** (no VIA_IFR) | `program_max≥5 dr68k=1 dec_expiries≥40 irq_fired=0` |
| **M9 done** | `dec_expiries≥200 irq_fired≥1` |

---

## Next step: find what reads 0x5000ed08

The question is: **what PPC code reads those 8 bytes as data, and what does it do with them?**

### Recommended probe (2 boots, ~5 min)

Probe the NK DEC scheduler decision point (`0x5032306c`) — the code that either sets a real
deadline or `DEC=0x7fffffff` (idle). Compare the task-context cell between stall and baseline:

```bash
# Boot A — stall: what does the NK see at the scheduling decision?
SS_NW_VIA_IFR=1 SS_TERM_DUMP=1 tools/ss-slot-boot.sh --label stall-probe --timeout 20 \
    --env 'SS_PROBE_PC=0x5032306c:r1,r0,[0x68ffd584],[0x68ffd588]'

# Boot B — baseline: same probe, healthy path
SS_NW_VIA_IFR=0 SS_TERM_DUMP=1 tools/ss-slot-boot.sh --label baseline-probe --timeout 20 \
    --env 'SS_PROBE_PC=0x5032306c:r1,r0,[0x68ffd584],[0x68ffd588]'
```

`0x68ffd584` = `[r1-0xa7c]` = the NK task-context cell the scheduler reads to decide whether
to arm a real deadline or go idle. If it's 0 in the stall boot and non-zero in baseline, that
confirms the ROM patch is preventing something from writing the task deadline.

### If the probe shows the cell is zero in the stall

The NK failed to initialize a task entry that depends on finding the interrupt handler's address.
The original first word `0x48e7f0f0` (`movem.l`) is likely the pattern it searches for.
Next step: search the NK PPC code for a load from ROM range `0x5000e000–0x5001f000` and find
what it does with the result (likely setting up a task block at `0x68ffd584`).

### Acceptance criteria (unchanged)

- `SS_PROBE_68K=0x5000ed08:5` fires (68k reaches the handler)
- `[PROGRESS] dec_expiries≥200 irq_fired≥1` with `SS_NW_VIA_IFR=1`
- Harness 353/353 throughout

---

## Standing operational notes

- Branch `macos-arm64`; never push unprompted.
- Parallel boots via `SheepShaver/tools/ss-slot-boot.sh` only (never global pkill).
- Cleanup: `SheepShaver/tools/ss-reap.sh`.
- Queued ideas: `docs/planning/BACKLOG.md`.
