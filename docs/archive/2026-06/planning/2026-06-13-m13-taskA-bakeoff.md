# M13 Task A — three-approach bake-off (parallel worktrees)

**Date:** 2026-06-13
**Parent plan:** `docs/planning/superpowers/plans/2026-06-13-m13-atrap-bootstrap.md`
(read its Task-0 addenda + "Task A — implementation attempt + ARCHITECTURAL WALL" first).
**Goal:** three subagents, one per approach (a/b/c), each in its own worktree, implement + test, and
report a structured verdict. We compare and pick the winner.

---

## The problem in one paragraph

Under `SS_M10_CGRP=1 SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1 SS_M11_FB=1`, the CGRP path delivers an EXT
interrupt to the ROM 68k handler at `0x5000ED08`, but it crashes ~1/3 of boots (SIGTRAP on DR
code-cache dead-fill `0xDEADBEEF` @ guest `0x100259dc`). DR_WARM (`0x5046e9d8`) is the 68k
opcode-dispatch loop (`lha r27,0(r24)` / `rlwimi r29,r27,3,13,28` / `mtctr r29` / `lhau r27,2(r24)`
/ `bctr`); it computes each handler address from **r29 (the dispatch base)**. The NK exception path
**clobbers r29/r30 and the D0-D7/A0-A6 gprs**; the baseline STUB restores only r24/r1, so the
dispatch jumps to garbage. The 68k register file is coherent in PPC gprs **only at the DR_WARM
between-instruction boundary**; everywhere else in DR-range it is transient scratch. Direct injection
from the interrupt poll fails because JIT block-chaining hides that boundary (`check_spcflags` runs
only at *unchained* block entries; DR_WARM is entered once then chained).

## Banked recon facts (DO NOT re-derive — verified 2026-06-13)

- DR_WARM = `0x5046e9d8` (disasm above). r29 dispatch base at normal entry = **`0x17ffeb20`**,
  r30 = **`0x17ffeb18`** — **stable across boots**, coherent ONLY at DR_WARM entry. [PROBE✓]
- A7 = **r1** in DR-dispatch context (probe: r1≈`0x17ffe9xx`, valid stack). `a_regs=gpr[16..23]`
  (A7=r23) is the *EMUL_OP* convention, a different context. [PROBE✓]
- DR dispatch-table trampolines at `0x5046e8c0` = `b`/`twui` slots (NOT pointers). Live slot targets:
  0:`0x50429d00` 1:`0x50429d80` 2:`0x5046fb00` 3:`0x5046fc00` 5:`0x5046fd00`; slots 4,6-15 = `twui`
  traps. NW-tramp setup: r29=`0x50480000` (dispatch-table base), r30=`0x50460000` (emul-code). [PROBE+DISASM✓]
- DR context block = `*(0x00002804)` = **`0x68ffe000`** (KDP+4 = `0x68ffe004` is inside it).
  Slots 2/3/5 are **register-save prologues**: `lwz r1,0x2804(0)` then `stw r6..r11, 0x13c..(r6)`
  where `r6 = *(r1+0x65c)`. [PROBE+DISASM✓]
- DEC park: `0x503230dc` is the NK DEC re-arm (`mtspr 22,rX`); it clamps DEC far when no near-term
  events are queued. `dec_expiries=5` ⇒ boot hasn't progressed enough to install periodic tasks, NOT
  a wrong rate. **dec-rate is coupled to delivery via boot progress** (a correct delivery that
  advances the boot should raise dec). [DISASM✓]
- Baseline (HEAD): harness 353/353; `[PROBE68K 0x5000ed08 match]` fires every boot; dec typically
  parks at 5 (one boot ran free to 39100 when no delivery was attempted).
- ROM dump `/Users/Shared/macemu/dumps/rom901.bin` is 4 MB (`0x50000000-0x50400000`) — the DR region
  `0x5046xxxx`/`0x50429xxx` is RAM-resident, NOT in the dump. Use runtime probes for it
  (`SS_PROBE_PC=<blockEntryPC>:[0xABS],r29,...`; `0x50312250` is a reliable block-entry probe PC).
- `git stash@{0}` holds a prior divergent rewrite: STUB relocated to ROM `0x50429f00` + a SAVE area
  at `0x68ffc400` (24 regs). Its delivery wiring was broken (don't reuse that), but the **SAVE-area
  context-restore scaffolding** may be useful. Mine it; don't trust its conclusions.

## The injection plumbing (baseline, in `SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp`)

`sheepshaver_cpu::deliver_pending_dec_exception()` (~line 1122): computes `dec_pending`/`ext_pending`,
decides DELIVER, then (EXT path, `if(!dec_pending)`) re-syncs the CGRP structs + re-writes a 16-word
STUB at `0x68ffc268` each delivery, sets `srr0/srr1/pc` to the NK EXT body. The STUB (also written in
`rom_patches.cpp` ~line 4090, `SS_M10_CGRP` block) builds a 6-byte 68k frame and `bctr`s to DR_WARM.

---

## Per-approach briefs

> **Each agent:** do your approach-specific RE FIRST; implement ONLY if RE supports it; if blocked,
> STOP and report the precise obstacle (do NOT force a non-working fix — that is the failure mode that
> burned the prior session). All changes stay gated under `SS_M10_CGRP` (default OFF) so the baseline
> stays byte-identical.

### Approach A — NK CGRP path + context restore
Deliver via the NK (safe-point by design) but make the DR register file survive. RE: what does the NK
EXT/CGRP path save, and where (the CGRP context-group save area)? Can our STUB restore r8-r23/r29/r30
from where the NK stashed them — or do we populate a CGRP save slot the NK restores from? Goal: the
existing baseline delivery stops crashing because DR state is intact at DR_WARM. (This is the deepest
RE; a clean "blocked because the CGRP save area is X / unknowable via Y" is an acceptable outcome.)

### Approach B — DR_WARM ROM-patch interrupt check
Patch the dispatch loop at `0x5046e9d8` so each between-instruction boundary checks a pending-EXT flag
and vectors to the handler when set (mirrors real 68k IPL checking). RE: r29-coherence — is r29 valid
at every DR_WARM entry, or only the first (pre-chaining)? How to make the patched check actually run
under chaining (no-chain for that block? patch the chained path?)? Watch perf (hottest loop). The host
sets the pending-EXT flag; the patched ROM code consumes it at the coherent boundary.

### Approach C — DR state-save entry vectors
Instead of jumping to DR_WARM, jump to a DR entry that saves the register file first (slots 2/3/5 →
context block `0x68ffe000`). RE (the load-bearing questions): what NORMALLY vectors to slots 2/3/5 —
are they safe to enter externally, or do they require synthesized 68k-exception conditions? What is the
exact save-set into `0x68ffe000` (all of r8-r22/r25-r31, or a subset)? Does entering one of them and
then reaching the handler leave a correct return path (RTE)? Implement only if entry is safe.

---

## Gate (falsifiable) + acceptance

Run ≥3 boots, `--timeout 90`, env `SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1 SS_M10_CGRP=1 SS_M11_FB=1`.
- **Primary:** ≥2/3 boots show NO `SIGSEGV`/`SIGTRAP` AND `dec_expiries ≥ 20` in `[NW-PROG sched]`.
- **Secondary evidence (report even if primary not met):** does `[PROBE68K 0x5000ed08 match]` fire,
  does the handler run past its first block (`SS_PROBE_68K=0x5000ed08:3;0x5000ed36:3`), is
  `irq_fired ≥ 1`, and is `dec_expiries` climbing (vs stuck at 5)? Per the dec-coupling, a correct
  delivery that advances the boot should raise dec; if delivery works but dec stays <20, say so — that
  is itself a finding (points to a second blocker, e.g. Task B trap availability).
- **Inner gates (must stay green):** `make build-ss`; `SS_HARNESS_BATCH=1 make test-jit` = 353/353.

## Shared rules (BINDING)
- **NEVER** `pkill`/kill-by-name. Diagnostic boots ONLY via `SheepShaver/tools/ss-slot-boot.sh`
  (acquires a slot, isolated prefs/logs, SIGTERMs at timeout). Concurrent boots in different slots are
  safe. Clean up with `ss-reap.sh`.
- Worktree build bootstrap (fresh worktree has no Makefiles): from the worktree,
  `cd SheepShaver/src/Unix && NO_CONFIGURE=1 ./autogen.sh && ./configure --enable-sdl-video
  --enable-sdl-audio --enable-jit --without-gtk --without-x --without-esd --with-vdeplug
  CC="ccache gcc" CXX="ccache g++" CPPFLAGS=-I/opt/homebrew/include LDFLAGS=-L/opt/homebrew/lib`,
  then `cd ../../ && make build-ss`. (ccache shares cache across worktrees → fast.)
- Keep all changes gated under `SS_M10_CGRP`; baseline (gate off) must stay byte-identical.
- Do NOT discard `git stash@{0}`.

---

## RESULTS & SYNTHESIS (2026-06-13)

| Approach | Verdict | Why |
|----------|---------|-----|
| **A** NK CGRP + context restore | **PARTIAL** | r29/r30 restore is proven *necessary* and advances the crash, but the STUB bypasses the DR's register-LOAD prologue so the 68k regfile + A7 are incoherent at delivery. |
| **B** DR_WARM ROM-patch IRQ check | **BLOCKED** | DR_WARM (0x5046e9d8) is a one-shot cold trampoline, not the dispatch loop (0 visits across 25949 dec-expiries). DR is a recompiler; no fixed guest address to patch. |
| **C** DR state-save entry vectors | **BLOCKED** | Slots 2/3/5 are volatile-only prologues (save r7-r13, not the non-volatile 68k file r14-r31); they presuppose coherence, not establish it. |

**Convergent conclusion (all three independently point here):** the 68k DR is a **recompiler**.
You cannot hand-inject an interrupt at any fixed PC with a hand-assembled register set. The 68k
register file is loaded by the DR's **warm-RESUME prologue** (slot 0 → 0x50429d00 → ~0x5046e1a0),
which reloads it from the **double-indirected** location `*(ctx+0x65c)+0x13c` (ctx = 0x68ffe000),
and only *then* dispatches. Our CGRP STUB branches straight to the DR_WARM *dispatch loop*,
skipping that load — so r8-r23 hold NK working pointers (ECB/CGRP/KDP), not D0-D7/A0-A6.

**What Approach A proved (the keeper):** restoring **r29=0x17ffeb20 / r30=0x17ffeb18** before
`bctr DR_WARM` is correct and necessary — it moves the crash from instant DR-cache dead-fill
(0x100259dc) to a far-running, JIT-compiled handler that SIGSEGVs only later on a garbage
A-register EA (0x60264000); one boot reached `[EXC] PROGRAM delivered #5`. This is a proven
building block (gated, baseline byte-identical, harness 353/353) that any real fix needs anyway.
It lives on branch `worktree-agent-a802d8897db63a8a8` (diff: +r29/r30 restore in the STUB writers
in `sheepshaver_glue.cpp` + `rom_patches.cpp`). **Caveat:** r29/r30 are hardcoded DR-arena
constants — probe-stable but must be read from live state before any default-on flip.

**The re-scoped Task A (the real next step):** drive delivery through the DR's warm-RESUME
prologue (~0x5046e1a0) so the DR loads the full regfile from `*(ctx+0x65c)+0x13c`, rather than
hand-assembling it. The open problem: the resume prologue resumes at the *saved* 68k PC, not our
handler PC — running handler-then-resume is exactly what the NK CGRP group mechanism coordinates,
and that wiring is not yet set up (the H1/H2 deep-RE wall). Next RE targets: (i) disassemble the
resume prologue at ~0x5046e1a0 and confirm the regfile load offset; (ii) RE the CGRP group
descriptor to learn how it specifies "vector to handler, then resume saved PC"; (iii) determine
the single trustworthy A7 at delivery (KDP+4 was non-deterministic: 0x17ffeb3e vs 0x68ffe000).

**Also confirmed:** `dec_expiries`=4-5 is coupled to boot progress (NK DEC re-arm clamps far when
no near-term work is queued), so the dec≥20 gate is unreachable until delivery actually advances
the boot — not a separate timer bug.

---

## RESUME-PROLOGUE RE — the concrete fix path (2026-06-13, post-bake-off)

Disassembled the DR warm-resume prologue (`0x5046e1a4`, RAM-resident, probe-dumped):
```
lwz r8, 0(r5) ; D0      lwz r16,0x20(r5) ; A0
lwz r9, 4(r5) ; D1      lwz r17,0x24(r5) ; A1
... r10-r15 (D2-D7)     lwz r18,0x28(r5) ; A2
                        lwz r19,0x2c(r5) ; A3 ... r20-r22 (A4-A6) at 0x30/0x34/0x38
lbz r6,0(r3) ; cmpwi cr5,r6,0xff      ; resume sub-mode select
bne cr5, 0x5046e208                    ; (mid-instruction vs boundary path)
lwz r24, 0x3c(r5)                      ; *** 68k PC reloaded from save+0x3c ***
lha r27, 0(r24) ; b 0x5046cdd8         ; fetch + dispatch (real dispatch entry, NOT DR_WARM)
```

**Mechanism (now fully mapped):** the DR resumes by reloading the complete 68k regfile — D0-D7/A0-A6
from `r5+0x00..0x38` and the **PC from `r5+0x3c`** — then dispatching. `r5` = the regfile save base
(the symmetric counterpart to the save prologues; ≈ `*(ctx+0x65c)+0x13c`). DR_WARM (0x5046e9d8) is
only the *fetch+dispatch tail* and assumes the regfile is already in registers — which is why
vectoring there (Approach A / the STUB) fails.

**Concrete re-scoped Task A design (implementable):**
1. The NK saves the interrupted 68k regfile to the save area when it takes the EXT (to confirm:
   probe the save area at delivery and verify it holds live 68k regs, not stale).
2. STUB/delivery: build the 6-byte 68k exception frame at `savedA7-6` (SR=0, PC=saved 68k PC), then
   **overwrite `save+0x3c` (the resume PC) = `0x5000ED08`** (the handler), and set `savedA7-6` as the
   new A7 in the save area.
3. Vector to the resume prologue (entry that establishes `r5` = save base), NOT DR_WARM. The prologue
   reloads the full regfile + PC=handler and dispatches the handler with correct state. Handler `RTE`
   pops the frame → resumes the original PC. No hand-restore of 16 registers; the DR does it.

**CONFIRMED (probe, 2026-06-13):** save base `r5 = ECB+0x740 = 0x68fff740` (r31=ECB=0x68fff000).
Dumped contents = live interrupted 68k regfile:
```
+0x00..0x1c  D0-D7   (e.g. 0000014c 0000a71e 00000050 ...)
+0x20..0x38  A0-A6   (1000b070 103ffc74 ... valid 0x10xx/0x103f stack-heap addrs)
+0x3c        PC      = 50033776  (valid 68k ROM PC; r24 <- here)
+0x40,+0x44  0,0
```
- **A7 is NOT in the save area** — kept live in `r1` (probe r1=0x103ffa2c, same stack region); the
  resume prologue does not reload r1. So a correct injection keeps A7 in r1 and builds the frame at r1-6.
- Captured around an SC/mixed-mode transition (`[EXC] SC delivered #1` immediately followed) → this is
  the DR's general 68k↔PPC transition save, reused for resume.

**Final empirical unknown (resolved by the implement+test, not more probing):** does the NK's EXT
delivery leave the interrupted 68k state in ECB+0x740 at our STUB's moment? If yes, the STUB just sets
`ECB+0x740+0x3c = 0x5000ED08`, builds the frame at r1-6, and vectors to the resume entry (the one that
sets r5=ECB+0x740 then reaches 0x5046e1a4). If no, the STUB must populate ECB+0x740 from wherever the
NK stashed the 68k state first. Either way Approach A's hardcoded r29/r30 restore becomes unnecessary —
the resume prologue + dispatch establish dispatch state from the save area themselves.

**Net: the re-scoped Task A is now a well-founded implement+test pass, not a guess** — the resume
mechanism is fully mapped (regfile + PC from ECB+0x740, A7 in r1, dispatch via the prologue).

### COMPLETE INJECTION MAP (all addresses probe/disasm-confirmed 2026-06-13)

Save side disassembled at `0x5046e160` (counterpart to the resume load):
```
stw r11..r22, 0xc(r5)..0x38(r5)   ; D3-D7,A0-A6 -> save area
stw r1, 0x10c(r3)                 ; 68k A7 -> r3+0x10c
stb r0, 0(r3)                     ; resume mode byte -> r3+0
```
| Item | Address | Notes |
|------|---------|-------|
| ECB base (r31) | `0x68fff000` | |
| Regfile save area (r5) | `0x68fff740` (ECB+0x740) | D0-D7 `+0x00..1c`, A0-A6 `+0x20..38`, **68k PC `+0x3c`** |
| 68k A7 save slot | `0x68fff50c` | = r3+0x10c, r3 = `*(ECB+0x710)` = `0x68fff400` |
| Resume mode byte | `0x68fff400` | `0xff` selects the instruction-boundary resume path (the one we want) |
| Resume vector | `0x5046e1a4` | loads regfile from r5, PC(r24) from r5+0x3c, then `lha;b 0x5046cdd8` (dispatch) |

**CAVEAT — resume vector is COLD-path:** `0x5046e1a4` is entered ~once naturally (probe: visit=1
even across a 35s boot; steady-state 68k runs in the `0x17faxxxx` recompiled cache). That does NOT
block the design — our STUB/delivery branches to it *deliberately* as a resume vector; it is valid
code that resumes from ECB+0x740 regardless of natural entry frequency. But it means a C++ "hook the
resume prologue" approach will NOT fire per-transition — the delivery must drive the resume itself.

**Implementation recipe (for the implement+test pass):**
1. Confirm (probe at delivery) that the interrupted 68k state is live in ECB+0x740 (+ A7 at 0x68fff50c)
   at the moment our STUB is invoked. If not, find where the NK stashed it first.
2. At delivery: A7 = `*(0x68fff50c)`; saved68kPC = `*(0x68fff77c)` (=save+0x3c). Build the 6-byte
   68k frame at A7-6 (`*(A7-6)=SR=0`, `*(A7-6+2)=saved68kPC`); write new A7 = A7-6 back to `0x68fff50c`;
   write handler `*(0x68fff77c) = 0x5000ED08`; ensure mode byte `*(0x68fff400)=0xff`.
3. Trigger the DR resume from ECB+0x740 (STUB sets r31=0x68fff000, r5=0x68fff740, bctr 0x5046e1a4 —
   or find the existing entry that the NK uses to resume 68k and let it run). DR loads regfile + PC=
   handler, dispatches the handler; handler `RTE` pops the frame -> resumes saved68kPC.
4. Approach A's r29/r30 restore is NOT needed via this path (the prologue/dispatch set dispatch state).

### IMPLEMENT-PASS RESULT (2026-06-13): resume-prologue = BLOCKED — step-1 prerequisite is FALSE

Implemented + tested (gated `SS_M10_CGRP`/`SS_M13_RESUME`, harness 353/353, baseline byte-identical;
C++ instrumentation `[M13-SAVEAREA]` + a 23-word resume STUB; on branch
`worktree-agent-aeb964c5878689a6e`). The deterministic step-1 check **falsified the design's
prerequisite**: at the host EXT-delivery moment the interrupted context is **PPC (NK/DR), not 68k**.
- `save+0x3c` = `0x50510030` (PPC DR addr), never a 68k PC; `A7@0x68fff50c` = `0x17ffebc0` (DR arena),
  not a 0x103fxxxx 68k stack; `D0@0x740`=0; EXT restart PCs all PPC (0x504a8608 etc.).
- ECB+0x740 holds the DR's **PPC-resume** context, not a 68k regfile. No coherent 68k context exists
  anywhere at this instant → "populate-first" is also dead.
- Gate boots: delivery→ park dec=5 or SIGSEGV; the only dec≥20 boot (9117) had ZERO deliveries.
  `[PROBE68K 0x5000ed08]` never matched. **Injection actively destabilizes a healthy scheduler.**

**CONCLUSION (4 independent confirmations: bake-off A/B/C + this).** Host-side hand-injection of a 68k
interrupt is architecturally impossible: the EXT is taken in PPC mode, the 68k regfile is coherent
only at the DR's between-instruction boundary, and forcing it breaks the NK scheduler. **M13 Task A
must be re-scoped away from injection entirely** → toward NK-native EXT→68k propagation: why doesn't
the NK deliver timer/VBL interrupts to the 68k world on its own, and what must the 68k world have set
up first (registered handler table, VIA IFR state, System interrupt handlers) — which loops back to
letting the boot progress rather than forcing interrupts into it.

---

## Deliverable (each agent returns)
1. Verdict: **LANDED** (primary gate met) / **PARTIAL** (secondary progress, primary not met) /
   **BLOCKED** (precise obstacle).
2. The diff (paths + summary).
3. Test evidence: harness score + the ≥3 boot results (dec_expiries, SIGSEGV y/n, PROBE68K, irq_fired).
4. Approach-specific RE findings (answers to the questions in your brief).
5. Honest risk notes (perf, fragility, hardcoded constants, return-path correctness).
</content>
