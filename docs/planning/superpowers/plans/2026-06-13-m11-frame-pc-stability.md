# M11a — Frame-PC stability: reliable interrupted-68k-PC at STUB entry

> **Status: COMPLETE 2026-06-13** · Branch: `macos-arm64`
> Predecessor: M10 COMPLETE (2026-06-13). Gate: `SS_M10_CGRP=1`.
> M10 open tail: r24 at STUB entry is NK-restored when the NK fully restores registers
> before the CGRP RFI, but is NK-internal in some timing runs → non-deterministic
> crash-after-probe. Acceptance: STUB-injected 68k exception frame has a reliable PC in
> all clean EXT deliveries; probe fires + no SIGSEGV within 120s in 3/3 slot runs.

---

## Goal

Find where the NK saves the interrupted DR PC at EXT exception time and read it
directly, eliminating the timing-dependent r24 ambiguity that causes occasional
crash-after-probe.

**PASS criterion (gate):** Three consecutive 90s slot runs with `SS_NW_PIC=1
SS_NW_IRQ_CONSUME=1 SS_M10_CGRP=1` all produce `SS_PROBE_68K=0x5000ed08` match(es) AND
no SIGSEGV in the boot log. Harness 353/353 unchanged.

**DIAGNOSTIC (not a gate):** `dec_expiries≥200 irq_fired≥1` in `[PROGRESS]` atexit
(already true at baseline; cited for regression detection only).

**Stop rule:** if a reliable save slot can't be found within 2 static RE sessions AND
2 boot-probe passes, park this and proceed to M11b (framebuffer). The crash is
intermittent and does not block M11b.

---

## What M10 established (carry forward verbatim)

These are pinned facts from M10 — **re-verify the dynamic ones with a probe before
depending on them in implementation.**

| Field | Value | Source |
|-------|-------|--------|
| CGRP base | `*(KDP-0x338)` = 0x68ffc1c0 | [PROBE✓] M10 |
| STUB base | 0x68ffc268 | [PATCH] rom_patches.cpp |
| KDP | 0x68ffe000 (SPRG0) | [STATIC] |
| KDP+4 = old A7 | `*(0x68ffe004)` = saved SPRG1 = pre-interrupt r1 | [PROBE✓] M10 |
| DR_WARM | 0x5046e9d8 | [STATIC] sheepshaver_glue.cpp `DR_WARM_BASE` |
| NK EXT handler | 0x50314880 | [STATIC] ROM disassembly |
| CGRP RFI target | STUB_ADDR = 0x68ffc268 | [PATCH] |

**Known ambiguity (the M11a problem):**

At the NK's `CGRP RFI` into the STUB, `r24` should hold the interrupted DR-emulator PC
(= interrupted 68k PC in the DR's register convention). In *most* runs this is correct
because the NK restores r24 from the saved context before RFI. But in some timing
scenarios the NK has nested interrupt state — `r16` points to a *different* context
block (0x68ffb5c0 instead of 0x68fff000) and r24 is NK-internal junk. Reading r24 live
is therefore non-deterministic across runs.

**Why `r16+0x1c4` doesn't help:** LEARNINGS 2026-06-13 confirmed that `r16+0x1c4`
contains the DR PC only when r16 = the specific DR context block (0x68fff000). When
the NK has nested, r16 = a different block that has garbage at +0x1c4. r16 value at
STUB entry varies with NK nesting depth.

---

## Task 0 — Recon: where does the NK save r24 at EXT entry?

**This is the entire milestone.** All implementation work is mechanical once Task 0
answers the following blocking question.

### Blocking question

> At what guest-RAM address does the NK save the interrupted DR-emulator's r24 at the
> moment of EXT exception entry (0x50314880: `bl 0x50313d40` = context-save), and is
> that address accessible at STUB-entry time (i.e., it hasn't been overwritten by the
> time the NK RFIs into STUB)?

### Approach

**Static RE first (no boots):**

1. Disassemble NK context-save routine at 0x50313d40.
   - The `bl 0x50313d40` at EXT handler entry is the context-save path. It saves the
     interrupted context somewhere in the NK's scratch region (near KDP).
   - Find where r24 is stored: look for `stw r24, 0xNNN(rYY)` or `stfd` cluster
     covering the GPR file. The save might be relative to r1 (stack pointer at entry),
     r2 (RTOC), or an explicit thread-context pointer in r16/r30.
   - Recipe: `SS_DUMP_ROM=/tmp/rom901_dec.bin ./SheepShaver` once (or reuse existing
     `/tmp/rom901_decompressed.bin` if still on disk — verify sha256), then capstone
     slice `0x313d40:0x313e00`.

2. Check whether the save slot survives to STUB entry.
   - The NK may re-use r24 as a scratch register between context-save and the CGRP
     delivery RFI. If it does, the save slot is the only reliable source.
   - Trace the register use from 0x50313d40 through 0x50314880's CGRP delivery path
     to the RFI target. Any `lwz r24, X(rY)` before the RFI is a restore — that's
     the save address.

**Probe validation (1-2 boots, slot protocol):**

3. Once the static RE names a candidate address (call it SAVE_ADDR), verify with:
   ```bash
   SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1 SS_M10_CGRP=1 \
   SS_PROBE_PC=0x50314880:r24,[SAVE_ADDR] \
   SheepShaver/tools/ss-slot-boot.sh --label m11a-task0 --timeout 60
   ```
   At EXT handler entry (0x50314880), `r24` should equal `[SAVE_ADDR]` if the NK
   saves before any modification. Or probe at the RFI address (wherever in 0x503148xx
   the CGRP RFI lives) to confirm `[SAVE_ADDR]` still holds the DR PC at that moment.

4. Tag findings with `[STATIC]` or `[PROBE✓]` and record in a Task-0 addendum before
   proceeding to implementation.

### Residue fallback (if no static answer within budget)

If the context-save routine is too complex to trace statically in 2 sessions:
- Probe `[r1:0x100]` at the EXT handler entry PC (0x50314880) to dump the NK's stack
  frame — the save is almost certainly on-stack. A 64-word dump will show r24's value
  at a known offset.
- This is an observation probe (no source edit, just `SS_PROBE_PC`), so it's cheap.

**Budget:** 2 capstone sessions (static) + 2 slot boots (probe). If still no answer,
park and go to M11b.

---

## Task 1 — Implementation (conditional on Task 0)

Once `SAVE_ADDR` and its stability are confirmed:

1. In `sheepshaver_glue.cpp` EXT shim (gated `SS_M10_CGRP=1`), change STUB instruction
   at offset +16 from:
   ```c
   WriteMacInt32(STUB_ADDR_EXT + 16, 0x7F0CC378u);  // mr r12, r24  (live r24 — non-deterministic)
   ```
   to a load from SAVE_ADDR. The load sequence depends on the address:
   - If SAVE_ADDR = `KDP + OFFSET` for small OFFSET: `lwz r12, OFFSET(SPRG0_reg)` — but
     SPRG0 is not directly loadable in STUB context; use the established `lis/lwz` pattern.
   - If SAVE_ADDR is a fixed guest address: use `lis r12, HI(SAVE_ADDR); lwz r12, LO(SAVE_ADDR)(r12)`.
   - Encoding budget: this replaces 1 instruction at offset +16; if the load sequence
     needs 2 instructions, it can take offsets +16 and +20 and shift subsequent
     instructions — but then STUB_SIZE must increase and rom_patches.cpp must match
     exactly. Prefer a 1-instruction fix if SAVE_ADDR is reachable.

2. Make the **identical** change in `rom_patches.cpp` `patch_68k()` STUB initializer
   (same gate). The two STUB initializers must remain byte-for-byte identical.

3. Gate: `SS_M10_CGRP=1` (no new gate; this is a correctness fix within M10's scope).

4. Run `SS_HARNESS_BATCH=1 make test-jit` — must stay 353/353.

---

## Acceptance

Run three consecutive 90s slot boots:
```bash
for i in 1 2 3; do
  SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1 SS_M10_CGRP=1 \
  SS_PROBE_68K=0x5000ed08:5 \
  SheepShaver/tools/ss-slot-boot.sh --label m11a-run$i --timeout 90
done
```

**PASS:** all 3 produce `SS_PROBE_68K=0x5000ed08` match(es) AND no SIGSEGV line in
the boot log. Harness 353/353. ROADMAP M11a → COMPLETE.

**FAIL-mode:** if SIGSEGV persists after implementing Task 1, log it as M11a-v2
(nested-context residual problem) and park; proceed to M11b.

---

## Authoritative inputs

| Doc | Section |
|-----|---------|
| `docs/AGENT-CONTEXT.md` | Current frontier, boot recipes, instrument caveats |
| `docs/HANDOFF.md` | M10 open tail, frame-PC non-determinism detail |
| `LEARNINGS.md` (2026-06-13) | STUB register state at entry; r16+0x1c4 unreliability |
| `SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp` | EXT shim, STUB_ADDR_EXT, current STUB words |
| `SheepShaver/src/rom_patches.cpp` | STUB_ADDR, CGRP init, current STUB words |
| `SheepShaver/tools/README-slots.md` | Slot protocol |

---

## Self-review / tensions flagged for red team

- **Is the save slot actually stable?** The NK may overwrite the save slot before
  the CGRP RFI (e.g., during the delivery function at 0x503148e0). Task 0 must
  verify stability explicitly — static tracing to the RFI, not just to the save.
- **The STUB is in guest RAM and the NK re-syncs CGRP between deliveries.** The EXT
  shim already re-writes the STUB on every EXT call. A 2-instruction load sequence
  that shifts subsequent instruction offsets would need careful counting — the shim
  and the patch initializer must agree on STUB layout. Flag this during Task 1 code
  review.
- **One EXT per 60s+ in diagnostic boot** means acceptance can't easily run 3 trials
  without a 3-4 minute wall-clock wait. The 90s timeout may be too short for some
  runs. Consider `--timeout 120` for acceptance, or accept "match=1 + no SIGSEGV"
  as the criterion (the ":5" cap is already collection-only, not a minimum count).
- **Stop rule is operationalized:** two static RE sessions + two probe passes. If that
  budget expires without a pinned SAVE_ADDR, park M11a and start M11b. Do not let
  NK context-save RE consume a third session — the crash is intermittent and M11b
  delivers visible progress.
