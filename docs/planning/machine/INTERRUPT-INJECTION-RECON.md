# Interrupt-injection recon — P-M5 crash + item 1b (2026-06-12)

> Task-0 recon for the likely next milestone: **NewWorld host→68k interrupt injection**.
> Inputs: TRAP-TABLE-RECON.md "FIX RECORD — instime-fix" (P-M5), EE-CHAIN-RECON.md D-6/D-7
> (item 1b). READ-ONLY: no source edits; env-knob instrumentation only. **Boots: 4 of ≤4**
> (slot protocol, rundirs under /tmp/ss-slots/slot0/runs/: `pm5-recon-crash`
> 20260612-011021.89050, `pm5-recon-seed` 20260612-011633.89566, `pm5-recon-romdump`
> 20260612-011919.89848, `pm5-recon-runmode` 20260612-012123.90016). Provenance:
> `tools/dump-manifest.sh --check` PASSED before any dump read; mirror slot reads were
> taken from rom901.bin (staged convention guest−0x50100000) and re-verified against a
> FRESH post-patch dump (`SS_DUMP_ROM`, boot 3) because rom901.bin predates the
> f808a7fb .Sony-abort lift. Claim: `docs/superpowers/.claims/pm5-recon.claim`.

## TL;DR — the answer table

| Q | Answer (one line) | Evidence |
|---|---|---|
| Q1 | **P-M5 ROOT CAUSE PINNED (and it is NOT the TM expiry):** boot-sequencer step at ROM ~0x2fa (planted `M68K_EMUL_OP_NAME_REGISTRY`=0xfe69, the patch site right after Enable60HzInts' 0x2c0) → mirror slot 0x504ff348 (`sheep 0x18000029; b 0x50466084` — the crash lr IS that slot, the coordinator's decode confirmed) → `EmulOp(OP_NAME_REGISTRY)` → `PatchNameRegistry()/InitCallUniversalProc()` → `FindLibSymbol()` (macos_util.cpp:229, run-mode==MODE_EMUL_OP branch) → `Execute68k(proc1)` where proc1 = FIRST `SheepMem::ReserveProc` = **0x50510000** (SheepMem base = ROM_BASE 0x50000000 + ROM_AREA_SIZE 0x500000 + SIG_STACK_SIZE 0x10000, main_unix.cpp:198/3395) → `sheepshaver_cpu::execute_68k` (sheepshaver_glue.cpp:1228): `gpr(24)=proc1+2`→**r24=0x50510002 exactly** (not a desync — the `gpr(24)+=2` fetch idiom), `gpr(29)=[KDP+0x1074] + opcode*8` with **[KDP+0x1074]=[KDP+0x1078]=NULL on the trampoline boot** ([PROBE✓] `[0x68fff074]=0 [0x68fff078]=0`) and opcode=0x558f (`subq.l #2,a7`, proc1 word 0) → `execute(0x2ac78)` (**crash r29=0x0002ac78=0x558f×8 exactly**) → wild PPC execution in zeroed low RAM → fetch fault at 0x100000. **Discriminator (boot 2): seeding the two words (`SS_SEED_MEM=0x68fff074=0x50480000;0x68fff078=0x50460000`) ELIMINATES the SIGSEGV** — boot runs the full 30s window, sc deliveries 173→248+, VIA ORB traffic, new frontier reached. | [PROBE✓ boot 1 crash regs: r24/r29/r27=0x2f08=proc1 word 1/r16-r19=0x5058xxxx = FindLibSymbol's four SheepVar args/r31=0x68fff000=KDP+0x1000]; [STATIC] glue 1228-1296, macos_util 229-307, thunks.h 162-181; [SEED✓ boot 2]; [PATCH-fresh] slot bytes + name_reg word 0xfe69 @ROM 0x2fa |
| Q2 | **Paravirtual donor chain** (file:line): PrimeTime stub → host timer armed (timer.cpp:432 PrimeTime → wakeup_time) → expiry in timer thread (timer.cpp:548/572/596 per PRECISE_TIMING flavor) → `SetInterruptFlag(INTFLAG_TIMER)` (main_unix.cpp:2792) + `TriggerInterrupt()` (glue:3042 → `ppc_cpu->trigger_interrupt()` ppc-cpu.hpp:539, SPCFLAG) → CPU thread `check_spcflags` (ppc-cpu.cpp:1832-1867) → `HandleInterrupt` (glue:3054) → **MODE_68K arm: `WriteMacInt16([KDP+0x67c],1)` + CR\|=[KDP+0x674] + Ticks++** (glue:3076-3090) → 68k emulator takes level-1 → ROM level-1 handler (via_int head `moveq #2,d0`, rom_patches:3793; via_int3 jmp for NEWWORLD CHRP, :3821) → 60Hz handler head = `M68K_EMUL_OP_IRQ` (via_int2, rom_patches:3808) → **OP_IRQ** (emul_op.cpp:811): consumes InterruptFlags, `INTFLAG_TIMER → TimerInterrupt()` (timer.cpp:607) → walks tmDescList, `Execute68k(tmAddr)` per expired task (timer.cpp:631) → the 60Hz task proc **0x5000bbb8** re-primes itself via `jmp ([$568])` (PrimeTime, −16626µs) and falls into the SAME fe6b OP_IRQ + `addq.l #1,$16a` (Ticks++) — the self-sustaining 60Hz cycle. Execute68k works there because the kernel-data emulator words [KDP+0x1074]/[KDP+0x1078] are live (NK/emulator init populated them). | [STATIC] all cited lines; [PATCH-fresh] 60Hz handler bytes at file 0xbbb8: `2049 203c ffffbf0e 2278 0568 4e91 6008 fe6b 4a80 6700ffe8 52b8 016a` |
| Q3 | **Newworld gap inventory** (link-by-link): (1) host timer thread/INTFLAG/TriggerInterrupt — EXISTS, profile-independent. (2) check_spcflags → exc-core hook first (DEC/EXT only; INTFLAG is NOT an exc-core source) → falls through to legacy HandleInterrupt. (3) **MODE_68K fake-poke ([KDP+0x67c]=1 + CR mask) FENCED on newworld** (M3a F3/M2 — corrupts live NK state; glue 3074-3083); only Ticks++(0x16a) survives → InterruptFlags is NEVER consumed on newworld; the one TM expiry's flag sits forever. (4) **via_int / via_int2 / via_int3 ARE ALL APPLIED on 9.0.1** ([PATCH-fresh] bytes: via_int @0xef2c, via_int2 fe6b @0xbbc8 — *inside the 60Hz task proc 0x5000bbb8 itself*, via_int3 jmp @0x16dd6; no [ROMPATCH] SKIP lines) — **falsifies the fix-record claim "via_int2 pattern absent on 9.0.1"** (that claim described the pre-f808a7fb world where patch_68k aborted at .Sony before reaching them). So OP_IRQ is planted and reachable the moment a level-1 interrupt reaches the 68k world. (5) **Execute68k unusable until Q1's two words are staged** — this also poisons the OTHER injection leg: HandleInterrupt's MODE_EMUL_OP arm (glue:3126, NOT newworld-fenced) calls Execute68k(proc) and would crash the same way if an interrupt lands during an EMUL_OP. (6) Where host posts SHOULD enter (architecture): **the exception path** — MACHINE-LAYER-PLAN classifies Execute68k/EMUL_OP injection as paravirtual-fiction machinery (plan line ~165, ~512) and M3a/W2-3 wired the real surfaces: DEC → 0x50313200, EXT → 0x50314880 (NK-published [KDP+0x374], wired, delivered in harness, never live-fired). Host device posts should assert PIC inputs (EXC_EXTERNAL) / ride the DEC tick — the NK + 68k world then dispatch level-1 natively into exactly the patched via_int chain of Q2, and OP_IRQ consumes InterruptFlags as on paravirtual. The 0x67c/CR fake-poke stays fenced; host-thread Execute68k *injection* (MODE_EMUL_OP arm) is paravirtual-only legacy. Execute68k as a *synchronous tool* (FindLibSymbol, TimerInterrupt task calls, Execute68kTrap) still needs Q1's fix regardless of delivery architecture. | [STATIC]+[PATCH-fresh]+[PROBE✓] as cited |
| Q4 | **[0x2810] ownership PINNED — and the 1b framing is CORRECTED**: on the live riser-on boot the run-mode word has exactly two writers, both NK: **set 1 @0x503143d4** (the switch-to-context HIT path, M6A-ONGOING-ENTRY-DESIGN §"hit → [0x2810]=1") and **clear 0 @0x50312b0c** (NK SAVE = switch-back entry, R-14's "the NK clears it on switch-back entry") — **151 sets / 151 clears, perfectly balanced, FINAL STATE 0** ([PROBE✓ boot 4 watchpoint, SS_JIT_WATCH_ADDR=2810]). D-7's mechanism claim ("the trampoline enters the cold 68k world without an FE02 backward switch, so the word still holds its native-window value") is **falsified**: the word is NOT stale-1; the cold 68k world runs with [0x2810]=0. The 97 DEFER_NATIVE counts are polls that landed *inside* the 151 transient native windows (DEFER_NATIVE requires run_mode≠0 at the provisional-DELIVER recheck — glue:1015-1041); between windows ([0x2810]=0) **no poll ever fires** (the HANDLE spcflag was consumed by the deferral; EE in the 68k world is 0 so no EE-edge re-raise) → the latched DEC starves on a **missing wake-up edge, not a wrong fence and not a stale word**. Therefore: do NOT clear [0x2810] at cold-68k-entry (nothing to clear; would mask real windows) and do NOT teach the fence a "cold world" case (the fence's decision was correct every time it fired). **Recommend option (c): re-arm the poll on deferral** — on EXC_DECIDE_DEFER_NATIVE leave/re-set SPCFLAG_CPU_HANDLE_INTERRUPT (or re-trigger_interrupt) so check_spcflags re-asks at subsequent block boundaries until the window exits; self-terminating (delivers or hits a real defer-EE), bounded by window length (~10²-10³ records), tripwire-countable. | [PROBE✓ boot 4]: watch trace + terminal exc=0/0/0/1/97/173/4-shape (deferred_native=97, delivered_dec=0) identical to D-7's |
| Q5 | Milestone task list below. | — |

## Q5 — recommended next-milestone shape ("M7: host→68k interrupt entry")

M7-critical (ordered):

1. **Execute68k newworld port (S)** — stage `[KDP+0x1074]:=0x50480000` (mirror
   LA_DispatchTable) and `[KDP+0x1078]:=0x50460000` (mirror LA_EmulatorCode) in the NW
   trampoline staging block (same family as ctx+0x1ec / KDP+0xf2c staging; the values are
   the mirror equivalents of main.cpp's `lp[0xa8>>2]/lp[0xac>>2]` kernel-data pair,
   rom_patches.cpp:1514-1515). **Fix shape seed-proven end-to-end (boot 2)**. Note
   [XLM_EXEC_RETURN_OPCODE]=0x284c is already staged ([PROBE✓] `0xfe410000`). Acceptance:
   default boot survives OP_NAME_REGISTRY + OP_INSTALL_DRIVERS; `SS_PROBE_68K=0x50510002`
   fires and the proc RETURNS (it never fired in boots 1/2 — crash/dispatch never reached
   the hook); parks at the boot-2 frontier, not SIGSEGV. This also de-mines the
   MODE_EMUL_OP injection arm (Q3 item 5).
2. **Deferral wake-up / item 1b disposition (S)** — Q4's option (c): re-arm the
   HANDLE spcflag on DEFER_NATIVE (and DEFER_EE is already edge-covered by the riser).
   Decision recorded here: fence semantics are CORRECT; run-mode clearing is a non-fix.
3. **Host-post delivery route (M/L)** — route recurring host posts through the exception
   core, not injection: model the 60Hz/TM kick as a PIC source (EXC_EXTERNAL via the
   wired-but-never-fired 0x50314880) or piggyback the DEC tick; the guest's own level-1
   dispatch then reaches the (verified-present) via_int/via_int2 chain and **OP_IRQ
   consumes InterruptFlags exactly as on paravirtual** — TimerInterrupt, SonyInterrupt,
   ADBInterrupt etc. all come back for free. Requires (1) (OP_IRQ's TimerInterrupt calls
   Execute68k) and benefits from (2). This is the architecture-consistent answer; keep
   the 0x67c/CR fake-poke fenced permanently on newworld.
4. **Ticks consumption / link 7 (S rider on 3)** — the 60Hz handler's `addq.l #1,$16a`
   is byte-present at file 0xbbcc-region; once OP_IRQ fires, D-6's "nothing posts Ticks"
   verdict should resolve itself — verify, don't build.

Deferrable: XLM_IRQ_NEST ownership (D-6 item 2, unchanged); newworld-fencing the
MODE_EMUL_OP injection arm for architectural consistency (after (1) it is merely
redundant, not dangerous); the boot-2 new-frontier characterization (first task-0 of M7:
terminal signature shows `PROGRAM #5 srr0=50324fec slot=5 r1=0 lr=0` + sc 0xffffffff
climbing — uncharacterized); EMULOP-COUNTS op-1 identity (cosmetic).

## Key evidence detail

### Boot 1 (`pm5-recon-crash`) — the crash anatomy [PROBE✓]

Terminal SIGSEGV registers (boot.log): `pc=00100000 lr=504ff348 ctr=0 r24=50510002
r29=0002ac78 r27=00002f08 r31=68fff000 r16=5058f4a8 r17=5058f5c8 r18=5058f5c4
r19=5058f4c4 r25=00000020`. Probe at the mirror dispatch (0x50466084, visit 1):
`[0x68fff074]=0 [0x68fff078]=0 [0x284c]=0xfe410000 [0x0]=0x00100000 [0x2810]=0`.

Decode: r29 = NULL-table + 0x558f×8; r27 = proc1's second word (`2f08` move.l a0,-(a7))
captured by execute_68k's prefetch; r16-r19 = the four `SheepVar` argument blocks
FindLibSymbol allocates (data region grows down from 0x50590000 = SheepMem base+0x80000);
r25=0x20 = the XLM_68K_R25 image. Every register is accounted for by the
FindLibSymbol→execute_68k frame — the chain is over-determined.

Crash timing vs the TM hypothesis: the OP_NAME_REGISTRY sequencer step runs microseconds
after Enable60HzInts returns (~0.11s); the first host TM expiry would be ~16.6ms later
(~0.127s) — the timing coincidence that misled the fix agent. The register evidence
excludes the TM leg: HandleInterrupt's MODE_EMUL_OP proc would give r29=0x3f3c×8=0x1f9e0,
and TimerInterrupt's task call would give r24=0x5000bbba. Neither matches.

Mechanics residue (non-load-bearing): (a) the exact instruction that left lr=0x504ff348
(the slot's own address; slots are `sheep; b` — no bl) is unpinned — it identifies the
live EMUL_OP slot but the setter wasn't traced; (b) the wild-execution slide from
0x2ac78 to the 0x100000 fetch fault (zero-filled RAM decodes as fall-through until the
first unmapped/non-executable page at 0x100000) — the fix record's "guest[0] reset-SSP
read" framing is decoration: [0]=0x00100000 holds the same value by coincidence (both
are "1 MB"); (c) SS_DR_R24_RING armed but no ring dump materialized in the rundir on
SIGSEGV — instrument gap worth a look next time it's needed.

### Boot 2 (`pm5-recon-seed`) — the discriminator [SEED✓]

`SS_SEED_MEM=0x68fff074=0x50480000;0x68fff078=0x50460000` (immediate form, applied at
NW-trampoline-end, before the sequencer): **no SIGSEGV**, 29s JIT session (blocks=7354,
98.4% coverage), sc deliveries grow past the crash point (0xffffffff ×233 vs ×107),
PROGRAM #5 (a new, later event), VIA ORB reads 3341. The two words are BOTH the fix's
proof and its implementation sketch.

### Boot 3 (`pm5-recon-romdump`) — patch-state byte proof [PATCH-fresh]

Fresh `SS_DUMP_ROM` (post-f808a7fb binary; manifest rom901.bin predates the tail lift so
it could NOT answer this): TM trap-table image entries #0x58/59/5a/93 = 0x2fd240/48/58/68
(instime-fix present ✓); name_reg fe69 @0x2fa; drvr_install fe68 @0x9c4; via_int patched
head @0xef2c; via_int2 fe6b @0xbbc8 **inside the 60Hz task proc 0x5000bbb8**; via_int3
jmp @0x16dd6; raw via_int/via_int2 search patterns no longer present (= patched).

### Boot 4 (`pm5-recon-runmode`) — the [0x2810] watch [PROBE✓]

Riser-on (`SS_NW_EE_RISER=1 SS_NW_DEC_PUBLISHED=1`), `SS_JIT_WATCH_ADDR=2810`:
151× `pc=503143d4 … value=00000001` / 151× `pc=50312b0c … value=00000000`, strictly
alternating, last write a clear. Terminal tuple deferred_native=97, delivered_dec=0,
dec pending=1 — matching D-7's acceptance boot numbers exactly (those deferrals all
accrue in the first ~0.13s; D-7's 60s boot then parked with nothing further happening).

## Falsifications / corrections of record

1. **Fix-record P-M5 hypothesis** ("first host TM expiry → TriggerInterrupt →
   Execute68k(task proc) injection leg") — FALSIFIED as the crash cause (Q1). The
   *underlying worry* (Execute68k unported on newworld) was right; the trigger was the
   guest's own boot sequence, no interrupt involved.
2. **Fix-record claim "via_int2 pattern absent on 9.0.1 → OP_IRQ unreachable"** —
   FALSIFIED by byte proof (boot 3); stale reasoning from the pre-tail-lift world.
   OP_IRQ is planted; what's missing is level-1 delivery + a working Execute68k.
3. **D-7 item-1b mechanism** ("[XLM_RUN_MODE] never cleared in the cold 68k world / no
   FE02 backward switch") — FALSIFIED by the boot-4 watch (balanced NK writers, final 0).
   The starvation is real but the cause is the missing post-deferral wake-up edge.
4. The TRAP-TABLE-RECON frontier prediction ("M3 delivery interplay is the thing to
   watch") — half-right: the wall IS in the host→68k machinery, but on the synchronous
   Execute68k tool path, not the async delivery path.

## Residues (named)

- **R-II1**: lr=0x504ff348 setter instruction (slot-entry convention) — unpinned, cosmetic.
- **R-II2**: SS_DR_R24_RING produced no SIGSEGV ring dump in the rundir — instrument gap.
- **R-II3**: boot-2 new frontier (PROGRAM slot=5 srr0=50324fec r1=0 lr=0; sc 0xffffffff
  storm-ish growth) — uncharacterized; first M7 recon target.
- **R-II4**: [KDP+0x1078] correct staging value assumed 0x50460000 by symmetry with
  rom_patches.cpp:1514-1515 (LA_EmulatorCode); the seed boot proves the PAIR works but
  gpr(30)'s actual consumption inside the mirror emulator was not traced.
- **R-II5**: whether 97 deferrals = polls strictly inside windows was inferred from the
  DEFER_NATIVE decision predicate (requires run_mode≠0), not per-poll correlated.

---

## RESULTS — M7-critical items 1+2 implemented (2026-06-12, label inj-s-fixes)

Both S items landed (`34d3d441` item 1, `3cb3b16e` item 2). Boots: **6 of ≤6** (slot
protocol, all rundirs under /tmp/ss-slots/slot0/runs/: fix1-default 013238.91687,
fix1-probe 013523.91973, fix1-emulop 013720.92236, fix2-riser-unbounded 014035.93239,
fix2-repin 014505.94241, fix2-default-ab 014705.94513). Gates: inner per commit, task
tier on final (test-jit plain 353/353, machine suite 13/13, e2e-test 122 passed).
Claim: `docs/superpowers/.claims/inj-s-fixes.claim`.

### Item 1 — Execute68k newworld port (`34d3d441`)

Staged exactly the Q5-item-1 pair in the trampoline staging block (beside the f31d475e
KDP+0xf28/0xf2c staging): `[KDP+0x1074]:=0x50480000`, `[KDP+0x1078]:=0x50460000`.

**THE NEW DEFAULT-BOOT BASELINE (fresh capture, boot 1, no env):** P-M5 SIGSEGV
**eliminated** (no SIGSEGV, no pc=00100000 — BOOT-VERDICT PASS with `--absent`); 44.4s
full-window JIT session (blocks=7354 / 98.4% coverage); terminal frontier =
**`PROGRAM #5 srr0=50324fec word=0fff0005 slot=5 r1=0 lr=0 -> entry=50314700`**, sc
selector storm `0xffffffff x233` (16 distinct), VIA ORB=3341, mtspr_dec=18,
dec_expiries=1 pending=1, SIGTERM park. This is byte-for-byte the recon's seed-boot
frontier (R-II3) — now reproduced as the unconditional default. R-II3 remains the
first recon target for M7 proper.

Instrument notes (honest misses, neither gating): (a) the acceptance sketch's
`SS_PROBE_68K=0x50510002` did **NOT** fire post-fix (boot 2) — the probe hook sits on
the top-level JIT dispatcher only; Execute68k's nested `execute()` path is probe-blind.
Completion evidence is the frontier itself: the sequencer continues past
OP_NAME_REGISTRY/OP_INSTALL_DRIVERS to the sc-storm park, which sits *after* the old
0.11s crash point. (b) `SS_EMULOP_COUNTS` (boot 3) still shows `1=1` only — the
mirror-slot EmulOp path bypasses `execute_emul_op`'s counter; the recon's "op-1
identity" cosmetic residue is unchanged, now with a mechanism candidate.

### Item 2 — post-DEFER_NATIVE wake-up edge (`3cb3b16e`)

**FALSIFICATION (dated addendum, one-iteration rule applied).** Q4's bounding claim —
"self-terminating, bounded by window length (~10²-10³ records)" — is **FALSIFIED at the
post-item-1 frontier**: the riser-on boot now parks INSIDE a native window that never
exits. The unbounded re-arm (boot 4) spun at ~107M re-polls/s —
`deferred_native=5,295,253,877` == executed blocks over 50s, `delivered_dec=0`, and the
frontier REGRESSED (sc=81 vs 173). Note the recon's Q4 watch predates item 1; the
deeper post-fix frontier runs native where the old one parked at run_mode 0. Boot-4 vs
boot-5 delivery difference is expiry *phase* (whether the latch lands during the
transient-window phase or the parked phase) — both consistent with this finding.
**ONE re-pin:** per-episode re-arm budget `EXC_NATIVE_REARM_CAP=65536` (>> transient
window length), reset on any non-native decision; exhaustion leaves the latch set, logs
once, returns to kick-driven polling. Consumption side: a re-armed HANDLE skips the
legacy HandleInterrupt fall-through in check_spcflags (else it would re-poll
SDL_PumpEvents + the unfenced MODE_EMUL_OP Execute68k arm per block boundary — Q3
item 5). Host-side only; exc_core/ExcDeliveryDecision untouched, test_exc_chain needs
no extension. Tuple note: `deferred_native` now counts every re-poll (cost meter).

**Acceptance (boot 5, riser-on `SS_NW_EE_RISER=1 SS_NW_DEC_PUBLISHED=1`, 60s):**
- **delivered_dec=3 — THE FIRST LIVE PUBLISHED-ROUTE DEC DELIVERIES EVER** (all
  `-> entry=50313200 (2-SPR)`; restarts 504b8008/504d5f58/504a73a8). dec_expiries=3,
  terminal **pending=0** — every expiry delivered, latch fully drained. Delivery #1
  landed inside the budget (a transient window exited); one cap exhaustion; #2/#3
  kick-driven on the post-delivery re-arm cycle.
- **No storm regression**: mtspr_dec=25 (zero-writes 0; D-7 cadence fix intact —
  healthy reload values incl. 0x017d7840/0x65c2-family). sc/program EXACTLY at the
  item-1 baseline (`0xffffffff x233`, PROGRAM #5 srr0=50324fec).
- **Ticks [0x16a] = 0** (probe at the parked hot PC 0x500e1ff4; `[0x2810]=1` there —
  direct confirmation of the parked native window). Frozen Ticks is per Q5 item 4 NOT a
  failure here — consumption arrives with item 3. Moving Ticks would have been headline
  news; it did not move.
- Re-poll cost: ≤65537 slow-path block boundaries per episode (~ms-scale); the
  unbounded variant's 100M/s spin is the documented counterfactual.
- Default-boot A/B (boot 6, riser off): item-1 baseline signature reproduced
  (PROGRAM #5, sc x233, mtspr_dec=18); cap line fires once on the undeliverable
  legacy-route latch — bounded, no spin, full 45s window.

**M7 item-3 entry state**: the EXC_EXTERNAL/PIC routing milestone now starts from a
**delivering DEC chain** (expiry → latch → bounded re-poll/kick → published-route
delivery → NK reprogram → next expiry) and a **working Execute68k** (OP_IRQ's
TimerInterrupt task calls are de-mined). New named residue:
- **R-II6**: the post-item-1 park holds `[0x2810]=1` indefinitely (boot-5 probe) — the
  fence permanently defers any latch that lands in the parked phase. The published
  route retires the KDP save shim (the fence's original corruption rationale), so
  whether DEFER_NATIVE should apply on the 2-SPR route at all is an open design
  question for item 3.

---

## M7 Task 0 — EXT/DEC post-path recon (2026-06-12, label inj-task0)

Plan: `docs/superpowers/plans/2026-06-12-interrupt-injection.md` (rev 2). Boots: **2 of ≤6**
(slot0 runs 20260612-021607.5156 default, 20260612-021835.5366 EXT-forced). Provenance ritual
run (manifest OK pre-recon); fresh post-PatchROM dump taken boot 1 (`/tmp/rom901_fresh_task0.bin`).

### Entry gate (E1–E4) — GREEN

| Row | Verdict | Evidence |
|---|---|---|
| E1 | **VERIFIED** | [PROBE✓] boot 1: `[0x68fff074]=0x50480000 [0x68fff078]=0x50460000` |
| E2 | **VERIFIED** | [PROBE✓] boot 1 reproduces the actuals: `PROGRAM delivered #5 srr0=50324fec word=0fff0005 slot=5 r1=0 lr=0 -> entry=50314700`, sc census 16 distinct `0xffffffff x233`/`0xfffffffe x17`, blocks=7354, no SIGSEGV, full 49 s session |
| E3 | **VERIFIED** (landed record `bf571e26`: delivered_dec=3, pending=0, mtspr_dec=25) + consistency: boot 2 riser-on shows the published DEC route delivering at scale (delivered_dec=3395) | record + [PROBE✓] |
| E4 | **VERIFIED** — published in the RESULTS section above; boot 1 `--expect` matched its named lines | [PROBE✓] |

### Blocking-answer table

| Q | Answer | Tag |
|---|---|---|
| **Q-I1** | **ROUTE = EXT** (decision-rule branch 1 satisfied). Instruction-level path: EXT body 0x314880 → shared prologue 0x313d40 → EE guard → `[[KDP-0x338]+0x20]<2` → `mtlr [KDP+0x5b0]`(=0x50325f00)`; blr` → **fallback body 0x325f00** (lock 0x312700; r20–r31 save via 0x3238d4; `[KDP+0xe80]++`; segment-switch; **PIC IACK `lwbrx` @+0x200a0**; vector queue `[KDP+0x910/0x912]` + pending bitmap `[r20+0xf28]`; **level := `lbz [0x3f00+vector]`** (lowmem vector→level table); EOI `stwx` @+0x200b0 on special/OOB legs) → ALL exits converge 0x3260fc → **`b 0x3254e0` = the 68k-post body**: `r23:=[KDP+0x67c]`; r28<0 skip / r28>0 `ori 0x8000`+`r31:=[KDP+0x674]`; gate `rlwinm. r7,bit 0x00200000`: SET → `sth r28,0(r23)` + CR `or r13,r31` / `and r13,[KDP+0x678]`; CLEAR → **staged post**: `[KDP-0x440] \|= mask`, `[KDP-0x43c] := r28` (sentinel 0xffff=empty; drained into `[[KDP+0x67c]]` at world-restore 0x324720) → unlock → restore 0x323944 → `b 0x312cb0`. **LIVE: the first EXT deliveries ever fired** (boot 2, `SS_NW_PIC=1 SS_NW_PIC_FORCE=1 SS_SCC_RX_INJECT=12:0D`, riser+published on): `EXT delivered #1: restart=50463028 srr1=0000f072 -> entry=50314880`; post body REACHED with `r7=0x00a80000` (bit 0x00200000 SET — from-emulator). **DEC-piggyback REJECTED by evidence**: the DEC body/exit tree never invokes the post family (timer service `bl 0x322eac` → `b 0x312cb0`; the only other `[KDP+0x67c]` readers are trace-log stubs 0x32345c and the restore-drain 0x324724). | [STATIC]+[PROBE✓] |
| **Q-I1 — THE LEVEL-SOURCE GAP (new, blocks Task B's post-value gate)** | At this frontier the post executes with **r28=0**: the guest's PIC virtual mapping (`[[KDP-0x20]+0xf18]`) is uninitialized so the IACK never reaches the PIC model (pic stats `i:0` with 68M deliveries), and the lowmem vector→level table 0x3f00 is zero (`[0x3f24]=0`). Observed post write = 0 (`[0x68fff070]=0`). A nonzero level via the fallback requires guest state Mac OS's native interrupt init owns. Options for the coordinator: (a) stage `[0x3f00+vector]` + PIC mapping — donor-less, same class as the rejected fake guest PIC init; (b) re-scope Task B's "post observed" gate to writer-PC + write-event (value 0 accepted at this frontier, value `level\|0x8000` deferred to post-guest-init); (c) none better — DEC-piggyback does not reach the post at all. **Not stop-rule NEITHER** (the post is reached without unstaged surfaces; the gap is the level INPUT). | [PROBE✓]+[STATIC] |
| **Q-I1 — A1 livelock proven in vivo** | The level-held source with no retirement path: **68,657,433 EXT deliveries in 40 s**, runaway tripwire fired, comp frozen, guest progress zero, re-delivery storm parked on restart=0x50318018 (the riser restore tail — the EE-rise re-delivery loop exactly as A1 derived). DEC was NOT starved (delivered_dec=3395 interleaved — DEC-before-EXT order + U13 held). The once-per-assert-edge latch is the only viable host-source shape; this is its measured counterfactual. | [PROBE✓] |
| **Q-I2** | `[KDP+0x67c]` = **0x68fff070** (ECB+0x70) — W2L-R1 RETIRED. Task B watch address: `SS_JIT_WATCH_ADDR=68fff070`. `[KDP+0x674]`=0x00e00000 (CR OR-mask), `[KDP+0x678]`=0xff9fffff (CR AND-mask). Expected written value class `level\|0x8000` (e.g. 0x8001) — at the current frontier the observed write is 0 (the level gap above). NK init writer: 0x310800 block, `[KDP+0x67c] := r8 + cfg[0x7c]` (ECB-relative — consistent with the probe). Deferred-post staging slot (new): `[KDP-0x43c]` level halfword + `[KDP-0x440]` mask accumulator, writers 0x311d30 (init) / 0x325674 (not-from-emulator post leg), drain 0x324720. | [PROBE✓]+[STATIC] |
| **Q-I3** | 68k chain [PATCH-fresh, from the boot-1 fresh dump]: CHRP level-1 @file **0xec50** = patched `jmp 0x5000ef20` (**CORRECTION: the recon's "via_int3 jmp @0x16dd6" was wrong** — that site holds `4ef9 41234567`, unrelated; 0xec50 is the first via_int3_dat hit, second raw hit 0xed08 stays unpatched) → 0xef20 `movem.l d0-d3/a0-a3,-(sp)` → `pea` ret → `movea.l $1d4.w,a1` → **patched via_int head @0xef2c** `moveq #2,d0` + 4×nop → dispatch `movea.w 0xef40(pc,d0),a0` → table[2]=lowmem **$192** (Lvl1DT slot) → `movea.l (a0),a0; jmp (a0)` → 60 Hz handler = task proc 0xbbb8 → **fe6b OP_IRQ @0xbbc8** → `tst.l d0; beq -0x18` → **`addq.l #1,$16a` (Ticks++)**. Pre-WLSC OP_IRQ returns d0=1 ⇒ **the Ticks++ tail is pre-warm-start-reachable** once a level-1 dispatch occurs. SS_PROBE_68K list (+2 idiom): `0x5000ec52, 0x5000ef22, 0x5000bbca`; expected order ec50→ef20→([$192])→bbb8/bbc8. CR-arm consumption: the post ORs `[KDP+0x674]`=0x00e00000 into saved r13 (the emulator world's CR); consumed by the DR emulator's interrupt poll (donor-analogy — Task B probes it). **`[0xcfc]`=0xffffffff ≠ 'WLSC' ⇒ PRE-warm-start regime at the frontier**: OP_IRQ runs the :812 pending-word clear + d0=1 leg only; no InterruptFlags consumption. | [PATCH-fresh]+[STATIC]+[PROBE✓] |
| **Q-I4** | (a) **Level predicate: `InterruptFlags≠0`** (whole-word — matches OP_IRQ's whole-block consumption); assert edge = SetInterruptFlag's newworld arm latching per call. **Pre-warm-start retirement story: NONE** (`[0xcfc]` probe above) — reachable pre-WLSC: delivery (proven), NK post-path execution (proven, value-0 caveat), OP_IRQ entry + :812 pending-word clear, the via_int2 Ticks++ tail; deferred post-WLSC: InterruptFlags retirement/deassert pairing, TimerInterrupt, task re-prime. (b) Assert-edge kick = `TriggerInterrupt()` from the timer thread (F5: store-release then kick; spurious-safe, missed-unsafe). (c) **Damper design**: dedicated `host_irq_latch` word (single-copy-atomic uint32; release store on the asserting thread; atomic-exchange-0 consume at delivery on the CPU thread). **A5 composition: OR at the POLL site** — `ext_pending = SheepExcExtPending() \|\| host_latch`; the host arm NEVER writes `exc_ext_pending_flag` (no clobber; C1 level-held stays PIC-only). Delivery consumes the latch only. Expected deliveries-per-assert = **exactly 1**; boot 2's 68M storm is the counterfactual. (d) `deferred_native` (re-poll meter post-3cb3b16e): bound **≤ 65537 × kick-episodes**, episodes ≈ host asserts + DEC expiries; if Q-I6's narrow lands, in-window deliveries make the per-assert re-poll count ~0–10² (no full-cap episodes); if the fence were kept, every parked-phase assert burns a full 65536 cap. Task B's invariant restates against this, not E3's class. | [STATIC]+[PROBE✓] |
| **Q-I5** | Baseline reproduced (boot 1) — E2 row above. R-II3 one static window: **0x324fec is the slot-5 placeholder word `0x0fff0005` inside an NK polling loop** (0x324f48..0x325000: `li r0,0x2e; sc`; on r3≠0 → `li r3,1; li r4,0` → the placeholder → loop). The PROGRAM#5 cadence and the `sc 0x2e`-family storm are this single wait loop. Next milestone's quarry, not chased. | [PROBE✓]+[STATIC] |
| **Q-I6** | **VERDICT (ii): fence NARROWED for the published route.** (1) The fence's stated rationale (exc_core.cpp:70–73) is the legacy KDP register-save shim — retired on the 2-SPR published route (M3A/W2-4 step 0). (2) State a published-route delivery would corrupt mid-native-window: **NONE** — the 2-SPR shim writes SPRG1/SPRG2 only (NK scratch consumed by the prologue); the NK bodies do complete save/restore (prologue r0,r7–r13 → ctx; the fallback's `bl 0x3238d4`/`bl 0x323944` entries are offset-matched to its r20–r31 clobber set); restart PC = JIT block start is a clean boundary; boot 2's delivery at restart=0x50463028 (mirror region) round-tripped. (3) Kept fence + the parked frontier (`[0x2810]=1` indefinitely, EE=1, pending — boot-1 probe + boot-4/5 record) = undeliverable by design ⇒ option (i) makes Task A's acceptance unfalsifiable. (4) Option (iii) impossible — the timer thread cannot observe `[0x2810]` transitions; no placement guarantee exists. Design shape: route-aware fence — skip the run_mode defer when the resolved entry is a published handler (EXT `external_entry≠0`; DEC only under `SS_NW_DEC_PUBLISHED`); legacy-route DEC keeps the fence. SRR1/riser compose: unchanged. Note: delivering in the emulator window is also exactly where the post's r7-bit-0x00200000 precondition holds (`r7=0x00a80000` observed). | [STATIC]+[PROBE✓] |

### Falsifications / corrections (this task)

- **None of this plan's pinned contracts falsified.** Corrections of record: (1) the recon Q3
  row's "via_int3 jmp @0x16dd6" → the patched site is **0xec50** (0x16dd6 holds an unrelated
  `jmp 0x41234567`); (2) **the canonical `rom901.bin` dump is STALE** — persisted 2026-06-11
  08:01, it PREDATES `f808a7fb` (2026-06-12 00:40, patch_68k .Sony-abort lift) and lacks the
  via_int/via_int2/via_int3 + 68k-HLE patches (raw patterns still present at 0xef2c/0xbbc8).
  Fresh dump at `/tmp/rom901_fresh_task0.bin` (boot 1). **Re-baseline of the shared canonical
  dump deferred to the coordinator** (denied as an unsanctioned shared-resource overwrite this
  task); until then, [PATCH] reads of 68k patch sites from rom901.bin are SUSPECT — use the
  fresh dump or the [PATCH-fresh] bytes recorded here.

### New residues (named)

- **R-II7 (the level-source gap)**: the EXT fallback's posted level is IACK-vector → lowmem
  `[0x3f00+vector]`-derived; both inputs are guest-init-owned and zero at the frontier — the
  post fires but writes 0. Blocks Task B's post-VALUE expectation as written; coordinator
  re-scope options recorded in the Q-I1 gap row.
- **R-II8**: `[KDP+0x910]` queue-depth halfword holds junk (0x5031-class code-pointer bytes)
  at the frontier and grows per fallback entry — the NK interrupt queue area is uninitialized
  pre-guest-init; harmless to the post path (all legs converge on the post), noted for any
  future fallback-behavior reasoning.

## M7 Task B — the consumption round trip (2026-06-12, label m7-taskB; verify-don't-build, zero source changes)

Plan + per-gate verdicts: `docs/superpowers/plans/2026-06-12-interrupt-injection.md`
"Task B results". Boots 4 of ≤5 (slot0 runs 20260612-025958.16193 / -030445.16716 /
-030701.16951 / -030924.17181), all env-on. Summary: **host EXT delivery → fallback →
post is LIVE; the round trip dies at the NK post's level test (r28=0); via_int/OP_IRQ/
Ticks never entered — the coordinator re-grade's staging-decision branch applies and
this task STOPPED for sign-off.**

### Evidence (the four boots)

- **Write-EVENT at 0x68fff070 following a host-sourced EXT delivery — ESTABLISHED**
  [PROBE✓+STATIC]: boot 4 (`SS_JIT_WATCH_ADDR=68fff070,168,68ffee80`) shows, in stderr
  order: `EXT pending ASSERTED (edge #1)` → `EXT delivered #1 restart=50494400 →
  entry=50314880` → EXT-entry probe `[0x68ffee80]=1` → `[WATCH] pc=50325f38
  addr=68ffee80 value=2 (was 1, record #3629854, sp=68ffe000)` — the fallback body's
  entry counter [KDP+0xe80] incremented exactly once, immediately after the delivery,
  and at NO other point between init (record #4529) and the delivery despite thousands
  of DEC/SC/PROGRAM deliveries: the fallback traversal is uniquely EXT-coupled. All
  fallback exit legs converge on the post 0x3254e0 (Task 0 [STATIC]); the gate-fail
  skip leg 0x5032562c had ZERO visits in all 4 boots; post-block entry observed
  r23=0x68fff070 r28=0 r7=0x00a80000 (bit 0x00200000 SET).
- **The graded watch on 68fff070 is structurally BLIND at this frontier** (instrument
  fact, recorded for future graders): the post's `sth r28,0(r23)` stores 0x0000 over
  0x0000 and SS_JIT_WATCH_ADDR is a change detector — a zero-level post can never trip
  it. Write-event grading at zero level needs the fallback-counter discriminator
  (68ffee80) or a post-block probe, as used here.
- **The level test is the break link** [STATIC, /tmp/rom901_fresh_task0.bin @0x3254e0]:
  `cmpwi cr7,r28,0` → `beq cr7,0x50325504` skips `ori r28,r28,0x8000` AND the
  `[KDP+0x674]` mask load (r31 stays 0); the sth stores 0; `bgt cr7` not-taken executes
  `and r13,r13,[KDP+0x678]` (=0xff9fffff) — at level 0 the post CLEARS the emulator-CR
  interrupt bits 0x00600000 rather than setting them. Pending halfword AND CR arm are
  both null ⇒ the DR dispatch poll has nothing to consume.
- **68k chain never entered** [PROBE✓]: `SS_PROBE_68K=0x5000ec52:8` (boot 1) and
  `0x5000bbca:8` (boot 4) — 0 matches in 59 s each. **Ticks did not move** (watch 0x168
  silent post-init; `[0x168]`=0 at delivery and at term). `[0xcfc]`=0xffffffff (pre-WLSC)
  every boot.
- **Invariants** [PROBE✓]: zero TRIPWIRE lines ×4 boots; `host-irq: edges=1 consumed=1
  deasserts=0 pending=0` (exactly-once re-proven); deferred_native=0 (boots 2/3 crash
  tuples); nest `[0x2818]` −59/−60 (expected drift class); DEC mtspr/expiry pair
  7fffffff@503230e4 / ffffffff@503230e8 (Task A's healthy class); sc/program census
  identical to E4 (16 distinct, 0xffffffff x233, PROGRAM#5 srr0=50324fec slot=5).
- **Anomaly R-II9 (named): SS_PROBE_LINEAR=1 under the env-on regime crashed 2/2**
  (SIGTRAP guest pc=0x50460c00; SIGSEGV guest pc=0x500e708c ea=0x55590000 after 5
  same-block DEC restarts) vs 0/2 without it, all else equal. Precedent class: W2-2's
  one-off SIGTRAP@0x5046dc1c. SS_PROBE_LINEAR is suspect under env-on until cleared;
  do not burn re-pin boots on crashes that correlate with it.

### The level-source staging PROPOSAL (dated addendum for coordinator sign-off — NOT implemented)

Per the Task-B re-grade: the round trip provably dies at the level test, so the staging
decision is now live. Proposal, for sign-off only:

- **The two words** (both guest-init-owned, both zero at the frontier — R-II7):
  1. `[[KDP-0x20]+0xf18]` — the NK-held PIC virtual base; until set, the fallback's
     IACK `lwbrx` never reaches the PIC model (pic stats i:0 across 68M deliveries).
  2. `[0x3f00+vector]` — the lowmem vector→level table the post's `lbz r28` reads
     (observed `[0x3f24]=0`).
- **Who owns them on a real boot:** Mac OS's native interrupt init (the MPIC driver /
  Interrupt Manager during InitInterrupts) writes both — they are init-time PLATFORM
  CONSTANTS, not per-interrupt event state.
- **Why staging them is not the fake-poke pattern:** the fenced fake-poke writes the
  EVENT (pending halfword / CR bits) from the host per-interrupt, bypassing the NK
  chain. Staging these two words writes configuration once, cold-start (trampoline
  time), after which EVERY interrupt still traverses PIC-IACK → vector table → NK post
  level test → 68k chain — the same class as the sanctioned trampoline-staged globals
  `[KDP+0x1074/0x1078]` (Execute68k pair) and `[KDP+0xf2c]` (TimebaseSpeed).
- **The open design wrinkle sign-off must resolve:** the host-irq latch deliberately
  bypasses the PIC (Q-I4/A5), so an IACK on a host-sourced delivery has no asserted PIC
  source to return a vector for. Either (i) the host source additionally asserts a
  reserved PIC source (joining the device rail early — IACK then returns its real
  vector, level read from the staged table), or (ii) the PIC model synthesizes a
  designated vector on IACK when the host latch armed the delivery (smaller, but a
  model fiction). Both shapes are S-sized; neither is authorized until signed off.
- **If sign-off declines:** the milestone ships per stop-rule 2 / Rev 2 B6 —
  delivery+post green pre-WLSC, consumption NOT-REACHED named as the next frontier
  (the level inputs are then simply part of "guest interrupt init", owned by whatever
  milestone first runs Mac OS's native interrupt init).

## M7 Task B-2 — the level-source staging (2026-06-12, label m7-taskB2; sign-off shape (i) implemented)

Per-gate verdicts + the staged-word table: the plan's "Task B-2 results". Summary: the
host source joined the PIC rail (reserved input 0x3F = `OPENPIC_IRQ_HOST`, edge-sensed,
vec=input, prio 8, IDR=CPU0, CTPR staged 0) + the two guest words staged
(`[[KDP-0x20]+0xf18]`=0xF3040000, `[0x3f3f]`=1). **R-II7 is RETIRED on the env-on
cluster: the level test passes** — first guest IACK of the OpenPIC model ever
(`src=0x3f vec=0x3f`), post writes 0x8001 to 0x68fff070 (the watch trips), CR bits SET.

### Evidence highlights (boots slot0 20260612-033407/-033746/-033954/-034428)

- **IACK leg traversal, ring-pinned** [PROBE✓, run -033954 ring #3733960..#3734012]:
  50314880 → 50325f00 (lock/save/counter) → segment dance → IACK block 0x50326050
  (`lhz [IRP+0xf88]`, mtmsr DR, **lwbrx @+0x200a0 → MMIO fault → do_iack**) → vector
  0x3f → ≠[IRP+0xf88](=0) skips immediate-EOI → `lbz [0x3f00+0x3f]`=1 → queue push +
  bitmap → post 0x3254e0: r28=1 → `ori 0x8000` → `sth 0x8001,(0x68fff070)` +
  `or r13,[KDP+0x674]` → `bgt cr7` skips the and-clear. The value-invisible-watch
  problem from Task B is gone (0→0x8001 is a change).
- **The byte-lane chain holds end-to-end**: lwbrx(guest BE) over the LE value-swap bus
  returns the NATURAL vector (0x3f, not a swap artifact) — F16's composition validated
  on the first live PIC read path.
- **Second traversal bounded**: a later not-from-emulator fallback pass IACKed empty →
  SPVE 0xff → OOB leg → guest EOI (`iacks=1/2 spurious=1 eoi_empty=1`) — bounded, no
  storm; the IACK's unconditional out-lower keeps the level-held EXT word retired
  (the A1 shape cannot recur on this path).
- **R-II10 (new, the moved break link): the parked-regime DR-poll gap.** The post's
  armed pending halfword (0x8001 at ECB+0x70) and CR arm are never consumed because the
  env-on frontier parks in the NK spin/`sc 0x2e` regime (R-II3's wait loop) and the
  DR/68k world never runs again post-delivery (comp frozen, jDR static, pure-NK ring
  windows around every increment/delivery). via_int (0x5000ec52+2) and OP_IRQ
  (0x5000bbca) probes: 0 matches, 59 s each, ×2 boots. The next frontier is scheduling
  (wake the 68k world on an EXT post), not interrupt plumbing.
- **Ticks correction of record**: Ticks (0x16a..0x16d) HAS been moving — host
  `HandleInterrupt` MODE_68K keep-set (glue:3399) +1 per invocation (0→11→12 across a
  boot), ring-attributed to NK-only windows (not addq). Task B's "Ticks did not move"
  read watch word 0x168 = Ticks' HIGH half (blind below 65536 ticks). Watch 0x16c for
  the LSB. The "first guest addq.l #1,$16a" headline remains unclaimed.
- **Lowmem wipe non-event**: the trampoline-staged `[0x3f3f]`=1 SURVIVED to edge #1
  (the one-shot re-assert found it intact) — the W2 68k-vector wipe does not reach
  0x3f00 on this boot class.
- **A/B**: gated-off boot byte-identical to the E4 baseline class (blocks=7354 exact,
  PROGRAM#5 srr0=50324fec slot=5, sc census x233/x17, zero PIC/host-irq output).

### Residue updates

- **R-II7 RETIRED** (env-on cluster): the level inputs are staged init-time platform
  constants (sanctioned class), every event guest-traversed (PIC-IACK → vector table →
  level test → post). Gated-off boots retain the Task-B zero-level behavior by design.
- **R-II8 still stands** (the junk queue depth) — and note it is load-bearing for leg
  selection: a CLEAN zero `[KDP+0x910]` would take the queue-empty leg 0x5032613c,
  which never IACKs (r28=-1 post-skip). If the queue area is ever initialized, re-check
  the IACK-leg reachability.
- **R-II10 named above** (parked-regime DR-poll gap) — the milestone's consumption
  remainder lives there.

## R-II10 / slot-5 park recon (2026-06-12, label slot5-recon) — the park is the NK IDLE TASK in its power-saving nap loop; slot 5 is NOT a missing service; the release IS the interrupt chain; the new break is the post-delivery consumption livelock (three shapes pinned)

Boots: **4 of ≤4** (slot0 `20260612-040140.28575` default + probes; slot0
`-040346.29469` env-on cluster; slot1 `-040606.34863` env-on spin-pin; slot1
`-040756.35057` env-on ctx-dump). Env-on cluster = `SS_NW_EE_RISER=1
SS_NW_DEC_PUBLISHED=1 SS_NW_HOST_IRQ=1 SS_NW_PIC=1` (B-2's TEST cluster). No
SS_PROBE_LINEAR (R-II9 honored); same 8-probe set held across boots 1–2.
Canonical dump verified pre-read: rom901.bin md5 d1a267a9 (MANIFEST checked);
every [STATIC] window below verified raw==patched against rom901_inventory.bin
unless noted.

### Answer table

| Q | Answer | Tag |
|---|---|---|
| **Q-S5a — what the park IS** | **The NK idle task.** Entry 0x50324f04 (`li r31,0` + the developer-name easter-egg loads: "idle task", "Alan", "Jim ", "Alex", "Derrick "); body 0x324f48–0x325000 [STATIC raw==patched]: a 30-register rotate (`mr r30,r1 / mr r1,r2 / … / mr r29,r30`) per iteration; when the delay counter r31 hits 0 → `li r3,0xc; li r4,1; li r0,0x2e; sc`; **r3==0 → loop immediately; r3≠0 → `li r3,1; li r4,0;` + the inline word 0x0fff0005 at 0x324fec (`twi` slot-5 callout); post-callout r3≠0 → r31:=0x989680 (10M rotations ≈ busy delay) → loop**. The loop has NO exit branch — `b 0x50324f48` unconditional at 0x325000. Release = scheduler preemption (an interrupt making another task runnable), never a loop exit. | [STATIC]+[PROBE✓] |
| **Q-S5a — selector 0x2e** | Body at NK dispatch target 0x5031bdfc (table NK_base+0xacb8, target=NK_base+table[sel]+4·sel): `li r8,0; bl 0x503148e0; r3:=r8,r4:=r9 → common exit 0x5031b124`. 0x3148e0 [STATIC]: bounds-checks r3(=0xc) as an index against the structure at `[KDP-0x338]` (`[+0x38]` table ptr, `[+0x44]` count): table null → r8=-0x7266; index OOB → r8=-0x7267; else a context-save + queue/processor-info walk. Idle args live-verified: `[PROBE✓] 0x5031bdfc r3=0xc r4=1`. Idle semantics = "may I power-save?" — nonzero return routes to the nap callout (observed path: one sc, then the twi). | [STATIC]+[PROBE✓] |
| **Q-S5a — what the twi does** | NK published 0x700 handler 0x50314700 [STATIC]: reads the faulting word, `xoris 0xfff` → slot id; slot<0x10 → counter `[KDP+0xe40+4·slot]++`, dispatch `mtlr [KDP+0x5f0+4·slot]`, **resume SRR0 := twi+4** (r10+=4 before blr) — the placeholder IS a call instruction. Slot-5 exit pointer is built by **NK cold-init 0x311770–0x311820** [STATIC]: default-fills all 16 with NK+0x46d0 (loud unimplemented), then overrides slots 0–8,15; **slot 5 := NK_base+0x9d20 = 0x50319d20**. | [STATIC] |
| **Q-S5a — the slot-5 service body** | **0x50319d20 = the NK power-management service** [STATIC raw==patched; PROBE✓ entry r3=1 r4=0]. r3 = mode (≤0xb; 8/9/0xb have special legs); **0x319d38: `and. r8,[KDP+0x670],r9` where r9 = live CR r13 (from-emulator) or `[KDP-0x440]` (the deferred-post mask) — if a pending 68k-interrupt post overlaps the mask `[KDP+0x670]`(=0x00200000 [PROBE✓]), return r3=0 immediately ("work pending, don't sleep")**. Else: capability byte `[KDP+0x6b8]` (=0x1b [PROBE✓] — mode-1 capable) gates; incapable → r3=-0x7267; capable → HID0 doze/nap bits + `mtmsr MSR\|0x8002[\|POW]` → **`b .` self-loop at 0x50319df8 with EE SET** — the architectural nap: the ONLY exit is an asynchronous interrupt (wake path 0x319dfc restores HID0, returns r3=0). | [STATIC]+[PROBE✓] |
| **Q-S5b — what service is slot 5** | **FE0F opcode / power management** — upstream SheepShaver's own table comment (`patch_68k_emul`, rom_patches.cpp:2113–2118: slot 0=Emulator start, 1=Mixed Mode, 2=Reset/FC1E, 3=FE0A, **4=Interrupt**, 5=FE0F) + M6A-ONGOING-ENTRY-DESIGN §1.2 ("FE0F opcode (power mgmt)", exit ptr `[KDP+0x604]`). Slot 5 was excluded from the Task-U stops + FE1F restore because upstream's `patch_68k_emul` statically replaces table slots 0–3,5 with paravirtual stub branches (PATCHED table at file 0x36e8c0: `48001040/4800113c/48001238/48001334/0/4800142c/0…`; RAW: all 16 twi placeholders) — slot 5 was never a placeholder in the patched table. **The service is NOT missing: the NK implements it itself (exit-array override → 0x50319d20) and it ran to completion on boot 1.** | [RAW-ROM]+[PATCH]+[STATIC] |
| **Q-S5c — does serving slot 5 release the park** | **NO — and nothing needs serving.** The slot-5 service is what PUTS the boot into the park's true resting state: boot 1 (default) [PROBE✓] sequence = idle entry (visit 1) → sc 0x2e (visit 1) → PROGRAM#5 → slot-5 service entry → **0x50319df8 nap self-loop entered (visit 1) and never exited** — visits of every other leg stayed at 1 (no 10M-spin: 0x324ff8 never fired; no capability-fail: 0x319e4c never fired). The default park = **the CPU napping with EE enabled, waiting for an interrupt that the default config never delivers**. No SS_SEED_MEM release exists: the loop polls no memory word — the release primitive is exception delivery itself. | [PROBE✓] |
| **Q-S5d — circularity verdict** | **YES — the park is waiting on exactly the interrupt chain this milestone built, and it is now resolvable-in-mechanism but blocked one link downstream.** Env-on, the nap park never forms (idle probes 0 hits, boots 2–4): the EXT edge pulls the DR into its interrupt path instead. But all three env-on boots LIVELOCK post-delivery without the 68k world consuming the interrupt: **(shape A, boots 2–3)** EXT delivered mid-DR → DR branches to entry-vector **slot 4 ("Interrupt", the restored twi)** → PROGRAM#4 (srr0=5046e8d0, lr=5046c4f4) → NK slot-4 service 0x50314660 [STATIC] = 3 instructions: `mtlr [KDP+0x5b0]; blr` (the SAME EXT/PIC-IACK fallback body 0x50325f00) → NK world-restore tail block **0x503244e8 livelock: 10^9 visits** [PROBE✓ boot 3], r10(saved-MSR-image)=0x9040 r12=0x5046c4f4 constant, comp frozen, jDR static, DEC still delivering ~88/s underneath; **(shape B, boot 4)** EXT delivered at restart=0x50465f28 → no PROGRAM#4 at all (the CR arm never reached the live DR context) → the 68k world runs flat-out (jDR +100M blocks/s, comp frozen) in a guest 68k busy-wait — the classic waiting-for-Ticks/VBL spin; **(shape C, B-2's ring-slowed boots)** the timing lets the boot reach the idle/nap park with the post armed but unpolled (R-II10 as originally stated). All three shapes share one missing link: **delivery → 68k-interrupt consumption-and-retirement round trip**. | [PROBE✓]+[STATIC] |
| **Q-S5e** | Next-task card below. | — |

### The consumption-livelock evidence (the new frontier, one link past R-II10)

- Shape A's livelock block 0x503244e8 [STATIC, in the 0x3242dc world-restore tail]:
  XER fixup + `mtcrf r13` + reload r0,r7–r13 from ctx + `lwz r6,0x18(r1); lwz r1,4(r1);
  bctr` — the restore-into-world tail executing 34–71M blocks/s without completing a
  world switch (jDR frozen). Boot-4 ctx dump at visit 1 (cold, pre-livelock):
  r6=0x68fff100 = ECB+0x100 emulator ctx (slot-fields +0x5c=0x5046e8c0, +0x9c=0x50480000,
  +0xe0/+0xfc=0x5046f900 visible). The livelock-time ctx was not captured (boot 4 took
  shape B; budget cap). The bctr resume target at livelock = the first open question of
  the next task.
- Shape A's PROGRAM#4 fired ONCE (counters then froze) — the cycle is NOT re-trapping
  through the twi; the spin is pure-NK downstream of one delivery.
- The interrupt arm is never retired in any shape: `edges=1, deasserts=0` class
  (per B-2), once-per-edge latch held, no second edge possible — consistent with the
  armed CR/post surviving unconsumed forever.
- DEC deliveries run healthily UNDER all three livelocks (exc field 0 growing ~88/s)
  — delivery machinery is not the gap; consumption/retirement is.

### Recommended next-task card (Q-S5e)

**"The slot-4 interrupt round trip: EXT post → DR interrupt vector → 68k IRQ dispatch
→ retirement → DR resume"** — size **M** (not a FE1F-class service build; slot 4's NK
side already exists and is 3 instructions; the work is making the
fallback-post/world-restore cycle terminate in a 68k interrupt the emulator actually
executes and RETIRES).

- NOT the task: implementing a slot-5 service (exists, NK-internal, conformant), or
  releasing the nap park directly (it releases itself the moment interrupts deliver —
  proven: env-on boots never park in the nap).
- Task-0 recon questions: (1) shape A's bctr resume target + why the restore tail
  cycles (ring-windowed boot at the livelock, or `[r6:0x180]` ctx probe that survives
  into the livelock regime); (2) what retires the CR arm / `[KDP+0x67c]` post on the
  donor (real-NK) path — the 68k side must clear cr2.lt via the slot-0 re-entry after
  servicing, find the clear site; (3) shape B's discriminator: why the mid-DR delivery
  failed to arm the LIVE CR (restore ordering vs the [KDP+0x674] OR into saved r13).
- Falsifiable acceptance gate: env-on cluster boot — after `EXT delivered #1`, (a)
  PROGRAM#4 fires AND the DR/68k world RESUMES (jDR grows past the delivery with comp
  unfrozen, no 0x3244e8/restart-block livelock ≥10s), and (b) the via_int/OP_IRQ 68k
  chain probes fire (`SS_PROBE_68K=0x5000ec52:8`, the Q-I3 list) — "the NK leaves the
  park and the DR/68k world runs post-delivery". Rider: Ticks LSB via watch word
  **0x16c** (B-2's corrected word) moving with DR-window attribution (the guest addq,
  not the host keep-set).
- The default-config frontier converges on the same fix: flip-cluster + working
  consumption ⇒ the nap park wakes per DEC/EXT and the scheduler has a runnable blue
  task whenever a 68k interrupt is pending (the slot-5 service's own
  `[KDP+0x670] & [KDP-0x440]` check then suppresses napping while posts are pending —
  the NK's designed coupling, already in the ROM).

### Falsifications / corrections

- **R-II10's framing is corrected, not falsified**: "the parked NK regime never runs
  the DR/68k world [to poll the post]" is shape C only and timing-dependent. Env-on
  no-ring, the post IS polled (shape A: the DR took its interrupt vector — the first
  slot-4 consumption ever observed); the break is the round trip's tail, not the poll.
- The Q-I5/R-II3 reading "the park is an NK `sc 0x2e` polling loop" is REFINED: the
  sc is incidental (one power-save query per idle cycle); the resting state is the
  0x50319df8 nap self-loop (EE on), not an sc storm. The 0x2e-family storm in earlier
  censuses = idle cycling when nap exits fast or fails — config-dependent.
- No pinned contract of the interrupt-injection plan falsified; B-2's chain-walk rows
  stand. The entry-vector slot-4 path was predicted "deliberately dead in the
  paravirtual design" (M6A §1.2) — on newworld env-on it is LIVE and load-bearing.

## R-II9 instrument-fix note (2026-06-12, label instr-hardening) — SS_PROBE_LINEAR no longer crashes: 0/2 at HEAD, suspicion DOWNGRADED (not fully cleared)

Bounded re-test of Anomaly R-II9 (2 of ≤2 boots, slot0 runs `20260612-051031.53249`
and `-051246.53544`): full env-on cluster (`SS_NW_PIC=1` on the post-flip defaults) +
`SS_JIT_TRACE_RING=1 SS_PROBE_LINEAR=1 SS_PROBE_CAP=32 SS_DR_R24_RING=1` + an 8-probe
set spanning the delivery chain (DEC entry 0x50313200, EXT entry 0x50314880, post
0x503254e0, fallback 0x50325f00, idle 0x50324f04, 0x2e body 0x5031bdfc, PROGRAM
0x50314700, livelock tail 0x503244e8) + `SS_JIT_WATCH_ADDR=68fff070,168:8`.

- **Result: 0/2 crashes** (vs the original 2/2). Both boots ran the full 60 s,
  74–132 linear-probe fires, healthy DEC regime (~88/s), EXT edge #1 delivered, and
  parked in the KNOWN shape-A consumption livelock (`pc=0x503244e8`, comp frozen) —
  the same terminal state as no-LINEAR boots of this class. The crash signatures
  (SIGTRAP@0x50460c00, SIGSEGV ea=0x55590000 after 5 same-block DEC restarts) did not
  recur.
- **Root-cause assessment (code-static + chronology)**: the linear-probe path is
  READ-ONLY — `probe_should_log()` + stderr prints; no guest-memory or register
  writes (MMIO reads refused) — so SS_PROBE_LINEAR could only ever perturb *timing*
  (print latency at block entry). The original 2/2 crashes ran the m7-taskB code
  (~03:00), which PREDATES both the B-2 level staging (`68c8fc3e`) and the pre-flip
  **ClearInterruptFlag lost-edge race fix (`2a452166`)**. The most plausible reading:
  the crashes were the timing-sensitive frontier class (probe latency at
  delivery-restart blocks widening a race window), and the race fix retired the
  window. Not definitively pinned — the original tuples cannot be re-run on the old
  code within budget.
- **Status: DOWNGRADED from "suspect — do not combine" to "not reproduced at HEAD
  (0/2)".** SS_PROBE_LINEAR may be used under the delivery regime again. The standing
  discipline is unchanged: hold instrument sets constant across A/B boots (this
  frontier class remains timing-sensitive), and if a LINEAR-correlated crash recurs,
  re-raise R-II9 with the new tuple rather than burning re-pin boots.
- No guard was landed: the prescribed suppress-while-delivery-in-flight guard would
  suppress exactly the highest-value fires (probing delivery entry/restart blocks IS
  the instrument's main use under this regime), and the crash it would insure against
  no longer reproduces.
- **Cross-reference (2026-06-12, post-slot-4 Task A)**: the crash CLASS recurred
  WITHOUT SS_PROBE_LINEAR — slot-4 Task A saw 3/7 env-on boots crash pre-engagement
  with the same signature family (DEC#1 delivered into 0x500eXXXX early-ROM code,
  5 same-block restarts then a register marching out of RAM; Task A's
  restart=0x500e7310 ×5 matches this note's original pc=0x500e708c "after 5
  same-block DEC restarts"). This TENSIONS the lost-edge-race attribution above:
  the class is delivery-into-early-ROM-code, not LINEAR-specific — LINEAR likely
  only perturbed timing into it. The open class is named in the slot-4 plan's
  Task-A residue + Task-C flip criteria; root-causing it belongs to that milestone's
  acceptance, not to instrument work.

## Slot-4 consumption recon (M8 Task 0, 2026-06-12, label s4t0) — fork-(iii) CONFIRMED LIVE: the riser's mtmsr re-raise fires mid-tail (EXT restart=0x50318018 observed) and the saved ctx carries the torn r10/r11=0x9040 images; the SRR0-image slot is ctx+0xfc (NOT +0xa4) and the resume-PC tear travels the register/CTR path, not the ctx slot

Boots (counted): **4 of ≤8** — b1 slot0 `20260612-051156.53432` (default, entry gate 1 +
Q-C4/nap-word probes, BOOT-VERDICT PASS); b2 slot1 `-051304.53665` (env-on `SS_NW_PIC=1`,
entry gate 2, PASS — shape A); b3r slot0 `-051623.59891` (env-on, PASS — shape A,
probe-arming failure, see instrument note); b4 slot0 `-051854.60285` (env-on, PASS — shape
A, the livelock-ctx capture). Not counted (disclosed): b3 `-051503.54470` crashed SIGSEGV in
DR code pre-delivery on a build raced with instr-hardening's landing (05e0d921/f1aca585);
per the rebuild-race rule it was re-run once after a fresh rebuild as b3r. Build state:
all counted boots at HEAD f1aca585 (+ instr-hardening's then-uncommitted glue/ppc-cpu
instrument edits in the working tree); no instrument-contract anomalies observed.

**Instrument note (self-inflicted, disclosed):** `ss-slot-boot.sh --env` splits on
WHITESPACE; a single quoted `'SS_NW_PIC=1;SS_PROBE_PC=…'` token becomes ONE env var
(`SS_NW_PIC="1;SS_PROBE_PC=…"`) — PIC still parsed ON (non-"0"), but **SS_PROBE_PC was
never armed** on b2/b3r (zero probe lines ≠ zero visits). Multi-var runs need repeated
`--env` flags; SS_PROBE_PC's own multi-PC `;` separator is safe inside its own var. This
cost Q-C1 one extra boot (3 boots on Q-C1 vs the ≤2 cap; capture succeeded on b4).

### Entry-gate record

- Default boot (b1): `--expect 'EXT delivered #1;;DEC delivered;;srr0=50324fec'
  --absent 'TRIPWIRE'` → EXPECT 3/3, BOOT-VERDICT PASS. Class confirmed: DEC #1–4
  `(2-SPR)` via 0x50313200, EXT #1 (restart=50319df8 = the nap loop), PROGRAM#5
  srr0=50324fec slot-5 park, E4 sc census.
- Env-on boot (b2): `--expect 'first-iacks: src=0x3f vec=0x3f;;EXT delivered #1'
  --absent 'TRIPWIRE'` → EXPECT 2/2, PASS. **Baseline shape on this machine/instrument
  set = SHAPE A** (classifier greps: slot-4 PROGRAM line `srr0=5046e8d0 word=0fff0004
  slot=4 … lr=5046c4f4` PRESENT; HB signature jNK-growing ~82M/s with jDR STATIC
  (2991623) + `comp frozen`). Reproduced on b3r and b4 (3/3 env-on boots → A; no
  B-producing config seen — Task B's shape-B arm needs its config-discovery boot per
  rev-2 B3, out of Task B's cap). Note the livelock block is RUN-VARIANT within the
  reload region: b2 spun at **0x503244f8** (HOT-PC), b4 (and instr-hardening's two
  051031/051246 boots) at **0x503244e8** — same family, the torn resume PC lands on
  whichever block boundary the delivery hits.

### Blocking-answer table

| Q | Answer | Tag |
|---|---|---|
| **Q-C1 root cause + FORK VERDICT** | **Fork-(iii) patched-tail non-atomicity — CONFIRMED LIVE.** (a) [STATIC] restore path: slot-4 service 0x50314660 `mtlr [KDP+0x5b0]; blr` → fallback 0x325f00 → exits converge → reschedule gate → tail 0x3244cc (`lwz r8,0xedc(r1); mfspr r1,SPRG0; mtlr r12; mtctr r10; mtcrf 0xff,r13; b 0x318000` [PATCH; raw = `mtspr SRR0,r10; mtspr SRR1,r11 … rfi @0x324524`, raw-vs-patched word diff: 0x3244d8/dc/e0 + 0x324524]) → stub 0x318000 (authority rom_patches.cpp:2522-2556, 7-word default-ON shape: `lwz r10,XLM_IRQ_NEST; subi; stw; mfmsr r10; rlwimi r10,r11,0,16,16; mtmsr r10; b 0x3244e4`) → reload region 0x3244e4–0x324524 (XER fixup; `mtcrf 0xff,r13`; reload r10/r11/r12/r13/r7/r8/r9 ← ctx+0x154/15c/164/16c/13c/144/14c, r0←+0x104; `lwz r6,0x18(r1); lwz r1,4(r1); bctr`). The ctx is `[KDP-0x14]` (save path 0x313ce0/0x313d40: `lwz r6,-0x14(r1)`). **The ctx SRR0-image slot is +0xfc, NOT the plan's verify-first +0xa4** — save sites `stw r10,0xfc(r6)` @0x312b4c/0x31a6b0/0x324584, restore `lwz r10,0xfc(r6)` @0x32448c (CONDITIONAL — the fast path can skip it, keeping the register-carried resume PC) /0x324778/0x324d54; **+0xa4 is the SRR1/MSR image** (`stw r11,0xa4(r6)` @0x312be0/0x3245d8). (b) [PROBE✓ b4] livelock capture at 0x503244e8, visits 10⁰..10⁹, registers CONSTANT: r1=0x68ffe000(KDP) r6=0x68fff000 r7=0xffa00000 **r10=0x9040 r11=0x9040** r12=0x504a73a8 r13=0x001018f8; `[r6:0x180]` ctx dump: **+0x154(r10-image)=0x9040, +0x15c(r11-image)=0x9040 — the riser's composed-MSR scratch SAVED AS GPR IMAGES (the torn ctx, by-construction values; clean r11 image = 0xd032/f072-class)**; +0xfc(SRR0-image)=0x5046e1a0 (the DR switch-in resume — the ORIGINAL world's resume PC, intact but never reached); +0xdc(CR-image)=0x20100000 (no 0x00e00000 arm bits); **+0x70=0x80010000 — the 0x8001 post ARMED in-frame, unretired**. **The mid-tail re-raise observed directly: `EXT delivered #1: restart=50318018` (b4) = the stub's post-mtmsr block boundary** — execute_mtmsr (ppc-execute.cpp:1432-1439) fired the EE 0→1 edge inside the patched tail exactly as A1 predicted. Steady-state loop (~38M visits/s vs DEC's 88/s: the loop itself is delivery-free): CTR pinned at the reload-region PC by the onset episode's torn resume; block 0x3244e8 reloads the torn images, `r6←[KDP+0x18]=0x68fff000, r1←[KDP+4]=0x68ffe000` (the save protocol's own back-pointers), bctr→CTR→itself; the 88/s DEC deliveries ride the 2-SPR fast exit (0x313ba4: `lmw r14,0x38(r8); mtlr; rfi` with entry SRR0/SRR1) and return INTO the loop without touching CTR. **Predicate refinement (recorded, not a falsification-trigger):** the written proxy predicate "ctx+srr0-image holds a reload-region PC" did NOT hold (+0xfc holds the DR PC) because the resume-PC tear travels the REGISTER/CTR path (the conditional +0xfc reload @0x32448c + the 2-SPR exits), not the full-save slot; the MECHANISM is confirmed by the two stronger direct observations (restart=0x50318018 mid-stub delivery; r10/r11 ctx images = 0x9040 scratch). The fork does NOT re-open: no DR/foreign-PC resume is being attempted-and-failed — the restore never completes because the loop re-enters itself. | [STATIC]+[PROBE✓]+[PATCH] |
| **Q-C2 retirement chain** | Site table (PC → action → word): **(1) emul_op.cpp:812** (OP_IRQ head, reached via fe6b @68k 0xbbc8 in the 60 Hz proc — the via_int chain) → `WriteMacInt16([[KDP+0x67c]],0)` → **0x68fff070 := 0, the guest-side halfword retire** (runs pre-WLSC: it precedes the HasMacStarted gate). (2) **NK post body 0x325520/0x32552c** [STATIC raw==patched]: on a pass with r28==0 (IACK'd vector, level 0) → `sth r28,0(r23)` writes 0 to 0x68fff070 AND `and r13,r13,[KDP+0x678]` clears the CR arm — the post's own level-0 retire leg; on r28<0 (queue-empty/spurious) `blt cr7 0x325530` skips BOTH (no retire). (3) DR-side cr2-arm clear at slot-4 dispatch: mirror code, **[PROBE✓]-only territory — statically unpinnable (no dump covers the mirror)**; recorded as the open half, covered by the hardened-downgrade conditions. (4) **main_unix.cpp:2930-2963 `ClearInterruptFlag`** → at InterruptFlags==0 (nw_host_irq_on) → `SheepExcHostIrqDeassert()` + the lost-edge re-check — the deassert that unlocks edge #2. Full cycle: post armed → DR consumes (slot-4/level-1 → via_int chain → OP_IRQ) → :812 clears halfword (+ INTFLAG consumption when HasMacStarted) → ClearInterruptFlag → deassert → latch re-armable. **Task-B falsifiable predicate:** watch 68fff070 records **0x8001→0x0000** with the attributing record in the PINNED writer family — (family a) DR-window record with r24≈0xbbca-class (the emul_op:812 path), or (family b) NK post-body PC 0x325520-class — AND temporally AFTER the 0x5000ec52/0x5000ef22 probes fire, AND bound to `edges/consumed/deasserts` advancing past 1 (all three hardened-B1 conditions stated in advance). | [STATIC]+[PATCH-fresh carried] |
| **Q-C3 shape-B discriminator** | **Image-selection defect, fix is ORDERING (code) — NOT a seed, NOT subsumed by A's fix** (A6 expectation confirmed statically). The from-emulator post leg (0x325520-2c) ORs `[KDP+0x674]` into the **VOLATILE working r13 only** — no store to the ctx CR image (+0xdc) and no write to the deferred pair. The deferred-post pair (`[KDP-0x440]` mask + `[KDP-0x43c]` halfword, writer 0x325674) is the NOT-from-emulator leg only; the scheduler-restore drain 0x324720-0x324750 re-applies exactly that pair (`sth [KDP-0x43c]→[[KDP+0x67c]]; or r13,r13,[KDP-0x440]` — itself conditional on task-flag bit 0x10 @0x3246a4). The scheduler restore reloads r13/CR from the target ctx (`lwz r13,0xdc(r6)` @0x324658/0x3246f8) — **any working-r13 OR dies on a ctx-reloading exit path**. Shape B = the EXT episode's exit took such a path; the arm reached neither the live CR nor the deferred mask. Fix class: make the from-emulator leg ALSO stage the deferred pair (mirror 0x325674) or write-through to the ctx CR image — bounded, the NK's own staged-post mechanism is the donor; Task B applies it as Q-C3's pinned fix, separate commit. (Even shape A's frame shows the lost arm: live r13=0x001018f8 and ctx+0xdc=0x20100000 both lack 0x00e00000 bits while +0x70 holds 0x8001.) | [STATIC] |
| **Q-C4 R-II8 on this path** | **Load-bearing-as-is for the IACK leg only; INERT for consumption/restore — no seed needed.** [STATIC] every `[KDP+0x910/0x912]` access in the NK window sits in the IACK/fallback family (0x325f44-0x3263c8) + init zeroing (0x310888) + reset (0x326fcc/fd4); zero readers on the restore/scheduler/drain path (0x3242xx-0x3247xx). [PROBE✓ b1] `[0x68ffe910]`=0x503224e8 (junk, non-zero) — the IACK-leg selection keeps working as today; nap-coupling words confirmed: `[0x68ffe670]`=0x00200000, `[0x68ffdbc0]`([KDP-0x440])=0. Verdict: inert here; R-II8 disposition unchanged (re-check only if the queue area is ever initialized). | [STATIC]+[PROBE✓] |

### The fix table (Task 0.5 — fork-(iii), for coordinator ACK)

| Item | Pin |
|---|---|
| Mechanism | **Defer the EE-edge re-raise past the bctr** (rfi-atomicity emulation): when the riser stub's `mtmsr` (guest PC ∈ [0x50318000,0x50318020), the stub window) takes the EE 0→1 edge with a source pending, do NOT `trigger_interrupt()` at that block boundary; latch a deferred-edge flag and fire it at the first subsequent block boundary whose entry PC is OUTSIDE the tail/reload window (i.e. after the 0x324524 bctr has executed and the resume PC is real). MSR.EE and the resume PC then become effectively atomic, as the raw NK `rfi` was (oracle: raw tail `mtspr SRR0,r10; mtspr SRR1,r11; … rfi` @0x3244d8-0x324524, rom901_inventory.bin). |
| Files | `ppc-execute.cpp` (execute_mtmsr re-raise predicate — the only trigger site); the deferred-edge latch + consume check in the spcflags/block-dispatch path (`ppc-cpu.cpp` check_spcflags neighborhood or exc_core helper); `rom_patches.cpp` untouched (stub bytes unchanged; export the stub-window constant if needed). No new service bodies, no new machinery — one latch + one predicate, the M7 fix-class. |
| Env gate | `SS_NW_IRQ_CONSUME` (default OFF, structurally inert off; flip LAST in Task C per plan). |
| Riser-conditional (A7) | The deferral activates only when the riser is armed (`MachineProfileIsNewWorld()` && SS_NW_EE_RISER not opted out) — consume-on+riser-off is inert by construction; cluster-join decision carried to Task C (fix edits the re-raise path ⇒ B4's fold-into-cluster arm is the live one). |
| Why it cures shape A | The torn save can no longer happen mid-tail: no delivery lands between the mtmsr and the bctr, so the saved ctx always carries the real resume PC and real r10/r11 images; the 0x9040-image self-loop cannot form. DEC cadence unharmed (delivery merely shifts ≤ a few blocks, to the first post-bctr boundary). |
| What it does NOT fix | Shape B (Q-C3's image-selection defect — separate bounded ordering fix in Task B, expected per A6) and the retirement chain (Task B's round-trip work). |

### Residue / falsification ledger

- No pinned contract falsified. The +0xa4-srr0 expectation was verify-first by rev-2's own
  precision note and is now PINNED CORRECTLY (+0xfc SRR0-image / +0xa4 SRR1-image); the
  Q-C1b proxy predicate refined as recorded above (mechanism confirmed by stronger direct
  evidence; fork not re-opened).
- Q-C1c ring window: NOT RUN (demoted optional per rev-2 A4; the cycle path was statically
  derived and live-confirmed without it).
- Shape-B-producing config: not observed in 3 env-on boots (all A) — Task B's
  config-discovery boot accounting stands (rev-2 B3).
- R-II8: disposition refreshed by Q-C4 (inert for consumption). R-II9: downgraded by
  instr-hardening (see their addendum above) — not relied on here (no SS_PROBE_LINEAR used).
- New instrument lesson for the house: the `--env` whitespace-split (see instrument note) —
  worth a README-slots line.

## Slot-4 consumption Task B (M8, 2026-06-12, label s4tb) — the round trip advanced FIVE legs live (post → Q-C3 staging → drain → slot-4 twi → 68k level-1 handler) and stops at the VIA-IFR dispatch: the handler rte's source-less, the post is never retired, Ticks not guest-claimed

Commits: `02a0b74e` (Q-C3 fix), `17e0d071` (ticks_keepset census), `377edbf6`
(MODE_EMUL_OP fence), `e6824327` (re-pin: starvation backstop kick). Boots counted
**5 of ≤5**: b1 `20260612-062232` (edge-miss, no engagement), b2 `-063150` (EXT +
post + staging proven; watch budget exhausted at the post trip), b3 `-063725`
(edge-miss; sentinel-init fact), b4 `-063939` (staging→drain→slot-4 twi proven; the
starvation falsification), b5r `-064659` (re-pin verification; SIGSEGV late,
post-evidence). Disclosed, not counted: b5 `-064535` (edge-miss pre-engagement;
re-run once per the pre-engagement protocol).

### The round-trip chain as observed (b4/b5r, env-on cluster + SS_NW_IRQ_CONSUME=1)

| # | Leg | Verdict | Evidence |
|---|---|---|---|
| 1 | Host EXT edge → delivery | ✓ (one-shot) | `EXT delivered #1: restart=500ed8ec` (b5r: same); **edge is a knife-edge race** — 3/6 env-on boots never got it (the ticker fires only while XLM_IRQ_NEST==0; nest drifts; named residue below) |
| 2 | NK from-emulator post | ✓ | watch `68fff070=80010000` @50325520, attributing block `50325520->502fd280` = the Q-C3 detour live |
| 3 | **Q-C3 staging (the fix, 02a0b74e)** | ✓ | `[PROBE 0x502fd280 visit=1] r13=… r28=0x00008001 r31=0x00e00000`; watch `[KDP-0x440]=00e00000` + `[KDP-0x43c]=8001` one record later (stub @0x2fd280, donor-mirror incl. task-flag 0x10) |
| 4 | Scheduler-restore drain 0x324720 | ✓ | watch @0x503246b0 mask→0, @0x50324734 halfword re-posted + sentinel ffff reset (records #3568082-84, ~70 records after staging) — **the arm now survives ctx-reloading exits** (Q-C3's defect closed; also closes shape B's lost-arm mechanism) |
| 5 | **Slot-4 twi (the re-graded PROGRAM#4 gate)** | ✓ | `PROGRAM delivered: srr0=5046e8d0 word=0fff0004 slot=4 lr=5046c4f4` — fires right after the drain |
| 6 | Latch fire / no starvation (re-pin e6824327) | ✓ post-re-pin | b4 falsified Task A's "kick guaranteed": `deferred=1434126 fired=0 latch=1`, VCLK `pending=1` (the latched edge IS the DEC's own delivery; EXT one-shot) — the 60 Hz backstop kick cures it: b5r VCLK `pending=0`, scheduler-quantum mtspr writes resumed, `deferred_ee=121` healthy |
| 7 | 68k level-1 handler runs | ✓ | r24 ring (b5r): repeated `5000ed0a…5000ee82…5000eecc(rte)` passes at the 60 Hz cycle; `delivered_program=48626` = the slot-4 re-trap loop while unretired |
| 8 | VIA-source dispatch → 60 Hz proc → OP_IRQ retire | **✗ — THE NEXT FRONTIER** | the handler's source dispatch (`bclr d6,(a4); beq; movea.l $6e4.w,a0; jsr (a0)` @0x5000ee9a, rte @0x5000eecc [RAW-ROM, m68k-dis]) finds NO VIA IFR source in the via6522 model and rte's source-less; PROBE68K 0x5000bbca = 0 matches; post still `80010000` at obs=1e7; the un-retired post re-traps slot-4 at ~1.2k/s |
| 9 | Multi-edge (edges past 1) | ✗ structurally pre-WLSC | retirement's deassert half (`ClearInterruptFlag`→`SheepExcHostIrqDeassert`) is HasMacStarted-gated (emul_op.cpp OP_IRQ head; Q-I4) — the plan's gate (c) presumed a cycle that cannot close pre-WLSC; recorded as a plan-gate flaw, not a code falsification |
| 10 | TICKS RIDER | **NOT guest-claimed** | the 60 Hz 0x16c movement is the host keep-set (b4: 16c=0x000b == `ticks_keepset=11` exactly; b5r 60 Hz era = ticker keep-set class); the chain stops at leg 8 before the 60 Hz proc — per the rider's fallback clause, the stop site IS the recorded frontier: **the via6522 model's IFR never presents the 60 Hz source bit to the 68k dispatch** |

### Verdict and stop (stop-rule 2)

Consumption machinery (this milestone's scope) is GREEN end-to-end: post → staging →
drain → slot-4 service → world switch → 68k handler execution, repeatably at 60 Hz.
The chain dies on the NEXT surface — a device-model gap (VIA IFR source bit), not
slot-4 consumption machinery. Per stop-rule 2: captured, nothing built for it.
Retirement (leg 8) and the multi-edge invariant (leg 9) remain RED pending that
frontier + WLSC respectively.

### Residue / census

- **EXT-edge one-shot flakiness (NEW, named)**: 3/6 env-on boots never saw the edge
  (b1/b3/b5: `edges=0`, ticks_keepset=0 — the 60 Hz ticker's `XLM_IRQ_NEST==0` exact
  test never sampled true). Feeds Task C's flip-criteria item (a) (EXT-only strand).
- **0x500eXXXX pre-engagement DEC-crash class (Task A's 3/7)**: 0/6 recurrence this
  task. Stays open as named (R-II9 cross-reference per the coordinator's note).
- **NEW late-SIGSEGV (b5r)**: one instance, post-evidence, during the 60 Hz slot-4
  cycle era — host pc 0x125382ef8, ea=0x4000010020c8 (RAM 0x10020c8); not the
  0x500eXXXX class; unreproduced; recorded only.
- Shape A/B/C: none formed post-fix in any engaged boot (b2/b4/b5r) — shape B's
  config-discovery boot is moot (its mechanism is closed by leg 4's evidence).

## Slot-4 consumption Task C (M8, 2026-06-12, label s4tc) — flip decision: SHIP GATED-OFF-GREEN; R-II10 disposition

**THE FLIP DECISION: `SS_NW_IRQ_CONSUME` stays default OFF** (standalone 17th gate;
full per-criterion verdict table + the named flip prerequisites:
`docs/superpowers/plans/2026-06-12-slot4-consumption.md` § "Task C results"). The
deciding evidence: on the post-flip-candidate config (default boot +
`SS_NW_IRQ_CONSUME=1`, boots 2/2) a NEW deterministic behavior line appears —
**SC#1 r0=0x0d r1=1017ffde lr=5046c5ac**, ahead of the canonical SC#1=0x3f, absent
from 19/19 prior boots — the live face of the Task-B review's P1 (the Q-C3 stub
stages the deferred pair UNCONDITIONALLY; the drain's and-clear leg at 0x324740-50
runs only on a ZERO staged mask [STATIC, raw==patched], so level-0 posts — i.e. EVERY
default-boot post, R-II7 — re-arm at the drain what the inline leg suppressed).
Park class otherwise identical, zero TRIPWIRE, `[IRQ-CONSUME] 0/0/0` (the latch never
engages on a default boot) — but "unchanged-or-better" is honestly falsified.

**R-II10 disposition (the three livelock shapes): CLOSED by this milestone.** Shape A
(torn-ctx restore livelock) — root-caused fork-(iii) and fixed (Task A, passive latch
+ backstop); shape B (lost CR arm) — mechanism closed by the Q-C3 staging fix (Task B
leg 4); shape C (armed-unpolled park) — superseded: post-fix the armed post drains and
re-traps slot-4 (Task B legs 4-7). **The named remainder is a NEW frontier, not
R-II10**: the via6522 IFR presents no 60 Hz source bit (Task B leg 8) — retirement +
multi-edge live there. **Re-score #3's prediction adjudicated**: the named M3-class
surprise resolved as fork-(iii) (our patch's non-atomicity), NOT unmodeled NK
protocol — the stop-rule-1 arm never fired; seeds-not-services held (zero new service
bodies; the fix set = latch + staging stub + backstop kick).

### Task-C residue additions

- **SC#1=0x0d divergence (NEW, named)**: default+consume only, deterministic 2/2
  (slot runs 20260612-070421 / -070707), vs 0/19 without. Candidate mechanism: the
  level-0 staged pair's drain re-arm waking an early DR doorbell poll. Recon question
  for the VIA-IFR milestone's Task 0; also flip prerequisite (2) for this gate.
- **0x500eXXXX class**: 0/3 today; pooled honest rate ~4/21 (Task 0: 1/5, A: 3/7,
  B: 0/6, C: 0/3). Open, pre-existing, not consume-caused.

## Residue disposition table (M8 Task Z close-out, 2026-06-12) — one row each, the consolidated state of record

| Residue | Disposition post-M8 | Pointer |
|---|---|---|
| **R-II7** (level-source gap — default boots post level 0) | **OPEN until the SS_NW_PIC flip** (retired on the env-on cluster by B-2's staged init words; on default boots the zero-level post is the real chain's correct pre-guest-PIC-init behavior). Consumption stays test-cluster-only BY DESIGN until that flip. | M7 Task B-2 "Residue updates"; ROADMAP SS_NW_PIC follow-on row |
| **R-II8** (`[KDP+0x910]` junk queue depth) | **UNCHANGED** — load-bearing-as-is for the IACK-leg selection only; Q-C4 verdict: INERT for the consumption/restore/drain path (zero readers [STATIC]; live junk value confirmed [PROBE✓]). No seed. Re-check only if the queue area is ever initialized. | M8 Task 0 blocking-answer table (Q-C4) |
| **R-II9** (SS_PROBE_LINEAR crash suspicion) | **OPEN, downgraded** — 0/2 at HEAD under the delivery regime (instr-hardening re-test `736ae4b8`); LINEAR may be combined again. The crash CLASS recurred WITHOUT LINEAR as the **0x500eXXXX pre-engagement class** (cross-ref `7874ee6d`): pooled honest rate ~4/21 (Task 0 1/5, A 3/7, B 0/6, C 0/3) — pre-existing delivery-into-early-ROM fragility, not consume-caused; named open class, carried into the VIA-IFR milestone's flip considerations. | "R-II9 instrument-fix note" + its cross-reference paragraph |
| **R-II10** (the parked-regime DR-poll gap / the three post-delivery livelock shapes) | **CLOSED by the M8 milestone** — shape A root-caused fork-(iii) + fixed (deferred EE-edge latch, `42ce3e0e`/`09b74fe4`); shape B's lost-arm mechanism closed (Q-C3 staging, `02a0b74e`); shape C superseded (post-fix the armed post drains and re-traps slot-4). | "Slot-4 consumption Task C" R-II10 disposition |
| **SC#1=0x0d divergence** (NEW, M8 Task C) | **OPEN, named** — deterministic 2/2 on default+consume-on boots vs 0/19 without (r0=0x0d r1=1017ffde lr=5046c5ac, ahead of the canonical SC#1=0x3f); candidate mechanism = the Q-C3 stub's unconditional level-0 staging re-arming at the drain. VIA-IFR Task-0 recon question AND `SS_NW_IRQ_CONSUME` flip prerequisite (2). | "Task-C residue additions" |
| **EXT-edge one-shot flakiness** (M8 Task B) | **OPEN, named** — 3/6 env-on boots never engaged (the 60 Hz ticker's `XLM_IRQ_NEST==0` exact test never sampled true); a test-cluster determinism issue, not default-correctness. Harness work for the VIA-IFR milestone. | Task B "Residue / census"; Task C criterion (d) |
