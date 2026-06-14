# LEARNINGS — macOS ARM64 JIT work

Durable lessons that prevent repeating expensive mistakes. Newest entries at the top.
For the full historical session journal: `docs/archive/2026-06/LEARNINGS-2026-06.md`.

---

## 2026-06-15 — A standing independent ADVERSARY reviewer catches both directions of error (SS_M18 S1 night)

During the autonomous SS_M18 S1 build, a third **Adversary** reviewer (mandate: distrust banked
findings, re-derive from primary parcel bytes, try to FALSIFY) ran on every deliverable alongside the
spec + technical reviewers. In one night it produced four catches the other two reviewers (often
UPHOLD/CLAIMS-HOLD) missed — and crucially in **both** error directions:

- **False-NEGATIVE averted (the classic):** the `paged_mmu.cpp` translation core's only test was a
  *same-author* spec-derived oracle. Both impl and oracle shared one mental-model bug — BAT scanned
  *before* the `MSR[DR]=0` real-mode check, so a DBAT match translated in real mode instead of
  returning EA=PA identity (PEM §7.4.1: real mode bypasses BATs too). The self-referential test passed
  green. The independent adversary, checking against the *architecture* not the test, found it. **A
  same-author differential test is blind exactly where author and oracle agree — an external oracle is
  OWED, not optional, before such code is wired.**
- **False-POSITIVE averted (the mirror, rarer):** a *prior* adversary had re-banded S1 partly on "the
  NK programs high BATs SPR 560–575 the oracle can't model." The next adversary re-derived and showed
  those writes are **feature-gated dead code** (gate bit `0x20` set for no presentable PVR) — chasing
  a ghost. Over-weighting an unverified premise as decisive is the *mirror* of the M8→M17
  false-negative; the adversary guards against it too.
- **Over-claim trimmed:** a "WINDOW-SAFE-BY-ANALYSIS" recon had *asserted* DBAT coverage of RAM/ROM
  while only *showing* IBAT loads (IBAT/DBAT use independent descriptors; the JIT does *data* access,
  so DBAT is the load-bearing one). Verdict corrected to SAFE-BY-MECHANISM / owed-to-G1.e.

**Rule earned:** for RE-heavy work, run the independent Adversary on *every* load-bearing deliverable —
especially "good news that unlocks risky work" and any claim validated only by a same-author oracle.
It is cheap relative to a five-milestone misdirection, and it catches the errors that consensus
reviewers rationalize past. (See `[[feedback_autonomous_overnight]]` for the standing policy.)

---

## 2026-06-14 — A load-bearing "never" from an EXACT-MATCH probe is the highest-risk claim in the project

**This is the SECOND five-milestone misdirection caused by a measurement artifact** (cf. the
session-5 HOT-PC "deadlock" retraction). M13's close-out due-diligence overturned the keystone of
M9→M13: "the 68k handler `0x5000ED08` never runs / interrupts are never delivered." It was never
true. `SS_PROBE_68K=0x5000ed08` is exact-match/edge-triggered, but the DR's first `lhau` advances
r24 `ed08→ed0a` *before* the dispatch-hook samples — so the probe was structurally blind to handler
entry. Re-targeting to `ed0a` matched **8/8 in plain baseline** (HLE OFF) with a genuine DR-built
`$64` level-1 autovector frame (saved `SR=0x2000` = IPL 0, legitimate delivery). Five milestones of
delivery work — Task A injection, M10 CGRP forging, the CGRP-registration thesis, Task C
`SS_NW_DR_AUTOVEC` — were aimed at a non-problem. Full evidence: `docs/planning/M13-FINDINGS-interrupt-delivery.md` §C-pin.7/8.

**The aggravating fact: the caveat was ALREADY DOCUMENTED and still missed.** `docs/AGENT-CONTEXT.md`
literally says for `SS_PROBE_68K`: *"r24 word+2: to catch word X probe X+2."* The `0x5000ED08` probe
never applied it. A documented instrument caveat is worthless if it is not applied to the
load-bearing claim that rests on the instrument.

**THE RULE (binding, extended 2026-06-14 after the re-retraction):**

1. A load-bearing **NEGATIVE** result — "X never runs / never fires" — that rests on an
   **exact-match / point probe** MUST be cross-checked before anything is built on it:
   (a) apply the word+2 rule, (b) ring-confirm, or (c) frame/state read at entry.

2. A load-bearing **POSITIVE** result — "X runs / delivery works" — from a **capped or
   aggregate probe** MUST attribute which source produced the entries. "The handler runs
   8/8" from a saturated count tells you *something* runs, not *which source*. You must
   uncap the probe AND correlate to the source under test (count delta, temporal
   correlation, or source-specific state at entry). The M13 retraction rode on ed0a "8/8"
   without asking "are these DEC ticks or Cuda EXT entries?" — the answer (all DEC, zero
   Cuda) inverted the conclusion.

3. A load-bearing **ATTRIBUTION** — "these entries come from source X" — must be verified
   against the actual mechanism, not inferred from timing or co-occurrence. The 64/64 ed0a
   entries were attributed to "DEC autovector" because they tracked DEC timing. Full RE of
   the DEC handler (M14 §7) proved the NK DEC handler does NOT signal the DR — it restores
   CR fully and returns. The ed0a entries come from `HandleInterrupt` `MODE_EMUL_OP`
   `Execute68k` (a completely different mechanism). Attribution by timing correlation ≠
   attribution by mechanism trace.

All three rules instantiate the same principle: **a probe result is load-bearing only for
what it discriminates.** Exact-match discriminates address but is blind to word+2.
Capped/aggregate discriminates "runs at all" but is blind to source. Timing correlation
discriminates "happens around the same time" but is blind to causation. The meta-trap:
positives feel trustworthy ("it works!") so nobody applies cap-scrutiny or mechanism-scrutiny
to them.

---

## 2026-06-13 — M13 Task A: the DR is a recompiler — you cannot hand-inject 68k interrupts

> ⚠️ **Superseded framing (2026-06-14):** this entry's premise — that delivery to `0x5000ED08` was
> the blocker worth solving — was the retracted artifact (see the 2026-06-14 entry above). The
> RE facts below about the DR being a recompiler remain accurate; the *motivation* (injecting
> interrupts) was chasing a non-problem. **Further correction (2026-06-14 session 2):** "native
> delivery already works" was also wrong — DEC does NOT signal the DR at all (see rule 3 above
> and M14-FINDINGS §7). The ed0a entries come from HandleInterrupt MODE_EMUL_OP Execute68k.

Three-approach bake-off (parallel worktrees) on "deliver a 68k interrupt to the ROM handler at
0x5000ED08 without the intermittent 0xDEADBEEF SIGTRAP." Two approaches falsified by RE, one
(NK CGRP) confirmed as the only sound path. Full write-up:
`docs/planning/M13-FINDINGS-interrupt-delivery.md` (process trail archived under
`docs/archive/2026-06/planning/`); code warnings at the STUB sites in
`rom_patches.cpp` (~DR_WARM const) and `sheepshaver_glue.cpp` (~STUB restore).

**The load-bearing fact: the 68k "DR" emulator is a RECOMPILER, not an interpreter loop.**
- DR_WARM (0x5046e9d8) is a *one-shot cold warm-entry trampoline*, NOT the per-instruction
  dispatch loop. Probe: 0 visits across 25949 dec-expiries of live DR execution (chaining on
  AND off). Steady-state 68k runs in a *dynamic code cache* at 0x17fa0000–0x17ffffff. There is
  **no fixed guest address** for the between-instruction boundary → you cannot patch/poll a fixed
  PC to get a per-instruction interrupt check (Approach B, falsified).
- Two dispatch tables: re-entering DR_WARM via our CGRP STUB runs with r29 = the COLD ROM table
  (~0x504920f8); the live warm DR uses r29 = a RAM table (0x17ffeb20) inside the cache. The cold
  table dispatches into uncompiled dead-fill (0xDEADBEEF → `stfdu` @ ~0x100259dc) → the SIGTRAP.
  This is the real root cause of the M10/M12 crash. (An earlier reading mistook the warm RAM
  r29 for "the" dispatch base — it is only the *steady-state* value, not what the STUB path runs.)
- DR dispatch-table slots 2/3/5 (0x5046fb00/fc00/fd00, byte-identical) are *volatile-only* trap
  prologues: they save r7–r13 + A7 + CR/LR into ctx 0x68ffe000, NOT the non-volatile 68k file
  (r14–r31). They presuppose a coherent register file rather than establish one → vectoring there
  relocates the crash, not fixes it (Approach C, falsified). The recon shorthand "register-save
  prologues snapshot the regfile" was an overstatement — they snapshot volatiles only.

**Methodology wins this session:**
- Re-baselining refuted a prior session's report (claimed dec=1 stall "predates our changes" and
  "STUB never fires" — both were a self-inflicted regression from a divergent STUB-in-ROM rewrite,
  parked in `git stash`). Always revert to HEAD and measure before theorizing on someone else's
  half-finished state.
- `dec_expiries=5` is NOT a wrong timer rate: 0x503230dc is the NK DEC re-arm that clamps DEC far
  when no near-term events are queued → it is a symptom of stalled boot progress, coupled to
  delivery. So even a correct injection won't reach the dec≥20 gate unless it advances the boot.
- The "chaining hides the boundary" wall applies to *host-side* interrupt-poll hooks, not to
  guest-code patches (chaining chains translations of the patched bytes) — but it is moot here
  because there is no fixed guest loop to patch anyway.

**Net:** correct 68k interrupt delivery must go through the NanoKernel's own CGRP/EXT path (which
resumes the recompiled world at a safe point); hand-rolled injection at any fixed PC is a dead end.

**4th confirmation (resume-prologue implement+test, C++-instrumented at the delivery point) — the
decisive pin:** at the host-side EXT-delivery moment the CPU is executing **PPC (NK/DR) code, not
68k code**, so there is NO interrupted-68k context to resume. `ECB+0x740` holds the DR's own
*PPC-resume* state (save+0x3c = a PPC addr like 0x50510030, not a 68k PC; the A7 slot 0x68fff50c =
DR arena 0x17ffebxx, not a 0x103fxxxx 68k stack); live r24 varies wildly (0 / random 68k PC). The
"populate-first" branch is also dead — no coherent 68k context exists anywhere at that instant.
**Inversion of the dec coupling:** the boot where we did NOT inject free-ran to dec_expiries=9117;
every boot where we DID inject parked at 5 or SIGSEGV'd. Our injection is actively *harmful* — it
disrupts an otherwise-healthy NK scheduler. So the frame "hand-inject a 68k interrupt at the host
poll" is falsified four independent ways here (bake-off A/B/C + this; a 5th follows in Diagnostic #2,
for five total). The 68k world receives interrupts
only when the **NK schedules it and propagates EXT→68k itself**; the open question is no longer "how
do we inject" but "why doesn't the NK propagate to 68k naturally / what must the 68k world have set
up first (registered handlers, VIA IFR state, System interrupt handlers) before it can" — which loops
back to needing the boot to progress on its own. Re-scope M13 away from injection accordingly.

**Diagnostic #1 (injection-OFF baseline, 2026-06-13) — premise confirmed + a metric corrected:**
With `SS_M10_CGRP=0` (`SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1 SS_M11_FB=1`): scheduler healthy
(dec_expiries=67393), but the 68k handler `0x5000ed08` **NEVER fires** (PROBE68K armed, 0 matches;
jit-analyze "No interrupt-delivered records found"). The 68k world wedges in a polling/wait loop —
HOT-PC at the DR interpreter `0x50468ae4` with r24 cycling scattered ROM 68k PCs (0x5000010a /
0x50038a2c / 0x5006621c …), comp frozen — i.e. it spins waiting for VBL/Time-Manager ticks that
never arrive → pre-System dead-end at ~15s ([ALARM] model-rejection/hang, WindowManager never up).
**CORRECTION: `irq_fired` is a MISLEADING metric** — it counts EXT *consumed at the NK*
(g_exc_consume_stats.fired, deferred-edge at PPC pc=0x50325fd0/0x50318014), NOT delivery to the 68k
handler. So "irq_fired=137" does NOT mean interrupts reached the 68k world; `0x5000ed08` running
(PROBE68K match) is the real delivery signal. This confirms the blocker is specifically **delivery
to the 68k handler 0x5000ed08**, which only ever fires under CGRP (on→handler runs but DR-re-entry
crashes; off→never runs). The forward path is NK-native EXT→68k propagation (why does the NK consume
the EXT but never route it to 0x5000ed08 — registered-handler-table / W2L-1 / CGRP routing), NOT
hand-injection.

**Diagnostic #2 (3-thread RE fan-out, 2026-06-13) — COMPLETE verified diagnosis. Delivery is a
3-stage chain; we satisfy stage 1, stages 2-3 never happen** (full re-scope:
`docs/planning/M13-FINDINGS-interrupt-delivery.md`, process trail archived):
1. **NK EXT (PPC) WORKS** — NK EXT body `0x50314880` is a PPC save→`rfi` return that by design never
   vectors to 68k; with no CGRP handler registered (CGRP+0x20=1<2, table empty) it routes every EXT to
   fallback `0x50325f00` which only updates the NK pending-bitmask. `irq_fired` counts these. [DISASM+PROBE✓]
2. **NK→DR IPL handoff MISSING** — nothing translates the NK bitmask entry into a pending 68k IPL the
   DR consults. This is the broken link.
3. **DR autovector (68k) never fires** — the DR, at a between-instruction boundary, would see a pending
   68k IPL above the SR mask and itself build the genuine 68k frame (vector `$64`) and vector through
   `[0x64]`. QEMU confirms this is THE working contract (9.2.1 oracle). [QEMU✓]
- **The 68k side is fully READY** (not the blocker): autovectors live `[0x64]=0x5000ed08 [0x68]=…ed10
  [0x6c]=…ed18`; NewWorld is **paravirtual** (software interrupt-source struct at `*(0x68ffefd0)`, NOT
  VIA IFR/IER hardware — corrects the M9 framing). The dispatcher's deferred/Time-Mgr/VBL pass at
  `0x5000ee58` runs **unconditionally after service**, so even a pending-less autovector entry ticks
  the starved queues and should advance the boot.
- **Re-scoped target:** drive the DR's OWN between-instruction autovector (set a pending 68k IPL level
  1, SR permitting) so the DR builds the frame and vectors to 0x5000ED08 — NOT inject a frame (dead 4×)
  and NOT route the NK EXT/CGRP path (M10 forge crashes the DR). Next RE: locate the DR's IPL latch +
  SR-mask check (DR resume/dispatch family 0x5046e1a4/0x5046cdd8) and how to set it host/PIC-side.

---

## 2026-06-13 — M12 session 7: Wave1 fix + A-trap bootstrapping wall (CGRP frontier)

**Wave1: 24-bit DR alias mapping (committed `348544cd`)**

The DR emulator runs in 32-bit mode but the early 68k ROM code at 0x5000ED00–0xEF00 uses
16-bit signed offsets into ROM with an implicit 24-bit address mask. The DR sign-extends
0xEFD0 → 0xFFFFEFD0 (valid in 32-bit mode) rather than masking to 0x00FFEFD0 (the 24-bit
result). Probe on guest 0x00FFEFD0 showed the value is zero at boot. Fix: map
0xFF000000–0xFFFFFFFF as anonymous zero (vm_mac_acquire_fixed) so both sign-extended and
24-bit-masked accesses get zero — no behavioral difference, no crash.

Result: eliminates SIGSEGV at ea=0xFFFFEFD0; 2/3 boots now run 30+ seconds
(dec_expiries=2000+). 1/3 boots still crash at ea=0x64A05014 (non-deterministic timing, same
DR 24-bit region, not blocking M12 gate).

**A-trap bootstrapping wall (M12 Task C FAIL — M13 input)**

Enabling CGRP interrupt delivery (SS_M10_CGRP=1) routes EXT interrupts to the ROM handler
at 0x5000ED08. That handler's preamble at ROM+0xED06 immediately executes A-trap 0xA9A8
(`_GetMasterPointerCount` or equivalent). The Mac OS Trap Dispatch Table lives at low mem
0x0E00–0x0FFF and is populated by the System file during startup — which hasn't loaded yet
at this point in the boot. The A-line exception vector (0x50429C10 set by the trampoline)
is a `bra.s *` stop stub, so ANY A-trap at this stage parks/exits the emulator.

**Rule:** Don't enable CGRP until the Mac OS Trap Dispatch Table is initialized (or the ROM
interrupt handler is bypassed). The M13 task is to either: (a) populate minimal trap entries
for the A-traps at 0x5000ED00–0xEF00 (0xA9A8, 0xA9A3, 0xA9A4, 0xA02E, 0xA198, 0xA05D),
(b) delay CGRP delivery until after System init, or (c) find the QuickDraw init path that
doesn't require the interrupt handler.

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
