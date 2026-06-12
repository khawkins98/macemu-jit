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
