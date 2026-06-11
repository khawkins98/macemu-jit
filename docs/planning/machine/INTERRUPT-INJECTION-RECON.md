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
