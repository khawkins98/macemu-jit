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
