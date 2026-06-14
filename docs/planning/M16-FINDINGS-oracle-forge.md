# M16 — NewWorld NK Interrupt-Routing Forge: Findings

> **CLOSED / banked RE — superseded by Operation NewSheep** (`docs/planning/newsheep/README.md`).
> The forge approach (this milestone) is closed; the branch's main aim is now running/reproducing the
> Trampoline producer. This doc's RE (Q1–Q7) remains valid and is leveraged by NewSheep.

**Status:** COMPLETE — DoD-3 NO-GO (2026-06-14). CGRP-table synthesis is not reachable from
static RE of our ROM; keeping it alive requires a larger host-owned EXT-handler stub that re-opens
the M10 crash class. RE banked (Q1–Q7); milestone closed. (Forge era closed → Operation NewSheep;
9.2 NewWorld is a HARD requirement. The earlier "pivot to compatibility-payoff" framing is superseded.)
**Plan:** `docs/superpowers/plans/2026-06-14-m16-oracle-forge.md` (rev-4, CLOSED) · **Spec:** `docs/superpowers/specs/2026-06-14-m16-oracle-forge-design.md` (rev-2, CLOSED)
**Predecessor:** `docs/planning/M15-FINDINGS-consumption-recon.md` "Addendum — misroute-why diagnostic".

## Task 0 — blocking-answer table

| # | Question | Answer | Status |
|---|----------|--------|--------|
| Q1 | Live `hnfo` base (NOT hardcoded `0x68ff4f00`) | **`0x68ff4f00`** — probe-confirmed `[0x68ffefd0]=0x68ff4f00`; host-asserted by our NW-trampoline (`[KDP+0xfd0]`) | ✅ |
| Q2 | Does paravirtual populate the NK structs? (R1) | not yet probed — but moot (see refinement: our trampoline already host-asserts the structs) | ⏸ |
| Q3a | hnfo address transfer QEMU↔us: LITERAL-OK \| SYNTHESIZE | **likely SYNTHESIZE** — QEMU rig is 512MB/9.2.1; literal pointer transfer unlikely. NOT yet run (strategic fork open) | ⏸ |
| Q3b | SS_SEED_MEM accepts the 3 targets (not MMIO-refused)? | **YES** — `0x68ff4f14`, `0x68ff4f28`, `0x68fff674` all `[SEED] … applied` | ✅ |
| Q4 | `r11` bit `0x8000` — already set on EXT frame, or must be forced? | **already set** — prologue `0x50313d40` does `mfspr r11,0x1b` (r11=SRR1); our EXT frame `srr1=0x9040`, so SRR1 & 0x8000 ≠ 0 → `50314888 beq` NOT taken; dispatcher proceeds to the struct read. NOT the blocker. | ✅ |
| Q5 | the real onward-route decider | **`*(KDP-0x338)+0x20 = [0x68ffc1e0] = 1`**, needs `≥ 2` (gates `50314898 blt 0x50314660`). SPRG0=KDP=`0x68ffe000`; ptr `[0x68ffdcc8]=0x68ffc1c0` (dynamic). Pinned to one field/value. | ✅ |
| Q6 | meaning of the field → safe forge value? | **NO-GO for minimal forge.** The `0x68ffc1c0` struct is the **"CGRP"** interrupt-group descriptor; its handler table is EMPTY (`+0x38`=0 guard, `+0x3c`=0 base, `+0x44`=0 count). Forging `[0x68ffc1e0]≥2` is SAFE (service routine self-guards: `beqlr`/`bgelr`) but INERT (empty table → no dispatch). Real fix needs the FULL CGRP table populated. | ✅ |

## Evidence

### Step 1 — live hnfo base (RUNDIR 20260614-153442.63803)
`[PROBE 0x50314880 visit=1] [0x68ffefd0]=0x68ff4f00`. The probe DID fire at the vector-entry PC (the M15 Task-1 "vector dispatch bypasses block-entry hook" caveat did not bite here). Trampoline log confirms our code host-asserts the Hnfo record: `[NW-TRAMP] W2 'Hnfo' record @68ff4f00 ([KDP+0xfd0]), id=0x3035, scratch=68ff5000`; `[NW-TRAMP] PIC-rail level source staged: [[KDP-0x20]+0xf18]=68ff4f18=0xF3040000 (NK-held PIC base) [0x3f3f]=1 (vector 0x3f -> 68k level 1)`.

### Step 3b — seed-MMIO safety (RUNDIR 20260614-153615.64107)
All three M14 §7 targets accept `SS_SEED_MEM` writes (`applied`, none `REFUSED (MMIO range)`). So the seed can be expressed via `SS_SEED_MEM` for the recon; production seed still gates behind `SS_M16_FORGE`.

### Step 4 — `r11`/SRR1 gate + the real decider (static RE of `rom901.bin`)
Prologue `0x50313d40` (called by `0x50314880 bl`): saves regs, then `mfspr r10,0x1a`/`mfspr r11,0x1b` → **r10=SRR0, r11=SRR1**, `mfcr r13`, `mr r8,r1` (r8 = `mfspr 0x110` = SPRG0-derived per-CPU base), `blr`. Back in the dispatcher:
```
50314884  rlwinm. r9, r11, 0, 16, 16   ; SRR1 & 0x8000  → SET on our frame (srr1=0x9040)
50314888  beq     0x50313ab0           ; NOT taken (bit set)
5031488c  lwz     r9, -0x338(r8)       ; r9 = *(SPRG0_base - 0x338)  ← routing struct ptr
50314890  lwz     r9, 0x20(r9)         ; r9 = struct->[0x20]         ← THE deciding field
50314894  cmpwi   r9, 2
50314898  blt     0x50314660           ; field < 2 → early-return stub (unserviced)
5031489c  bl      0x503238ac           ; field >= 2 → service path (sets err code 9 @ -0x238(r8))
```
**Refinement vs M14 §7:** the onward route is gated by `*(r8-0x338)+0x20 ≥ 2`, not the M14 forge triplet. **Service-path RE (resolves the success-vs-error question):** `0x503238ac` is a context-SAVE helper (stores r14–r31 to context block `r6`, returns SPRG0). So `5031489c` IS the real servicing path (saves full context, sets dispatch state `=9` at `[r8-0x238]`, calls `0x503148e0`), and `blt 0x50314660` is the "nothing to service" early-return stub. **⇒ the forge wants `field ≥ 2` (take the service path).** A zero/uninitialized field → `field<2` → early-return → never reaches `0x5000ec50`, matching M15's 0/510.

**SPRG0 (`r8`) chain (static RE):** at `0x50314620`: `lwz r8,0x5a0(r1)` (r1 = prior SPRG0) → `mtspr 0x110,r8` (new SPRG0) and `mtspr 0x113, r8+0x360` (SPRG3 = the `[KDP+0x360]` vector table). So SPRG0 = a per-CPU/KDP base loaded from `*(prior_SPRG0+0x5a0)`. The deciding pointer `*(SPRG0-0x338)` is itself loaded through this chain — its **runtime value + the `+0x20` field can only be pinned with a live register-snapshot probe** at the dispatch (capture SPRG0 and `*(SPRG0-0x338)+0x20` via `SS_JIT_TRACE_RING` regs-at, or a probe on a block where `r8`=SPRG0). That + RE of what `0x503148e0`/the service path expects in the indexed structures is the remaining Task-0 GO/NO-GO work.

### Step 5 — runtime walk of the deciding field (probe, RUNDIRs 154333 / 154424)
SPRG0 = KDP = `0x68ffe000` (confirmed: SPRG3 = SPRG0+0x360 = the `[KDP+0x360]` vector table). Probes at the EXT entry `0x50314880` (visit=1):
- `*(SPRG0-0x338)` = `[0x68ffdcc8]` = **`0x68ffc1c0`** (routing-struct ptr, in NK lowmem)
- struct head `[0x68ffc1c0]` = `0x00010001`
- **deciding field `*(0x68ffc1c0+0x20)` = `[0x68ffc1e0]` = `0x00000001` = 1**

`cmpwi r9,2; blt 0x50314660`: **1 < 2 → branch taken → early-return → EXT unserviced.** This is the exact mechanical cause of M15's 0/510-to-`0x5000ec50`, isolated to one field at one address with one value.

**Forge target (precise):** make `[0x68ffc1e0] ≥ 2`. (Address is `*(KDP-0x338)+0x20`; the `0x68ffc1c0` base is dynamic — resolve it at seed time from `[0x68ffdcc8]`, do NOT hardcode.) `[0x68ffc1e0]` accepts `SS_SEED_MEM` writes? — to verify (not in the original 3-target probe).

### Step 6 — the field's meaning + the GO/NO-GO verdict (RE of `0x503148e0` + struct probe, RUNDIR 165654)

The `0x68ffc1c0` struct is the **"CGRP" interrupt-group descriptor** (`[+0x04]=0x43475250="CGRP"`, `[+0x00]=0x00010001` version/tag — the same CGRP as M15's "fallback `0x50325fd0`"). Live field dump at `0x50314880`:

| off | value | role (from `0x503148e0` RE) |
|---|---|---|
| +0x00 | `0x00010001` | version/tag |
| +0x04 | `"CGRP"` | identity |
| +0x08/+0x0c | `0x68ffdcc8` | back-ptrs (= KDP-0x338) |
| +0x1c | `0x68ffd56c` | ptr (compared at `50314968` for a slow-path call) |
| **+0x20** | **`0x00000001`** | **gate field** — `50314894 cmpwi 2; blt` |
| +0x38 | `0x00000000` | handler-table **guard** — `50314910 or.; beqlr` |
| +0x3c | `0x00000000` | handler-table **base** (`lwzx r20,r8,idx`) |
| +0x40 | `0x00000000` | stack-table base (`lwzx r1,r9,idx`) |
| +0x44 | `0x00000000` | entry **count** — `50314914 cmplw idx; bgelr` |

**Service routine `0x503148e0` semantics:** index by source #, then `if [r22+0x38]==0: beqlr` (no table → return), `if idx >= [r22+0x44]: bgelr` (out of range → return); else load handler descriptor from `[r22+0x3c]`, set SRR0=`[entry+0]`, SRR1, r1=stack, and `rfi` to the handler. **The routine self-guards** — an empty table returns cleanly, no crash.

**VERDICT — NO-GO for the "change one word" forge.** Forging `[0x68ffc1e0]≥2` passes the gate *safely* (M10-crash risk retired for THIS write — service path self-guards on the empty table) but is **inert**: the empty CGRP table makes `0x503148e0` `beqlr` immediately, never dispatching to `0x5000ec50`. The boot does not advance.

**What the real fix requires:** populate the CGRP handler table — `[r22+0x38]` (guard), `[r22+0x3c]` (descriptor-array base), `[r22+0x40]` (stack-array base), `[r22+0x44]` (count), and the descriptor entries themselves (`[entry+0]`=handler SRR0 → eventually the path to `0x5000ec50`, `[entry+4]`=TOC/r2). This is precisely the table that guest IM init builds and never does here, and writing a host-owned version that `rfi`'s correctly IS the M10-class forge. Its *correct contents* require either (a) RE of the guest IM-init code that registers CGRP sources, or (b) reading a populated CGRP from a boot where IM init runs (QEMU mac99 — now justified, format-only).

### Implication for the milestone
The minimal-forge hope is closed. M16 should re-scope to **CGRP-table synthesis**, with its own Task 0 = "obtain the correct CGRP handler-descriptor format + the source→handler mapping that lands on `0x5000ec50`" via IM-init RE and/or the QEMU oracle (format/semantics, not literal addresses). This is a larger effort than a one-word seed; recommend a fresh planning pass before implementation. The gate-field forge (`[0x68ffc1e0]=2`) remains useful as a *probe* to confirm the service routine reaches its self-guard (cheap validation that the RE is right), but not as a fix.

## Strategic fork — RESOLVED (2026-06-14): route B chosen, then closed by red-team

The user chose **(B) Static-RE-first** and the milestone was re-scoped (plan rev-4 / spec rev-2) to
**CGRP handler-table synthesis**. A pre-implementation red-team round (3 adversarial reviewers,
SHA `df627fe0`) then fired the pre-authorized early NO-GO. Historical routes, for the record:
- **(A) QEMU-oracle-first**: weak (512MB/9.2.1 ≠ our 256MB/9.0.1; literal values don't transfer).
- **(B) Static-RE-first** (chosen): RE `0x503148e0` + the service path from OUR ROM. Closed by Q7 below.

## Q7 — CGRP-table-synthesis viability (red-team verdict) — NO-GO

**Q7a — Is the handler-descriptor FORMAT pinnable from static RE?** **YES.** `0x503148e0` fully
disassembles. Entry layout confirmed: `[entry+0]`=SRR0 (handler PC, `503149bc mtspr 0x1a,r18`),
`[entry+4]`=r2/TOC (`503149c8 lwz r2,4(r20)`), SRR1 from `r19` (`503149c0 mtspr 0x1b,r19`, sourced
`5031499c lwz r19,-0x964(r1)` then masked). `[CGRP+0x3c]` is a **pointer array** indexed by
source#·4 (`503148ec slwi r20,r3,2`; `503149b0 lwzx r20,r8,r20`; `503149b4 lwz r18,0(r20)`);
`[CGRP+0x40]` is a **parallel stack-pointer array indexed by a DIFFERENT index** (`r16` from
`[r23-0x116]·4`, `503149c4 lwzx r1,r9,r16`). So a synthesizer needs two arrays + per-source stacks.

**Q7b — Is the synthesis TARGET VALUE (`[entry+0]` SRR0 that chains to `0x5000ec50`) obtainable?**
**NO — and this is the milestone-killer.** Confirmed by ROM scan (`rom901.bin`, 4 MB):
- `0x5000ec50` appears as a word **0 times** anywhere in the ROM — no PPC code references it.
- `0x5000ec50` itself is **68k code** (`4ef9 5000ef20` = `JMP $5000EF20`), reachable only after the
  DR/68k emulator reads a pending-interrupt flag — never a PPC `rfi` target.
- The `"CGRP"` tag (`0x43475250`) appears **0 times** in the ROM → the descriptor is materialized at
  runtime by disk/CFM-loaded IM-init (the code M15 proved never runs), not by inline ROM code.
- No inline ROM builder stores to `[base+0x38/+0x3c/+0x44]` off `*(KDP-0x338)` (only unrelated
  BAT/SPRG context-save collisions at `0x5031a088`/`0x50318664`).
⇒ The handler PC the entry must point at is runtime-registered and ROM-absent. Static RE of our ROM
cannot produce it, and QEMU (format-only; literal values don't transfer) cannot either.

**Q7c — Scratch ownership.** The plan's candidate `0x68ff5000` is **provably live** — the
authoritative sub-KDP occupancy map (`docs/archive/2026-06/machine/M6A-ONGOING-ENTRY-DESIGN.md:690`)
records ROM machine-detect overwriting it each cold cycle; the identical "free gap" claim was already
falsified once (lines 701–703). Safe gaps exist (`0x68ff6084..0x68ff7000` etc.) but require an
occupancy-map extension + a watchpoint-quiescence gate — additional work the re-scope under-budgeted.

**Q7d — Crash class.** Q6's self-guard proof covers only the *empty* table. A *populated* table
fires the `rfi`, re-opening: a valid guest stack w/ sane `[r1+0x648]`, a correct rfi MSR from
uncontrolled NK state (`[KDP-0x964]` masked), and the **M10 DR-reentry `0xDEADBEEF` crash that hit
~1/3 of boots** (the class M13 reverted M10 to escape).

### VERDICT — DoD-3 (NO-GO), milestone closed
Route B (static-RE-first) is closed: the format is RE-tractable but the one value that matters
(`[entry+0]` SRR0) is unobtainable from our ROM or QEMU-as-literal. The only surviving path —
synthesizing a **host-owned NK-EXT-handler PPC stub** (manufacture the DR pending-flag + valid stack
+ correct MSR, survive the M10 DR-reentry crash class) — is materially larger than the re-scope
assumed, and CGRP is per M15 only the **first of N** frozen structs (success would buy "advance to
the next wall," not Finder). Per the project's COMPATIBILITY-PAYOFF focus and the pre-authorized
early-NO-GO rule, the milestone is **closed**; the RE is banked here (Q1–Q7); the frontier pivots to
making the already-booting 8.6–9.0.4 usable. The gate-field probe (`[0x68ffc1e0]=2`, safe/inert)
remains the residual RE-confirmation artifact. The host-stub route stays documented above as the
re-entry point should NewWorld 9.x become a hard requirement.
