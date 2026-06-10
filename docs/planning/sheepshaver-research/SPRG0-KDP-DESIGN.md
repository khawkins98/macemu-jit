# New World NanoKernel boot-time supervisor environment — DESIGN for SheepShaver

> **⚠️ Path A research artifact (2026-06-10).** This design was for the manual NW trampoline
> approach. The **[Machine Layer](../MACHINE-LAYER-PLAN.md)** now handles supervisor state
> setup through the machine-profile module. The KDP field analysis and nanokernel boot
> sequence documentation remain useful reference for the Machine Layer's MMU work.

Scope: exactly what host-side state SheepShaver must construct before/at entry to the
parcels (Mac OS ROM 9.0.1) nanokernel so the first spinlock-acquire stops deadlocking.

All ROM offsets are decoded-image offsets; guest PC = 0x50000000 + offset.
NanoKernel citations are elliotnunn's reverse-engineered source in /tmp/NanoKernel/.

--------------------------------------------------------------------------------
0. CORRECTION TO THE PREMISE (load-bearing)
--------------------------------------------------------------------------------
0x3263e0 is NOT "the first nanokernel spinlock." It is the parcels ROM's
**serial/console debug-print helper** — called from ~250 sites across 0x310000-0x328000
(verified by branch scan), with the classic byte loop + \n/\r/\\b escape handling and a
DR-toggled `stb r24,6(r28)` byte write. It uses SPRG0 as a global per-CPU pointer purely
incidentally (every nanokernel routine does).

The ACTUAL deadlock is the first spinlock-acquire at **0x312700** (block 12 of boot, per
the existing SS_LOG_FIRST_BLOCKS trace). 0x3263e0 is reached only as part of the panic/log
path AFTER acquisition goes wrong. Fixing SPRG0 satisfies BOTH (same root cause). Disasm of
0x312700:

    0x312700  lwarx  r9,0,r8        ; try-acquire lock at [r8]
    0x312704  cmpwi  r9,0
    0x312708  mfspr  r9,SPRG0        ; <-- SPRG0
    0x31270c  bne    0x312730        ; lock held -> contended path
    0x312710  lwz    r9,-0x340(r9)   ; lock token = [SPRG0-0x340]   <-- NEGATIVE index
    0x312718  stwcx. r9,0,r8
    ...
    0x312730  stmw   r22,-0x94(r9)   ; save regs below SPRG0
    0x312740  lwz    r29,-0x340(r22)
    ...
    0x3127a8  lwz    r30,-4(r22)     ; r30 = [SPRG0-4]  (secondary ptr)
    0x3127ac  lwz    r28,-0xb30(r30) ; flag = [secondary-0xb30]

With SPRG0=0: `lwz r9,-0x340(0)` reads garbage, the lock token is wrong, MSR[EE]=0 so no
other context runs to release -> self-deadlock (the documented "Recursive spinlock").

TWO BASES, not one (corrected after caller trace):
  * SPRG0            -> lock TOKEN at [SPRG0-0x340]
  * r1 = [SPRG0-4]   -> a SECOND structure (the elliotnunn-style KDP). The lock WORD is at
                        [r1-0xb50]; callers do `addi r8, r1, -0xb50` then `bl 0x312700`.
Caller disasm (e.g. 0x312f90, 0x313004, 0x313884) shows the pattern:
    lwz   r1, -4(r1)         ; r1 = [SPRG0-4]  (the KDP)
    addi  r8, r1, -0xb50     ; r8 = lock word address
    bl    0x312700
And 0x313884 then does `lwz r1,-4(r1); lwz r27, 0x694(r1)` — a POSITIVE read at +0x694 =
exactly elliotnunn KDP.HtabLastEA (Defines.s:414). => [SPRG0-4] IS the elliotnunn-style KDP,
read BOTH negatively (-0xb50) AND positively (+0x694). This VALIDATES elliotnunn's positive
layout for the secondary pointer (it is byte-compatible there), and shows the per-CPU/SPRG0
block is a thin wrapper whose [-4] points to that KDP.

ON THE r31=0x68ffdcc0 DIAG (do not over-read it): inside 0x312700, `mr r31, r8` (0x31273c),
so r31 = the caller-passed lock-word address, NOT an SPRG0-derived value. In that diag SPRG0
was 0 (r22=0 after `mr r22,r9; mfspr r9,SPRG0`). r31=0x68ffdcc0 therefore proves the CALLER
computed the lock address from a 0x68ffe000 base — i.e. SheepShaver's patched
`r1 = XLM_KERNEL_DATA = KernelDataAddr` (rom_patches.cpp:712), reached via [SPRG0-4]. It is
CONSISTENT WITH (not proof of) SPRG0 = 0x68ffe000; but combined with the nanokernel's own
SPRG0==KDP-base invariant (Init.s `mtsprg 0,r1`; mfsprg r1,0 everywhere) and the patch
already seeding r1=KernelDataAddr, SPRG0 = KernelDataAddr = 0x68ffe000 is the correct target.

--------------------------------------------------------------------------------
1. WHAT SPRG0 HOLDS AT NANOKERNEL ENTRY
--------------------------------------------------------------------------------
SPRG0 = the per-CPU / Kernel-Data-Page base pointer. In elliotnunn's source this is set in
Init.s ("InitKernelMemory"):

    subi    r1, r11, 0x2000      ; KDP = HTAB - 0x2000
    mtsprg  0, r1                ; SPRG0 = KDP base = r1

and every interrupt/kernel routine reloads it with `mfsprg r1,0` (Exceptions.s:199,
ExternalInts.s:63/155/..., Floats.s:4, Crash.s:2). So r1 == SPRG0 == KDP base is the
universal nanokernel invariant.

THE CONVENTION DIFFERENCE THAT MATTERS: elliotnunn's KDP (Defines.s:303, `KDP RECORD 0`) is
addressed entirely with POSITIVE offsets (base=0): EWA r0-r31 at 0x00-0x7f, ConfigInfoPtr at
0x630, CodeBase 0x64c, PageMapPtr 0x684, CrashFPSCR 0x900, PageMap at 0x920, low-mem info
ptrs at 0xFC0. The real 9.0.1 parcels ROM instead indexes the SPRG0 pointer with NEGATIVE
offsets (-0x340, -0x900, -0xb30, -4) plus a second pointer at [SPRG0-4]. So:

  * In elliotnunn (v2.x branch as built), SPRG0 points to the BASE (offset 0) of a single
    KDP, all-positive.
  * In the shipped 9.0.1 image, SPRG0 points into the MIDDLE/TOP of a per-CPU block; the
    block's own fields are below it (negative), and [SPRG0-4] is a back-pointer to a SECOND
    structure (also negative-indexed: [ptr-0xb30]).

=> elliotnunn is a reliable STRUCTURAL map (two-level: per-CPU block + KDP, fields like
locks/flags/save-area), but it is NOT byte-exact for 9.0.1's offsets. Treat the named
offsets below as 9.0.1-specific (ROM-derived), not elliotnunn-derived.

Observed/required at [SPRG0 ± n] for 9.0.1 (SPRG0 = 0x68ffe000):
  [SPRG0-4]      = 0x68ffdffc : back-pointer to the secondary structure ("KDP'/EDP'")
  [SPRG0-0x94]   = 0x68ffdf6c : start of a register save area (stmw r22 in contended path)
  [SPRG0-0x98]   = 0x68ffdf68 : saved LR slot
  [SPRG0-0x108]  = 0x68ffdef8 : start of the r24-r31 save area used by the debug helper
  [SPRG0-0x10c]  = 0x68ffdef4 : saved CR (debug helper)
  [SPRG0-0x110]  = 0x68ffdef0 : saved LR (debug helper)
  [SPRG0-0x340]  = 0x68ffdcc0 : the SPINLOCK TOKEN value (what makes/breaks the deadlock)

--------------------------------------------------------------------------------
2. THE KDP LAYOUT — POSITIVE AND NEGATIVE
--------------------------------------------------------------------------------
Confirmed: the parcels nanokernel indexes its SPRG0 base BOTH ways. The reads we can see
are all negative (the boot/lock/log paths sit below the base). elliotnunn's positive layout
(ConfigInfoPtr@0x630, CodeBase@0x64c, PageMapPtr@0x684, low-mem ptrs@0xFC0) is the part the
nanokernel reads once it is running normally; the boot/lock machinery uses the negative
region, which in elliotnunn does not exist (their EWA/save-area is positive at 0x00-0x7f).

Conclusion on "base-pointer convention": elliotnunn sets SPRG0 = KDP base = HTAB-0x2000 and
reads positively. 9.0.1 sets SPRG0 = a per-CPU block top and reads negatively, with the
elliotnunn-style KDP reachable via [SPRG0-4]. The two are the same idea at different points
in the structure; do not assume the byte offsets transfer.

--------------------------------------------------------------------------------
3. WHAT [KDP-0x900] / the early negative reads LIKELY ARE
--------------------------------------------------------------------------------
- [SPRG0-0x340] : the spinlock token / "lock value to store" for the global kernel lock.
  This is the field that decides the deadlock; in a healthy boot it is a small constant
  (CPU id / nonzero owner tag) read then `stwcx.`-stored into the lock word at [r8].
- [SPRG0-0x900] (read by the debug helper at 0x3263fc) : best-supported hypothesis is a
  pointer/handle to the serial/console output device or its register block (the helper then
  writes the byte to [r28+6] = device data port). It is NOT a lock; it is consumed only by
  the print path. Cannot name it from elliotnunn (their console path differs); test by
  watching what value it must hold for the helper to not fault.
- [SPRG0-4] -> [..-0xb30] : a boolean/flag that, when nonzero, makes the spin loop recompute
  the decrementer deadline every iteration (disables the lock-timeout). This is why
  SS_SYNTH_DEC could not break the wedge. Likely a "lock-timeout-disabled" / debug flag in
  the secondary structure.

None of these three is namable byte-exactly from /tmp/NanoKernel; they are 9.0.1-specific.
The empirical method (section 5) surfaces each in turn.

--------------------------------------------------------------------------------
4. FULL LIST OF TRAMPOLINE-ESTABLISHED STATE THE NANOKERNEL ASSUMES ON ENTRY
--------------------------------------------------------------------------------
From Init.s entry contract + Reset.s reads + the ROM:

GPRs at entry (Init.s header):
  r3 = NKConfigurationInfo ptr   (SheepShaver: ROMBase+0x30d000  — already set)
  r4 = NKProcessorInfo ptr
  r5 = NKSystemInfo ptr
  r6 = NKDiagInfo ptr
  r7 = 'RTAS' if present (else anything)
  r8 = RTAS proc if present
  r9 = NKHWInfo ptr
  (SheepShaver's patched 0x310000 path substitutes its own r1/r13/r14/r15 and never runs the
   real Init.s record-copy, so r4-r9 are effectively unused under the patch model.)

SPRGs:
  SPRG0 = per-CPU/KDP base               <-- THE MISSING PIECE (Trampoline-set on real HW)
  SPRG1/2 = exception scratch (Crash.s mfsprg 1/2) — not read before the wall
  SPRG3 = user-space vector table ptr (Reset.s `mtsprg 3`) — SheepShaver NOPs the ROM's
          SPRG3 setup, fine as long as no exception is actually dispatched pre-handoff

MSR: real HW enters with MSR set up; nanokernel recomputes user MSR itself (Init.s:
  MsrEE|MsrPR|MsrME|MsrIR|MsrDR|MsrRI). The deadlock is WITH MSR[EE]=0 (interrupts masked) —
  that is the nanokernel's own pre-handoff state, not a SheepShaver bug.

SDR1/HTAB: real Trampoline builds a real HTAB and sets SDR1 = HTABORG|HTABMASK; KDP is then
  placed at HTAB-0x2000 (Init.s) — i.e. KDP is DERIVED from the real HTAB. SheepShaver fakes
  SDR1=0xdead001f, HTAB ptr=0xdead0000 (rom_patches.cpp) and uses a FIXED KernelDataAddr that
  is unrelated to the fake HTAB. Reset.s ("ResetHTAB") reads SDR1 and zeroes the last PTEG of
  the HTAB at HTABORG — under flat addressing the aarch64 JIT treats these as no-ops/dropped,
  so the inconsistency is latent until something dereferences HTABORG for real.

BATs: nanokernel loads BATs from ConfigInfo (Reset.s "ResetBatRanges"); JIT no-ops them
  (flat addressing). Latent.

--------------------------------------------------------------------------------
5. DESIGN FOR SHEEPSHAVER (minimal host-side construction)
--------------------------------------------------------------------------------
Goal: make SPRG0 = 0x68ffe000 (= KernelDataAddr = KERNEL_DATA_BASE) at nanokernel entry, and
guarantee the negative region [SPRG0-0xc00 .. SPRG0) is real, zero-initialized, writable RAM.

5a. PLUMBING — let set_register reach SPRG0.
  ppc-cpu.cpp set_register() currently abort()s on anything that is not GPR/FPR/CR/FPSCR/
  XER/LR/CTR/PC/SP (lines 718-739). SPRG storage exists: ppc-registers.hpp:254 `uint32
  sprg[4]`. Add a case (and the symmetric get_register case):
      case powerpc_registers::SPRG0: regs().sprg[0] = value.i; break;   // + SPRG1..3
  (or expose a direct `ppc_cpu->regs().sprg[0] = ...` from glue). Small, general, safe.

5b. SPRG0 INJECTION — in init_emul_ppc (sheepshaver_glue.cpp ~1286, right after GPR3/GPR4):
      ppc_cpu->set_register(powerpc_registers::SPRG0,
                            any_register((uint32)KernelDataAddr));   // 0x68ffe000

5c. BACK-POINTER at [SPRG0-4] (= the KDP). The ROM reads [SPRG0-4] then indexes it both
  ways: lock word at [KDP-0xb50], and positive fields like [KDP+0x694]. First probe: point it
  at a backed, zeroed KDP region (see 5d/5e). Write at guest [0x68ffdffc]:
      WriteMacInt32(KernelDataAddr - 4, <KDP guest addr>);
  NOTE (corrected): the [KDP-0xb30] "timeout-disable" flag and the spin loop at 0x3127a0 are
  NOT on the success path and do NOT need tuning — see 5d. They only matter if contention is
  reached, and on that loop flag=0 leads to `bgt 0x312818` = the timeout PANIC, so neither
  flag value is "safe." The goal is to never enter contention at all (5d).

5d. THE REAL FIX — the lock WORD must be zeroed, backed RAM.  *** Primary catch. ***
  Trace the acquire: `lwarx r9,0,r8` reads the lock word [r8]=[KDP-0xb50]; `cmpwi r9,0`;
  `bne 0x312730` (contended) — so if [KDP-0xb50] reads **0**, the first acquire SUCCEEDS
  (stwcx. at 0x312718, blr at 0x31272c) and the contended spin loop is NEVER entered. The
  diag wedged precisely because [KDP-0xb50] (≈0x68ffdcc0) was UNMAPPED -> read nonzero ->
  looked "held" -> contended spin -> self-deadlock. So zeroing that word is the fix; the
  back-pointer and timeout flag are secondary.
  This requires the NEGATIVE region below the KDP/SPRG0 base to be backed RAM.
  KERNEL_DATA_BASE=0x68ffe000 is its own shmget region of only KERNEL_AREA_SIZE=0x2000
  (kernel_data_init / shm_map_address in main_unix.cpp), mapped at 0x68ffe000 and 0x5fffe000.
  The struct KernelData is positive-only (uint32 v[0x400] = 0x1000, then EmulatorData ed at
  +0x1000). The bytes BELOW 0x68ffe000 (0x68ffd4d0..0x68ffe000, ~0xb30) are NOT part of that
  mapping and are NOT guaranteed backed (Mac RAM is RAMBase=0..RAMSize; 0x68ffe000 ~1.76 GB
  is the dedicated kernel area, separate from RAM). So the parcels nanokernel's negative
  indexing writes/reads UNMAPPED memory -> fault.
  FIX OPTIONS (pick the smallest that boots):
    A. Enlarge the kernel-data mapping DOWNWARD: in kernel_data_init, map a region that also
       covers at least 0x1000 below KERNEL_DATA_BASE (e.g. base the shmat at
       (KERNEL_DATA_BASE-0x1000) with size 0x3000), and set SPRG0 = KERNEL_DATA_BASE so the
       negative offsets land in the new low page. Zero it. Minimal, contained.
    B. Place SPRG0 ABOVE the bottom of the existing area instead: set SPRG0 =
       KERNEL_DATA_BASE + 0xc00 (so [SPRG0-0x340], [SPRG0-0x900], [SPRG0-0xb30] all fall
       INSIDE the existing 0x68ffe000..0x68fffffe mapping). This needs NO new mapping — but
       it then COLLIDES with SheepShaver's 1.1-tuned positive KernelData usage (see 5e), and
       breaks the r31=0x68ffdcc0 arithmetic the ROM already computed, so it is the riskier
       choice. Prefer A.

5e. CONFLICT WITH THE 1.1-TUNED KernelData — IMPORTANT.
  SheepShaver's `struct KernelData` (cpu_emulation.h:47) is the 1.1-ROM layout: positive
  offsets only, 0x1000 bytes, with BAT slots at 0x300-0x324 written by the SR/BAT-load
  routine, plus fields main.cpp seeds. The parcels nanokernel's POSITIVE reads (ConfigInfoPtr
  @0x630, CodeBase@0x64c, PageMapPtr@0x684, low-mem ptrs@0xFC0 per elliotnunn) would land in
  the SAME 0x68ffe000 page and likely DO NOT match SheepShaver's 1.1 field placement. Two
  consequences:
    - The existing KernelData CANNOT simply BE the parcels KDP for the positive fields — the
      layouts differ. But for the NEGATIVE boot/lock region (which 1.1 never touches) there
      is no conflict; that region is free.
    - Therefore: keep SPRG0 = KernelDataAddr (so negatives are free scratch), give the
      negative region its own backing (option A), and DO NOT assume the positive parcels KDP
      fields are satisfied — they are the NEXT wall, surfaced by the forcing-function once the
      spinlock clears.
  Recommendation: a SEPARATE, parcels-only KDP region is the clean long-term answer, but it
  is NOT needed to clear 0x312700. For the first probe, reuse KernelDataAddr as SPRG0 with a
  downward-extended mapping; treat positive-field fidelity as a later iteration.

5f. SUMMARY OF THE MINIMAL PATCH (first probe):
  1. set_register/get_register: add SPRG0..3 cases (ppc-cpu.cpp).
  2. kernel_data_init: extend the kernel-data mapping to also cover >=0x1000 below
     KERNEL_DATA_BASE; zero it.
  3. init_emul_ppc: SPRG0 = KernelDataAddr; WriteMacInt32(KernelDataAddr-4, <KDP guest addr>)
     so [SPRG0-4] is a backed KDP whose [KDP-0xb50] lock word reads 0 (the primary fix).
  4. Boot with SS_LOG_FIRST_BLOCKS + SS_JIT_TRACE_RING; expect the 0x312700 self-deadlock to
     clear (acquire succeeds, blr at 0x31272c) and the boot to ADVANCE to the next unmet
     dependency (next negative/positive KDP read, then the jump68k handoff — the known later
     frontier).

--------------------------------------------------------------------------------
6. RISKS / UNKNOWNS / ORDER OF ATTACK
--------------------------------------------------------------------------------
RISKS:
- The deeper MMU wall: nanokernel derives KDP from a REAL HTAB (Init.s) and PrimeHTAB/PutPTE
  the InterruptCtl/KernelData/EmulatorData logical areas (Reset.s "PrimeHTAB"). SheepShaver's
  fake SDR1=0xdead001f means any code that actually walks/zeroes the HTAB at HTABORG
  (Reset.s "ResetHTAB" zeroes last PTEG at [SDR1&~0xffff]) will touch 0xdead0000 ->
  fault. The aarch64 JIT drops mtsr/mtbat and (per rom_patches notes) treats these as
  no-ops; this holds only as long as nothing DEREFERENCES the fake HTAB. The spinlock fix
  does not address this; it is the documented "second wall" (cross-ref MMU-NANOKERNEL-MP-PLAN
  and NEW-WORLD-ROM-SUPPORT-PLAN "second wall" section).
- Positive parcels-KDP field mismatch vs the 1.1 KernelData (5e) — expect new faults once the
  lock clears; harvest each as a correctness item.
- jump68k handoff is still un-redirected (NEW-WORLD-ROM-SUPPORT-PLAN frontier #3) — even a
  clean spinlock will eventually reach it.

RECOMMENDED ORDER (smallest probe first):
  P0. Add SPRG0..3 to set_register/get_register (general correctness; test-jit must stay 100).
  P1. SPRG0 = KernelDataAddr only (no mapping change yet). Boot, watch the FIRST faulting
      negative address. If it faults below 0x68ffe000, that confirms 5d.
  P2. Extend kernel-data mapping downward + zero (so [KDP-0xb50] reads 0); set [SPRG0-4] to a
      backed KDP; re-boot. Expect the FIRST acquire at 0x312700 to succeed (no contention).
  P3. (Only if contention still occurs) inspect the spin loop at 0x3127a0; remember flag=0
      there leads to the timeout PANIC, so the fix is still "make the first acquire succeed,"
      not flag-tuning.
  P4. Follow the forcing-function: each new KDP field the nanokernel reads is the next item.
  P5. Only then take on jump68k + the HTAB/SDR1 MMU consistency (the big build-out).

EMPIRICAL TEST HOOKS: SS_LOG_FIRST_BLOCKS (block-by-block boot trace), SS_JIT_TRACE_RING +
ppc_jit_dump_trace_ring (the 5-PC spin signature disappears when fixed), SS_JIT_VERIFY (rule
out codegen divergence — was already clean), SS_JIT_WATCH_ADDR on 0x68ffdcc0 (the lock token)
to watch the acquire succeed.
