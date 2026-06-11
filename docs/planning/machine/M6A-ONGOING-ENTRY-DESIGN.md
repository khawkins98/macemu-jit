# M6a Wave 2 rung 2 — ongoing-entry design: what the entry-vector table really is, why Boot B actually died, and the smallest honest re-entry contract

> **Status:** 📋 Static-RE + design recon memo (READ-ONLY session — no builds, no boots) ·
> **Created:** 2026-06-11 · **Branch:** `macos-arm64`
> **Inputs:** `M6A-DR-HANDOFF-ANALYSIS.md` (incl. Wave 1 results), `M3A-ENTRY-TABLE.md`,
> raw 9.0.1 decompressed parcels image `/tmp/rom901.bin` / `/tmp/rom901_inventory.bin`
> (UNPATCHED), post-PatchROM dump `/tmp/rom901_decompressed.bin` (PATCHED — capstone PPC BE,
> base `0x50000000`), `rom_patches.cpp` (`patch_68k_emul` :1420ff, `PatchROM_NW_trampoline`
> :714ff), `sheepshaver_glue.cpp` (`interrupt()` :654ff, `deliver_pending_dec_exception()`
> :767ff, `[NW-TRAMP]` :1737–2042), `main_unix.cpp` `HandleInterrupt` :2146ff,
> **Boot B crash log `/tmp/m6a-bootB.log` (full register dump — re-read this session)**.
> **Caveat:** facts are tagged **[RAW-ROM]** (verified against the unpatched image),
> **[PATCH]** (read from our patch source), **[CRASH]** (Boot B register dump), or
> **[STATIC]** (disassembly inference). Open items are tagged **[PROBE-On]**.

---

## 0. TL;DR — three corrections and a verdict

1. **The "real ROM entry table" does not exist.** The raw 9.0.1 ROM has `twi 31,r31,idx`
   *placeholders* at static `0x36e8c0` and **nops** at `0x36f900` **[RAW-ROM]**. The
   `b +0x1040` table[0], the "ongoing entry" at `0x36f900`, and all its siblings are
   **SheepShaver's own paravirtual stubs**, written by `patch_68k_emul()` **[PATCH]**.
   There is no Apple ongoing-entry code to restore — table[0]'s "original" `b +0x1040`
   that `PatchROM_NW_trampoline` displaced was already *ours*. Rung 2 is therefore a
   **design choice**, not an RE-recovery task.

2. **Boot B did not die in interrupt delivery.** The crash log shows
   `delivered_dec=0 deferred_ee=0 deferred_depth=0` **[CRASH]** — no DEC was ever
   delivered. The SIGSEGV is at trampoline `pc=0x50429b40`, `ea=0x4000fffff69c`
   = guest `0 − 0x964`: the SS_M6A_USER_MSR instruction `lwz r0,-0x964(r1)` executed
   **with r1=0**. The trampoline assumed an NK-live entry (r1=KDP); the actual
   architectural entry into table[0] arrives with a **context-restored register file**
   (r1=0, LR=0, r24=`0x5000002a`, r29=`0x50480000`, CTR=`0x5046e8c0`=table[0],
   everything else zero) **[CRASH]**. The handoff memo's framing ("first DEC delivery
   dies at the trampoline") was the red-team *prediction*; the telemetry refutes it.
   **The Boot B blocker is a 2-instruction fix** (r1-independent MSR load), not the
   re-entry contract.

3. **The NK's interrupt save/restore protocol is already resume-correct with the M3a
   shim.** Disassembly of the live handler (static `0x312b0c`+) shows it saves the full
   file through r6 (r10→ctx+0xfc, r12→ctx+0xec, r14–r31→ctx+0x174..0x1fc incl.
   r24/r29/r31, CR/XER/CTR, r1 via [KDP+4]) and the scheduler restore tail
   (`0x3244cc`) resumes via `mtlr r12; mtctr r10(=ctx+0xfc); mtcrf r13; bctr` —
   i.e. with the shim's `r10=r12=restart-PC`, an interrupted 68k world round-trips **by
   construction**, *without* ever touching table[0] **[STATIC]**. Table[0] is only the
   *first-dispatch* route. The "always-cold destroys 68k state on every interrupt"
   fear is real only if table[0] is genuinely re-entered — which is unproven and
   probably rare-to-never on the fidelity boot ([PROBE-O1]).

**Recommended rung: Rung 1 (S)** — make the user-MSR write r1-independent and re-run
Boot B. That single change un-blocks SS_M6A_USER_MSR *and* turns Boot B into the live
round-trip experiment rung 2 needs. The discriminator (cold-vs-resume at table[0]) is
Rung 2 and should be built only after Rung 1's telemetry shows table[0] re-entries
actually occur.

---

## 1. Q1 — the entry-vector table: real semantics

### 1.1 Raw ROM vs patched ROM

| Static addr | Raw 9.0.1 parcels ROM [RAW-ROM] | Post-PatchROM [PATCH] |
|---|---|---|
| `0x36e8c0..dc` (table) | `0fff0000 0fff0001 … 0fff0007` — `twi 31,r31,idx` slot placeholders | `b 0x36f900 / b 0x36fa00 / b 0x36fb00 / b 0x36fc00 / ILLEGAL / b 0x36fd00 / ILLEGAL…` |
| `0x36f900..ff` | nops (16+ words) | SheepShaver "Emulator start" stub |
| `0x36e900` (prologue) | `twui r31,0x10; …` — real ROM code, both images identical | unchanged |

On real hardware the emulator is relocated to RAM and the NK/Trampoline fills these
slots during emulator init; the ROM copy carries indexed trap placeholders. SheepShaver
(OldWorld lineage) replaces them statically because it replaces that init. The mirror
copy at `0x46e8c0` inherits the patched table via the 2MB memcpy (`rom_patches.cpp:699`,
which runs **after** `patch_68k_emul`), so the mirror "ongoing entry" `0x46f900` is the
**paravirtual stub**, byte-identical.

### 1.2 Slot semantics (from `patch_68k_emul`, :1426–1448)

| Slot | Target | Meaning | Exit pointer |
|---|---|---|---|
| 0 (`+0x00`) | `0x36f900` | **Emulator start / (re)enter 68k dispatch** | `[KDP+0x5f0]` |
| 1 (`+0x04`) | `0x36fa00` | Mixed Mode switch | `[KDP+0x5f4]` |
| 2 (`+0x08`) | `0x36fb00` | Reset / FC1E opcode | `[KDP+0x5f8]` |
| 3 (`+0x0c`) | `0x36fc00` | FE0A opcode (QD3D) | `[KDP+0x5fc]` |
| 4 (`+0x10`) | `POWERPC_ILLEGAL` | "Interrupt" — deliberately dead in the paravirtual design (interrupts are flag-injected, §3) | — |
| 5 (`+0x14`) | `0x36fd00` | FE0F opcode (power mgmt) | `[KDP+0x604]` |
| 6,7 | `POWERPC_ILLEGAL` | unused | — |

The table base is published at `[KDP+0x648]` (architecturally
`LA_EmulatorCode + [ConfigInfo+0x84]` = `…+0xe8c0`; glue seeds the mirror value
`ROMBase+0x46e8c0` since Wave 1).

### 1.3 What slot 0 ("ongoing entry") actually does — and what state it expects

All five stubs are the *same* 27-instruction NK→emulator handoff, differing only in the
exit pointer:

```
mtctr r1                      ; preserve caller r1
lwz   r1, XLM_IRQ_NEST(0)     ; nest++  (paravirtual interrupt-mask protocol)
addi/stw …
lwz   r1, XLM_KERNEL_DATA(0)  ; r1 = KDP
stw   r6, 0x18(r1)            ; save caller r6 at KDP+0x18
mfctr r6 ; stw r6, 4(r1)      ; save caller r1 at KDP+0x4
lwz   r6, 0x65c(r1)           ; r6 = ECB
stw   r7..r13, 0x13c..0x16c(r6) ; save caller r7-r13 (stride 8)
mfcr  r13                     ; r13 = caller CR
lwz   r7, 0x660(r1)           ; r7 = emulator flags word
mflr  r12                     ; r12 = caller LR
rlwimi. r7,r7,8,0x80000000    ; compose "entered emulator" flag bit
lwz   r10, 0x5f0(r1) ; mtlr r10  ; exit = [KDP+0x5f0]
mr    r10, r12                ; r10 = caller return address
lis/ori r11, 0x0002f072       ; r11 = MSR fiction
rlwimi r7,r7,27,0x20
blr                           ; → [KDP+0x5f0]
```

**[KDP+0x5f0] = `0x366080`** (primary world; glue seed) — disassembled **[STATIC]**:

```
0x366080  lhau  r27, 2(r24)        ; fetch next 68k opcode (r24 = 68k PC)
0x366084  rlwimi r29, r27, 3, 0xd, 0x1c  ; handler = table-base | opcode<<3
0x366088  mtlr  r29
0x36608c  lhau  r27, 2(r24)        ; prefetch operand word
0x366090  bgelr cr2                ; dispatch — UNLESS cr2.lt (interrupt pending)
0x366094  b     0x5036d114         ; → emulator interrupt/event slow path
```

So **slot 0's contract**: it is the NK→emulator *transition*, not a context restore. It
expects the **full DR register convention already live in the GPRs** —
r24 = 68k PC (with the lhau prefetch protocol; r27 = prefetched operand), r29 = opcode
dispatch-table base, r30 = emulator code base, r31 = EmulatorData (=ECB), r25 = SR MSB,
r1 = 68k A7, r8–r23 = D0–D7/A0–A6, cr2 field = mode/interrupt bits. It saves the
*caller's* (NK's) r7–r13 into the ECB save slots, increments the nest counter, and
resumes the dispatch loop. It does **not** read guest[0]/[4] and does **not** rebuild
any table — cold-start work belongs exclusively to the `0x36e964` body. There is no
"ctx save area at [ECB]+0x100" input to slot 0; the +0x100-based ctx story attaches to
the scheduler restore (§2.3), not to the stub.

Note the wrinkle: the stub's exit consumes `[KDP+0x5f0]` = **primary-world** `0x366080`.
The mirror stub at `0x46f900` therefore *jumps back into the never-compiled primary
emulator* — the dormant cross-world constant Wave 1 rev-2 finding 6 deliberately left
(`sheepshaver_glue.cpp:1927–1936`). Any rung that makes table[0]'s ongoing arm live must
also retarget `[KDP+0x5f0/0x5f4]` → `0x50466080` (mirror).

---

## 2. Q3 — the NK's save/restore protocol and the M3a shim

### 2.1 Interrupt save (handler entry path, static `0x312b0c–0x312bd0` [STATIC])

The M3a KDP shim (glue :804–859) reproduces the paravirtual `interrupt()` prologue:
`[KDP+4]=r1`, `[KDP+0x18]=r6`, `r6=[KDP+0x65c]=ECB`, r7–r13 → `ECB+0x13c..0x16c`,
then `r1=KDP, r7=flags, r8=0, r10=r12=restart, r13=CR, r11=SRR1`, CR splice. The NK
handler continues the save **through the same r6**:

| ctx slot (r6=ECB) | Saved value |
|---|---|
| +0xd4 / +0xdc | XER / r13 (= pre-handler CR, from the shim) |
| +0xec / +0xf4 | **r12 (= restart)** / CTR |
| **+0xfc** | **r10 (= restart)** — the slot the scheduler resume reads |
| +0x10c | r1 (re-read from [KDP+4]) |
| +0x114..+0x134 | r2–r5, [KDP+0x18] (=r6) |
| +0x174..+0x1fc | r14–r31 — incl. **r24 (68k PC) @+0x1c4, r29 (table base) @+0x1ec**, r30, r31 |

So a DEC delivered mid-68k-execution saves the **complete DR register file**: the shim
covers r7–r13 (D-regs live there), the NK covers r14–r31 (remaining D/A regs, PC,
table base, ED) plus r1 (A7) — nothing of the 68k world is lost at entry.

### 2.2 Restore — both exits converge on the shim's restart

The exception-return tail (`0x312c90`) reloads r24–r31 from ctx, then branches to the
scheduler (`b 0x503242a8`); the dispatch/restore tail (`0x3244cc`):

```
lwz   r8, 0xedc(r1)
mfspr r1, SPRG0          ; r1 ← per-CPU pointer
mtlr  r12                ; ctx r12 = restart   (M3a shim)
mtctr r10                ; ctx+0xfc = restart  (M3a shim)
mtcrf 0xff, r13          ; ctx CR
b     0x50318000         ; → XLM_IRQ_NEST--  (SheepShaver patch_nanokernel stub)
…                        ; → bctr — resume at restart
```

With `r10=r12=restart-PC` (the M3a "honest upgrade"), **the resume lands at the
interrupted block-entry PC with the restored register file** — architecturally exactly
what (a) in the design question asks for. The restart PC is a block-entry guest PC
(JIT chain-entry poll stores it before body code; DR blocks resume fine because the
prefetch state r27/r24 is part of the GPR file). **No rfi-to-restart is needed; the
scheduler-restore route IS the resume**, and the shim already feeds it.

### 2.3 Does the M3a shim need a MODE-aware (68k-world) variant? Mostly no.

The shim ABI composes with the NK's own save regardless of what world was interrupted —
the GPR file is saved/restored uniformly. The genuinely mode-relevant deltas are small:

1. **`[KDP+0x660]` (emulator-flags word) is seeded 0** (glue :1926). The stub and the
   handler compose r7 from it (`rlwimi. r7,r7,8,…` flag bit; handler tests
   `andi. r8,r7,0x30`, `xoris r7,r7,0x80` family). With 0, the handler classifies every
   delivery as "not from the emulator". Consequence unknown but plausibly it skips the
   post-a-68k-interrupt arm. → probe + seed in Rung 2 ([PROBE-O4]).
2. **XLM_IRQ_NEST imbalance**: the restore tail *decrements* (the `0x318000`
   patch) while the matching *increment* lives in the slot-0 stub — which is NOT on the
   M3a delivery path. Each round-trip drifts the counter negative (the observed
   `0xFFFFFFFF` = one restore, zero entries — now mechanically explained). M3a ignores
   the counter; `HandleInterrupt`'s `>0` early-out treats negative as "enabled"; the
   tick-thread gate (`==0`) is already dead. Cosmetic for now; retire or balance the
   protocol on the fidelity profile in Rung 2/3 (it is paravirtual residue).
3. **EE re-assertion**: the trampoline's `mtmsr` (when fixed, §4) runs on every table[0]
   entry — guest-side, so the execute_mtmsr EE-edge re-raise fires correctly. No shim
   change needed.

### 2.4 What breaks today — precisely

- **Boot B**: `lwz r0,-0x964(r1)` with r1=0 at the architectural table[0] entry —
  pre-delivery, pre-cold-start SIGSEGV (§0.2). Not a round-trip failure.
- **After that fix**: the *first* genuine 68k-world DEC round-trip is live-untested. Two
  candidate failure modes, both probe-able: (i) the handler's "from emulator" arm
  misbehaves on flags=0; (ii) if any NK path re-enters the world **via table[0]**
  rather than via saved-restart (e.g. a task-level reschedule that re-dispatches the
  emulator task at its entry vector), the always-cold trampoline resets guest[0]/[4]
  and restarts the 68k boot — silent state destruction. Whether (ii) ever occurs is
  **[PROBE-O1]**, the gating fact for Rung 2's cost.

### 2.5 The route into table[0] — best current model (answers PROBE-2 provisionally)

The Boot B register file at the crash **[CRASH]** is a context-restore signature, not
NK-live state: zeros everywhere except `r24=0x5000002a` (= ConfigInfo+0xfd8 68k reset
vector AND glue's ctx+0x1c4 seed), `r29=0x50480000` (= LA_DispatchTable AND glue's
ctx+0x1ec seed — also set by the trampoline itself before the faulting load),
`CTR=0x5046e8c0` (= `[KDP+0x648]`, the entry-vector table), `LR=0`, `r1=0`,
`r5=0x68ff4000` (kernel-pool-shaped). Model: **the NK dispatches the emulator world as
a task — scheduler-restore of a mostly-zero, ConfigInfo-derived context whose resume
slot holds the entry-vector address** — `mtctr; bctr → table[0]`. Two sub-candidates
for whose context: the NK's own emulator-task ctx (built by cold-init's `0x3109xx`
ctx writers from ConfigInfo) or glue's ECB+0x100 pre-population — the latter predicts
`CTR=0x5046f900` (its ctx+0xfc seed), which **contradicts** the observed
`CTR=0x5046e8c0`; and `r1=0` contradicts the restore tail's `mfspr r1,SPRG0`
(SPRG0=KDP, seeded and modeled). So the first dispatch likely takes a *different*
NK path than the steady-state restore tail, and **glue's ECB ctx pre-population
(:1968–1992) is probably dead scaffolding** — [PROBE-O2] settles both.

---

## 3. Q2 — how the paravirtual path does 68k-world interrupt round-trips

Reference: `main_unix.cpp:2146` `HandleInterrupt` + glue `interrupt()` :654.

| Run mode | Paravirtual mechanism | NK involvement |
|---|---|---|
| `MODE_68K` (the 68k world) | **No exception, no NK, no context save at all.** Glue pokes `[[KDP+0x67c]] = 1` (pending 68k interrupt level) and ORs the CR mask `[KDP+0x674]` into the **live CR** (and, in MODE_NATIVE's variant, into the *saved* CR at `[[KDP+0x658]]+0xdc`, so it survives a context restore). The DR emulator's own dispatch (`bgelr cr2`, §1.3) falls through to its interrupt slow path (`0x36d114`) at the next instruction boundary and delivers a **68k-level interrupt** internally — frame push, SR, vector — then continues. Resume is implicit: the emulator never left. | none |
| `MODE_NATIVE` (PPC code) | `interrupt()` — KDP/ECB register save (same offsets as the stub), nested `execute(NK handler)`, return via `r10=r12=trampoline(EXEC_RETURN)`; host-side restore of pc/lr/ctr/r1. | NK handler runs as a *subroutine* |
| `MODE_EMUL_OP` | Synthesizes a 68k frame and `Execute68k`s through vector `$64` directly. | none |

On newworld today, the MODE_68K arm is fenced down to `Ticks++` (M3a rev-2 F3/M2) and
XLM_RUN_MODE is pinned at MODE_68K; the **only** delivery channel is the M3a DEC hook.

**Minimal fidelity-profile equivalent (design answer):** keep the M3a hook as the *PPC
exception* layer and let the **NK itself** do what the paravirtual glue used to fake:
the NK's handler posts to the emulator through the same guest-visible machinery
(pending-level word via `[KDP+0x67c]`, CR-bit into the saved CR ctx+0xdc so the
restored world's next `bgelr cr2` falls through to `0x46d114`). All of that is guest
code we already run; our obligations reduce to honest **entry plumbing** (the shim —
done), honest **exit plumbing** (restart resume — done by construction, §2.2), the
**flags seed** `[KDP+0x660]`, and **not destroying the world** if table[0] is
re-entered. No new host-side delivery mechanism is needed for the 68k world.

---

## 4. What ongoing-entry support requires (the design)

(a) **Interrupt round-trip save/run/resume** — already structurally present:
shim save (r7–r13) + NK save (rest) + scheduler restore to `r10=restart`. Requirements
left: r1-independent trampoline (R1), flags seed (R4), nest-protocol decision (R5).

(b) **Cold-vs-resume discrimination at table[0]** — required only for genuine table
re-entries:

- **R1 (Boot B unblock, S):** make the user-MSR write r1-independent. Preferred:
  `lis r0,0; ori r0,r0,0xd032; mtmsr r0` (UserModeMSR is itself a glue-seeded
  constant; the `[KDP-0x964]` indirection bought nothing but the r1 dependency).
  Documented deviation from "derive from ConfigInfo" — acceptable at diagnostic rung;
  R6 restores derivation. Keep `SS_M6A_USER_MSR` default-OFF until R3 validates.
  Note `0xd032` sets PR=1 — verify the JIT/interpreter MSR fiction tolerates PR
  (it carried 0xf072 = PR=1 for years; low risk).
- **R2 (discriminator, S–M):** guest-side flag branch in the trampoline — no
  self-modifying code, no JIT invalidation:
  ```
  lwz   rT, <scratch>(0)      ; scratch word in already-mapped guest DATA page
  cmpwi rT, 0
  bne   ongoing               ; warm: skip fixups + cold-start
  li    rT,1 ; stw rT,<scratch>(0)
  …existing 12-insn cold fixups…
  b     mirror cold-start 0x5046e964
  ongoing:
  [mtmsr block]               ; EE re-assert on every entry
  b     0x5046e8c0+? → see below
  ```
  Scratch placement: a glue-designated word in the sub-KDP pool (`0x68ff4xxx`,
  mapped+zeroed, data-only — never compiled, so no SMC hazard). NOT in the ROM zero-run
  (same page as trampoline code → JIT page-invalidation hazard).
- **R3 (ongoing arm target, M — the real design decision):** what does "resume" mean
  on a table[0] re-entry? The entrant arrives with a restored DR register file (that is
  the table's contract, §1.3). Honest target = the slot-0 stub semantics with the
  cross-world constant fixed: `[KDP+0x5f0]/[0x5f4] := 0x50466080` (mirror re-dispatch)
  and ongoing arm = `b 0x5046f900`. Cheaper variant (skips the stub's NK-save +
  nest++, defensible because the M3a path saves via the shim instead):
  ongoing arm = `b 0x50466080` directly — resume the dispatch loop, period.
  Decide after [PROBE-O1] shows who re-enters and with what in LR/r7 — if nobody ever
  does, R3 collapses to "keep always-cold but r1-safe" + a tripwire probe counter.
- **R4 (flags seed, S):** `[KDP+0x660]` — determine the bit the handler's
  from-emulator tests consume ([PROBE-O4]) and seed it; suspect it gates the
  post-68k-interrupt arm (§3's KDP+0x67c/CR posting).
- **R5 (nest protocol, S, gated):** on the newworld fidelity profile, neutralize the
  `0x318000` decrement (and don't reintroduce the stub increment) OR keep both balanced
  via R3's stub route. Own A/B boot; paravirtual untouched.
- **R6 (derivation hygiene, M, later):** trampoline constants from ConfigInfo/SPRG0
  chain; reconcile/delete glue's dead ECB ctx pre-population once [PROBE-O2] confirms;
  matches handoff-memo rung 3.

---

## 5. Rung ladder (Q4)

| Rung | Work | Cost | Verification probes |
|---|---|---|---|
| **0** | **Probe pack** (one sanctioned session, no rebuild): [PROBE-O1] table[0] entry census — `SS_JIT_NO_CHAIN=1 SS_PROBE_PC='0x5046e8c0:r1,r6,r7,r24,r29;0x50429b40:r1'` (LR/CTR via full dump): how many entries per boot, register signature, does it EVER re-enter post-cold? [PROBE-O2] who built the restored ctx — dump `[KDP+0x648]`, ECB+0x1fc/+0x1c4/+0x1ec and the NK ctx candidates at the entry; zap-test glue's ECB pre-population. [PROBE-O4] `[KDP+0x660]` consumption — probe the handler flag tests (`0x50412bbc`-region) with r7 dumps. | S | the pack IS the verification |
| **1** | **R1** — r1-independent `mtmsr` (2-insn trampoline edit); rerun Boot B | S (hours; `make test-jit`=100 + OldWorld 8.6 e2e smoke — edits newworld-gated) | Boot B no longer SIGSEGVs at `ea=…fff69c`; either reaches Boot A's frontier (`pc=0x50466ee0` shim desert) with `delivered_dec≥1` — the first live 68k-world round-trip, captured — or fails INSIDE delivery, which is exactly rung-2's recon |
| **2** | **R2+R3+R4** — discriminator + ongoing arm (+ `[KDP+0x5f0/4]` mirror fix if stub route chosen) + flags seed; scope from rung-1 telemetry | M (1–3 d) | guest[0]/[4] watch (`SS_JIT_WATCH_ADDR=0,4`): never rewritten after first entry; `delivered_dec` > 1 with jDR/comp still growing across deliveries; scratch-flag probe shows cold exactly once |
| **3** | **R5** nest retirement (gated) + SS_M6A_USER_MSR default-ON for the fidelity profile; acceptance = sustained DEC deliveries during 68k execution with stable boot progression to the shim-desert wall | S–M | `XLM_IRQ_NEST` watch (`SS_JIT_WATCH_ADDR=10264`) stays balanced/parked; heartbeat `exc=` counters climbing |
| **4** | **R6** derivation hygiene + dead-scaffolding removal | M | regression suite only |
| — | Full synthesized-Trampoline / NK-native emulator init (handoff memo rung 5) | L | not needed for this wall |

**Recommended: Rung 0 and Rung 1 together.** Rung 1 is two instructions and converts
Boot B from a crash into the round-trip experiment; Rung 0's census tells us whether
Rung 2 is load-bearing or a tripwire. Do **not** build the discriminator first — its
design (R3 target choice) depends on facts only rungs 0–1 produce.

---

## 6. Honest residue

1. The first-dispatch route into table[0] (§2.5) is a high-confidence inference from
   the crash register file, not a probe-verified fact; `r1=0` vs `mfspr r1,SPRG0` is
   unexplained under the steady-state-restore model — the first dispatch must use a
   different NK path. [PROBE-O2].
2. Whether table[0] is ever re-entered after cold start on this boot is unknown;
   everything in Rung 2 is sized by that single fact. [PROBE-O1].
3. The handler's flag-arm behavior on `[KDP+0x660]=0` (and what the correct seed is)
   is undisassembled past the entry block. [PROBE-O4].
4. PR=1 under the JIT during 68k-world execution is assumed benign by precedent
   (0xf072 fiction), not re-verified post-M3a/M5.
5. The slot-4 "Interrupt" vector is ILLEGAL by paravirtual design; if the real NK's
   posting path ever branches through it (instead of the CR/level mechanism), it will
   trap loudly — treat such a trap as signal, not noise.

---

## Rung 2 contracts (Task 0 recon addendum, 2026-06-11 — BINDING for Tasks T..X)

> Recon for plan `docs/superpowers/plans/2026-06-11-m6a-rung2-mixedmode-switch.md`.
> Three bounded probe boots (≤50 s each, `SS_NW_MM_POOL=1 SS_JIT_NO_CHAIN=1`,
> `/tmp/m2accept.prefs`; logs `/tmp/t0_boot{1,2,3}.log`) + static capstone of the
> patched dump. Tags: **[RAW-ROM]** = `/tmp/rom901_inventory.bin` (verified raw:
> `twi` slot placeholders at 0x36e8c0), **[PATCH]** = our patch source,
> **[STATIC]** = disassembly of the patched dump, **[PROBE✓]** = live-verified.

### Provenance (rev 2 C9 — resolved)

- `/tmp/rom901.bin` **is PATCHED** (entry table at file 0x36e8c0 = `48001040 4800113c …`),
  as is `/tmp/rom901_decompressed.bin`. A fresh `SS_DUMP_ROM` dump this session
  (`/tmp/rom901_patched_t0.bin`) is **byte-identical** to `/tmp/rom901.bin`
  (md5 `e432df64122a5a89ec1c08d0bdf01609`) — so `rom901.bin` = current-binary post-PatchROM
  state and all prior file-offset reads against it are [PATCH]-tagged, not [RAW-ROM].
- `/tmp/rom901_inventory.bin` is the genuine **UNPATCHED** image (slot placeholders
  `0fff0000…000f` — note: **16** placeholders, confirming the 16-slot table in the raw ROM
  too) — the only [RAW-ROM] source on disk.
- The 4 MB dump covers the primary copy only; the **mirror region (host offset 0x400000+:
  trampoline 0x429b40, table[0] redirect 0x46e8c0, pool seed) is NOT in any dump** — mirror
  facts are [PATCH] (source) or [PROBE✓] only. Primary↔mirror: mirror = primary+0x100000
  via the rom_patches.cpp:699 memcpy, which runs **after** patch_68k_emul and **before**
  PatchROM_NW_trampoline.

### The opcode-service map (file 0x3ff000+ = mirror dispatch slots, 8 B/opcode) [STATIC]

| Opcode | Slot prologue | Service body |
|---|---|---|
| FE01 | `lwz r5,0x9e0(r31)` | `b 0x5046de10` → `mtlr r5; lwz r7,0(r1); blr` → **[ECB+0x9e0]=0x5046de1c** [PROBE✓] = RoutineDescriptor parser |
| FE02 | `lwz r4,0x714(r31)` | `b 0x5046e120` (same save/switch protocol as FE01's PPC path, continuation r4=[ECB+0x714]) |
| FE07 | `rlwimi r29,r27,3,0xd,0x1c` (pre-computes next dispatch) | `b 0x5046e380` (frame validator — see Q-C) |

### Q-A — the 0x220 save record [STATIC + PROBE✓] — **PINNED**

- **The DR itself writes only the 0x10-byte trailer**: `[rec+0x210]=r0`,
  `[rec+0x214]=in-use bitmask bit`, `[rec+0x218]=previous record (chain)` (alloc site
  0x5046e324–e358). The body `+0x000..0x20F` is the **NK's 68k-emulator-context save area**:
  the record's *physical* address is registered at `[MMCB+0xd8]` (MMCB=[ECB+0x710]
  =0x68fff400 [PROBE✓]) so the NK can save the 68k context there while the world is
  switched out. The *virtual* pointer rides in the switch frame at frame+0xE8
  (= the stack copy of MMCB+0xd8).
- **Allocation is conditional**: the allocator runs only when `[MMCB+0xd8] ≠ 0` (beq cr7 at
  0x5046e300 skips it). Live: `[MMCB+0xd8]=0x68ffb8e0` [PROBE✓] — the NK ECB build supplies
  an **initial record at 0x68ffb8e0** (NK-owned, outside our pool) — so every switch
  allocates. Free is exact LIFO (Q-F): observed `[ECB+0xEC]=0` at every allocator entry
  across 10M iterations [PROBE✓] ⇒ **max concurrent depth observed = 1**.
- **Record-count answer for Task T (the blocking part): keep 4 records + the slot-15
  (`+0x3c`) loud stop.** Depth is bounded by MixedMode nesting (68k→PPC→68k→…); the boot's
  real nesting past the spin is unknowable until Task V — the plan's fallback (4 + loud
  stop, never guess bigger) is adopted as the pinned sizing. Residue R-1: true nesting
  depth, re-measured at Task Y via the loud stop never firing.

### Q-B — post-allocator control flow + consumed slots [PROBE✓] — **PINNED**

- **Consumed entry-vector slots under the live FE01 path: slot 1 ONLY** (stub 0x5046fa00:
  10M visits; slots 2/3/5 stubs 0x5046fb00/fc00/fd00: **zero** visits; slot 0: exactly 1
  visit = cold start; slot 15 (+0x3c): never under pool-on). Call site: FE01 service
  `bnel cr2, 0x5046e8c4` (0x5046e19c) after saving D0-D7/A0-A6 → `[ECB+0x708]`=0x68fff740
  block (+0x00..0x38, +0x3c=68k PC, +0x40=r0) and populating the MMCB **as an NK context
  block**: `+0xfc`=resume PC=[ECB+0x700]=**0x500ebc20**, `+0x10c`=r1(68k A7),
  `+0x114`=r2=[ECB+0x704]=0x10025864 (native glue RTOC), `+0x1ec`=r29=routineRecord ptr
  (0x10024df4), `+0x1f4`=r30=RD (0x10024de8), `+0x1fc`=r31=[ECB+0x708] save block — i.e.
  **FE01 manufactures the MixedMode context's register file in the ctx GPR slots the NK
  scheduler restore reads** (ctx+0x174+8·(n−14) for r14–r31; doc §2.1).
- Slot-1 stub register state at entry [PROBE✓]: r3=0x68fff400 (MMCB = the switch-target
  context ID), r4=0x00200000, r5=0x68fff740, r7=RD 0x10024de8, r24=0x100266f2,
  r25=0x0827 (0x800 = in-MixedMode), r29=0x504ff008, r30=0x50460000, r31=ECB,
  LR=0x5046e1a0 (service return), CR=0x40100000 (cr2eq clear ⇒ bnel taken).
- **8-vs-16 resolution recorded**: the table is 16 slots [RAW-ROM: 16 `twi` placeholders;
  PATCH: slots 0–3,5 branches, 4 and 6–15 `POWERPC_ILLEGAL==0` per emul_op.h:26]. The
  design-doc §1.2 "8 slots" was a truncation. Slot 15 (+0x3c) = allocator-exhaust target;
  slot 4 = the deliberately-dead interrupt vector; no collision.
- **Advance parked-PC enumeration (Task U gate: live parked PCs ⊆ this set)** — unique
  `bra.s *`-class loud stops in the mirror zero run above 0x429c30, one per non-consumed
  slot: slot 4→0x50429c40, 6→0x50429c50, 7→0x50429c60, 8→0x50429c70, 9→0x50429c80,
  10→0x50429c90, 11→0x50429ca0, 12→0x50429cb0, 13→0x50429cc0, 14→0x50429cd0,
  **15→0x50429cf0 (Task T's exhaust stop)**. (These are PPC slots — stops are PPC parked
  branches `b *`, written verify-zero-first at PatchROM time.) Slots 0–3,5 keep their
  static branches; only slot 1 is live-consumed; slots 2/3/5 stay as-is (static stubs,
  unconsumed ≠ parked).

### Q-C — the retry pivot / FE07 semantics [PROBE✓ + STATIC] — **PINNED; stop-rule NOT fired**

The live spin circuit (all legs probe-verified, 10M visits each):
`e1a0` (return from NK, command byte 0) → `e2a0` switch: stack→supervisor
(`[ECB+0x4c]` via helper 0x5046d7a8), push 0x310 frame on the 68k stack
(`+0`=68k PC 0x100266f0, `+4`=PPC return 0x5046e244, `+8`=CR composite, `+0xc`=marker
`0x1300xxxx`, `+0x10..0x310`= MMCB copy), allocate record, then via the d7a8 LR-trick fall
into the **68k exception raiser 0x5046d7cc**: push 8-byte format-E frame
(`SR,PC,fmt/vec=0xe05c`) and dispatch the 68k through **vector 0x5c = 0x10008fb0**
[PROBE✓ `[0x5c]`] (installed by MixedMode parcel init) → stub `FE07; bne.b; rte / jmp
0x500049c4` → FE07 (0x5046e380) **validates the frame and tests the copied command byte**:

- fmt nibble ([A7+6]>>12) must be 0xb or 0xe; marker [A7+0x14]==0x1300; then
  **r4 = lbz [A7+0x18] = the frame's copy of the command byte [MMCB+0]**:
  `0` → CR0(68k Z)=EQ → `rte` → DR's format-E RTE service **0x5046e408**: restore MMCB
  from the frame, free the record, pop 0x318, resume at frame+4 (0x5046e244) via
  `b 0x5046c3ec; bctr [ECB+0x818]=0x5046c410` → **retry loop** (`b 0x5046e154`: re-save,
  re-call slot 1). `0xc`/`0x14` → page-touch probes through [MMCB+0xfc]/[MMCB+0x34] then
  same CR0 logic; **non-zero else** → NE → `jmp 0x500049c4` (the 68k-side continuation).
- Observed forever: command byte = 0x00 ([MMCB+0] word = 0x00000006 = [ECB+0x7a8] re-seeded
  at 0x5046e35c each pass; big-endian byte 0 = 0) [PROBE✓ frame dump].

**The root cause sits one level down: the NK switch-to-context service.** The slot-1 stub
exits via `[KDP+0x5f4] = 0x503143a0` [PROBE✓] (and `[KDP+0x5f0]=0x50313bf8` — both NK
addresses; the §1.3 "0x366080" claim is corrected: on the live newworld boot the slot exits
go to the **staged NK**, not the primary emulator). 0x503143a0 [STATIC]:

```
mtcrf 0x3f, r7            ; r7 = stub-composed [KDP+0x660] flags word
bnel  cr2, 0x50312cb0     ; cr2eq==0 ⇒ "not from emulator" ⇒ bounce back, nothing done
and.  r8, r4, r13 ; bne 0x50312cb0   ; caller-CR bit 0x00200000 must be CLEAR
r8 = r3 & ~0x3F           ; r3 = 0x68fff400 = the requested context ID (the MMCB)
MRU lookup [KDP+0x340/348/350/358] (paired ctx ptrs at +0x344/34c/354/35c)…
  hit → [0x2810]=1; [pairctx+0x5c]=[KDP+0x648]; [KDP-0x14]=pairctx; b 0x50312b0c (NK SAVE)
  miss → slow path: cmpw vs r6, page-table validate (bl 0x503154b8), [KDP+0xea4]++,
         install into MRU, loop to hit path; validation failure → r8=2; b 0x50312ab4
```

**Live: the spin takes the FIRST branch** — 0x50312cb0 visits = 10M; hit path 0x503143c4,
slow path 0x5031440c, error 0x50312ab4, save 0x50312b0c: **all ZERO** [PROBE✓].
`[KDP+0x660]=0` ⇒ r7=0 ⇒ cr2eq=0 ⇒ every slot-1 call is classified "not from the
emulator" and returns having done nothing ⇒ command byte stays 0 ⇒ FE07 EQ ⇒ unwind ⇒
retry. The MRU cache is uninitialized poison (`-1/0` ×4 pairs [PROBE✓]); `[0x2810]`=0,
`[KDP+0xea4]`=0.

**The spin-breaking condition Task V must satisfy (in scope — slot/seed/record plumbing +
pinned conventions; the machinery itself is the staged NK's own):**
1. `[KDP+0x660]` bit **0x00200000** set (the stub's rlwimi chain only touches bits
   0x80000000 and 0x20, so the bit passes straight into cr2eq) — this is also
   [PROBE-O4]'s answer (see probe pack).
2. The context lookup for ID 0x68fff400 must succeed: either the slow-path page-table
   validation passes on the flat model (unverified — falsifiable live), or Task V
   **pre-seeds an MRU pair** `[KDP+0x340]=0x68fff400, [KDP+0x344]=<ctx>` (the pair value
   is consumed as `[pair+0x5c]:=[KDP+0x648]` and `[KDP-0x14]:=pair`; the natural value is
   the MMCB itself — one-iteration rule applies if falsified).
3. Then the NK's own save (0x50312b0c) + scheduler switch resume the MixedMode context at
   `[MMCB+0xfc]=0x500ebc20` — **the TVector entry is performed by the ROM's native
   MixedMode glue, not by hand-rolled trampoline code**. No new NK surface, no L-class
   Trampoline, no FE07-side service is missing (FE07 is complete in the DR). The
   stop-rule does **not** fire.

FE07 ownership note for Task V: FE07 needs **no implementation** — its `bne` falls through
exactly when the command byte the completion path writes is nonzero (and the 0xc/0x14
probe classes behave as above). What Task V owes FE07 is only a correct command byte.

### Q-D — TVector entry convention [STATIC + PROBE✓] — **PINNED**

- RoutineDescriptor @0x10024de8 [PROBE✓]: `aafe 07 00 | 00000000 | 00 00 0000` →
  goMixedModeTrap, version 7, rdFlags 0, routineCount=0 (single routine);
  routineRecords[0] @+0xC: procInfo=0x000000E1 (**cc = kCStackBased(1)**), ISA=1 (PPC),
  routineFlags=0x0004, procDescriptor=0x10024758. TVector [PROBE✓]: code=**0x500cef8c**,
  RTOC=**0x10024754**. (Rev 2 C7 confirmed: RD=0x10024de8, procDescriptor field
  =0x10024758.) The FE01 parser reads exactly these fields (version path: routineCount==0
  + ISA==1 → the switch path; ISA==0 would just redirect the 68k PC to procDescriptor).
- The native glue at **0x500ebc20** [STATIC] is entered as the restored MixedMode context
  (register file = the ctx slots FE01 wrote, Q-B): expects **r30=RD** (validates
  version==7; else `b 0x500ef75c` fallback), **r29=routineRecord**, **r1=68k A7**
  (args base; r28=r1+4 walks caller args), **r2=its own RTOC (0x10025864)**, **r31=the
  68k register save block 0x68fff740** (e.g. `[r31+0x38]`=A6 staged to `[r1-4]`). It
  aligns a PPC frame (`stwux r1,r1,-N`, 64-byte aligned, back-chain tagged `|1`) and
  dispatches per procInfo cc through its TOC table `[r2+0x16c+cc*4]; bctr`.
- **Task V probe-gate table at `SS_PROBE_PC=0x500cef8c`** (exact conformance per plan
  rev 2 P7; classes where a literal is not architecturally fixed):

| Register | Expected | Class |
|---|---|---|
| CTR | 0x500cef8c | exact (bctr entry) |
| r2 | 0x10024754 | exact (TVector[1] RTOC) |
| r1 | < 0x103ffe5c, 64-byte aligned, `[r1]` = back-chain tagged `\|1` | class: PPC frame on the 68k stack |
| LR | 0x500exxxx (ROM glue return) | class: MixedMode glue region |
| r30 | 0x10024de8 (RD) | class: glue-preserved, non-gating |
| MSR (stored) | per-context (NK switch srr1), EE state recorded not gated | diagnostic |

  (The cc=1 dispatch leg that loads CTR/r2 from the TVector is [STATIC]-inferred from the
  `[r2+0x16c+cc*4]` table; the r2/CTR literals above are pinned from the probed TVector
  contents, so the gate is falsifiable regardless.)

### Q-E — MSR across the switch (time-boxed) — **VERDICT + recorded fallback**

- **Verdict (new, supersedes the SS_M6A_USER_MSR framing):** the architectural MSR
  transition for the MixedMode excursion is **per-context, carried by the NK context
  switch** (the resumed ctx's SRR1 slot) — not a trampoline-side `mtmsr`. Task V should
  need **no** SS_M6A_USER_MSR involvement for FE01 completion; the trampoline mtmsr block
  stays what it is — a table[0]-cold-start diagnostic. **SS_M6A_USER_MSR remains
  quarantined default-OFF** (known-broken diagnostic; document in Task Z).
- The ctx MSR slot's live value for the manufactured MixedMode context is **unverified**
  (which ctx offset the NK rfi takes SRR1 from on this path was not chased within the
  time-box). If the resumed context runs EE=1, the deferred-DEC backlog delivers during
  the native excursion through M3a plumbing — Task V watch item, not a blocker.
- **Zero-page slide (SS_M6A_USER_MSR=1): NOT root-caused** within budget (0 of its 2 boots
  spent — deliberately, since the verdict removes it from the critical path). Named
  residue R-2; explicitly NOT a Task V blocker (plan rev 2 P5 fallback adopted).
- **PR=1 re-verification (rev 2 C10):** the rung-1 night-run datum stands as the post-M3a
  evidence: a real DEC delivery with `srr1=0xd032` (PR=1) round-tripped through the NK and
  **resumed the guest correctly** (M6A-WAVE2-SHIM-RECON "Wave 2 night-run results").
  Sustained PR=1 execution under the JIT remains untested at scale — residue R-3, gated
  naturally by Task V's probe sub-contract (the TVector excursion runs under the
  ctx MSR; any PR-intolerance surfaces there loudly).

### Q-F — FE02 / switch-back mechanics [STATIC + PROBE✓] — **PINNED (with a re-scope)**

- **Who frees the pool bit:** the DR's format-E RTE service **0x5046e408** [PROBE✓ 10M]:
  validates marker `[A7+0x14]==0x1300` (else `b 0x5046c6d8` error), copies the frame's
  MMCB image back over the MMCB, and if the restored `[MMCB+0xd8]` chain head ≠ 0:
  `[MMCB+0xd8] := [rec+0x218]` (pop) and `[ECB+0xEC] &= ~[rec+0x214]` (free). Resume:
  `mtlr [frame+4]; b 0x5046c3ec → bctr [ECB+0x818]=0x5046c410` (status recompute →
  back into the service's retry/continuation at 0x5046e244).
- **Where the 68k resumes on completion:** the command-byte 0xff path (0x5046e1f4):
  r24 := `[saveblk+0x3c]` (the saved 68k PC), r25 &= ~0x800, dispatch — i.e. the 68k
  resumes at the instruction after the $AAFE site with D/A registers reloaded from the
  save block (results written there by the native side).
- **Re-scope finding:** the **FE02 opcode is NOT the forward switch-back** on the
  MPLibrary path. FE02's service (0x5046e120) is the *same* save/slot-1 context-switch
  protocol with continuation `[ECB+0x714]=0x500ef258` [PROBE✓] — it is the
  **PPC-calls-68k direction** (native glue parks itself, dispatches a 68k routine; the
  68k completion re-switches to the native ctx which resumes at 0x500ef258). Task W's
  real deliverable is therefore: the native completion path (glue, after the TVector
  returns) switching **back to the 68k-emulator context** through the same NK
  switch-to-context service, with the command byte set so FE07 falls through (or 0xff so
  e1f4 resumes). Residues: R-4 — the exact command byte the genuine native completion
  writes (0xff per the e1f4 semantics is the design-consistent value; verify at Task W
  live); R-5 — the emulator-side resume point after the NK re-switch (the saved emulator
  ctx from 0x50312b0c resumes inside the slot-1 stub return → 0x5046e1a0; verify with
  the r24 ring per rev 2 C5).

### Probe pack results

- **[PROBE-O1] table[0] census (BASELINE, pool-on)** [PROBE✓]: visit=1 per boot —
  the cold start — and **never re-entered**; the FE01 spin lives entirely inside the
  DR service + NK bounce. Entry signature: all-zero file except r5=0x68ff4000,
  r24=0x5000002a, r29=0x50480000, CTR=0x5046e8c0, LR=0, r1=0 — the §2.5 context-restore
  signature, now probe-verified. (Task X re-censuses post-W per plan rev 2 P1.)
- **[PROBE-O2] who built the restored ctx** [PROBE✓]: CTR=0x5046e8c0 (≠ glue's ctx+0xfc
  seed 0x5046f900) and r1=0 (≠ `mfspr r1,SPRG0` restore tail) ⇒ **glue's ECB ctx
  pre-population is dead scaffolding confirmed**; the first dispatch is an NK
  ConfigInfo-derived direct dispatch (r5=0x68ff4000=NKSystemInfo-shaped), not the
  steady-state scheduler restore.
- **[PROBE-O4] `[KDP+0x660]` consumption** [PROBE✓ — **pinned, R4 unblocked**]: the
  0x50412bbc-region probe never fired (no NK interrupt deliveries this boot), but the
  flags word's live consumer was caught elsewhere: the slot-stub composes r7 from
  `[KDP+0x660]` and **0x503143a0's `mtcrf 0x3f,r7; bnel cr2,…` consumes it as the
  from-emulator classification — the consumed bit is 0x00200000 (cr2eq)**. R4's seed =
  set bit 0x00200000 (the rlwimi-composed bits 0x80000000/0x20 are separately derived
  flag bits, not preconditions). Caveat: the *interrupt handler's* flag tests (the
  original O4 target) remain unexercised — same bit family, verify when deliveries run.

### Blocking-answer status (plan rev 2 P3 gate)

| Blocker | Status |
|---|---|
| Task T ← Q-A record count / pool sizing | **PINNED** (4 records + slot-15 loud stop; depth-1 observed; fallback rule adopted) |
| Task U ← Q-B consumed-slot map + parked-PC enumeration | **PINNED** (slot 1 only; enumeration above; 16-slot resolution recorded) |
| Task V ← Q-C pivot + Q-D register table | **PINNED** (flags bit 0x00200000 + MRU/ctx lookup + NK-native switch; FE07 needs nothing; TVector gate table above) |
| Task V ← Q-E verdict-or-fallback | **VERDICT** (per-context MSR via NK switch; SS_M6A_USER_MSR quarantined; slide = residue R-2, not a blocker) |
| Task W ← Q-F | **PINNED** (free site, resume site, marker contract; FE02-direction re-scope recorded; R-4/R-5 are verify-at-W residues, not unknowns about *whether* the surface exists) |
| Task X ← O1 baseline + O4 | **PINNED** (O1 baseline = 1 cold entry, 0 re-entries; O4 bit pinned ⇒ R4 actionable) |

**Stop-rule: NOT fired.** FE01 completion requires only: two KDP seeds
(`[KDP+0x660]` bit 0x00200000; MRU pair `[KDP+0x340/0x344]` — or a live-validated
slow-path pass), the already-staged pool (Task T), and the staged NK's own
switch/save/restore machinery — squarely "slot population + save-record plumbing +
pinned conventions". The one structural surprise is *favorable*: the TVector call is
made by the ROM's own native glue (0x500ebc20), so Task V writes seeds, not call glue.

### Residue register (explicit)

- **R-1**: true MixedMode nesting depth (pool sizing evidence) — re-measured at Task Y
  via the slot-15 loud stop.
- **R-2**: SS_M6A_USER_MSR zero-page slide — not root-caused; quarantined diagnostic.
- **R-3**: sustained PR=1 execution under the JIT — single-delivery datum only.
- **R-4**: the exact command byte the genuine native completion writes (0xff expected).
- **R-5**: the emulator-side resume point after the switch-back (expect slot-1 stub
  return → 0x5046e1a0; r24-ring verification at Task W).
- **R-6**: the slow-path page-table validation (`bl 0x503154b8`) behavior on the flat
  model — bypassed if the MRU pre-seed is used; verify whichever route Task V takes.
- **R-7**: the ctx SRR1 slot/value the NK rfi uses for the manufactured MixedMode
  context (EE state during the native excursion).
- **R-8**: `[ECB+0x7a8]`(=6)/`[MMCB+0]` word sub-bytes and the `0x7df2f700` constant
  written at 0x5046e35c — semantics unknown; restored by e408 each cycle, inert so far.
- **R-9**: the interrupt-handler-side `[KDP+0x660]` flag tests (original O4 probe
  target) — unexercised this boot; same bit family as the pinned consumer.

### Sub-KDP occupancy map (Task T, 2026-06-11 — AUTHORITATIVE; extend before placing anything here)

The mapped+zeroed sub-KDP region is `[0x68FF4000..0x68FFC000)` (32 KB,
`vm_acquire_fixed` + memset in `sheepshaver_glue.cpp` init_emul_ppc; KDP=0x68FFE000,
shmem boundary 0x68FFC000). Every known occupant, ascending:

| Range | Size | Occupant | Writer / consumer |
|---|---|---|---|
| `0x68FF4000..0x68FF4120` | 0x120 | NKSystemInfo block | glue seeds; NK/ROM read (also PROBE-O2's r5 cold-dispatch value) |
| `0x68FF4120..0x68FF4DF0` | — | free (reserve for NKSystemInfo growth) | — |
| `0x68FF4DF0..0x68FF4EBC` | 0xCC | IRP bank table (banks 0–25; irp_base=0x68FF4000, +0xDF0) | glue seeds bank 0; NK reads |
| `0x68FF4EBC..0x68FF4F00` | — | free | — |
| `0x68FF4F00..0x68FF4F80` | ~0x80 | 'Hnfo' hardware-info record (`[KDP+0xfd0]` target; fields to +0x76) | glue seeds; ROM machine detect reads |
| `0x68FF5000..0x68FF5800` | 0x800 (reserve) | 'Hnfo' writable scratch record (`[hnfo_rec+0x08]`); observed writes +0x10..+0x17 (ROM+0xAC20 copy-out) | ROM machine detect writes each cold cycle |
| `0x68FF5800..0x68FF6080` | 0x880 | **MM save-record pool, 4 × 0x220** (`[ECB+0xE0/E4]`; existence bitmap 0xF0000000) — Task T relocation (was 0x68FF5000, rev 2 C1 collision with the scratch) | trampoline re-seeds per entry; DR allocator 0x5046e304 + NK context save |
| `0x68FF6080..0x68FF6084` | 4 | **RESERVED: Task-X R2 cold/ongoing discriminator scratch word** | Task X (data-only; not the ROM zero run) |
| `0x68FF6084..0x68FF7000` | — | free (pool growth headroom if R-1 ever demands it) | — |
| `0x68FF7000..` | — | NK pool/heap free-list (KDP-0x7000) | NK cold-init builds; extent NK-owned — treat `0x68FF7000..0x68FFC000` as NK territory |
| `0x68FFB8E0..0x68FFBB00` | 0x220 | NK-supplied initial MM save record (`[MMCB+0xd8]` chain head, Q-A [PROBE✓]) | NK ECB build; DR chain/free |

Notes:
- The pool's 4-record sizing is Q-A's pinned answer (observed depth 1; fallback rule
  "4 + loud stop, never guess bigger"). Exhaustion now parks at the slot-15 loud stop
  `0x50429cf0` (Task T) — the R-1 tripwire.
- The old pool base 0x68FF5000 was claimed "free gap / no other users" by the seed-site
  comment — falsified by the repo's own Hnfo seed (glue) + the machine-detect copy-out.
  Lesson recorded for Task Z: free-space claims in this region require THIS map.

### Task T results (2026-06-11)

- Pool relocated 0x68FF5000 → **0x68FF5800** per the map above; Task-X scratch word
  reserved at 0x68FF6080.
- `SS_NW_MM_POOL` promoted to **newworld profile default-ON**; `=0` opts out (A/B),
  `=1` harmless explicit-on. Paravirtual/OldWorld untouched (whole block inside
  `MachineProfileIsNewWorld()` gating). NOTE the opt-out A/B semantics changed: with
  the slot-15 loud stop planted, pool-off now parks the first FE01 at 0x50429cf0
  instead of reproducing the old ~80 ms reboot loop (loud park > silent loop).
- Cold-arm invariant (rev 2 C3): the trampoline pool block is split — (a) idempotent
  constant re-asserts ([0xE0/E4/E8]) safe on every entry; (b) the `[ECB+0xEC]` wipe
  (2 words, `li r0,0; stw r0,0xEC(r28)`) carries the COLD-ARM-ONLY invariant comment.
  No structural guard exists yet (no discriminator until Task X builds R2; today
  always-cold == every-entry, PROBE-O1), so the choice made is: documented invariant +
  code structured so Task X's discriminator wraps exactly those two words.
- Slot-15 loud stop: entry-vector slot 15 (ROM+0x46e8fc, verified POWERPC_ILLEGAL==0
  before write) → `b 0x50429cf0`; parked stub `b *` at mirror 0x50429cf0 (site
  verified zero before write). PatchROM-time only (rev 2 C4); both writes
  verify-zero-first (rev 2 C6).

### Task V results (2026-06-11) — Q-C/Q-D CONFIRMED LIVE, zero falsifications

- **Implementation = the two pinned Q-C seeds, nothing else.** Env-gated
  `SS_NW_MM_SWITCH=1` (default OFF until Task Y), a 9-word trampoline extension
  (rom_patches.cpp, between the pool block and the `li r28,0` restore):
  `[KDP+0x660] |= 0x00200000` (OR, not store — the "from emulator" cr2eq bit) and
  MRU pair-0 seed `[KDP+0x340]=0x68fff400, [KDP+0x344]=0x68fff400` (key + ctx ptr
  = the MMCB; the slow-path page-table validation, residue R-6, stays bypassed).
  **Placement rationale:** both are KDP-page state — NK cold-init wipes/poisons the
  KDP page after glue-time (the [KDP+0xfd0] precedent; the MRU poison was probed live
  post-trampoline on an unseeded run), so the seeds are trampoline-resident guest
  stores (run at table[0] dispatch, after NK init). Layout: pool+switch maximal =
  46 insns ending ROM+0x429bf8, under the 0x429c00 stop stubs (msr+switch mutually
  exclusive by the quarantine below).
- **No FE07 code, no MSR write, no save-record writes** — confirming the Q-C/Q-E
  scoping: the DR's FE01 service + the staged NK switch/save/scheduler + the ROM's
  native glue at 0x500ebc20 did everything once classified "from emulator" and the
  MRU lookup hit. The NK hit path (`[pair+0x5c]:=[KDP+0x648]; [KDP-0x14]:=pair;
  b 0x50312b0c`) behaved exactly as pinned.
- **Switch⇒pool (rev 2 C11):** SS_NW_MM_POOL=0 forced alongside SS_NW_MM_SWITCH=1 is
  treated as misconfiguration — loud `[NW-TRAMP] V: MISCONFIG` line, pool re-enabled
  (switch wins). SS_M6A_USER_MSR=1 under switch-on is DISABLED loudly (Q-E quarantine
  + word budget 49>48); R-2 unchanged.
- **Probe gate (PASS):** `SS_PROBE_PC=0x500cef8c` visits 1/10/100+ —
  CTR=0x500cef8c (exact ✓), r2=0x10024754 (exact ✓), r1=0x103ffe00 (64-byte aligned,
  < 0x103ffe5c, `[r1]`=0x103ffe5d = back-chain tagged `|1` ✓), LR=0x500ecac0
  (0x500exxxx glue class ✓), r30=0x10024de8 (RD ✓); also r29=0x10024df4
  (routineRecord), r31=0x68fff740 (save block), r26=0x000000E1 (procInfo) — the whole
  Q-B/Q-D register story live-confirmed.
- **Diagnostic (recorded, not gates):** the FE01 retry spin is **GONE** — comp
  unfroze 3573→3600, jNK collapsed from ~116M/20s (bounce spin) to ~16K/50s, jDR
  carries the load. **New frontier:** TVector excursions run repeatedly (≥100 visits);
  the 68k advances deep into ROM hardware-init code and parks in a poll loop
  (r24 ring tail cycling 0x500004e4..0x5000057c region) with an MMIO storm —
  SCC reads ~7.6M/10s (IER-dominated), VIA ORB ~450K/10s — and CUDA traffic
  (packets=8905, i2c=6165 ALL to absent devices: addrs 41,4F,B5,91,80,C1,28,71,9D;
  pram_rd=2055). exc=0/1/0; [ALARM]/[STALL] watchdog fires at 15s (pre-WindowManager,
  30M blocks/s spin). Reading: the MixedMode wall is down; the next wall is
  device-model surface (I2C/PMU-adjacent probing — unmodeled targets), Task W/Y
  territory plus the named M-class frontier ladder.
- **Switch-off default boot:** baseline unchanged (0 TVector visits, comp frozen
  3573, the FE01↔NK spin signature) — seeds fully inert when gated off.
