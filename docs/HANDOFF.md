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

### Next step: probe the NK EXT delivery path

Compare the EXT handler (`0x50314880`) between stall and baseline to find where delivery
diverges:

```bash
# Boot A — stall
SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1 SS_NW_VIA_IFR=1 SS_TERM_DUMP=1 \
  /path/to/SheepShaver/tools/ss-slot-boot.sh --label stall-ext --timeout 25 \
  --env 'SS_PROBE_PC=0x50314880:r8,r9,r10,r11,[0x68ffe67c],[0x68fff070]'

# Boot B — baseline
SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1 SS_NW_VIA_IFR=0 SS_TERM_DUMP=1 \
  /path/to/SheepShaver/tools/ss-slot-boot.sh --label base-ext --timeout 25 \
  --env 'SS_PROBE_PC=0x50314880:r8,r9,r10,r11,[0x68ffe67c],[0x68fff070]'
```

Also confirm 68k never reaches handler in stall:
```bash
SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1 SS_NW_VIA_IFR=1 SS_TERM_DUMP=1 \
  /path/to/SheepShaver/tools/ss-slot-boot.sh --label stall-68k --timeout 25 \
  --env 'SS_PROBE_68K=0x5000ed08:5'
```

If the EXT handler probe shows different register values at the delivery decision point,
disassemble the NK EXT handler from `0x50314880` to find the guard condition and what it reads.

### Acceptance criteria (unchanged)

- `SS_PROBE_68K=0x5000ed08:5` fires (68k reaches the handler)
- `[PROGRESS] dec_expiries≥200 irq_fired≥1` with `SS_NW_VIA_IFR=1 SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1`
- Harness 353/353 throughout

---

## Standing operational notes

- Branch `macos-arm64`; never push unprompted.
- Parallel boots via `SheepShaver/tools/ss-slot-boot.sh` only (never global pkill).
- Cleanup: `SheepShaver/tools/ss-reap.sh`.
- Queued ideas: `docs/planning/BACKLOG.md`.
