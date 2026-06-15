> **ARCHIVED 2026-06-12** — Moved to archive during the Reku pass.
> May contain outdated assumptions, resolved questions, or superseded plans.
> Current state: `docs/HANDOFF.md` · `docs/planning/ROADMAP.md`
> **Reason:** milestone complete
>

# OS trap table recon — the InsTime/SysError-12 wall (P-M4) (2026-06-12)

> Stream-A recon for the post-tm_task-fix frontier (SLIDE-WALL-RECON.md "NEW FRONTIER
> (P-M4)", commit 2ff7765f): 9.0.1 newworld diagnostic boot parks deliberately at
> SysError 12 (dsCoreErr), 68k PC 0x500047ae `bra.b *`, D0=0x0c, D1=0xa458 (_InsXTime,
> OS trap #0x58). READ-ONLY task: no source edits; instrumentation via env knobs only.
> **Boots: 2 of the ≤3 budget** (slot protocol): `instime-recon-tbl`
> (rundir 20260612-001749.65198 — park trap-table sample + 0x560 watchpoint) and
> `instime-recon-thunk` (rundir 20260612-002548.71330 — InitTimeMgr-thunk target +
> $dd0/$dd4/$dd8). Provenance: `tools/dump-manifest.sh --check` PASSED before any
> [RAW-ROM]/[PATCH] read (raw 7b1378be…, patched e432df64…). Claims label:
> `instime-recon`. File-offset conventions: ROM 68k/NK = guest−0x50000000; DR
> staged region (0x5046xxxx/0x504xxxxx) = guest−0x50100000 (SLIDE-WALL convention;
> staged-file reads are corroborated live below, per the pack's mirror caveat).

## TL;DR — the answer table

| Q | Answer (one line) | Evidence |
|---|---|---|
| Q1 | OS trap table = **lowmem 0x400**, 256 entries × 4 bytes (Toolbox at 0xe00 × 1024); dispatcher = ROM **0x5000dfa0** via vector $28, OS leg `move.b d1,d2 ; jsr ([$400,d2.w*4])` @0xdfe8–0xdfec; the DR A-line slots (`0x50480000\|op<<3` → `lwz r3,0x720(r31); b 0x50469660/0x50469720`) have a **fast path that itself indexes the same lowmem table** (ptr cached at [r31+0x7b4], index `(op&0xff)<<2`) when vector $28 matches the cached dispatcher, else fall back to the generic 68k exception → $28 → 0x5000dfa0. Both levels read **0x400**. | [RAW-ROM] dis 0xdfa0–0xe060; [STATIC staged-file] slots 0x3d22c0/0x3b0ff8 + handler 0x369660, corroborated [PROBE✓] (park [$28]=0x5000dfa0; slot(0x61ff)=0x504b0ff8 = SLIDE-WALL's live block) |
| Q2 | Populator = ROM installer **0x5000e0c8**: copies the ROM-embedded offset table at `[ROM+0x22]` = file **0xa8ed0** (1024 Toolbox + 256 OS entries, rebased +ROMBase), NULL offsets → dsCoreErr stub **0x5000e12e**. The 9.0.1 image **genuinely ships NULL for #0x58/0x59/0x5a/0x93** (InsTime/RmvTime/PrimeTime/Microseconds — 37 OS entries NULL total). No 68k-ROM code installs #0x58 afterwards (exhaustive SetTrapAddress-site scan; watchpoint: **only two writers of 0x560 ever** — lowmem 0xffffffff fill @r24=0x500005f6, installer stub @r24=0x5000e112). On real hardware the TM is native-runtime-installed (TM names appear only in PEF loader strings; the native init world is Path-A-parked). **Our divergence is double**: (a) upstream's HLE TM replacement writes at `find_rom_trap(0xa058)` which is **0 on 9.0.1** (would clobber ROM offset 0), and (b) it never even runs — `patch_68k()` **aborts at the .Sony section** (`return false` ~line 3551: no `DRVR 4`/PCFloppy `ndrv` in the parcels ROM), silently dropping the whole EMUL_OP tail (ADBOp/InsTime/RmvTime/PrimeTime/Microseconds/PowerOff/scrap) — confirmed unapplied in the PATCH dump. | [RAW-ROM]+[PATCH] table@0xa8ed0 byte-identical; [RAW-ROM] dis 0xe0c8–0xe12c; [PROBE✓] watch 0x560 boot 1; [STATIC] rom_patches.cpp ~3546–3612 |
| Q3 | **Selectively missing, population ran**: at park, every ROM-image entry matches expectation (e.g. #0x57=0x50038a30, #0x5b=0x5000e5d8), NULL-image entries hold the stub 0x5000e12e — **except #0x5d = 0x50007990**, a one-off runtime install (`move.w #$a05d,d0; _SetOSTrapAddress` @0x7934, SwapMMUMode→`moveq #1,d0; rts`). So the installer + one-off installs all ran; the TM cluster (#0x58/59/5a, #0x93) has **no installer anywhere in the 68k ROM**. #0x58 is the first missing trap *called*. | [PROBE✓] boot 1 park fields 0x400/0x54c/0x550/0x55c/0x560/0x564/0x568/0x56c/0x574/0xe00/0xe04; [RAW-ROM] dis 0x792e–0x7992 |
| Q4 | By-design: the installer pre-fills NULL entries with stub **0x5000e12e** (`movem.l d0-d7/a0-a7,$c30 ; moveq #$c,d0 ; bra.l SysError@0x500049e6`), so any unimplemented OS trap raises **SysError 12** deliberately. Chain: sequencer `bsr.l 0x5000e520` @0x2c0 (Enable60HzInts, TM arm: `btst #6,$dd4` set → NewPtr 0x16 + task proc 0x5000bbb8 + `_InsXTime` @0xe53a, period −16626 µs = 60.15 Hz via `jmp ([$568])`) → dispatcher → stub → SysError stores code at $af0, no debugger → magic check `$db0≠0x5a932bc7` @0x50004646 → park `bra.b *` @0x500047ae. Park stack pins it: ret-addrs 0x5000e53c (word after the _InsXTime) and 0x500002c6 (sequencer) on the frame. | [RAW-ROM] dis 0xe12e/0xe520–0xe554/0x2ba–0x2c6; [PROBE✓] boot 1 [r1:0xc0] + [0xc30:0x40] (d0=0xc) |
| Q5 | **(iii) at the 68k level + (ii) compounded**: the 9.0.1 68k ROM genuinely contains no Time Manager (native-runtime responsibility, parked world); AND our boot additionally lost upstream's entire EMUL_OP suite to the patch_68k .Sony abort. Minimal CORRECT path = make the **guest's own installer** install the HLE TM: populate the ROM trap-table IMAGE (file 0xa8ed0+0x1000+4·{0x58,0x59,0x5a,0x93}) with patch-space stubs carrying upstream's exact EMUL_OP bodies (OP_INSTIME/RMVTIME/PRIMETIME/MICROSECONDS, timer.cpp host TM — the same architecture every working SheepShaver boot incl. paravirtual uses; not a bypass). Verify-zero-first (entries proven NULL [RAW-ROM]). Plus fix the patch_68k tail reachability (the .Sony `return false` swallows PowerOff/ADBOp/scrap too). | see "Recommendation" |

## Key evidence detail

### The dispatch path (Q1)

DR slot for 0xa458 = 0x504d22c0: `lwz r3,0x720(r31); b 0x50469660` (file 0x3d22c0;
toolbox traps a8xx+ branch to 0x50469720 instead). Common handler 0x50469660
[STATIC staged-file 0x369660]:

```
lwz   r5,0x28(r28) ; cmplw cr7,r3,r5 ; bne cr7,0x5046d770   ; vector $28 == cached dispatcher?
lwz   r6,0x7b4(r31)                                         ; OS trap table ptr (lowmem 0x400)
rlwinm r7,r29,31,22,29                                      ; r7 = (opcode & 0xff) << 2
lwzx  r24,r6,r7                                             ; new 68k PC := table[trap#]
... (push frame: [r31+0x724] header, trap PC at +0x1c) ... dispatch r29 = slot(new opcode)
```

So even the DR's accelerated A-line path consumes **the same lowmem 0x400 table** the
ROM installer fills — populating the image is sufficient for both dispatch levels.
The slow path (vector mismatch) raises the generic 68k exception → `jsr ([$400,d2.w*4])`
in 0x5000dfa0. RTD-based dispatch tables at lowmem **0xe00** (Toolbox, `jsr ([$e00,d2.w*4])`
variant at 0xdfc8 for ac00+) complete the picture. [RAW-ROM dis 0xdfa0–0xdff2]

### The installer and the image (Q2)

0x5000e0c8 [RAW-ROM]: `a3 := [$2ae] (ROMBase); a2 := a3 + [a3+0x22]` → file 0xa8ed0.
Loop 1: 0x400 longs → $e00 (Toolbox); loop 2: 0x100 longs → $400 (OS). Each: 0 →
stub 0x5000e12e, else +ROMBase. Vector $28 := 0x5000dfa0 (@0xe124). OS image entries
#0x50–0x67 [RAW-ROM==PATCH]: 53/58/59/5a/5d NULL; 0x93 (Microseconds) also NULL;
37 NULL total (`53 58 59 5a 5d 6b 73 85–8c 93–97 99 9b 9c 9e–a3 a8 a9 …`).

Watchpoint (boot 1, `SS_JIT_TRACE_RING=1 SS_JIT_WATCH_ADDR=560`): exactly 2 writes in
the entire boot —
```
[WATCH] pc=50490608 addr=560 value=ffffffff (was 00000000 … r24=500005f6)   ; lowmem -1 fill (0x5f0 init)
[WATCH] pc=50491608 addr=560 value=5000e12e (was ffffffff … r24=5000e112)   ; installer loop 2
```
Nothing ever populates it. [PROBE✓]

Negative results bounding "who would, on real HW" [STATIC]: exhaustive scan of the 68k
region for `_SetTrapAddress`-family sites (a047/a247/a647) found installs for a05d
(SwapMMUMode stub), aa59/aa5a/aa6b/aafa/aafb/aafe/a912 — **none for a058/a059/a05a/a093**;
no `move.w #$a458`-style immediates; no trap-number tables; 'expt' walk @0x2560 is a no-op
(ROM resource map parse: 157 resources, **zero 'expt'**); gpch 1207 patches a860 only;
the sequencer step right before Enable60HzInts (0x2ba → thunk 0xe558 → `[$dd8]+0x50`
module fn+0x08) resolves to **`moveq #0,d0; rts`** — a no-op stub (the module at
[$dd8]=0x5000e184 is the UniversalInfo block; its +0xff0 fn table = NK/PIC interrupt
services, KDP-relative). [PROBE✓ boot 2: a0=0x5000ea6c at the thunk jmp, exactly the
static prediction.] InsTime/InsXTime strings appear **only** in PEF loader
import/export string tables (ProcessMgrSupport, NativeNub, InterfaceLib, drivers) —
the implementation is native-runtime, i.e. the parcels world Path A parked.

### Enable60HzInts arms and the HWCfgFlags note (Q4 context)

0x5000e520 [RAW-ROM]: `btst #6,$dd4.w` — set → TM arm (the InsXTime raise); clear →
**VIA arm**: `movea.l $1d4.w,a0 ; move.b #$82,$1c00(a0)` (vIER enable CA1 — classic
VBL). Park probes: [$dd4]=0xc301bf26 (bit 6 set — and that long is **written by our
own univ_info patch**, rom_patches.cpp lp[0x24>>2]; but the RAW 9.0.1 table value is
0xc001bf00 — bit 6 set there too, so the TM arm is the ROM's own choice, not a patch
artifact); [$1d4]=0xf3016000 = the M2-modeled VIA base. So a bit-6 flip would route
60 Hz through our VIA model instead — recorded as alternative B below, NOT recommended
(contradicts the raw ROM's own flags).

### patch_68k tail abort (Q2 divergence, second leg)

[PATCH dump proof]: ADBOp body @0x2b3fc raw==patched, ROM offset 0 raw==patched —
the EMUL_OP section after the .Sony lookup never executed. rom_patches.cpp ~3546:
`sony_offset = find_rom_resource('DRVR',4…)`; both it and the `ndrv` PCFloppy fallback
miss on the 9.0.1 parcels ROM → `return false` (NOT lenient-guarded) → PatchROM's
tolerant caller (line ~653) continues the boot with the whole tail unapplied:
**.Sony/.Disk/.AppleCD drivers, drvr_install, SERD, ADBOp, InsTime/RmvTime/PrimeTime/
Microseconds, Egret, shutdown, PowerOff, via_int*, scrap** — a standing inventory of
further walls behind this one. Even without the abort, the TM writes would land at
`ROMBaseHost + 0` (find_rom_trap → NULL): a latent header-clobber bug of the same
patch-offset-poison class as tm_task.

## Recommendation — W2-4 / M-next task shape

**Task: "HLE TM via the guest's own trap-table image" (size S/M, one rom_patches.cpp
work item + gates + ≤2 acceptance boots):**

1. Reserve 4 small stubs in ROM patch space (alongside CHECK_LOAD_PATCH_SPACE) with
   upstream's exact EMUL_OP bodies: OP_INSTIME (+rts), sr-masked OP_RMVTIME and
   OP_PRIMETIME, OP_MICROSECONDS (+rts) — the bodies already exist at rom_patches.cpp
   3591–3611; emul_op.cpp/timer.cpp handle them on every profile today.
2. Write the **trap-table image** entries (file `[ROM+0x22]`+0x1000+4·{58,59,5a,93}) =
   stub offsets, **verify-zero-first** (entries are NULL by [RAW-ROM] proof; loud
   `[ROMPATCH]` skip on mismatch, per the tm_task guard idiom). The guest installer
   0xe0c8 then installs them natively; both DR dispatch levels consume the same table
   (Q1), and Enable60HzInts' own `jmp ([$568])` PrimeTime call works unmodified.
3. Same task or fast-follow: **fix the patch_68k .Sony abort** (lenient-guard the DRVR-4
   lookup like its siblings) so the rest of the EMUL_OP tail (PowerOff at minimum —
   clean host exit) applies on 9.0.1; audit which tail patches are wanted vs retired
   on the newworld profile (ROM-PATCH-AUDIT.md owns the verdict table).
4. Acceptance gate: boot with `--expect` no `SysError`-12 park signature / absent
   `pc=500047b0`; probe [0x560] = patch-space stub; frontier-capture boot for the
   first wall past Enable60HzInts (the 60 Hz task proc 0x5000bbb8 will now install and
   PrimeTime arms host timers — M3 delivery interplay is the thing to watch).

This is (per the house rule) not a bypass: HLE TM is the **established architecture of
every working SheepShaver boot** (paravirtual Finder boot runs exactly these EMUL_OPs);
the guest's own installer does the installing; the genuinely-absent native TM
(parcels world) remains the long-term fidelity item and is *additive* later — retiring
these stubs at that milestone mirrors the tm_task RETIRE@M3 pattern.

**Alternative B (recorded, not recommended)**: clear HWCfgFlags bit 6 in the univ_info
patch → Enable60HzInts takes the VIA arm → 60 Hz via the M2/M3 VIA CA1 model. More
"machine-layer" in flavor, but falsifies the raw ROM's own flags (0xc0 has bit 6 set)
and leaves InsTime NULL for the *next* caller — the wall merely moves.

**Stale-claim correction** (SLIDE-WALL-RECON "The 60Hz / W2-4 note" says "the missing
piece is the trap-table population (guest software level) + M3 real delivery, not ROM
patching"): population is NOT guest-reachable in this environment — the 68k ROM has no
TM and the native installer is parcels-world. The missing piece IS ROM patching (image
population) plus the patch_68k tail fix. That file is claim-owned elsewhere; fold on
next doc-sync.

## Residues (named, not load-bearing)

- **R-TT1**: which native-runtime component installs the TM on real Core99 boots
  (NativeNub vs a parcels init) — unpinned; irrelevant until the parcels world resumes.
- **R-TT2**: OS trap #0x53 (also NULL, also never installed) — identity unknown;
  it is not called before the park.
- **R-TT3**: the full consequence inventory of the patch_68k .Sony abort (drivers,
  scrap, Egret…) — step 3's audit owns it.
- **R-TT4**: PROBE68K regfile at the thunk jmp shows a4=0x5000e12e persisting from the
  installer — harmless register residue, noted for future stack/reg reading.
- **R-TT5**: [$dd0]=0x0300001c, [$ddc]=0x1fffffe4 semantics (UniversalInfo-derived
  lowmem) — uncharted bookkeeping.

## Boots used (2 of ≤3, slot protocol, default newworld diagnostic template)

1. `instime-recon-tbl` (slot0, 20260612-001749.65198): SS_JIT_WATCH_ADDR=560 +
   SS_PROBE_PC=0x504b07f0 with 15 fields (table sample, [$28], [0xc30:0x40] SysError
   reg save d0=0xc, [r1:0xc0] caller-chain stack). Park signature reproduced exactly.
2. `instime-recon-thunk` (slot0, 20260612-002548.71330): SS_PROBE_68K=0x5000e566:6
   (a0=0x5000ea6c at the thunk jmp) + SS_PROBE_PC fields [$dd0]/[$dd4]/[$dd8]/[$ddc]/
   [$1d4]/[0x560]. SS_DR_R24_RING armed but not needed for the verdicts.

No source files were touched; no rebuild performed (existing binary booted as-is,
concurrent w2-4 stream undisturbed).

---

## FIX RECORD — instime-fix (2026-06-12, Stream A)

> **Review (2026-06-12): APPROVE** — stub bodies upstream-verbatim (byte-matched against
> the deleted in-place code in the same diff); patch space 0x2fd240 collision-free (the
> ADDR_MAP memset ends exactly at 0x2fd240 exclusive; raw dump shows kckc filler); image
> offset arithmetic independently derived from find_rom_trap's OS leg; the .Sony lift
> gated on g_rom_904_lenient only and dormant on 1.1 (lookup succeeds); paravirtual
> doubly unreachable. P2 nits: the lineage citation at the patch-space site lacks the
> upstream commit SHA (standing rule — fix on the next rom_patches touch); stub bodies
> are written before the per-entry zero check (harmless, verified kckc space).
> **P1 forwarded to the P-M5 recon**: lr=0x504ff348 = mirror slot 0xfe69 =
> OP_NAME_REGISTRY / fall-through from OP_INSTALL_DRIVERS (0xfe68), NOT the TM cluster
> (0xfe60-0xfe63) — the newly-live tail EMUL_OPs (ADBOp/PowerOff/scrap) are a co-equal
> P-M5 hypothesis beside the TM-expiry leg.
>
> **P-M5 RESOLVED in recon (2026-06-12, INTERRUPT-INJECTION-RECON.md `5accbcf8`)**:
> the TM-expiry hypothesis is FALSIFIED — the crash is OP_NAME_REGISTRY →
> FindLibSymbol → Execute68k dispatching through NULL `[KDP+0x1074]/[KDP+0x1078]`
> (never staged on the trampoline boot); seeding the two words eliminates the SIGSEGV.
> Also falsified from this record: "via_int2 absent on newworld" — via_int/2/3 ALL
> apply on 9.0.1 post-tail-lift (byte-proven; via_int2's OP_IRQ sits inside the 60Hz
> task proc 0x5000bbb8).

The Q5 recommendation shipped: commits `f808a7fb` (implementation, gate default-OFF +
.Sony-abort lift + tail guards) and the flip commit (newworld-default-ON). Six slot
boots (≤6 budget) + live paravirtual `make e2e`.

**What shipped** (`SheepShaver/src/rom_patches.cpp`):

1. **.Sony abort lifted** (lenient mode only): no DRVR 4 / PCFloppy ndrv → banner-skip
   the driver replacement, resume the EMUL_OP tail. Every write site in the resumed
   tail is now verify-target-first — 14-site audit in the f808a7fb commit message
   (the banked SERD-0 hazard guard included; on 9.0.1 the tail's other find_rom_trap
   targets are real: ADBOp=0x2b3fc, PowerOff=0xe5d8, scrap a9fc/fd/fe, abf7=0xbcc0).
2. **TM cluster**: upstream's exact stub bodies in new `TIME_MANAGER_PATCH_SPACE`
   = ROM 0x2fd240 (kckc filler, checked at the site): InsTime@+0x00, RmvTime@+0x08,
   PrimeTime@+0x18, Microseconds@+0x28. Trap-table IMAGE entries at
   0xa8ed0+0x1000+4·{58,59,5a,93} populated verify-zero-first. Gate: newworld
   default-ON, `SS_NW_TM_TRAPS=0` opt-out.

**Acceptance evidence** (slot rundirs 20260612-00410?/0043??/0048??/0051??/0054??):

- Installer ran: `[WATCH] pc=50491608 addr=560 value=502fd240 (was ffffffff … r24=5000e112)`
  — lowmem [0x560] := ROMBase+0x2fd240, written by the guest's own installer loop. ✓
- Stub dispatch: `SS_PROBE_68K=0x502fd242` (the word+2 probe quirk) fired with
  d1=0xa458 (_InsXTime), d2=0x58, a1=0x5000bbb8 (the 60Hz task proc), DR slot
  r29=0x504ff300 = mirror EMUL_OP slot(0xfe60=OP_INSTIME). ✓
- **Enable60HzInts completes**: sequencer-return probe 0x500002c8 fired (68k PC
  0x500002c6) with d0=0, a1=0x502fd258 (PrimeTime stub residue) — InsXTime + the
  `jmp ([$568])` PrimeTime both executed, host TM armed. ✓
- **SysError-12 park GONE** (no pc=500047ae; baseline HOT-PC 0x50467ed4/r10=0x58 spin absent). ✓
- Gated-off A/B (`SS_NW_TM_TRAPS` unset pre-flip): baseline park signature reproduced
  exactly — exc=0/5/0/0/173/4, ALARM stall, HOT-PC 0x50467ed4 r9=50004a9e r10=00000058. ✓
- **Ticks [0x16a]: did NOT move** — watch on 0x168 saw only the lowmem init fills
  (ffffffff→0000ffff→00000000); no tick writes before the crash. Flagged loudly.

**NEW FRONTIER (P-M5)**: SIGSEGV ~0.13s into boot, milliseconds after Enable60HzInts
returns. Signature: guest pc=0x00100000 (= guest[0], the NW-trampoline 68k reset-SSP
value), ea=0x400000100000 (instruction fetch at guest 0x100000), lr=0x504ff348
(mirror EMUL_OP slot region), 68k r24=0x50510002 (odd, out-of-range), terminal
tuple exc=0/1/0/0/173/4. Mechanism hypothesis (UNPINNED — boot budget exhausted):
the first host TM expiry (~16.6ms after PrimeTime, period −16626µs) delivering
INTFLAG_TIMER → TriggerInterrupt → host-initiated 68k execution
(TimerInterrupt→Execute68k of task proc 0x5000bbb8, timer.cpp:609→631) on the
newworld profile, where Execute68k/interrupt-injection is unported — exactly the
recon's predicted "M3 delivery interplay". Note OP_IRQ (the paravirtual TimerInterrupt
caller) is unreachable on 9.0.1 (via_int2 pattern absent → skipped), so the delivery
path here is the async TriggerInterrupt leg. Next stream owns this wall.

**Rebuild-race note**: boots 4-6 picked up the concurrent dec-cadence stream's
virt_clock changes (VCLK line changed zero=5/expiries=5 → small/mid/expiries=1
across builds); the TM crash signature is IDENTICAL across both builds, so the
verdicts are build-independent.
