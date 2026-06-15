# NewSheep — S3 recon: the NanoKernel's supervisor-ownership (primary evidence)

**Status:** COMPLETE (2026-06-15) · READ-ONLY static recon feeding S3 Task-0 (two-supervisor reconciliation).
**Parcel:** `/tmp/newsheep/dump-9.0.1/Parcels.src/MacROM.src/NanoKernel-v02.27`
md5 `61c176e90b6365e84e5c660d703e56af` (re-verified), 105280 B, base `0x50310000`
→ parcel guest range **`0x50310000 .. 0x50329b40`**.
**Method:** `[STATIC-CAPSTONE]` — capstone PPC big-endian, bounded windows (no whole-parcel disasm,
no `src/**`, no builds/boots). EmulatorCode (`0x50360000`) and the DR pair (`0x5046/0x5048xxxx`) are in
**separate parcels**, NOT this file — see Q-S3.1.

> **★ Headline for the architecture decision:** co-residency is statically IMPOSSIBLE. The real NK is the
> SOLE, RESIDENT owner of DEC + the exception vectors + the 68k-dispatch path, and NEVER `blr`-yields to a
> caller. → S3's CO-OWN candidate is scored OUT; yield-fully fails Q1's no-yield-point; the choice is
> **REPLACE vs thin-shim (SS-as-hardware / NK-as-OS)**.

**Conventions observed (the NK's supervisor register model):**
- `SPR 0x110` (SPRG0) = exception **save-scratch page** base (`r1` inside handlers).
- `SPR 0x111`/`0x112` (SPRG1/2) = saved-state scratch (saved GPR / saved LR).
- `r6` = **KDP** base — all per-task/scheduler/saved-context fields are `r6`-relative.
- `r1` in the DEC scheduler body = a large per-CPU/KDP structure base (`0x5a0`, `0xe8c`, `-0xb50`).
- `SPR 0x1a`/`0x1b` = SRR0/SRR1; `SPR 0x16` = **DEC**; `SPR 0x113` = DBAT/SDR-class.

---

## Q1 — "no handoff boundary": the NK is install-then-resident [STATIC-CAPSTONE]

NK entry `0x50310000`:
```
0x50310010  mfmsr r0
0x50310014  rlwinm. r0, r0, 0, 0x1b, 0x1b   ; test MSR translation bit
0x50310024  addi  r12, r3, 0x40             ; r12 = resume PC
0x5031002c  mfmsr r11 ; andc r11,r11,r10    ; new MSR (translated supervisor)
0x50310034  mtspr 0x1a, r12                 ; SRR0 = resume PC
0x50310038  mtspr 0x1b, r11                 ; SRR1 = new MSR
0x5031003c  rfi                             ; → run resident
```
Exactly the Q1 install: `mfmsr → mtspr SRR0/SRR1 → rfi` into translated supervisor mode. **After `rfi`
the NK runs resident.** Every other entry is an exception handler the hardware re-enters; each ends by
returning via `rfi`, never `blr` to a host caller. **No observed yield point** — strictly
resident-and-owns-everything. SS cannot co-own by being "called into": the NK never calls out.

---

## Q-S3.2 — the NK owns DEC + the exception vectors

### (a) DEC reschedule loop @`0x50313200` — NK owns the timer/scheduling regime
```
0x50313200  bl    0x50313d40            ; universal exception SAVE prologue
0x50313204  lwz   r8, 0x5a0(r1)         ; scheduler state
0x50313228  bl    0x50324a98            ; scheduler/run-queue decision
0x50313234  mtspr 0x16, r8              ; *** re-arm DECREMENTER (SPR22=DEC) ***
0x50313238  lwz   r16,0x184(r6) ...     ; restore task GPRs from KDP (r6-relative)
0x50313264  bl    0x50312700            ; task dispatch
0x50313270  lwz   r8, 0xe8c(r1); +1; stw ; bump scheduler tick counter (dec_expiries source)
0x503132a0  bl    0x5032391c            ; RESTORE epilogue
```
**The NK owns DEC**: the DEC body re-arms the decrementer itself (`mtspr 0x16`@`0x50313234`), reads/writes
its own scheduler structures (KDP `r6` + per-CPU `r1`), makes the run-queue decision, dispatches tasks.
The `0xe8c(r1)` increment is the in-kernel tick that surfaces as the `dec_expiries` heartbeat. Timer +
scheduling live entirely inside the resident NK; nothing is delegated outward.

### (b) The exception handler bodies are the NK's live handlers
All four open with the universal SAVE (`bl 0x50313d40`) and close through RESTORE (`bl 0x5032391c`) + `rfi`:
- **DEC `0x50313200`** — above.
- **PROGRAM `0x50314700`** — restores CR, classifies the program sub-type, opens an MMU/translation
  window (`mfspr/mtspr 0x113`) to read the faulting word. Pure supervisor work.
- **EXT `0x50314880`** — reads interrupt/task state (`lwz -0x338(r8)…`), services the interrupt
  (`bl 0x503148e0`), sets a pending flag in the SPRG0 page, dispatches (`b 0x50312cb0`).
- **SC `0x50314ac0`** — the NK's own fast-trap vector (negative selectors `-1/-2/-3`): `cmpwi r0,-3 …`
  restores SRR1/LR from SPRG1/2 and returns via `rfi`. Does NOT forward to SS.

### (c) The universal SAVE/RESTORE + the KDP vector model
Shared prologue `0x50313d40` (the `0x50313d34` hot-PC the heartbeat samples): `mfspr r1,0x110` (save
page) → stash KDP → save GPRs into KDP (`r6`-relative `0x104..0x16c`) → read SRR0/SRR1/saved-LR → `blr`.
`0x5032391c` is the matching RESTORE. The NK keeps all saved architectural state in the **KDP (`r6`)**;
the hardware low-mem vectors (in the separate ExceptionTable parcel @`0x50300000`) trampoline into these
`0x50314xxx` bodies; the live dispatch addresses are installed into the **KDP+0x360 vector table** at boot
(runtime — see UNPROVABLE). **The NK owns the exception vectors.**

---

## Q-S3.1 — how the NK reaches 68k (the load-bearing one) [STATIC-CAPSTONE]

**Primary evidence (negative-space, decisive on the "own path" half):**
- EmulatorCode + the DR pair are in **separate parcels** (Configfile: `EmulatorCode @BASE+0x360000`), not
  in NanoKernel-v02.27. Full-parcel word scan: **zero 4-aligned references** to `0x50360000`/`0x50460000`/
  `0x50480000`, and **no `lis` immediate** loading hi-half `0x5036/0x5046/0x5048` (the two stray `0x5048`
  halfword hits are mid-instruction, not word-aligned `addis` immediates).
- → the NK does NOT branch to the 68k emulator by a hardcoded address; it loads an entry pointer from a
  **runtime structure** (the ECB / KDP-relative dispatch slots populated at boot) and dispatches via the
  task-resume path (`b 0x50312cb0`/`bl 0x50312700`). The 68k world is just another task the resident NK
  schedules and `rfi`s into.
- **SS's `execute_68k` (`sheepshaver_glue.cpp:1461`) is orthogonal and NOT on this path.** The NK never
  `blr`-returns to a host caller (Q1) and never references SS host code. SS's `execute_68k` is the
  forge-era host-side shortcut; the real NK's 68k entry is its own ECB-driven task dispatch.

**Architecture consequence:** the real NK is the SOLE owner of the 68k-dispatch path once resident. SS's
`execute_68k` and the NK's ECB dispatch are two different doors into the same EmulatorCode parcel; they
must not both be live, or they fight over the 68k task context (KDP vs SS host state).

---

## UNPROVABLE-statically (owed to a probe boot at S3-impl)

1. **The KDP+0x360 vector-table population** — the handler bodies are proven static; the vector→body table
   lives in runtime KDP (no static address cluster in the parcel). Owed: a probe dumping `[KDP+0x360]`
   after install to confirm the four entries → `0x50313200/0x50314700/0x50314880/0x50314ac0`.
2. **The exact ECB/dispatch slot the NK loads to enter 68k** — negative-space proves it's not a hardcoded
   branch; the precise KDP/ECB offset + entry pointer are runtime — owed to a probe at the DR first
   dispatch reading the resume pointer.
3. **Whether SS `execute_68k` is reachable at all under the real-NK regime** — requires the S3-impl boot
   with the real NK resident to confirm SS's host path is provably dead (orthogonal), not merely unused.

---

## Biggest open question for the architecture choice

Since the real NK is sole, resident owner of DEC + the exception vectors + the 68k-dispatch path — and
never calls out — S3 cannot make SS and the NK co-own the supervisor. The load-bearing decision is
**which supervisor is resident**: either SS's host-side machinery (`execute_68k`, host IRQ/timer) is fully
retired in favor of running the real NK, or the NK is reduced to a produced-data artifact. Static evidence
says co-residency is impossible; the impl-probe owed is confirming SS's host path is provably dead under a
resident real NK.
