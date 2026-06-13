# LEARNINGS — macOS ARM64 JIT work

Durable lessons that prevent repeating expensive mistakes. Newest entries at the top.
For the full historical session journal: `docs/archive/2026-06/LEARNINGS-2026-06.md`.

---

## 2026-06-13 — NW frontier boot is non-deterministic; a bare SIGSEGV is NOT a regression

Building a NewWorld boot-progress signal (`make nw-northstar`) surfaced that the
all-on diagnostic boot (`SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1 SS_M10_CGRP=1`) is **~50/50
non-deterministic between two branches at the SAME frontier**, by timing alone:

- **Park branch** — reaches atexit, `program_max=8 dr68k=1 ext=1 segv=0`, parks at the
  DEC long-park.
- **Post-EXT crash branch** — after `EXT delivered #1` and the CGRP STUB handoff, the
  68k world runs off into a wild-execution wall and SIGSEGVs at a **variable** `ea`
  (observed `0x100000`, `0x55590000`, `0x0c29411a`), often **before the atexit readout
  prints** (so `program_max` reads 0 from the log even though the boot got *further*
  than the park branch).

**Consequences for any NW regression signal:**
1. **SIGSEGV-presence is useless as a pass/fail** — the crashing branch made *more*
   progress than the clean one. A naive `grep SIGSEGV → fail` cries wolf on ~half of
   all runs and gets ignored within a week (this is the "0xDEADBEEF noise" the M11 retro
   flagged).
2. **The crash `ea` is not a stable signature** (wild execution → varies) — don't
   allowlist addresses.
3. **Classify by DURABLE in-boot markers**, not the maybe-absent atexit readout:
   `[DR68K] first instruction` (68k DR started) and `EXT delivered #1` print *during*
   boot and survive the crash. A crash at/after those = known frontier wall (`ok`); a
   crash with DR never starting = a real regression below the frontier.

`nw-northstar`'s verdict encodes exactly this. The underlying crash is the M10/M11 open
tail (interrupted-PC handoff after CGRP RFI) and is a real future correctness item — but
it is *known frontier noise today*, not a per-run regression.

## 2026-06-13 — CGRP+0x20 is zero throughout entire Mac OS 8.6 paravirtual boot

Watch on guest addr 0x68ffc1e0 (= CGRP base 0x68ffc1c0 + 0x20) through a full 8.6 boot
to Finder idle (~141s, 1.4 billion records): value never leaves 0x00000000. Mac OS 8.6 /
OldWorld 1.1 ROM NK does not use or initialize this CGRP struct. The paravirtual interrupt
path that drives the working 8.6 boot is independent of the CGRP mechanism entirely.

**Rule:** CGRP at 0x68ffc1c0 is NewWorld-only (9.0.1 ROM NK). `SS_M10_CGRP` is correctly
gated on `MachineProfileIsNewWorld()`. M10/M11a changes carry zero risk to the 8.6 path.

---

## 2026-06-13 — M11a: r24 at STUB entry is always the interrupted 68k PC (static RE)

Deep NK static RE (2026-06-13 session 2) traced the full EXT→CGRP→STUB delivery path:

1. `bl 0x50313d40` (context-save): saves r0, r7-r13 to ctx. r24 **not touched**.
2. `bl 0x503238ac`: saves r14-r31 to ctx — `stw r24, 0x1c4(r6)` at 0x503238f0 captures r24. r24 **not modified**.
3. `bl 0x503148e0` (CGRP delivery): uses r16-r23 as temporaries. r24 **never touched**. Ends with RFI.
4. At STUB entry: r24 = interrupted 68k PC — **always stable, never clobbered**.

The restore counterpart (`bl 0x5032391c` at 0x503148b0, `lwz r24, 0x1c4(r6)`) is only reached on the CGRP delivery early-exit path (beqlr/bgelr returns), **never** on the successful RFI path. So `mr r12, r24` in the STUB is always reading the correct value.

**M10 open tail ("non-deterministic crash-after-probe") does not reproduce:** 3/3 × 90s slot runs with `SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1 SS_M10_CGRP=1 SS_PROBE_68K=0x5000ed08:5` all produced match=1/5 and no SIGSEGV. M11a COMPLETE.

**Previous LEARNINGS correction:** "r16+0x1c4 is UNRELIABLE" was correct (r16 ≠ ctx at all times), but "live r24 is non-deterministic" was speculative and wrong. The NK does not clobber r24 between EXT entry and CGRP RFI.

---

## 2026-06-13 — M10 CGRP delivery: key findings

**CGRP field layout confirmed** (9.0.1 ROM, KDP=0x68ffe000):
- CGRP base: *(KDP-0x338) = 0x68ffc1c0
- CGRP+0x20 = NK delivery function (0x503143a0); +0x38 = non-zero guard; +0x3c = TABLE_BASE
- +0x40 = STACK_TABLE; +0x44 = count ≥ 10 (src_idx=9 for EXT); +0x4c = *(KDP-0x1c) (live-mirrored)
- TABLE (40B) + STACK (40B) + DESC (8B) + STUB (64B) must be in guest RAM (NK re-syncs these from ROM tables between deliveries; must restore on every EXT call)

**STUB register state at entry (NK CGRP RFI)**:
- r1 = 2 (NK-internal value, NOT old_A7); must load A7 from KDP+4 (= *(0x68ffe004) = saved SPRG1)
- r16 = *(KDP-0x14) at delivery time (context block; the NK may have nested further by STUB time)
- r24 = interrupted 68k PC (when NK fully restores from saved state before CGRP RFI)

**r16+0x1c4 is UNRELIABLE for frame PC**: different NK context blocks (r16 value varies by nesting level) have garbage at +0x1c4. `mr r12, r24` (live r24 at STUB) is the best available source for the interrupted PC without deep NK RE. Non-deterministic crash remains when NK modifies r24 before CGRP delivery RFI.

**EXT fires only once in diagnostic boot**: The NW diagnostic config (9.0.1 ROM, nogui, newworld) generates 1 host-IRQ edge per 60s+ window. The 68k handler at 0x5000ed08 acknowledges the VIA interrupt; without full Mac OS 8.6 initialization (60Hz VIA reprogramming), no further EXT fires occur. "5 matches" in `SS_PROBE_68K=0x5000ed08:5` is the collection cap, not a requirement count — one match proves the criterion.

**`mr r12, r24` encoding** (may-need-verify): `or r12, r24, r24` = 0x7F0CC378. Verify if ever suspecting a STUB encoding bug.

## AltiVec bottom line (⭐ read first)

Real-app AltiVec WORKS end-to-end as of 2026-06-07. The AArch64 JIT always compiles
PPC AltiVec→ARM64 NEON (validated by `make test-jit`, 353/353). The only gap was
detection: under the OldWorld 1.1 ROM the guest never registers the `'ppcf'` gestalt.
Fix = opt-in `altivec` pref → `SS_FORCE_ALTIVEC` env registers `'ppcf'` via `_NewGestalt
$A3AD` with bit `0x10` (= `1<<gestaltPowerPCHasVectorInstructions`, NOT `0x40` — the
0x40 typo caused a multi-hour "FC ignores gestalt" detour). Verified: Fractal Carbon
detects AltiVec and runs its vector kernel through the JIT (`AltiVec=160`). Opt-in/
default-off because 8.6/9.0 don't VR-context-switch (single-app-safe).

---

## 2026-06-12 — Use SS_PROBE_68K for 68k code paths

`SS_PROBE_PC` fires at PPC JIT block-entry addresses. It is **completely blind** to 68k
ROM addresses. When investigating a 68k interrupt handler (e.g. 0x5000ed08), the right
tool is:
```bash
SS_PROBE_68K=0x5000ed08:5 SheepShaver/tools/ss-slot-boot.sh --label probe --timeout 25
```
`SS_PROBE_68K=0xPC:N` fires at the DR dispatch hook, first N matches (linear), shows the
68k regfile (d0–d7, a0–a7) and PPC context.

**Rule: when the bug is in 68k code, start with SS_PROBE_68K. Use SS_PROBE_PC only for
PPC-world investigation.**

---

## 2026-06-12 — QEMU rig: five pitfalls + one stage-matching rule

Tools: `SheepShaver/tools/qemu-rig.sh` + `qemu-mon.py`. Full pitfall context also in
`docs/AGENT-CONTEXT.md` "Instruments" section.

1. **ANSI escape sequences in the monitor.** Strip with `re.sub(r'\x1b\[[0-9;]*[A-Za-z]',
   '', ...)` AND skip everything before the first `\n`. Use `qemu-mon.py` — it handles this.
   The monitor accepts only ONE connection at a time; `ConnectionRefusedError` after a stale
   session → kill and restart QEMU.

2. **`xp` reads physical RAM; `x` reads virtual (guest) address space.** After the NK
   enables the MMU, always use `x` for Mac OS guest memory. `xp /wx 0x168` returns zero;
   `x /wx 0x168` returns the live Ticks counter.

3. **QEMU mac99 MMIO addresses differ from real hardware.** MacIO BAR0=`0x80000000` in
   QEMU, `0xF3000000` on real G4. Our SheepShaver targets `0xF3xxx`. Use QEMU for
   **behavioral** reference, never for addresses.

4. **The level-1 interrupt handler address is heap-allocated.** Always read `x /1wx 0x64`
   first. Don't hardcode `0x47d0ba`. During boot the vector changes rapidly — read it once,
   reuse that value.

5. **`btst d6,(a4)` at 0x5000ee9a was WRONG.** `0x5000ee9a` is mid-word of a 4-byte
   `tst.l $d94.w` at `0x5000ee98`. Verify all runtime observations against the static ROM
   disassembly before building on them.

6. **Probe at the right boot stage.** QEMU 50s probes show Finder-steady-state. Our
   SheepShaver first-EXT-interrupt is equivalent to QEMU's 3–5s window. Compare like with
   like. The rig's `--ladder` option probes at 3s, 5s, 10s, 30s.

---

## 2026-06-05 — Rule out boring causes first

When a harness "doesn't work": rule out the cheap causes FIRST — wrong coordinates,
clicking empty desktop, a noisy signal (idle hook emits spurious one-off `frontApp='Finder'`
frames), a stale binary — with one clean functional test BEFORE theorizing about internals.
Three "deep bug" theories (RawMouse Toolbox bug, direct-ADB rewrite, hold-the-button) all
dissolved under one proper A/B. See also: VNC clicks were NEVER broken (LEARNINGS archive,
2026-06-05).

---

## 2026-06-02 (session 5 RETRACTION) — "STUCK"/HOT-PC detections are sampling artifacts

A "spin-wait deadlock" theory at 0x50313d34 was built from register-state inference without
disassembling the actual instructions. When the ROM was finally dumped and run through
capstone, every pillar collapsed:

1. **0x50313d34 is NOT a spin-wait.** It is the NK exception/interrupt dispatcher — a
   comparison chain that dispatches from a table. It is the hottest PC in the system.
2. **"STUCK at pc=50313d34" is a sampling artifact.** Two consecutive 5-second heartbeats
   on the same PC does NOT mean the guest is hung. The system was never stuck.
3. A guest-memory-corrupting "fix" (`a46cda99`) was committed on this theory and had to
   be reverted (`9f9e617e`).

**Methodology rule:** never infer code behavior from register state alone. Disassemble the
actual instructions first (single lldb attach + memory dump + capstone, then detach
immediately).

---

## 2026-06-02 (session 2) — VBL timer death from repeated lldb attach/detach

Multiple `lldb -p PID … -o detach` invocations in sequence cause the macOS kernel to
defer the 60 Hz VBL timer. The ROM's early-boot spin-wait at 0x5031040c loops forever
without ticks. **Fix:** attach lldb AT MOST ONCE per run, do minimal investigation,
detach immediately. If the emulator gets stuck in a 2-block loop with all-zero a0-a7
registers in the trace ring → the VBL timer died → restart.

---

---

## 2026-06-12 (session 4) — NK EXT handler PR-bit gate; CGRP is function pointers, not counters

**NK EXT delivery requires user-mode (PR=1) at interrupt time.**
The NK EXT handler at 0x50314880 extracts `r11.bit16` (= PR, the PPC user-mode flag)
immediately after saving context. If PR=0 (kernel mode) → jumps to fallback at 0x50314660
and returns without touching CGRP. The DR emulator runs in kernel mode by default
(`SS_M6A_USER_MSR=0`), so **every** external interrupt fires in kernel mode → CGRP delivery
path is never reached → 68k interrupt handler at 0x5000ed08 never fires, regardless of CGRP
state.

**CGRP+0x20 is a function pointer, not a "registered group count."**
The `cmpwi r9, 2; blt bail` guard at 0x50314894 is checking whether the function pointer
is a valid address (≥2 = non-null). NK cold-start writes `NK_base + 0x3da0 = 0x503143a0`
there via init code at 0x503115f8. Value 1 in our boot means that init code never ran for
this CGRP instance. Do not read this field as a count.

**The ROM patch stall (dec_expiries=5) was a DATA corruption, not code-path skipping.**
The 8 bytes at 0x5000ed08 are scanned as PPC data by the NK during boot. Replacing them
with OP_IRQ_NW+rte corrupted the pattern the NK was looking for, breaking NK scheduler
initialization before DEC ever fires real tasks. No 68k execution is involved.

**SS_PROBE_68K can arm but never fire for structural reasons (not a probe bug).**
If the target address is unreachable due to a mode check (e.g., the PR-bit gate above),
the probe will never match even in long runs. Rule out structural delivery blockers before
concluding the probe is wrong.

---

*For the full session journal (pre-archive), see `docs/archive/2026-06/LEARNINGS-2026-06.md`.*
