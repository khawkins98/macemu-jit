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

> **CORRECTION (2026-06-11, W2 rev 3.1 item 4 — the paragraph above is STALE):** the
> paravirtual-seed framing does not survive contact with the live newworld boot. The
> glue's `0x366080`/primary-world values are dead scaffolding (PROBE-O2 already showed
> the glue ctx pre-population dead); **the LIVE `[KDP+0x5f0]/[0x5f4]` values are
> NK-REBUILT staged addresses — `0x50313bf8` / `0x503143a0` (Q-C probe authoritative)**,
> written by NK cold-init after glue time. The warm switch-back path traverses them
> CORRECTLY (slot-0 stub → `[KDP+0x5f0]`=0x50313bf8 selector → `beq cr2` restore leg);
> retargeting them to `0x50466080` would destroy BOTH switch directions. The required
> action is **verify-and-leave** (Task X) — the retarget directive above is STRUCK.

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
- **R-10/R-11/R-12**: defined in "Task W results" below ([ECB+0xEC] ctx-save junk
  accumulation; junk FP state from the pool-word overlay; the [KDP+0x65c] world-flip
  discipline — R-12 RESOLVED by Task W2, see "Task W2 results").
- **R-13** *(W2 rev 3.1 item 7)*: stale-MMCB window — the warm arm sets
  `[KDP+0x65c]:=MMCB` and only the slot-1 region restores ECB; if slots 2/3/5 (or any
  other entry) are ever consumed while the word still holds the MMCB, their save
  protocol would write into the MMCB. Inert today (Q-B: slot 1 is the only consumed
  slot); re-check whenever a new slot becomes live.
- **R-14** *(W2 rev 3.1 item 7)*: backward-half mid-switch DEC once EE delivery is
  live — during the BACKWARD save `[0x2810]`=0 (the NK clears it on switch-back entry)
  so the W2 DEC fence is open while the native ctx is being saved; benign by
  same-values (the shim would save the registers the NK save is writing anyway) —
  recorded, not fixed.
- **R-15** *(W2 rev 3.1 item 5)*: reset-re-init — the NK's single cold-init writer of
  `[KDP+0x658]` (`stw r12,0x658(r1)` at static 0x310834) re-runs on any NK
  reset/re-init and re-garbages the word, while the W discriminator scratch
  (0x68ff6080) stays warm → the cold-arm re-seed would NOT re-run. No live NK re-init
  observed; becomes load-bearing if one ever appears (tripwire: a boot-3-class SIGSEGV
  resume at a data address).

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
- *(Task-V review fold-in, cold/warm classification for Task X)*: (a)-class — the
  `[KDP+0x660]` OR may stay on both Task-X arms (idempotent). The MRU pair-0
  re-assert is provisionally both-arms but Task X must re-evaluate: a warm
  re-assert can evict an NK-installed MRU entry → unverified slow path (R-6).
  (Task-W status: the W discriminator routes warm entries around the trampoline
  entirely, so both seeds are structurally cold-only today.)

### Task W results (2026-06-11) — round trip FALSIFIED through existing machinery; warm arm built; STOP-RULE fired on the second falsification

**Verdict: implementation, partial.** The "completion already works" hypothesis from
Task V's live evidence is FALSE; Task W landed the table[0] cold/warm discriminator +
two seeds (env-gated under `SS_NW_MM_SWITCH`, default OFF), which kills the
reset-per-excursion failure and pins the full switch-back contract — but the round
trip's last leg (the NK restore handing back the *emulator* ctx) hit the
**dual-ctx surface** and the one-iteration rule's second-falsification trigger
escalated to the stop-rule. Boots: `/tmp/taskw_boot{1..5}.log` (probe recipes inside).

**Falsification chain (each leg live-probed):**

1. **Boot 1-2 (pre-W verification, the plan's round-trip sub-contract):** legs (a)/(b)
   FAIL through existing machinery. The completion path never reaches the DR services
   (0x5046e1f4 / 0x5046e408 / 0x5046e1a0: ZERO visits) and never calls the NK switch
   service backward (0x503143a0 sampled visits all forward r3=0x68fff400; slow/error/
   save legs zero) — instead the native completion re-enters the 68k world **by
   branching through the entry-vector table** (the forward hit path plants
   `[ctx+0x5c]:=[KDP+0x648]` for exactly this), arriving at table[0] with the native
   glue's register file (warm signature r1=0x103ffa2c native stack, r24=junk;
   ≥100 re-entries/boot). The always-cold trampoline then rewrote guest[0]/[4]
   (1050 WATCH hits) and cold-started the 68k — **a 68k reset per excursion**
   (ring: `100266f2 → 0 → 1 → 5000002c`; design doc §2.4 failure mode (ii), live).
   Task V's "boot advances deep into hardware-init" was the reboot cycle, not progress.
2. **Pool-word/ctx-save overlay (boot 1, [STATIC] confirmed):** the NK ctx save
   (0x50312b0c) writes through r6=[KDP+0x65c]=ECB with stride-8 slots at
   +0xd4..+0x1fc — `[ECB+0xEC]`=saved r12 (live: 0x5046e1a0), `[ECB+0xE4]`=saved r11
   (=0x0002f072, the stub MSR fiction), and the FP-restore helper 0x50313e18 does
   `lwz r8,0xe4(r6); lfd f31,0xe0(r6)` — **the DR pool words [ECB+0xE0..0xEC] overlay
   the NK ctx FP/GPR slots**. Sub-contract leg (b) as written ("[ECB+0xEC] returns to
   pre-call value") is unfalsifiable — the in-use bitmap is clobbered to saved-r12 at
   every switch-out; allocation survives by accident (bit 0 of 0x5xxxxxxx is clear).
   The warm arm re-asserts [0xE0/E4/E8] (without it the next allocation computes the
   record PHYS base from 0x0002f072 → NK would save 0x220 bytes into low 68k RAM).
3. **Boot 3 (warm arm v1):** table[0] cold exactly once ✓, guest[0]/[4] stable ✓
   (leg (c) PASS: 6 WATCH hits = one trampoline pass + the guest's own legit vector
   install at 0x504662a4), TVector ran, completion entered the warm arm →
   slot-0 stub → NK selector service — SIGSEGV: scheduler restore resumed at
   0x68fff740 (data). Re-pin boot 4: **the completion calls table[0] with
   r3=0x000000ff — THE COMMAND BYTE rides in r3 as the NK selector** (R-4 verified:
   0xff confirmed) — and LR=0x500ef258 (its own next-resume continuation); the NK leg
   0x50313cc8 `beq cr2` → 0x50312af8 `lwz r9,0x658(r1)`: **the switch-back restore
   target is [KDP+0x658]** — live GARBAGE (=1; no NK writer exists — it is a
   Trampoline-init surface). *(AMENDMENT 2026-06-11, W2 rev 3.1 item 5 — the "no NK
   writer exists" clause is FALSIFIED: there is exactly ONE cold-init writer in the
   NK image, `stw r12,0x658(r1)` at static 0x310834, dump-verified — the live-garbage
   source. It runs during NK cold-init, PRE-table[0], so the W cold-arm seed wins;
   there is NO post-init/switch-path writer. The Trampoline-init-surface conclusion
   stands; the writer census is corrected. Consequent residue R-15: an NK
   reset/re-init re-runs 0x310834 while the discriminator scratch stays warm.)*
   Semantics confirmed by the paravirtual CR-injection
   (`[[KDP+0x658]]+0xdc` = parked emulator saved CR; glue :2276, main_unix :2678).
4. **Boot 5 (warm arm v2, + cold-arm seed [KDP+0x658]=ECB): SECOND falsification —
   the self-switch.** The seed is consumed and the NK switch completes, but
   **[KDP+0x65c] (save-target consumed by the slot stubs) and [KDP+0x658]
   (restore-source) are the same block (ECB)**: the warm save overwrites the parked
   emulator ctx (forward-saved resume [ECB+0xfc]=0x5046e1a0 → replaced by native
   LR=0x500ef258), and the restore hands back the just-saved native state — the
   native world resumes at its own continuation having switched to nobody. The
   continuation 0x500ef258 then faulted reading `[saveblk+0x3c]` (rewritten by the
   glue's $AAFE tail-call block 0x500ef220-254 to 0x486eff9e — sub-finding: the
   completion primitive's [r31+0x3c] handling includes a tail-call rewrite when the
   68k resume instruction is itself $AAFE-class; raw datum, not yet decoded).

**The corrected contract (pinned for the re-scope):** the architectural switch-back
is: native completion writes results + `[saveblk+0x3c]`=resume 68k PC (0x500ed400:
`stw r25,0x3c(r31)`; live r25=0x5000fcf2 = the post-$AAFE PC, derived from the Q-D
probe and the ring's `5000fcf2 → 10024dea/10024de8` $AAFE call-site chain), then
calls table[0] with r3=0xff and LR=its continuation; the NK saves the outgoing
(native) ctx into [KDP+0x65c] and restores [KDP+0x658] → resume the parked FE01
service at [ECB+0xfc]=0x5046e1a0 → frame protocol → FE07 reads command 0xff → NE →
e1f4 reloads r24/D/A from the save block → 68k resumes. **The missing surface is the
[KDP+0x65c] current-world flip discipline**: it must hold ECB while the 68k world
runs (forward switch parks the emulator there) and the MMCB during native excursions
(warm switch-back parks the native ctx there — self-consistent with Q-F's
"re-switches to the native ctx which resumes at 0x500ef258"). No NK code maintains
either word — both are Trampoline-init surfaces; on real hardware the REAL slot-stub
bodies (Trampoline-written; the raw ROM has `twi` placeholders AND nop'd stub pages)
plausibly encode the flip. Implementing it touches the slot-1 stub body (the WORKING
forward direction) — beyond this task's one-iteration budget ⇒ **stop-rule trigger 1
(re-scope), not improvisation**. The candidate fix for the next iteration: warm arm
sets [KDP+0x65c]:=MMCB before `b 0x5046f900`; slot-1 stub body (ours, patch_68k_emul)
prepends [KDP+0x65c]:=ECB (idempotent on the first call); both env-gated with the
switch. Risk to re-check: the M3a DEC shim also consumes [KDP+0x65c] — the flip is
architecturally right for it (interrupted-world ctx) but unverified.

**What landed (kept, env-gated `SS_NW_MM_SWITCH=1`, default OFF):**
- table[0] → cold/warm discriminator at mirror 0x50429d00 (24 words, NEW
  verified-zero region above the 0x429c40..0x429cf0 stops; trampoline untouched at
  46/48). Cold arm: scratch 0x68ff6080 test-and-set (the Task-X R2 reserved word,
  used minimally — Task X refines), + seed [KDP+0x658]=ECB, → trampoline. Warm arm:
  pool-constant re-assert [ECB+0xE0/E4/E8] (overlay repair; [0xEC] untouched —
  in-use record live at warm entry), restore r28 (CTR stash), → 0x5046f900 (the
  displaced original slot-0 stub). Clobbers r0/cr0/CTR only (ABI-volatile; the
  slot stubs clobber CTR by design).
- The [ECB+0xEC] wipe + V seeds are now STRUCTURALLY cold-only under switch-on
  (rev 2 C3 upgraded from documented-invariant to structural there).
- Switch-off: region not written, table[0] → trampoline direct, byte-identical
  behavior (verified: the generic branch encoder reproduces 0x4BFBB280).

**Sub-contract scoreboard:** (a) FE01→TVector→switch-back round trip: **FAIL** (the
self-switch; the 68k never resumes at 0x5000fcf2 — e1f4/e408/e1a0 all zero visits in
every boot). (b) [ECB+0xEC] pre-call restoration: **premise falsified** (ctx-save
overlay; replaced by the warm re-assert + residue R-10). (c) table[0] cold exactly
once + guest[0]/[4] stable after first entry: **PASS** (boots 3/5, probe + watch).

**Residue updates:** R-4 **resolved** (command byte = 0xff, rides in r3 as the NK
selector). R-5 **corrected** (resume route = table[0]/slot-0/selector-0xff; e1a0 is
the post-restore resume, blocked on the flip discipline). NEW **R-10**: [ECB+0xEC]
in-use word accumulates ctx-save junk (bits 1/3 phantom-used; depth ceiling ~2 of 4
records until the overlay is resolved). NEW **R-11**: the NK FP-restore reads
[ECB+0xE0/E4] as FPSCR/flags and `lfd f31` from the pool words — junk FP state during
switches (inert so far; the 68k world barely uses FP). NEW **R-12 (THE frontier)**:
the [KDP+0x65c] current-world flip discipline — the named re-scope item, candidate
fix above.

### Task W2 results (2026-06-11) — the world-flip discipline: ROUND TRIP PASSES; new frontier = the sc/syscall_entry wall

**Verdict: R-12 RESOLVED, zero falsifications of the flip contract.** The rev 3.1
corrected design landed faithfully (all env-gated `SS_NW_MM_SWITCH=1`, default OFF;
boots `/tmp/w2_boot{1,2,3}.log` + `/tmp/w2_boot_off.log`):

1. **Slot-1 flip region** (rev 3.1 item 1): retargeted 7-word region at verified-zero
   mirror 0x50429d80 — `mtctr r1 / lwz r1,0x2804(0) / lis+ori r0=ECB /
   stw r0,0x65c(r1) / mfctr r1 / b 0x5046fa00` (clobbers r0/CTR only). The MIRROR
   table word at ROM+0x46e8c4 retargeted with verify-EXPECTED == 0x4800113c
   (the verify-zero idiom's sibling for nonzero sites); switch-off: neither write.
   Live: `[PROBE 0x50429d80 visit=1]` with the forward-switch register file
   (r3=0x68fff400 MMCB, r4=0x00200000 the cr2eq bit, LR=0x5046e1a0).
2. **Warm-arm flip** (item 2): 5 words ([KDP+0x65c]:=MMCB via explicit r28=KDP
   re-derive) after the pool re-asserts, before `mfctr r28 / b 0x5046f900`.
   Region re-derived: **30 words, 0x429d00..0x429d74 (end exclusive 0x429d78)**,
   verify-zero-first over the whole enlarged region; bne offset +0x24→+0x28.
3. **DEC fence** (item 3): `deliver_pending_dec_exception` defers while
   `[XLM_RUN_MODE]` (0x2810) != 0 — new `deferred_native` counter (4th field of the
   `exc=` heartbeat tuple + `[EXC]` crash line). Live: exc=0/1/0/0 — inert as
   predicted (delivered=0 regime).
4. **Hardening** (item 6): cold-arm `[KDP-0x14]:=ECB` one word after the [KDP+0x658]
   seed (register state verified: r28=KDP, r0=ECB live there).
5. **Corrections** (items 4/5): the 0x658 writer census amendment above (writer at
   static 0x310834 dump-verified); the §1.3 wrinkle paragraph corrected
   (verify-and-leave; retarget STRUCK); residues R-13/R-14/R-15 registered.

**Round-trip sub-contract (carried from W) — PASS:**
- (a) TVector `0x500cef8c` visit=1 (register file exactly the Q-D table; r25=0x5000fcf2
  the post-$AAFE PC). Ring (boot 2): the $AAFE call chain
  `… 5000f4xx (CFM caller region) → 5000fce2..5000fcf0 (jsr (a4)) → 5000fcf2 →
  10024dea/10024de8 → (excursion) → 5000dfa2 …` — continued 68k execution, NO reset
  signature (pre-W2: every excursion ended `100266f2 → 0 → 1 → 5000002c`). The
  literal post-excursion re-record of 0x5000fcf2 is masked by the ring's 4-entry
  dedup (it sits 2 entries before the RD); the SECOND excursion provides the
  unmasked direct evidence: ring `… 5000dfc8 → 100266f2 → 0 → 1 → 50033776
  50033778 …` where **[saveblk+0x3c] probed at e1f4 = 0x50033776** (boot 3,
  `[r5:0x60]` dump: save record +0x3c = 50033776, +0x30 = 10024de8 the RD) — the
  completion-written resume PC IS the next 68k PC in the ring. Disassembly closes
  the first chain: 0x5000fcf0 `jsr (a4)`, 0x5000fcf2 `move.w d0,d7 … rts` (the
  MixedMode result epilogue).
- (b) Completion-side DR services EXECUTE: `0x5046e1a0` visit=1 (the parked FE01
  service resumed — the leg that was structurally impossible pre-W2) and
  `0x5046e1f4` visit=1 with **r6=0x000000ff — the command byte read live** (probe
  granularity is block-entry, so counts are events, not executions). e408 (free
  path): 0 block-entry visits — recorded, likely subsumed into a chained block.
- (c) Cold-once + guest[0]/[4] stability: trampoline cold write exactly once
  (WATCH record #4666 pair); the only later guest[0]/[4] writes are the guest's own
  legit 68k vector install (pc=50490e00, r24=0x500389fe guest code) — same class as
  W boot 3's 0x504662a4. No reset transitions.

**Diagnostic (recorded, not a gate) — the next frontier, captured honestly:**
the boot is TRANSFORMED. The V-era "hardware-init poll loop + SCC/CUDA MMIO storm"
reading is dead (it was the reset cycle): CUDA now 13 packets/9 i2c (vs 8905/6165),
jNK 4104 (vs 116M), one pass. The chain runs: TVector → MPLibrary init →
**the CFM parcel-by-name caller region RUNS** (ring: straight-line 0x5000f400..f466
= file 0xf4xx) → second MixedMode round trip → **`sc` at pc=0x500d638c with
unresolved syscall entry — SRR0=0x500d6390 SRR1=0x00007072 lr=0x500cf108
r1=0x103ffb50** (the [EXC] FATAL capture-abort). MPLibrary's init does NOT return
yet: it advances to its first kernel service call. This is EXACTLY stop-rule
trigger 2 / plan Task-Y named wall (i) — the vector-0xC00 syscall_entry frontier,
the NEXT milestone's named problem; no staging beyond the existing abort-capture.
(Under `SS_EXC_SC=legacy` the boot survives the sc and wedges in a 52M/s comp-frozen
spin at 3672 — the legacy path is not a viable bridge; diagnostic only.)
*(Update 2026-06-11: RESOLVED by the NK-syscall-surface milestone —
`syscall_entry=0x50314ac0` is the newworld default; see `M3A-ENTRY-TABLE.md`
"Syscall entry resolution". The FE1F service surface frontier is in turn RESOLVED
(FE1F-service-surface milestone, same date, default-on `be0e02cb`); the current
frontier is the DSAT stack-underflow wall — see "Task C results" below.)*

**Switch-OFF boot:** byte-identical baseline preserved (no W/W2 region writes, no
slot-1 retarget, 0 TVector visits, the FE01↔NK spin signature) — see
`/tmp/w2_boot_off.log`.

### Task X results (2026-06-11) — collapsed scope (plan rev 3/3.1): verify-and-leave + re-census + sub-contracts; R3 ratified

Scope after the W/W2 collapse: the `[KDP+0x5f0/4]` verify-and-leave check, the
post-W2 re-census (rev 2 P1), fresh re-assertion of the X sub-contracts, the W2
review-minor fold (slot-1 flip gating), and the scratch-word protocol disposition.
Evidence boots: `/tmp/taskx_boot1.log` (switch env-on, 6 probes + WATCH 0/4 +
r24 ring) and `/tmp/taskx_boot2_legacy.log` (SS_EXC_SC=legacy, heartbeat capture).

1. **`[KDP+0x5f0/4]` verify-and-leave [PROBE✓]** (rev 3.1 item 4 — the retarget is
   STRUCK): at table[0] COLD entry visit=1 — the earliest post-NK-init observable —
   `[KDP+0x5f0]=0x50313bf8` `[KDP+0x5f4]=0x503143a0` (the NK-rebuilt staged pair),
   and byte-identically at the first WARM entry (0x50429d3c visit=1). The glue's
   primary-world seeds (`0x50366080`, sheepshaver_glue.cpp seed site) are therefore
   **dead on arrival** — NK cold-init overwrites them before the first dispatch.
   CHOICE: comment-only, seeds kept (a pre-NK-rebuild reader inside NK cold-init
   cannot be excluded without instrumenting NK init; the known consumers — the
   slot-stub exits — first run post-rebuild). Documented at the seed site.
2. **Post-W2 re-census (rev 2 P1) — R3 stub route RATIFIED.** table[0] census with
   the switch on: COLD exactly once (0x50429d00 visit=1: r3=0, scratch
   [0x68ff6080]=0 → set to 1). WARM (0x50429d3c) visit=1 carries exactly the
   legitimate completion signature: **r3=0x000000ff (the command byte / NK
   selector), r1=0x103ffa2c (native stack), scratch=1** — and no other warm-entry
   class appears; neither probe reached the visit=10 sample (total table[0]
   entries < 10 in the boot-to-sc-wall window, consistent with the two known
   excursions). Slot-15 exhaust stop 0x50429cf0: zero visits; no parked-PC stop
   entered. **Census verdict: nothing enters table[0] warm besides the completion
   path — the R3 choice (stub route, landed in W) is ratified.**
3. **X sub-contracts re-asserted from fresh evidence (all PASS):**
   - *Cold path exactly once*: one trampoline guest[0]/[4] write pair (WATCH
     record #4666, pc=50429b40), scratch 0→1 once.
   - *guest[0]/[4] stable*: the only later writes are the guest's own legit 68k
     vector install (pc=50490e00, r24=0x500389fe guest code — the known class);
     no reset transitions.
   - *jDR/comp growing across re-entries*: fresh legacy-sc heartbeat —
     comp=3672 (vs 3600 cold-only post-V, 3573 pre-V frozen), jDR=1432M and
     growing, jNK=4104 flat (no bounce spin), exc=0/1/0/0 (DEC fence inert);
     JIT first-compiles continue across both excursions in the probe boot.
4. **W2 review minor FOLDED** (rom_patches.cpp): the slot-1 flip block now arms
   only when the W discriminator actually landed (`tbl0_target == w_offset`);
   otherwise a loud `[NW-TRAMP] W2: slot-1 flip SKIPPED` line — a half-armed flip
   (ECB half with no MMCB half) would re-open the Task-W boot-5 self-switch.
5. **Scratch-word protocol**: the minimal test-and-set landed in W is CONFIRMED
   sufficient — the census shows binary cold/warm discrimination is the only
   consumed semantics; no refinement needed. (The word stays reserved at
   0x68ff6080 in the occupancy map; R-15's reset-re-init tripwire unchanged.)

### Task Y results (2026-06-11) — rung-2 acceptance PASS (pre- and post-flip); SS_NW_MM_SWITCH is the newworld DEFAULT

**Fix budget consumed: ZERO** — no contract falsified at acceptance; no
acceptance-time unlocks needed.

**Pre-flip battery (env `SS_NW_MM_SWITCH=1`; boots /tmp/taskx_boot1.log,
taskx_boot2_legacy.log, tasky_ring.log):**
- (a) Canonical gates: build-ss OK; batch + plain test-jit 353/353 score=100;
  machine tests 11/11 ALL PASS (4794 checks); e2e-test 122 passed; paravirtual
  `make e2e` PASS (clean lifecycle — booted to Finder, clean shutdown, exit 0;
  the trampoline lambda early-returns on non-newworld, so paravirtual is
  structurally byte-identical).
- (b) Pool provisioned (mm-pool=ON default), slot-15 exhaust stop 0x50429cf0:
  zero visits. PASS.
- (c) Consumed-slot gate (Q-B: slot 1): the slot-1 route executes —
  0x50429d80 visit=1 with the forward-switch signature (r3=0x68fff400 MMCB).
  Nonzero counter; NOT vacuous. PASS.
- (d) Round trip: TVector visit=1 (r25=0x5000fcf2); fresh ring reproduces the W2
  pattern — `… 5000f45a..f466 (CFM caller) → 5000fce2..fcf0 (jsr (a4)) →
  5000fcf2 → 10024dea/10024de8 → 5000dfa2 … (continued, no reset)`, and the
  second excursion's resume is literal: `… 5000dfc8 → 100266f2 → 0 → 1 →
  50033776 …` with **[saveblk+0x3c] probed at e1f4 = 0x50033776** and
  **e1f4 r6=0x000000ff** (command byte live). PASS.
- (e) Cold-once + guest[0]/[4] stable: PASS (Task X evidence, same tree).

**FLIP:** `SS_NW_MM_SWITCH` promoted to newworld profile default-ON
(rom_patches.cpp; opt-out `SS_NW_MM_SWITCH=0`, mirroring the pool's polarity).
Misconfig logic adjusted: `SS_NW_MM_POOL=0` under the (now-default) switch is a
loud misconfig + pool forced on; a pool-off A/B now requires `SS_NW_MM_SWITCH=0`
as well. Paravirtual/OldWorld untouched.

**Post-flip battery (NO switch env var — true default; boots
/tmp/tasky_flip_boot1.log, tasky_flip_hb.log, tasky_optout_boot.log):**
- (a) Full canonical gates re-run: build-ss OK; batch + plain test-jit 353/353;
  machine 11/11 ALL PASS; e2e-test 122 passed; paravirtual `make e2e` PASS. ✓
- (b) mm-pool=ON + mm-switch=ON (default) in the fixup line; slot-15 stop zero
  visits. ✓
- (c) Slot-1 route visit=1 (r3=0x68fff400). ✓
- (d) Round trip holds: TVector r25=0x5000fcf2; warm entry r3=0xff native stack;
  e1f4 r6=0xff, [saveblk+0x3c]=0x50033776. ✓
- (e) Cold-once (scratch 0→1 once, WATCH #4666 pair) + guest[0]/[4] stable (only
  the guest's own vector install at pc=50490e00). ✓
- Opt-out A/B (`SS_NW_MM_SWITCH=0`): byte-identical pre-switch baseline — comp
  frozen 3573, jNK 116M FE01↔NK bounce spin, 0 TVector visits, no W/W2 region
  writes, no slot-1 retarget. ✓

**FLIP STATUS: LANDED** (no gate failure; no revert).

**Diagnostic outcomes (recorded, NOT gates) — the next-milestone baseline,
re-confirmed on the default config:** the FE01 retry spin is gone; MPLibrary
init runs; the CFM parcel-by-name caller region executes (ring, file 0xf4xx);
the boot advances through TWO MixedMode round trips and dies at **the sc wall**:
`[EXC] FATAL: sc at pc=500d638c with unresolved syscall entry (SRR0=500d6390
SRR1=00007072 msr=00007072 lr=500cf108 r1=103ffb50)` — identical pre- and
post-flip. Heartbeat baseline: comp=3672, jNK=4104, jRAM=0, exc=0/1/0/0
(DEC fence inert), mmio=S:65540/V:70183(IER=65539,ORB=3848). CUDA baseline:
13 packets / 9 i2c all to absent devices (addrs 41,4F,B5,91,80,C1,28,71,9D),
pram_rd=3, adb=0 — quiet (the V-era storm reading is dead). **Nothing new
appears between the round trip and the sc wall.** MPLibrary's init does not
return; vector-0xC00 syscall_entry is THE frontier (stop-rule trigger 2:
captured, stopped — no staging beyond the abort-capture). R-1 note: the
slot-15 stop never fired through acceptance — 4-record pool sizing stands.
*(Update 2026-06-11: the syscall_entry frontier is RESOLVED — NK-syscall-surface
milestone, newworld default `syscall_entry=0x50314ac0`; the FE1F service surface
frontier that followed is ALSO resolved (FE1F-service-surface milestone, default-on
`be0e02cb`) — current frontier = the DSAT stack-underflow wall. See
`M3A-ENTRY-TABLE.md` + `M6A-WAVE2-SHIM-RECON.md` "Frontier update (FE1F Task C
closeout)" + "Task C results" below.)*

---

## FE1F native callout (selector 0x31) — fe1f-service-surface plan Task 0 (2026-06-11)

> Recon addendum for `docs/superpowers/plans/2026-06-11-fe1f-service-surface.md`
> (body + Rev 2, Rev 2 binding). **All blocking answers PINNED.** Boots used:
> **3 of ≤8 newworld** (`/tmp/fe1f0_{1,2,3}.log`, 45 s SIGTERM + SS_TERM_DUMP,
> default newworld config `/tmp/m2accept.prefs`); paravirtual donor boot
> **skipped** (0 of ≤1 — Q-F2(b) pinned statically, the donor datum is moot:
> the "writer" is not a writer at all, see Q-F2). Tag discipline carried:
> [RAW-ROM]=/tmp/rom901_inventory.bin, [PATCH]=patch source / patched dump,
> [STATIC]=capstone of the dumps, [PROBE✓]=live-verified this session.

### Provenance (re-established)

- `/tmp/rom901_inventory.bin` md5 `7b1378be15d99ac1a15ab2fc22bcc56f` (raw; the
  16 `twi` placeholders `0fff0000..0fff000f` at 0x36e8c0 confirmed, incl. the
  slot-14-duplicates-0x0d quirk); `/tmp/rom901.bin` md5
  `e432df64122a5a89ec1c08d0bdf01609` (patched) — both match the recorded anchors,
  no re-dump needed.
- The three free static wins re-confirmed raw==patched: file 0xf240 region
  (`4e56 0000 / 7031 / fe1f / 2d40 000c / 202e 0008 / 6704 / 2240 2288` + the
  selector-$36 sibling at 0xf260), dispatch slot 0x3ff0f8 (`80bf09dc 4bf6ea48`),
  service body 0x36db44 (mtlr r5 / marshal / blrl / unmarshal / dispatch-resume).
  **Provenance NOT falsified — branch (ii) stands; branch (i) stays dead.**

### Q-F1 — the dispatch route [PROBE✓] — PINNED (one address correction)

- **Plan typo corrected**: the mirror dispatch slot is
  `0x50480000 | (0xFE1F<<3) = 0x504FF0F8`, not the plan's `0x504F70F8`.
- Live slot words `[0x504FF0F8]=0x80bf09dc [0x504FF0FC]=0x4bf6ea48` [PROBE✓] —
  exactly the static `lwz r5,0x9dc(r31); b →0x5046db44` pair, relocated.
- Body `0x5046db44` visited (visit=1) at the park boot with `r5=0x5046e8e0`
  already loaded by the slot — **route = opcode table slot → body → blrl,
  [PROBE✓]**. The 0x2C stop-stub did NOT catch it (no falsification; trigger 3
  not invoked). NK twi decoder 0x50314700: zero visits (expected — see Q-F2).

### Q-F2 — the continuation chain + slot 8's real identity — PINNED (THE finding)

**(a) Confirmation probes [PROBE✓]:** `[ECB+0x9dc]` = `[0x68fff9dc]` =
**0x5046e8e0** (T-C1 confirmed). Live mirror bytes at 0x5046e8e0..ec =
`4bfbb390 4bfbb39c 4bfbb3a8 4bfbb3b4` — **NOT zero**: rung-2 Task U's parked-stop
branches (slot 8 → `b 0x50429c70`, slots 9-11 → their stops). The Rev-2
"zero/placeholder expected" is amended: slot 8 holds the Task-U loud stop.

**(b) The real installer of slot 8 — STATIC, fully closed: NOBODY writes slot 8.
The raw `twi 31,r31,8` placeholder IS the mechanism.** On real hardware the
entry-vector slots are not patched by any init; executing a table slot raises a
**program interrupt (0x700)**, and the NK's published 0x700 handler decodes it:

- NK 0x700 handler = **0x50314700** [STATIC raw==patched; PROBE✓ live:
  `[KDP+0x37c]`=`[0x68ffe37c]`=0x50314700 — the vector-handler-table recipe
  `[KDP+0x360+(vector>>6)]` re-validated against `[KDP+0x390]`=0x50314ac0 = the
  sc surface's known entry]. The handler checks the faulting PC against
  `[KDP+0x648]` (=0x5046e8c0 live [PROBE✓]); table offsets {0, 0xc, 0x20, 0x40}
  and ranges dispatch via the **exit-pointer array `[KDP+0x5f0+4·slot]`** with
  the per-slot counter `[KDP+0xe40+4·slot]++` and **r10 = caller LR** (= the
  FE1F body's post-blrl return — the callout protocol). Off-table twi sites
  decode the instruction word instead (`lwz r8,0(r10); xoris r8,r8,0x0fff`).
- The exit-pointer array is built by **NK cold-init** (file 0x311770-0x311820
  [STATIC]): default-fills all 16 entries with NK+0x46d0 (loud "unimplemented,
  r8=2" exit), then overrides slots 0-8 and 15. **Slot 8 = NK_base+0xaca0 =
  0x5031aca0** (slots 0/1 = +0x3bf8/+0x43a0 — exactly the rung-2 [PROBE✓] values
  0x50313bf8/0x503143a0, closing rung 2's "writer never identified" residue for
  the exit pointers; NK_base = `[KDP+0x64c]` = 0x50310000 [PROBE✓]).
  **`[KDP+0x610]` = 0x5031aca0 [PROBE✓ live]** — the publication ALREADY runs on
  our boot.
- **0x5031aca0 = the NK selector-service gateway** [STATIC]: saves r14+ into the
  ctx (helper 0x503238ac), reads the **selector from `[ctx+0x104]` = the trapped
  context's saved r0** (ctx layout +0xfc=PC, +0x104=r0, +0x10c=r1 — Q-B's stride
  confirms), bounds-checks vs **0x86**, and dispatches through the offset table
  at NK+0xacb8: `target = table[sel] + NK_base + 4·sel`. **This is the same
  dispatcher the 5 sc deliveries traverse** ([PROBE✓]: 0x5031aca0 visited on the
  park boot with r3=0x00050001 = the first sc's register row) — the gateway,
  common exit (0x5031b124 → ctx restore 0x5032391c → resume), lock idiom, and
  ExcEnter conventions are all already exercised 5× successfully per boot.
- **Selector 0x31 body = 0x5031d204** [STATIC] (table entry at 0xacb8+0xc4 =
  0xd140; 0xd140+0xc4+0x50310000): allocates **0x20 bytes from the NK kernel
  pool** (bl 0x5032281c), brands it `'EVNT'` with self-pointing list head
  (+8/+0xc = an empty event queue), takes the kernel lock, **registers it with
  id 9** (bl 0x503251b0), stores `[SPRG0-8]+0x60` at +0x14, and returns
  **r4 = the object pointer, r3 = 0 (noErr)**; registration failure frees the
  block and returns r3=0 with r4 unset; alloc failure → r3=-4. Selector 0x31 =
  "create/register the EVNT kernel object" — a CFM-prep native service.

**Verdict: V2' (population is the fix; value pinned) — with the route named
honestly.** The slot-8 service is **staged** (the whole chain is real NK code in
the staged image; the publication init already runs). The pinned population value
for mirror vector slot 8 is **the raw placeholder `0x0fff0008` itself**
(verify-EXPECTED first: current content = Task-U stop branch `0x4bfbb390`), and
the service contract is reached via **twi → 0x700 program-interrupt delivery to
the NK-published handler [KDP+0x37c]=0x50314700** — the sc-surface idiom one
vector over. **STOP-RULE trigger 1 (V3, unstaged service): NOT FIRED.**
**RE-SCOPE FLAG for the coordinator (recorded, not improvised):** the plan body's
"no powerpc_cpu changes / no host-side execution seam" assumption is falsified —
the FE1F surface needs a **0x700 EXC delivery class** (exc_core currently has
EXC_SC + interrupt only; `twi` is undecoded in the CPU core). This is
exception-delivery surface (the precedented EXC/sc idiom), NOT an HLE NativeOp
(the named-wrong fix stays wrong — the service body runs as real guest NK code).
Task A's shape changes from "seed a pointer" to "restore placeholder + deliver
0x700"; the plan owner must ratify before Task A starts.

### Q-F3 — selector-0x31 semantics + the conformance vector — PINNED

**Callout-entry register table (body 0x5046db44, block-entry = pre-marshal)
[PROBE✓ visit=1]:**

| Reg | Value (live) | Class / meaning |
|---|---|---|
| r8 (=D0) | **0x00000031** | the selector — MANDATORY exact row |
| r9 (=D1) | 0x00000002 | the requested id (id=2 at the live park) |
| r16 (=A0) | 0x100037c0 | the ExpandMem pointer-array base (pointer-class; = `[[0x2b6]+0x310]`) |
| r17 (=A1) | 0x103ffc74 | stack-class |
| r24 | 0x5000f248 | 68k PC (post-trap-word) |
| r5 | 0x5046e8e0 | continuation = [ECB+0x9dc], pre-loaded by the dispatch slot |
| r0, r3 | 0 / 0x5000e454 | pre-marshal (marshal `mr r0,r8 / mr r3,r16 / …` happens inside the block) |

The DR register-file convention (r8..r15=D0-D7, r16..r22=A0-A6) is
**probe-confirmed** at the FE1F site (the body's marshal caveat is resolved).
Post-marshal ABI at the blrl: r0=0x31(selector), r3=A0, r4=D1(id), r5=A1, … .
NOTE: the NK service consumes the selector from **ctx+0x104 (saved r0)**, not
from a marshaled GPR — delivery must preserve r0 through the exception save.

**Return predicate (T-C2 confirmed):** r3(→D0) = **0 = noErr** (status stored at
0xc(a6); error arms store nonzero — 0xffff8d8e availability-check class,
−50/paramErr class); r4(→A0) = **pointer-class** (the EVNT kernel-pool object) —
the 68k tail `movea.l d0,a1; move.l a0,(a1)` stores **A0** through the stack-arg
slot pointer. The 0xf250 `beq.s` is the null-ARG guard (`move.l 8(a6),d0` —
the pointer arg reload), NOT a result test — T-C2's deletion of the old
"d0==0 skip-arm" gate stands.

**id→index map (68k table at 0x5000e3a0, 12-byte entries {id, index, flags},
raw==patched [STATIC]):** id1→idx1(flags 0x8), id3→idx1(0x4), id4→idx1(0x2),
id5→idx4(0x8000), id2→idx7(0x80000000). Unknown id → error −50.

**THE index for the f240 site: id=2 [PROBE✓ r9] → index 7.**

**Slot-address recipe [PROBE✓ end-to-end]:** `[0x2b6]` = ExpandMem (live
0x10003420) → `[ExpandMem+0x310]` = array base (live **0x100037c0**, == A0 at the
callout) → slot = base + 4·(index−1). Live slot for index 7 = **0x100037d8**,
value 0x00000000 (empty — the `tst.l (a3)` arm that triggers the FE1F call).
Index-1 slot [0x100037c0]=0x00000002 (pre-filled, non-pointer-class — recorded).
**Task B's watch address: 0x100037d8** (SS_JIT_WATCH_ADDR hex +
SS_JIT_TRACE_RING=1; probe-field fallback `[0x100037d8]`).

**Diagnostic map:** the sibling stub at file 0xf260 = selector **$36**
(`link / movea.l $c(a6),a0 / move.l 8(a6),d1 / moveq #$36,d0 / fe1f`) — called
by the e3e0 routine right after a successful slot fill (args: slot value +
table index) — the SECOND callout the advancing boot will issue.

### Q-F4 — the park mechanism, NAMED — PINNED (H1)

- **PRIMARY (probe visit counts):** body 0x5046db44 visit=1 (no visit=10 line);
  post-return unmarshal 0x5046db6c **ZERO visits** — the callout NEVER returns.
  blrl target 0x5046e8e0 visit=1, then the Task-U branch; **the park PC is the
  slot-8 parked stop 0x50429c70 [PROBE✓ visit with r0=0x31, r24=0x5000f248]** —
  a `b *` self-loop (heartbeat-silent, wall-clock continues: the exact captured
  baseline signature). Rev-2's prediction ("blrl → 0x5046e8e0") refined by one
  hop: blrl → slot 8 → `b 0x50429c70` → self-loop.
- **Ring corroboration:** 5000f242/f246/f248 each appear EXACTLY ONCE in the
  full 839,284-transition ring (taskC3 + fe1f0_1, identical). Dedup semantics
  re-verified in writing: the ring skips only values matching the LAST 4
  recorded (ppc-cpu.cpp r24ring_record) — a genuine e3e0→f240 re-loop would
  re-record; capacity 2M entries (1<<21), idx monotonic ⇒ 839,284 ⇒ never
  wrapped. (DIAGNOSTICS.md M6a ring section.) **H1 (one-shot-never-returns)
  CONFIRMED; H2 (FE07-style poll) dead.**
- **Exit path:** the body's unmarshal+dispatch-resume (0x5046db6c..db90) is the
  only return route [STATIC] — Task B's gate (a) (db6c ≥1 visit) is meaningful
  (H1 regime, P-M5 satisfied).
- **World-flip/MSR:** the current park performs none (self-loop; no MSR writes
  in slot/body). The real continuation runs in the NK exception regime — MSR
  routing identical in class to the sc surface (SRR0/SRR1 save, EXC discipline);
  the service is allocate+register with **no tick/EE dependency** — EE-chain
  contingency (trigger 1 via EE-required) NOT invoked. Residues recorded:
  [KDP+0x65c]/[0x2810]/XLM_IRQ_NEST untouched by the FE1F body [STATIC].

### Q-F5 — staged-surface audit of the slot-8 service — VERDICT: GO

Bounded enumeration (≤12 functions consumed: 0x700 handler 0x314700, init
cluster 0x3117xx, gateway 0x31aca0 + dispatcher 0x31aed0, ctx-save helper
0x3238ac, selector body 0x31d204, common exit 0x31b124, pool alloc 0x32281c,
registration 0x3251b0, pool free 0x3229a0 (by symmetry), lock 0x312700 — all
[STATIC] on raw==patched bytes):

| Structure | Where | Status |
|---|---|---|
| Exit-pointer array `[KDP+0x5f0..0x62c]` + `[KDP+0x610]`=0x5031aca0 | KDP | **published live [PROBE✓]** |
| Vector-handler table `[KDP+0x37c]`=0x50314700 | KDP | **published live [PROBE✓]** |
| `[KDP+0x648]`/`[KDP+0x64c]` | KDP | 0x5046e8c0 / 0x50310000 [PROBE✓] |
| ctx save area (+0x104 = saved r0 = selector) | NK ExcEnter | exercised 5×/boot by the sc surface |
| NK kernel pool (lock KDP-0xad0, free list KDP-0xab0, counters KDP-0x430) | KDP-rel | free-list head `[0x68ffd558]`=0x68ff7008 ≠ self ⇒ **backed, non-empty [PROBE✓]** |
| Registration ID table `[KDP-0xa98]` | KDP-rel | =0x68ffd240 nonzero [PROBE✓] (8-byte entries, cap 0x1fa) |
| `[SPRG0-8]+0x60` per-CPU datum | NK regime | ExcEnter-established (sc-exercised) |
| Selector counter array `[KDP+0xef4]` | KDP | =0 ⇒ counting skipped [PROBE✓] |

**Verdict in writing: the selector-0x31 service runs on staged state.
Seed-class fix list for Task A: EMPTY** — no data seeds. The fix is
**route-class** (two items, pinned): (1) mirror vector slot 8 := raw placeholder
`0x0fff0008` (verify-EXPECTED: current = 0x4bfbb390 Task-U stop; PatchROM-time,
env-gated `SS_NW_FE1F_SURFACE`); (2) twi → 0x700 EXC delivery to the published
`[KDP+0x37c]` (new EXC class + CPU-core twi raise — **the Q-F2 re-scope flag;
coordinator ratification required before Task A**). The P-C1 V1-with-empty-list
middle case does NOT apply (a concrete provisioning obligation exists).
Nothing EE/tick-shaped appears in the service (EE-CHAIN cross-ref: contingency
only, untriggered).

### Blocking-answer table (Rev-2 P-C1) — ALL PINNED

| Task | Blocks on | Status |
|---|---|---|
| A | Q-F1 + Q-F2 (value+installer+route verdict) + Q-F3 (entry table) + Q-F5 verdict (GO + fix list) | **ALL PINNED** — but gated on the Q-F2 re-scope ratification (0x700 delivery surface) |
| B | Q-F3 (return predicate) + Q-F4 (park mechanism + exit path) | **PINNED** |
| C | Q-F5 full enumeration | **PINNED** |

### Residues (explicit)

- R-F1: the plan's `0x504F70F8` typo (correct: 0x504FF0F8) — Task Z carries the
  correction into the plan doc if edited again.
- R-F2: `[0x100037c0]`=2 (index-1 slot pre-filled with a non-pointer) — meaning
  unknown, not load-bearing for index 7; revisit if ids 1/3/4 misbehave later.
- R-F3: the e3e0 routine's post-fill call chain (f260/$36 stub operand
  alignment hand-decoded, not capstone-verified — capstone-M68K eats fe1f).
- R-F4: 0x50429c70 probe printed visit=1 only — the self-loop is chain-compiled
  (no dispatcher re-entry), so visit counts there understate spin; park identity
  rests on the r0/r24 row + heartbeat silence, which agree.
- R-F5: the NK pool's grow/backing limits unprobed (free list non-empty today;
  alloc-failure leg returns r3=-4 — observable, non-fatal).

### Baseline re-confirmation (charged boot 1)

Ring tail exact (`… 5000e43a 5000e43c 5000e43e 5000f242 5000f246 5000f248`,
839,284 transitions); blocks=3836 complete=3836; MMIO macio 65539 / scc 65540
w=2 idle=256 / via 70183 w=307; CUDA 13 pkts, 9 i2c absent, pram_rd=3; VCLK
mtspr_dec=4 dec_expiries=1 pending=1; 5 SC deliveries 0x3f/0x19/0x14/0x19/0xf
→ entry 0x50314ac0; heartbeat silent. Byte-identical class to the Task-C
capture.

### Task A results — the trap-placeholder restore + the 0x700 delivery surface (2026-06-11, plan rev 3 RATIFIED shape)

**Policy retirement (dated note, plan rev 3 item 1):** rung 2's "dead slots get
loud stops" policy (Tasks T/U) is RETIRED as of 2026-06-11 — the slots were
never dead. The raw ROM's `twi 31,r31,N` placeholders (0x0fff000N at file
0x36e8c0+4N, raw md5 7b1378be…) are load-bearing: executing a slot raises a
program interrupt (0x700) which the NK's published handler decodes into the
exit-pointer dispatch. The Task-T/U stops remain the GATED-OFF state (the
byte-identical park baseline); under `SS_NW_FE1F_SURFACE=1` rom_patches restores
the raw words over them (verify-EXPECTED == the exact stop branch words,
PatchROM-time). The rom_patches site carries the same dated note.

**Slot-15 verdict:** slot 15 IS a raw trap-placeholder (`0x0fff000f` at file
0x36e8fc, re-verified against the raw image this session) → restored with the
rest (slots 4, 6–15; slot 14 = raw `0x0fff000d`, the duplicate-0x0d quirk,
restored verbatim). Task T's allocator-exhaustion diagnostics moved to
telemetry: `SheepExcProgramShim` decodes the slot id from the trap word and
emits a loud `[EXC] PROGRAM slot-15 (DR allocator EXHAUSTION)` line on every
slot-15 delivery (+ the delivered-program counter). Exhaustion now reaches the
NK's own slot-15 exit (cold-init publishes it; see Q-F2).

**Where twi/tw land in the core (pinned by reading the decode):** NEITHER is in
`ppc-decode.cpp`'s table (no primary-3 / 31-xo-4 entries) — both fall to the
INVALID entry (`execute_illegal`) which is CFLOW_TRAP, so interpreter-decoded
blocks already END at the trap site. The aarch64 JIT explicitly falls back
(ppc-jit.cpp `case 3` twi / op31 `case 4` tw / `case 2` tdi → `return false`),
so the interpreter arm runs in JIT boots too. The newworld arm lives at the top
of `execute_illegal`'s SHEEPSHAVER section (after the SS_TEST_HEX clean-exit,
preserving the harness contract): trap-TAKEN + program_entry resolved →
`ExcEnter(pc(), msr, EXC_PROGRAM)` + shim + atomic apply (no increment_pc;
SRR0 = the trap instruction verbatim per PEM). Trap-taken + UNRESOLVED → loud
`[EXC] FATAL` capture-abort with the trap word + slot-id decode. UNTAKEN
twi/tw falls through to the legacy illegal path (recorded limitation — no
untaken sites exist; the placeholders are all TO=31 unconditional).
exc_core: `EXC_PROGRAM` class + `program_entry` appended LAST;
SRR1 = (msr & 0xFFFF) | 0x00020000 (PEM program-interrupt bit 14 = trap,
bit 15 = 0 ⇒ SRR0 points AT the offending instruction; literal TEST-PINNED in
test_exc_core Test 8 — existing checks untouched, 25 → 37 checks).

**SPRG/shim verdict (the Q-F2-chain question): YES — the sc shim's two SPR
writes are also expected on the 0x700 path.** The 0x700 handler 0x50314700
opens with `bl 0x50313d40` — the SAME save helper the sc family uses
([STATIC], raw==patched): it consumes SPRG1 at 0x313d4c (`[KDP+4] := SPRG1`,
the caller-r1 save) and SPRG2 at 0x313d84 (`r12 := SPRG2`, the caller LR);
the handler's fast rfi exit restores LR from SPRG2 / r1 from SPRG1
(0x314ad8..ae8). `SheepExcProgramShim` (the sc-shim's sibling in glue)
transcribes exactly those two writes — nothing else (the handler's other
inputs are NK-maintained staged state).

**Probe sub-contract (PASS), env-on boot (slot protocol, 60 s):**
- slot-8 word probe field `[0x5046e8e0]=0x0fff0008` ✓ (the restore landed);
- `[EXC] PROGRAM delivered #1: srr0=5046e8e0 word=0fff0008 slot=8 r1=103ffa38
  lr=5046db6c -> entry=50314700` ✓ (SRR0 = the slot-8 twi address; entry =
  the published handler; LR = the FE1F body's post-blrl return — the callout
  protocol exactly);
- callout-entry conformance RE-ASSERTED (baseline-true, annotated):
  `[PROBE 0x5046db44 visit=1] r5=0x5046e8e0 r8=0x00000031 r9=0x00000002
  r16=0x100037c0 r17=0x103ffc74 r24=0x5000f248` — exact match to Q-F3's table.

**Gated-off A/B (PASS):** boot without the env var reproduces the 0x5000f248
park baseline byte-identically — ring tail exact (`… 5000e43e 5000f242
5000f246 5000f248`, 839,284 transitions), blocks=3836 complete=3836, selector
list 0x3f/0x19/0x14/0x19/0xf → 0x50314ac0, CUDA 13 pkts / VCLK pending=1,
zero [NW-FE1F] lines, delivered_program=0.

**Diagnostic (recorded, NOT chased — Task B/C territory):** the boot runs FAR
past the park. The H1 park is GONE: post-return unmarshal 0x5046db6c visited
(`r3=0x00000000` = noErr-class at block entry; r4=0x00120001 — NOT obviously
the EVNT-pointer class, Task B's return-predicate check must read this
carefully). TWO FE1F deliveries (both slot 8; #2 at r1=103ffa34 — consistent
with the f260/$36 sibling stub). delivered_sc rose 5 → 13. The Task-B watch
address 0x100037d8 (ExpandMem slot, index 7) FILLS: `[WATCH] pc=5046aa64
value=50049574 (was 0)` at record #1072456, then churns
(500497ae → 0 → 5004960a → 0 over ~30k records — the slot is rewritten by
subsequent guest code; 5 WATCH events total). NEW FRONTIER SIGNATURE: guest
SIGSEGV, ea=0x40000fffff42 (guest 0x0fffff42, just BELOW RAMBase 0x10000000),
PPC pc=0x504661a0 (DR emulator mirror, `stwu r3,-4(r1)` family), 68k
r24=0x50004ad0, DR r1=0x0fffff46 — a 68k-side stack push below RAM bottom,
~4.48 M ring records past the old park (vs 839 k). Captured per stop-rule
trigger 2; not staged beyond capture.

**Gates:** build-ss clean; batch + plain test-jit 353/353 score=100; machine
suite ALL PASS (test_exc_core grew 25 → 37 checks); e2e-test 122 passed;
paravirtual e2e — see commit record.

### Task B results — selector-0x31 round trip conformance + the CORRECTED return predicate (2026-06-11)

> Evidence task for `docs/superpowers/plans/2026-06-11-fe1f-service-surface.md`
> Task B (Rev 2 P-M2/P-M5 + Rev 3 item 4). Doc-only (no source edits — a Task-A
> review ran in parallel). Boots: **3 slot-protocol env-on boots** (within the
> ≤3 + ≤1-diagnostic budget; the frontier capture rode the evidence boots):
> `fe1f-taskB-roundtrip` (/tmp/ss-slots/slot0/runs/20260611-191401.62442),
> `fe1f-taskB-dserr-diag` (…/20260611-192238.63154),
> `fe1f-taskB-flineloc` (…/20260611-192323.63265) — all 60 s, all ending in the
> known frontier SIGSEGV (the capture; see instrument notes). Static RE against
> the re-verified dumps (raw md5 7b1378be…, patched e432df64… — both re-checked
> this session).

**THE RETURN-PREDICATE CORRECTION (dated refinement entry — one-iteration
discipline applied; judged a PREDICATE REFINEMENT, not a falsification — see
judgment below). Two distinct mis-pins found and corrected:**

1. **The slot-address recipe was off by 4.** The e3e0 routine's slot
   computation (file 0xe428..0xe430, raw==patched, capstone-M68K):
   `move.l $4(a2),d0 / subq.l #1,d0 / lsl.l #2,d0 / lea.l $4(a0,d0.l),a3` —
   the `lea` carries a **+4 displacement** Q-F3's recipe missed. Corrected:
   **slot = array_base + 4·index** (not base + 4·(index−1)). For the live
   id=2 → index 7 site: slot = 0x100037c0 + 0x1c = **0x100037dc**, NOT
   0x100037d8. **Task A's watch (and its "fill → churn" lifecycle reading)
   was on the wrong word**: the 0x100037d8 traffic (50049574/500497ae/
   5004960a/0 churn) is unrelated neighbor-slot traffic from much later 68k
   code (r24≈0x5003366c/0x5004a1ce/0x50038172). R-F2 is also re-read: under
   the corrected recipe index-1's slot is base+4, so `[0x100037c0]=2` is the
   array's word 0 (header/count-class), not a pre-filled slot.
2. **r4 (→A0) is the kernel-object ID HANDLE, not the EVNT pointer.** Q-F2's
   static decode misread the service tail: at 0x5031d270 `mr r4,r8` copies
   r8 = **the RETURN of the registration call `bl 0x503251b0`** (NK internal
   ABI returns in r8), not the object pointer (which lives in r31 and stays
   kernel-internal: stored into the directory entry +4 at 0x503252a0, and the
   handle is written back at object+0 via `stw r8,0(r31)` at 0x5031d264).
   The registration tail (0x503252d4/0x503252f0) encodes the handle as
   `rlwimi r8,r19,16,0,15` → **handle = (directory-index << 16) | generation**.
   Live r4 = **0x00120001 = directory slot 0x12, generation 1** — exactly the
   sc surface's 0x00050001-class ID-directory precedent.

**Corrected return predicate (pinned):** at the post-blrl return (body
0x5046db6c): r3(→D0) = 0 (noErr status, stored at 0xc(a6)); r4(→A0) =
**nonzero NK kernel-object ID handle, (index<<16)|generation class**; the f240
tail stores A0 through the 8(a6) slot pointer → **[base+4·index] := handle**.
The T-C2 deletion of the "d0==0 skip-arm" gate stands (null-ARG guard only).

**Refinement-vs-falsification judgment:** REFINEMENT. The pinned MECHANISM —
callout returns, r3=0 noErr, nonzero r4 success token stored through the slot
pointer, e3e0 proceeds to the $36 follow-up — held exactly as pinned. What was
wrong were two static misreads (a missed lea displacement; `mr r4,r8` traced to
the wrong r8 definition), both corrected statically and confirmed live in the
SAME evidence boot — no re-pin boot consumed, no second falsification, no
escalation.

**Round-trip sub-contract scoreboard (env-on `SS_NW_FE1F_SURFACE=1`, fresh
evidence, boot 20260611-191401):**

- **(a) The callout RETURNS — PASS.** `[PROBE 0x5046db6c visit=1] r3=0x00000000
  r4=0x00120001 r5=0x103ffc74 r6=0x00000050 r24=0x5000f248 [0x100037dc]=0x00000000`
  — ≥1 unmarshal visit; slot still empty at the return (the store is the 68k
  tail's, post-resume — sequencing as decoded). Meaningful under H1 (P-M5):
  the Task-0 park (zero db6c visits) is GONE. (Post-unmarshal D1/A1 receive
  clobber-class r5/r6 — recorded, harmless: the 68k tail does not consume them.)
- **(b) Callout-entry conformance re-asserted (baseline-true, annotated) —
  PASS.** `[PROBE 0x5046db44 visit=1] r5=0x5046e8e0 r8=0x00000031 r9=0x00000002
  r16=0x100037c0 r17=0x103ffc74 r24=0x5000f248 [0x5046e8e0]=0x0fff0008` — exact
  Q-F3 table match + the restored slot-8 placeholder word (Task A's change
  observable re-confirmed).
- **(c) Result conformance per the CORRECTED predicate — PASS.** r3=0/
  r4=0x00120001 at the return (above), and the ground-truth store:
  `[WATCH] pc=50491440 addr=100037dc value=00120001 (was 00000000 record
  #3391079 … r24=5000f258)` — the corrected slot receives EXACTLY r4's handle,
  written at 68k r24=0x5000f258 = the f240 stub tail (`movea.l d0,a1;
  move.l a0,(a1)` at 0xf252..f254). Store-through executed; skip-arm vacuous
  per T-C2.
- **(d) The 68k advances — PASS.** The trace ring (full 68k register file per
  record; 262,144-record window of 4,480,459 total) shows r24 monotone past the
  f24x family — f258 → e44x → … → the new frontier tail
  `50004ac0 → 50004ac6 → 50004aca → 50004ace → 50004ad0` (crash) — vs the park
  baseline tail `… 5000f242 5000f246 5000f248` at 839,284. Frontier signature
  re-confirmed byte-identical class to Task A's capture (same crash registers,
  ea=0x40000fffff42, PPC pc=0x504661a0; ring total 4,480,459 vs 4,480,458 —
  1-record jitter class). *Instrument note:* the r24-ring atexit does NOT fire
  on the SIGSEGV path (crash handler traps out before atexit) — the JIT trace
  ring (which carries r24 + D/A registers per record) is the crashing-boot
  substitute and is strictly richer.
- **(e) The ExpandMem slot fills, corrected class — PASS.** [0x100037dc] :=
  0x00120001 (the handle class) at record #3391079 and **REMAINS until the
  crash** (no later WATCH events on the corrected word). Lifecycle
  characterized: pre-init neighbor churn (small non-handle values, writes from
  r24≈0x5003366c region) → **whole-array zeroing** at r24=0x5003817x
  (pc=50491600, records #1101915-16 — both watched words zeroed by the same
  loop = the ExpandMem array init) → 0 → the FE1F fill → stable. Task A's
  "fill → consume → refill" churn reading is RETIRED (wrong word + pre-init
  traffic mis-sequenced).

**Invariant carry-over — ALL PASS (same boot):** cold-once (trampoline
guest[0]/[4] write pair exactly once, WATCH record #4667, pc=50429b40; the only
other guest[0]/[4] writers are the pre-trampoline NK low-mem init writes
(records #213/215, pc=0x503109xx — known WAVE0 class) and the guest's own legit
vector install (pc=50490e00, r24=0x500389fe/0x50038a02 — the W-boot-3 class));
delivered-DEC=0 (`delivered_dec=0 deferred_ee=1 deferred_depth=0
deferred_native=0` — identical class to baseline); slot-15 unvisited
(delivered_program=2, both slot 8; no slot-15 line); rung-2 round trip intact
(`[PROBE 0x500cef8c visit=1] r25=0x5000fcf2` — the TVector excursion runs);
sc surface: **delivered_sc=13** (was 5) — selectors #1-5 unchanged
(0x3f/0x19/0x14/0x19/0xf, same r1/lr rows), **#10 = 0x50** [PROBE✓ visit=10 at
0x50314ac0]. *Instrument cap (recorded honestly):* the SC print caps at 5 and
no per-selector counter exists, so selectors #6-9/#11-13 are unenumerated this
task — a print-cap/counter bump is a source change deferred (Task C/Z
candidate). Selector 0x50's NK table entry → 0x50321040 [STATIC].

**Diagnostic map (recorded, not gates):** TWO FE1F callouts fire (selector
$31 then $36, both via slot 8; #2 at r1=103ffa34 = the f260 sibling as
predicted). Selector $36's service is STAGED: 0x5031d6b4 [STATIC] — handle
lookup via 0x50325380 (r3=the $31-returned handle), type-id==9 ('EVNT')
validation, an index-argument (r4=D1, bounds-checked vs 8) object operation —
i.e., an EVNT-object op family, not an unimplemented selector. The third
sibling stub at file 0xf280 (selector **$34**, args a1=8(a6)/a0=$10(a6),
store-through + status like f240) is the family's next member [STATIC].

**NEW FRONTIER (P-M4 artifact, named — stop-rule trigger 2: captured,
stopped):** the boot completes the slot fill, then ~600 trace records later a
**68k System Error ID 10 (line-1111 / F-line class)** is raised:
`[WATCH] addr=af0 value=000a0000` (DSErrCode := 0x000A, written at
r24=0x50004a14 = the SysError handler's `move.w d0,$af0`, record #3391719) with
**saved faulting PC [$C70] := 0x5000e448** (record #3391708) — INSIDE the e3e0
routine, at the $36-stub argument-push sequence (0xe444..0xe44c) right after
the successful fill. The saved PC is convention-skewed (0xe448 holds
`move.l $4(a2),-(a7)`, not an F-line word) — consistent with a synthetic/
DR-raised 68k exception (the T-M3 FE10..FE1E raise-body class) rather than a
literal F-line fetch; **pinning the raise site is the next milestone's recon
question** (suspect: the selector-$36 callout's return/continuation, NOT the
$31 round trip, which is clean end-to-end). The crash mechanics, fully
decoded [STATIC file 0x49a0..0x4b00, raw==patched + trace-ring tail]: SysError
entry 0x500049e0 (regs → $0C30, SR → $C74, popped PC → $C70, vector-stub
`bsr.b` ladder at 0x49b0..0x49c8 encodes the error ID), wait-spin on
`bset #7,$C2C / beq` (the ~1.09 M-record gap between raise and crash), then the
**Deep-Shit-Alert draw setup** at 0x50004a9e: sr:=$2500, $A58/$A5A swap, fake
A5 world := SP−4, `suba.w #$15a,a7` (QuickDraw frame), `pea -4(a5)` +
`_InitGraf` ($A86E at 0x4ad6). Entered with A7=0x100000a0 (a RAM-bottom
fallback stack), the suba takes A7 to 0x0fffff46 and the pea writes 0x0fffff42
< RAMBase → host SIGSEGV at the DR push site 0x504661a0. Live registers at
raise/crash: d0=0x0a (the ID), d6=0x40 ($AF0's prior content — an earlier
DSErrCode-word residue, unpinned), d7=0x36 ([$2BA] nonzero, selector-$36-hued
residue). **Name: the DSAT stack-underflow wall — a post-CFM-prep system error
(ID 10) whose alert machinery itself underflows RAMBase.** Not staged beyond
capture.

**Instrument notes (carried):** SS_JIT_WATCH_ADDR aligns watch addresses down
to 4 (e.g. `2ba` watches the word at 0x2b8); the watch detects word-size guest
writes via the 32-bit word change (DSErrCode at $AF0 appears as
`value=000a0000`); r24-ring atexit skipped on SIGSEGV (trace ring substitutes);
boots are record-for-record deterministic across runs (watch record numbers
#3391079/#3391690/#3391719 line up across the three boots).

**Gates:** doc-only task (no source edits — stated reason: Task-A review ran
in parallel on the same tree; the smoke-set exemption per the plan's
evidence-only clause). All sub-contract evidence above is fresh this session.

### Task C results — acceptance battery + the default flip (2026-06-11)

> Acceptance task for `docs/superpowers/plans/2026-06-11-fe1f-service-surface.md`
> Task C. Boots: **5 slot-protocol boots** (pre-flip env-on + gated-off;
> post-flip default + explicit opt-out + 1 DSAT diag), all 60 s; run dirs
> under /tmp/ss-slots/slot{0,1}/runs/20260611-19{37,38,43}*. Commits:
> `2949ec32` (fix-budget counter), `be0e02cb` (the flip). **FLIP LANDED —
> no gate failure post-flip, revert-on-red not invoked.**

**Fix-budget item (plan Task C item 2, authorized by the Task B record):**
per-selector sc delivery counter in glue (`exc_sc_sel_counts`, first 16
distinct selectors in arrival order; atexit dump + explicit crash-path dump —
the atexit does not fire on SIGSEGV). Counters-for-counts (rev 2 P-m6); the
cap-5 triage prints unchanged. Inner gates on its commit: build-ss clean,
batch test-jit 353/353, machine suite 12/12.

**THE SC SELECTOR LIST, finally enumerable (instrument-gap correction, NOT a
falsification):** the park baseline's "5 sc deliveries" was always the cap-5
PRINT artifact — no parked boot ever printed delivered_sc (the term-dump path
has no [EXC] stats line; only the crash path does, and parked boots die by
SIGTERM). True counts:

- **Parked/opted-out boot: 8 deliveries, 7 distinct** —
  0x3f ×1, 0x19 ×2, 0x14 ×1, 0x0f ×1, 0x27 ×1, 0x40 ×1, 0x42 ×1
  (identical pre-flip-unset and post-flip-=0).
- **FE1F-armed boot: 13 deliveries, 9 distinct** —
  0x3f ×1, 0x19 ×2, 0x14 ×1, 0x0f ×3, 0x27 ×1, 0x40 ×1, 0x42 ×2, 0x50 ×1,
  0x4d ×1 — the FE1F advance adds exactly 5 (0x0f +2, 0x42 +1, +0x50, +0x4d).
  Consistent with Task B's probe-sampled #10=0x50; the previously
  unenumerated #6-9/#11-13 are now pinned.

The 5 printed SC lines are byte-identical throughout; P-m2's "selector list
exact" gate reads on the prints and passes. Tracker sections quoting "5 sc
deliveries" as a TOTAL (rather than the printed list) should read it as the
print-cap artifact from here on.

**Battery scoreboard — env-on pre-flip (`SS_NW_FE1F_SURFACE=1`, boot
20260611-193759):**

- (a) full gates: build-ss clean; batch AND plain test-jit 353/353 score=100;
  machine suite 12/12 ALL PASS (incl. test_exc_core 37 checks); e2e-test 122.
- (b) Task A probe sub-contract: `[0x5046e8e0]=0x0fff0008` ✓;
  `[EXC] PROGRAM delivered #1: srr0=5046e8e0 word=0fff0008 slot=8 lr=5046db6c
  -> entry=50314700` ✓; callout-entry probe exact Q-F3 match (r5=0x5046e8e0
  r8=0x31 r9=2 r16=0x100037c0 r17=0x103ffc74 r24=0x5000f248) ✓.
- (c) Task B round trip: db6c visit=1 r3=0 r4=0x00120001 ✓; WATCH
  `[0x100037dc] := 0x00120001` at record #3391078 (1-record jitter vs Task B's
  #3391079), r24=5000f258, HOLDS to the crash ✓; trace ring 4,480,458 total
  (jitter class vs 4,480,459) ✓.
- (d) carry-over: cold-once WATCH pair record #4666 class ✓; pre-NK writes
  #213/215 + the legit vector install (50490e00) as known classes ✓;
  delivered_dec=0 deferred_ee=1 deferred_native=0 ✓; TVector probe
  `[PROBE 0x500cef8c visit=1] r25=0x5000fcf2` (MM round trip intact) ✓;
  delivered_sc=13 / delivered_program=2 (both slot 8, no slot-15) ✓.
- (e) gated-off boot (unset, SS_DR_R24_RING=1): ring tail
  `… 5000e43e 5000f242 5000f246 5000f248`, 839,284 transitions ✓;
  blocks=3836 complete=3836 ✓; CUDA 13 pkts / VCLK pending=1 ✓; SC prints
  exact ✓; zero [NW-FE1F] lines, zero PROGRAM deliveries ✓.

**THE FLIP (commit `be0e02cb`):** `SS_NW_FE1F_SURFACE` is the newworld profile
default — explicit-"0"-only opt-out (`getenv`+`strcmp` polarity, the
SS_NW_SC_SURFACE precedent; NOT MachineEnvFlag), one gate covering both the
rom_patches placeholder restore and the glue 0x700 delivery surface; loud
`[NW-FE1F] FE1F surface OFF` opt-out announce; the execute_illegal FATAL
message re-worded to name the opt-out. All flip lines inside
MachineProfileIsNewWorld()/PatchROM_NW_trampoline blocks — paravirtual e2e
SUBSTITUTED by the structural-inertness argument + the gated-off A/B boots
(stated in the commit per the gate-tier policy).

**Battery scoreboard — post-flip, NO env vars (boots 20260611-194301 default,
20260611-194304 opt-out, 20260611-194357 DSAT diag):**

- (a) full gates re-run green (same numbers: 353/353 ×2, 12/12, 122).
- (b)–(d) ALL re-asserted on the default boot with no env vars: armed line
  with the new polarity text; PROGRAM #1/#2 slot 8 → 0x50314700;
  callout-entry exact; db6c r3=0/r4=0x00120001; WATCH slot fill at record
  **#3391079 (Task B-identical)** and holds; cold-once #4667
  (Task B-identical); delivered_dec=0; TVector r25=0x5000fcf2; sc=13
  distinct=9 (same list); ring 4,480,459 total (Task B-identical).
- (e) explicit `SS_NW_FE1F_SURFACE=0` boot: park baseline byte-identical —
  ring tail exact, 839,284, blocks=3836 complete=3836, CUDA/VCLK class,
  SC prints exact, no restore line, no PROGRAM deliveries, sc selectors
  8/7-distinct == the pre-flip gated-off boot. Known instrument-class deltas
  only: the [NW-FE1F] OFF announce + the [EXC] sc selectors line.

**DIAGNOSTIC — the DSAT stack-underflow wall re-confirmed on the default
config (P-M4 artifact; stop-rule trigger 2, captured not chased):** SysError
ID 10 class intact, record-for-record: `[$C70] := 0x5000e448` at record
#3391708 (Task B-identical; r24=0x5000499a, the SysError entry), `[$AF0] :=
0x000A` (word 000a0000) at #3391720 (1-record jitter; r24=0x50004a14), then
the wait-spin and the alert-draw underflow → SIGSEGV ea=0x40000fffff42 at
0x504661a0, crash regs identical (r24=50004ad0, r1=0fffff46). **NEW vs Task
B** (watches on $AF0/$C70 from boot): `[$AF0]`/`[$C70]` are 0xffffffff
boot-fill at records #22123/#22603 (r24=0x500005f6); `[$C70]` cleared to 0 at
#32194 (r24=0x50004958); `[$AF0]` → 0xffff0000 at #1840155 (r24=0x500650b6).
So the pre-raise `[$AF0]` halfword was **0xffff**, and Task B's "d6=0x40 =
$AF0's prior content" reading is CORRECTED — d6's 0x40 is NOT from $AF0
(residue source unpinned; next-milestone recon item). The DSAT wall is THE
frontier (M6A-WAVE2-SHIM-RECON.md frontier update carries it).
