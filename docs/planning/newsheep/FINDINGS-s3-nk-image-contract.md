# NewSheep — S3 recon: NK-image CONTRACT DIFF (real NanoKernel vs SS synthetic supervisor)

**Status:** COMPLETE (2026-06-15) · READ-ONLY static recon. Validates the landed S3-impl
RESTRUCTURE (`2026-06-15-ss-m18-s3-impl-replace.md`, gate `SS_M18_NK_SUPERVISOR`) against
the real NK image, before live composition.
**Parcel:** `/tmp/newsheep/dump-9.0.1/Parcels.src/MacROM.src/NanoKernel-v02.27`
md5 `61c176e90b6365e84e5c660d703e56af`, 105280 B, base `0x50310000` → range `0x50310000..0x50329b40`.
**Method:** `[STATIC-CAPSTONE]` PPC big-endian, bounded windows + one targeted store-scan
(displacement 0x360..0x39c). No whole-parcel dump, no src/**, no builds/boots.
**Trace cross-ref:** `/tmp/nk-mmu-trace.log` NOT present at recon time — claims below are
static-only; the trace's sc/EXT events are owed as live corroboration (Track A oracle).

---

## 0. Entry / install prologue — CONFIRMS Q1 (resident, no yield)

`0x50310000 b 0x5031000c` → real prologue:
```
0x5031000c  crclr cr5eq
0x50310010  mfmsr r0
0x50310014  rlwinm. r0,r0,0,0x1b,0x1b      ; test MSR[IR/DR]
0x50310018  beql  0x503104a8              ; (translated-already path)
0x50310024  addi  r12,r3,0x40             ; resume PC
0x50310028  mfmsr r11 ; li r10,-0x7fd0 ; andc r11,r11,r10  ; new supervisor MSR
0x50310034  mtspr 0x1a,r12 (SRR0) ; mtspr 0x1b,r11 (SRR1)
0x5031003c  rfi                           ; → resident
```
**Matches `FINDINGS-s3-nk-ownership.md` Q1 byte-for-byte.** Install-then-resident; the `andc`
mask `-0x7fd0` (clears MSR bits) is the supervisor-MSR construction. No `blr`-yield exists.

---

## 1. EXCEPTION VECTOR TABLE — TWO layers; the install seam refines the KDP-table model

### 1a. The four primary handler bodies (CONFIRMED, pinned)
All four open with the universal SAVE `bl 0x50313d40` and (except SC fast-traps) close via
RESTORE `bl 0x5032391c` + `rfi`:

| Vector | Body addr | Confirmed behavior |
|---|---|---|
| DEC | `0x50313200` | re-arms own decrementer (§4) |
| PROGRAM | `0x50314700` | opens MMU window (`mtspr 0x113`) to read faulting word |
| EXT | `0x50314880` | services IRQ, dispatches `b 0x50312cb0` (§3) |
| SC | `0x50314ac0` | `-1/-2/-3` fast-traps + general dispatch (§2) |

### 1b. The SAVE prologue `0x50313d40` (the `0x50313d34` hot-PC neighbour) — CONFIRMED
```
mfspr r1,0x110 (SPRG0 save page) ; stw r6,0x18(r1) ; mfspr r6,0x111 (SPRG1) ;
lwz r6,-0x14(r1) ; stw r0,0x104(r6) ... stw r13,0x16c(r6)   ; save GPRs into KDP (r6)
mfspr r10,0x1a (SRR0) ; mfspr r11,0x1b (SRR1) ; mfcr r13 ; mfspr r12,0x112 (SPRG2/saved-LR) ; blr
```
Confirms the KDP (`r6`) save model: saved arch state lands at KDP `0x104..0x16c`; SRR0/SRR1
in r10/r11; the matching RESTORE is `0x5032391c`. **`r1` inside handlers = SPRG0 save page**,
which the prior findings call "KDP" — note the conflation below.

### 1c. The runtime install seam — NEW STATIC FINDING (partial confirm + falsifiable divergence)
Store-scan for displacements 0x360..0x39c found the install cluster at `0x50311390`:
```
0x50311390  lwz   r9,0x64c(r1)            ; r9 = relocation base (from per-CPU page +0x64c)
0x50311394  lis r8,0 ; ori r8,r8,0x4b80   ; r8 = 0x4b80
0x5031139c  add   r8,r8,r9               ; r8 = base + 0x4b80
0x503113a0  stw   r8,0x37c(r1)           ; table slot +0x37c
0x503113a4  lis r8,0 ; ori r8,r8,0x4bc0 ; add r8,r8,r9
0x503113b0  stw   r8,0x39c(r1)           ; table slot +0x39c
0x503113b4  addi  r8,r1,0x360            ; r8 = &table (per-CPU page + 0x360)
0x503113b8  mtspr 0x113,r8              ; *** SPRG3 = vector-table base ***
```
**Confirms (refines):** there IS a runtime-relocated dispatch table at `<page>+0x360`, whose
base is cached in **SPRG3 (SPR 0x113)**, with per-slot entries = `relocbase + fixed_disp`. This
is exactly why T2 must RESOLVE from the live table and must NOT hardcode (Stop-rule #9 vindicated).

**FALSIFIABLE DIVERGENCE (integration must resolve):** the only direct stores into the
0x360..0x39c table are **slots +0x37c and +0x39c**, holding `relocbase+0x4b80` and
`relocbase+0x4bc0`. With relocbase=`0x50310000` these are `0x50314b80`/`0x50314bc0` — which are
**NOT** any of the four primary handler bodies (PROGRAM 0x4700 / EXT 0x4880 / SC 0x4ac0 / DEC
0x3200). And **no direct store into +0x374 (the EXT slot the shim reads) or +0x390 (SC) was
found in the scan.** Two readings, both falsifiable at the S3-impl probe:
 - **(R1)** the `<page>+0x360` SPRG3 table is a *secondary/per-task* dispatch table (alternate
   entry points 0x4b80/0x4bc0), and the PRIMARY EXT/SC/DEC/PROGRAM vectoring is owned by the
   separate low-mem **ExceptionTable parcel** (`@0x50300000`, 49152 B, a distinct file) whose
   hardware vectors trampoline into the `0x50314xxx` bodies. → the shim's "KDP+0x374" would then
   be reading the *wrong* table.
 - **(R2)** +0x374/+0x390 are populated by a template-copy / loop (not a direct `stw`), so the
   scan missed them and the amendment's slot map (0x374=EXT, 0x37c=PROGRAM, 0x390=SC) still holds.

**Honest verdict:** the *table-base-in-SPRG3, runtime-relocated-slots* MODEL is CONFIRMED; the
exact **slot→handler assignment (0x374=EXT) the EXT-injection shim depends on is NOT statically
confirmed and is contradicted by the only direct stores found.** This stays UNPROVABLE-static
(consistent with `FINDINGS-s3-nk-ownership.md` UNPROVABLE-1) and is the load-bearing item owed
to T2's live probe (dump `[SPRG3+0x14]` i.e. table+0x374 after install). **Contract-diff claim
CD-1 (falsifiable): after the real install runs, the cell the shim reads as the EXT vector must
contain `0x50314880`; if it contains `0x50314b80`/garbage, the shim is reading the wrong table
(R1) and must re-point at the low-mem ExceptionTable trampoline instead.**

---

## 2. SC HANDLER `0x50314ac0` — CONFIRMED; richer than prior findings

```
0x50314ac0  cmpwi r0,-3 ; bne 0x50314aec
            ; -3 fast-trap: restore SRR1 from saved (rlwinm fixups), LR from SPRG2, MSR from SPRG1, rfi
0x50314aec  cmpwi r0,-1 ; bne 0x50314b10   ; -1 fast-trap (tests bit via rlwinm.), rfi
0x50314b10  cmpwi r0,-2 ; bne 0x50314b38   ; -2 fast-trap, rfi
0x50314b38  bl 0x50313d40                  ; GENERAL syscall (selector >= 0): SAVE prologue
0x50314b3c  lwz r9,0xe60(r1); addi r9,r9,1; stw   ; *** general-syscall counter ***
0x50314b48  oris r11,r11,2                  ; set SRR1 bit
0x50314b50  b   0x5031aca0                  ; → general dispatcher (toward task/CFM/68k entry)
```
**Contract diff vs SS:** SS routes PPC `sc` → `execute_syscall()` → `execute_illegal()` (no-op,
PC+=8 double-increment, `HANDOFF:299-313`). The real NK SC vector is a 3-way negative-selector
fast-trap (`-1/-2/-3`, each restoring SRR1/LR/MSR from SPRG1/2 and `rfi`-ing) PLUS a general
path (selector ≥ 0) that bumps a counter at `0xe60(r1)` and branches to the general dispatcher
`0x5031aca0`. **S3-T4 retirement (make `0x50314ac0` live, retire SS `sc`-as-illegal) is the
CORRECT contract** — the double-increment becomes moot. **CD-2 (falsifiable): under the gate,
the NK idle-task `sc` (negative selector) must hit `0x50314ac0` and `rfi` WITHOUT passing
through `execute_illegal`; if the `0xe60` general-counter increments on an idle-task `sc`, the
selector classification is wrong (the idle loop uses negative selectors, not the general path).**

---

## 3. EXT DISPATCH `0x50314880` — CONFIRMED; the shim's seam is deeper than "vector to 0x50314880"

```
0x50314880  bl 0x50313d40                  ; SAVE
0x50314884  rlwinm. r9,r11,0,0x10,0x10     ; test SRR1[EE]
0x50314888  beq 0x50313ab0                 ; (EE clear → spurious path)
0x5031488c  lwz r9,-0x338(r8) ; lwz r9,0x20(r9) ; cmpwi r9,2 ; blt 0x50314660
                                            ; reads interrupt-source block (KDP-rel -0x338), +0x20 priority
0x5031489c  bl 0x503238ac
0x503148a0  li r9,9 ; stw r9,-0x238(r8)     ; set pending/level field
0x503148ac  bl 0x503148e0                  ; *** SERVICE routine ***
0x503148b0  bl 0x5032391c                  ; RESTORE
0x503148b4  cmpwi r8,-0x725e ... -0x725d/-0x725f  ; service result codes → dispatch
0x503148d8  b  0x50312cb0                  ; task dispatch
```
Service routine `0x503148e0` reads its interrupt-source structures KDP-relative
(`-0x1c/-0x238/-0x338(r23)`, r23=SPRG0 page), checks priority masks (`0x38/0x44(r22)`), returns
a service code.

**Contract diff vs SS / the EXT-injection shim:** SS delivers device IRQs via
`SheepExcExtSetPending`/host-IRQ latch → `deliver_pending_dec_exception()` →
`ExcEnter(EXC_EXTERNAL, &g_exc_entry_table)`. The shim re-points `ExcEnter` at the NK's real EXT
vector. **But the NK EXT handler does NOT take a pending bit as a parameter — it READS its
interrupt-source state from KDP-relative memory (`-0x338(r8)`, then `+0x20`) and a priority
block (`0x38/0x44(r22)`).** So **CD-3 (load-bearing, falsifiable): vectoring to `0x50314880` is
necessary but NOT sufficient — the shim must also ensure the NK's interrupt-source block (the
structure at KDP`-0x338`, fields `+0x20` priority / `+0x38`/`+0x44`) reflects an asserted device
so the service routine `0x503148e0` doesn't classify the IRQ as spurious and bail.** This is the
M14 Cuda IFR/IER wall restated: the shim feeds the EXT *vector*, but the NK reads a *device/PIC
state block*; if that block is the forged-and-now-retired SS structure, the NK services nothing.
The shim spec in the amendment under-specifies this — it names the vector resolve, not the
source-block population. **Owed to S4 device-model wiring** (the IFR/IER → NK-source-block seam).

---

## 4. DEC HANDLER `0x50313200` — CONFIRMED (matches prior findings exactly)
```
0x50313200  bl 0x50313d40                  ; SAVE
0x50313204  lwz r8,0x5a0(r1)               ; scheduler state
0x50313228  bl 0x50324a98                  ; run-queue decision
0x50313234  mtspr 0x16,r8                  ; *** re-arm DEC (SPR22) ***  (value from -0x9d4(r1))
0x50313244  b  0x50312cb0                  ; task dispatch
0x50313270  lwz r8,0xe8c(r1); +1; stw      ; scheduler tick (dec_expiries source)
```
**Contract diff vs SS:** SS's synthetic supervisor (`deliver_pending_dec_exception`) owns the DEC
latch and re-delivers via `ExcEnter`. The NK is the live rescheduler — it re-arms its OWN
decrementer (`mtspr 0x16`@`0x50313234`, value loaded from `-0x9d4(r1)`) and runs its own
run-queue (`0x50324a98`). **S3 YIELD of `virt_clock`/`process_timers` is the CORRECT contract.**
**CD-4 (falsifiable, = G3.a): under the gate, the `0xe8c(r1)` tick must advance (dec_expiries
rising) while SS `process_timers` ticks = 0.**

---

## 5. CONTRACT-DIFF SUMMARY — does the landed S3 RESTRUCTURE match the real NK?

| Seam | S3 landed restructure | Real NK (static) | Verdict |
|---|---|---|---|
| Entry / residency | RETIRE synthetic supervisor; NK resident | `mfmsr→mtspr SRR0/SRR1→rfi`@0x5031003c, no yield | **MATCH** (CONFIRMED) |
| DEC | YIELD scheduler; NK owns DEC | self-re-arm `mtspr 0x16`@0x50313234 | **MATCH** (CONFIRMED, CD-4) |
| SC | NK SC live + retire SS sc-as-illegal | 3 fast-traps (−1/−2/−3) + general dispatch @0x50314ac0 | **MATCH** (CONFIRMED, CD-2) |
| EXT vector | resolve NK EXT vec from KDP+0x374, ExcEnter re-point | body @0x50314880 confirmed; **slot 0x374 NOT statically confirmed** (only +0x37c/+0x39c stores found, =0x4b80/0x4bc0) | **PARTIAL — CD-1 divergence; owed live probe** |
| EXT source state | shim feeds the pending flag / vector | NK READS interrupt-source block (KDP−0x338, +0x20 prio) — not a param | **GAP — CD-3; shim under-specified, owed S4 device→source-block wiring** |
| Vector-table model | hardcoded KDP+0x360 base, +0x374 EXT slot | runtime-relocated table, base cached in **SPRG3**, slots = relocbase+disp | **MODEL MATCH; slot-map UNCONFIRMED** |

**Confirmations (load-bearing, static-pinned):** entry/residency, DEC self-re-arm, SC fast-trap
structure, all four handler bodies + SAVE/RESTORE KDP model, the runtime-relocated table-in-SPRG3
mechanism (vindicates "resolve, don't hardcode").

**Divergences / owed (falsifiable for S1↔S3 integration):**
- **CD-1** — the EXT slot the shim reads (KDP+0x374) is unconfirmed; the only direct table stores
  (+0x37c/+0x39c) point at `0x50314b80`/`0x50314bc0`, not the primary handlers. Either the shim
  must read the **low-mem ExceptionTable parcel** (`@0x50300000`, separate file) for primary
  vectors, OR +0x374/+0x390 are template-copied (scan-missed). Probe: dump `[SPRG3+0x14]` post-install.
- **CD-3** — EXT delivery is two-part: vector + interrupt-SOURCE-block population. The amendment
  specifies only the vector; the NK reads `-0x338(r8)/+0x20/+0x38/+0x44`. S4 device models must
  populate that block, or the NK services nothing (M14 wall restated).
- **CD-2/CD-4** — confirmations expressed as live-boot gates.

**Net:** the REPLACE restructure's RETIRE/YIELD/KEEP decisions MATCH the real NK's ownership
model on every synchronous seam. The two open items are both on the **host→NK asynchronous EXT
seam** (exactly the seam the BINDING AMENDMENT added) — and both are SHARPER than the amendment
states: not just "resolve the EXT vector" but "resolve from the RIGHT table (CD-1)" and "populate
the SOURCE block the NK reads (CD-3)." Both are owed to S3-impl's own boot / S4 device wiring,
consistent with the milestone's GATED-MERGE-RESIDUE scope. **No re-pick of the architecture is
warranted** — REPLACE+EXT-shim stands; the shim's spec needs the CD-1/CD-3 refinement before T2's
live confirmation.


### ★ CD-1 RESOLVED = R2 CONFIRMED (2026-06-15, track-1b probe)
The shim's `KDP+0x374` slot is CORRECT (holds `0x50314880`, the NK EXT handler). Proven by the in-tree live-KDP probe (2026-06-11, `glue:114-118`): the four sibling vectors + relocbase are mutually self-consistent at the install-seam offsets, and `git log -S 0x374` confirms no host forge — the slot is written by the real NK install, the shim only reads it. R1's `+0x37c/+0x39c=0x4b80/0x4bc0` stores are a SECONDARY table, not the primary KDP. **Keep the shim as landed.** Fresh faithful-path re-dump owed-on-S1 (the NK can't write KDP@0x68ffe000 until the live BAT exists — inseparability). **CD-3 stays the open EXT-seam item (owed S4 device wiring).**
