# M3a Task 0 — Live entry table + recon findings (9.0.1 staged NK)

> **Status:** ✅ Complete — 2026-06-10. Method: SS_PROBE_PC absolute-word dumps on live
> 9.0.1 newworld diagnostic boots (`/tmp/m2accept.prefs`), per the rev-2 two-anchor,
> probe-verified protocol (plan `2026-06-10-machine-layer-m3a.md` Task 0). Every value
> below is **probe-verified** unless marked inferred.

## The entry table

| Field | Value | Mode | Evidence |
|---|---|---|---|
| `interrupt_entry` | **`0x50412b1c`** (legacy default) — **gate-selectable to `0x50313200` since W2-4 step 0 (2026-06-11, `SS_NW_DEC_PUBLISHED=1`, default OFF; flip = W2-4 final acceptance)** | legacy: **KDP-SHIM** (see below) · published route: **2-SPR shim** (SPRG1:=caller r1, SPRG2:=caller LR — the sc/program/EXT shape; KDP shim retired on that route) | legacy: 16 live words at 0x50412b0c–0x50412b48 byte-identical to static 0x312b0c–0x312b48 (`409b001c 92260024 …`) · published: NK-published `[KDP+0x384]` = 0x50313200 [PROBE✓ ×2: Q-S1 boot 1 + W2-4 step-0 boot 1], primary copy; see "W2-4 step 0" below |
| `syscall_entry` | **`0x50314ac0` — RESOLVED (2026-06-11, NK-syscall-surface Tasks 0/A/B/C; newworld DEFAULT since Task C, opt-out `SS_NW_SC_SURFACE=0`)** | bare ExcEnter + 2-SPR shim (SPRG1:=caller r1, SPRG2:=caller LR) | Primary copy, NK-published `[KDP+0x390]` [PROBE✓]; see "Syscall entry resolution" + "Task C results" below — the M3a descope is formally CLOSED |
| `program_entry` | **`0x50314700` — RESOLVED (2026-06-11, FE1F-service-surface Tasks 0/A/B/C; newworld DEFAULT since FE1F Task C `be0e02cb`, opt-out `SS_NW_FE1F_SURFACE=0`)** | bare ExcEnter(EXC_PROGRAM) + the SAME 2-SPR shim (the 0x700 handler opens with the same save helper `bl 0x50313d40` the sc family uses); SRR0 = the trap instruction verbatim, SRR1 trap bit `0x00020000` (PEM, test-pinned) | Primary copy, NK-published `[KDP+0x37c]` [PROBE✓]; evidence: `M6A-ONGOING-ENTRY-DESIGN.md` "FE1F native callout" + Task A/B/C results. Consumers: the raw entry-vector `twi 31,r31,N` trap-placeholders (restored over rung-2's parked stops) — the DR FE1F native-callout route |

**Three exception classes now resolved** (interrupt / syscall / program). **The
resolution mechanism generalizes**: the NK publishes its per-vector handlers in the
`[KDP+0x360]` table, indexed `vector>>6` — `[KDP+0x390]` (0xC00 sc) and `[KDP+0x37c]`
(0x700 program) were each pinned with one probe word. Any FUTURE exception class
resolves the same way: probe `[KDP+0x360 + (vector>>6)]`, deliver via ExcEnter with the
PEM mask/SRR semantics for that class, and transcribe only what the handler's save
helper consumes (so far always the same two SPRs: SPRG1:=caller r1, SPRG2:=caller LR).

## Findings

1. **Relocation delta = +0x100000 EXACTLY, byte-identical copy.** Both anchors agree:
   - interrupt entry: live `0x50412b1c..48` == static `0x312b1c..48`, all 16 words.
   - check_work: live `0x50426880..bc` == static `0x326880..bc`, all 16 words (incl. the
     relative `bl` word `4bfebe49` — relative branches survive copy unchanged; no immediate
     fixups observed in these windows).
   - The Wave-0 "+0x100004" was a heartbeat-sampled-PC-one-instruction-in artifact.
   - **Consequence:** static-dump RE remains valid for the staged kernel via `static + 0x100000`;
     live confirmation per new target is still cheap (one probe) and recommended.

2. **The real vector pages are EMPTY — DIRECT-VECTOR mode is out; KDP-SHIM confirmed.**
   16-word dumps at 0x100, 0x300, 0x500, 0x900: all zeros. The staged NK installed no
   architectural vector code (consistent with V=P + the nanokernel's own dispatch design).
   M3a Task 4 performs the KDP register-save shim before `ExcEnter`, as planned.

3. **The `[NW-INT]` silence is SOLVED: `XLM_IRQ_NEST` (@0x2818) = `0xFFFFFFFF`.**
   The tick thread's gate (`ReadMacInt32(XLM_IRQ_NEST) == 0`, main_unix.cpp:2216) is
   permanently false on this boot → `TriggerInterrupt` never fires from the tick thread →
   no `[NW-INT]`, and **the 60 Hz re-trigger safety net does NOT exist on this boot** —
   the EE-edge re-raise (plan Task 3) is load-bearing, exactly as the rev-2 C1 hypothesis
   predicted. `XLM_RUN_MODE` (@0x2810) = 0 (MODE_68K) — the MODE_NATIVE arm is unreachable
   today (its newworld fence stays; cheap insurance). XLM signature 'Baah' present @0x2800.

4. **KDP shim inputs are live:** `[KDP+0x65c]` = `0x68fff000` (the ECB — matches the
   `[NW-TRAMP] ECB=68fff000` boot line; the handler-prologue r6 target is sane).
   `[KDP+0x660]` = `0x00000000` (the r7 source — `interrupt()` rlwimi's bit 0 in, so the
   shim will pass r7=0x80000000; same value the paravirtual path would compute from this
   memory; recorded as a watch item for Task 7 outcome (b) analysis).

5. **Boot context for acceptance:** idle-loop spin PCs 0x50426884–0x50426b1c = staged
   check_work/idle cluster (static 0x326880-family + 0x100000), polling the real SCC at
   0xF3012000 (Wave-0 §8). MSR observed 0xf072 at the wall (EE=1); the spin's `mtmsr`
   writes are DR-toggles preserving EE — outcome-(c) (EE-masked) remains low-probability.

## Task 7 acceptance results (2026-06-10)

### The first real PPC exception ever delivered

```
[EXC] DEC delivered #1: restart=50429b40 srr1=0000f072 msr=00001040 -> entry=50412b1c
```

The KDP shim satisfied the handler ABI; the handler ran natively, reprogrammed DEC itself
(`mtspr_dec 3->4`), and exited via its r7-flag `blr` path as designed. Delivery at
`entry=0x50412b1c` matches the Task 0 probe-verified interrupt_entry (KDP-SHIM mode, above).

### Boot-A root cause and fix (commit ab8e5ac6)

The cold MSR fiction `0xf072` has `EE=1` from instruction zero. The first DEC expiry
therefore delivered into NK cold-init, whose state was: all registers zero, LR=0. The
handler ran correctly (shim ABI satisfied), then exited via the r7-flag `blr` path with
LR=0 → jumped to address 0 → `ignoreillegal` zero-page march → SIGSEGV at the 0x100000
mapping edge.

Architecturally, reset MSR has EE=0; the OS enables interrupts when ready. Fix: the
newworld trampoline now seeds MSR=0x7072 (the fiction minus EE bit). Verified live:
heartbeat shows `exc=0/1/0` (delivered=0, deferred_ee=1, deferred_depth=0) — the
cold-init expiry defers correctly, boot reaches the console spin intact.

### Acceptance reinterpretation — both delivery directions verified

The post-fix boot frontier is the NK **Thud debug console** (SPIKE-S3 §2.5), whose
**designed wake is a serial character, not a timer**. EE stays honestly masked at the
console prompt. The same behavior occurs with and without `SS_ROM_SKIP_JUMP68K`; on a
diskless, System-less diagnostic boot this is plausibly the NK's designed end state.
**[SUPERSEDED 2026-06-10, M6A-DR-HANDOFF-ANALYSIS.md]:** the console is designed, but it is
reached because the boot process DIED at the first 68k opcode dispatch (garbage dispatch
base lands the opcode handler inside the console help-text printer) — not because diskless
boots end there.

**Carry-forward (not failure):** M3a verified both directions of the delivery machinery —
delivery when EE permits, and deferral (with correct re-raise) when EE is masked.
"Idle loop wakes via real delivery" in the M3 row sense is gated on the boot proceeding
past the console (M6 PPC→68k handoff + M3b external-source wiring). Recorded explicitly
as a carry-forward, not a regression.

**Carry-forward additions (final-review follow-ups, 2026-06-10):**
1. The depth-deferral branch (`execute_depth != 1` -> deferred_depth) is **live-untested**
   (every boot shows `deferred_depth=0`); M3b's deliverability harness vector is its first
   real exercise.
2. **Post-close spike:** a disk-attached Mac OS 9 boot (with AND without
   `SS_ROM_SKIP_JUMP68K`) parks at the SAME console spin (comp=781, identical signature)
   - the console parking is NOT a diskless artifact; the frontier is the NK
   handoff/initial-task gap (couples to M6), not external interrupts. Full analysis:
   `M5A-BOOT-HANDOFF-ANALYSIS.md` (in progress). M3b must not be scoped on the assumption
   that external-interrupt wiring alone advances the boot.

**Planned debug knob dropped:** `SS_EXC_FORCE` (deliver once ignoring EE) was in the plan
but never implemented — the deferral evidence (exc=0/1/0 telemetry) came for free from the
heartbeat, making the knob moot. Noted as dropped.

### End-to-end machine-layer demonstration (commit a2dd1ff8)

`SS_SCC_RX_INJECT=25:0D` (inject one CR at T+25s) fed the console its designed wake signal.

Evidence (two consecutive two-heartbeat windows, same boot):
- **Pre-injection:** JIT compile counter frozen at 781 blocks (two heartbeats identical).
- **Post-injection:** compile counter 781 → 791 (two heartbeats, 10 new code blocks compiled
  and executed in direct response).
- Console processed the byte and returned to its prompt-wait loop.

This is an airtight A/B within one boot: M2 scheduler → M1 bus/backpatch → SCC Rx →
`check_work` → console. Every machine-layer milestone composing in one observable event.
M1's carried-forward consumer-(b) Rx-path coverage is closed.

## Probe recipes used (for reproduction)

```bash
# Run 1 — XLM + KDP + vector pages (5 probe PCs, 16 fields each):
SS_PROBE_PC='0x504268d0:[0x2810],[0x2814],[0x2818],[0x281c],[0x68ffe65c],[0x68ffe660],[0x2800],[0x2808];0x50426aec:[0x900],...;0x504268bc:[0x100],...;0x50426b1c:[0x300],...;0x50426884:[0x500],...'
# Run 2 — two-anchor delta verification:
SS_PROBE_PC='0x504268d0:[0x50412b1c],...,[0x50426880],...;0x50426aec:[0x50412b3c],...,[0x504268a0],...'
# Both: SS_ROM_LENIENT=1 SS_ROM_SKIP_JUMP68K=1, perl-alarm 20-25s, /tmp/m2accept.prefs
```

---

## Syscall entry resolution (vector 0xC00) — NK-syscall-surface plan Task 0 (2026-06-11)

> **Status:** ✅ All blocking answers pinned. Method: static capstone RE of the raw dump
> + 2 bounded diagnostic boots (`/tmp/sc0_1.log`, `/tmp/sc0_2.log`; budget 2/8 used) +
> the FATAL-capture r0/r3..r10 extension (commit 780bbc34). Stop-rule: **NOT fired.**
>
> **Provenance re-established:** `/tmp/rom901_inventory.bin` md5
> `7b1378be15d99ac1a15ab2fc22bcc56f` ([RAW-ROM], 16 twi placeholders);
> `/tmp/rom901.bin` == `/tmp/rom901_patched_t0.bin` md5
> `e432df64122a5a89ec1c08d0bdf01609` ([PATCH]). Every static window below verified
> raw==patched. Tags carried: [RAW-ROM]/[PATCH]/[STATIC]/[PROBE✓].

### Q-S1 — the live syscall handler entry + the copy resolution [PROBE✓ + STATIC]

**`syscall_entry = 0x50314ac0` — the PRIMARY copy (0x503xxxxx).** Resolution chain:

1. **The NK's vector dispatch is a per-vector handler-pointer table at `KDP+0x360`,
   indexed by `vector>>6`, reached through SPRG3** ([STATIC]: the vector-page stub
   template image lives at file `0x300000..0x302fff` — one stub per 0x100 stride; the
   0xC00 stub at file 0x300c08 is
   `mtspr SPRG1,r1 / mflr r1 / mtspr SPRG2,r1 / mfspr r1,SPRG3 / lwz r1,0x30(r1) /
   mtlr r1 / blrl` — handler = `[SPRG3+0x30]`; DEC's stub loads `[SPRG3+0x24]`
   = 0x900>>6). NK init sets `SPRG3 = KDP+0x360` ([STATIC] 0x3113b4) — so the syscall
   handler is published at **`[KDP+0x390]`**.
2. **[PROBE✓] (boot 1, at TVector and warm-table[0] entry): `[KDP+0x390] = 0x50314ac0`**;
   siblings `[KDP+0x384]`(DEC)=0x50313200, `[KDP+0x374]`(ext-int)=0x50314880,
   `[KDP+0x37c]`=0x50314700, `[KDP+0x39c]`=0x50314240; **`[KDP+0x64c]` (NK relocation
   base) = `0x50310000`** — the NK publishes PRIMARY-copy addresses throughout.
3. **Copy-ambiguity resolution (P-m5 asymmetry, deliberate and recorded):** the live
   syscall entry follows what the NK publishes → **primary copy 0x50314ac0**
   (= static file 0x314ac0 + ROMBase, no +0x100000). `interrupt_entry` stays
   0x50412b1c (staged copy) — both copies are byte-identical at their anchors and
   delivery there is Task-7-proven; NOT silently "fixed" (out of scope). The glue
   comment must record this asymmetry when Task A lands the default.
4. **T-M1 per-target byte re-confirmation [PROBE✓]** (boot 2): 12 live words across
   0x50314ac0..0x50314b50 match the static bytes exactly (incl. `4bfff209` =
   `bl 0x50313d40` and `48006150` = `b 0x5031aca0`).
5. **The 0xC00 page classification [PROBE✓]** (boot 1, 0x40 bytes at [0xC00..0xC3C]):
   **junk/uninstalled** — `f3018000 ffffffff ffffffff ffffffff 00000000 0000ffff
   ffffffff ffffffff | ffffffff ffffffff ffffffff 00ffffff 00000000 ...` — not code,
   not a pointer table (the M3a vector-page-empty finding now covers 0xC00: the staged
   NK never installs its vector-page image; the stub TEMPLATE in the ROM image is the
   transcription source). Per rev 2 P-M3 this is the in-scope case: the stub's
   postconditions (Q-S2) are transcribable host-side; no 0xC00 code execution needed.

### Q-S2 — handler entry ABI + shim verdict [STATIC + PROBE✓]

Register/state → expected value/class at entry to **0x50314ac0** (Task A's probe gate):

| Row | Expected at entry | Class | Evidence |
|---|---|---|---|
| r0 | syscall selector (first sc: 0x3f) | exact (per sc) | [PROBE✓] boot 2 |
| r3..r10 | caller args, UNTOUCHED (first sc: r3=0x00050001, r4=0x10026710) | live caller values | [PROBE✓] |
| r1 | caller r1 (the handler clobbers it; the STUB has already saved it) | =caller r1 | [PROBE✓] r1=0x103ffb50 at entry |
| **SPRG1** | **caller r1 — THE SHIM WRITE #1** | exact | stub template [STATIC]; consumed at 0x313d48 (`[KDP+4]:=SPRG1`) and the negative fast paths |
| **SPRG2** | **caller LR — THE SHIM WRITE #2** | exact | stub template [STATIC]; consumed at 0x313d84 (r12:=SPRG2 → ctx → exit `mtlr r12`); boot 2 showed r12=0 without it |
| SPRG0 | KDP (0x68ffe000) | NK-maintained, already correct | [PROBE✓] boot 2: r1=KDP at 0x313d40 block entry (post `mfspr r1,SPRG0`) |
| SPRG3 | KDP+0x360 | NK-maintained (stub-only consumer; handler body doesn't read it) | [STATIC] 0x3113b4 |
| SRR0/SRR1 | sc+4 / PEM-masked SRR1 | ExcEnter owns (+4 verified live) | mfspr 0x1a/0x1b at 0x313d78/7c |
| MSR | ExcEnter mask result (0x7072→0x1040 class) | masks are LAW; NK kernel code runs IR/DR-off (M3A Task 7 precedent) | [PROBE✓] handler ran to completion at it |
| LR | don't-care (the real stub clobbers it via blrl; handler uses SPRG2) | any | [STATIC] |
| `[KDP-0x14]` | current ctx — the syscall SAVE TARGET (mid-excursion: **MMCB 0x68fff400**) | staged, NK/W2-maintained | [PROBE✓] boots 1+2 (r6=0x68fff400 at dispatcher) |
| `[KDP-0x10]` / `[KDP-4]` | flags word (0x00a00006) / kernel r1 (=KDP 0x68ffe000) | staged | [PROBE✓] boot 1 |

**Handler flow** ([STATIC], all primary-copy): 0x314ac0 checks fast negative selectors
(-1/-2/-3: SPRG1/SPRG2-restore + rfi, pre-save); positive falls to 0x314b38:
`bl 0x50313d40` (save: `[KDP+4]:=SPRG1`, `[KDP+0x18]:=r6`, r6:=`[KDP-0x14]` ctx,
ctx+0x104:=r0, ctx+0x13c..0x16c:=r7..r13, then r10:=SRR0 r11:=SRR1 r13:=CR r12:=SPRG2
r7:=`[KDP-0x10]` r8:=KDP r1:=`[KDP-4]`) → `[KDP+0xe60]`++ → `oris r11,r11,2` →
`b 0x5031aca0` (saves r14..r31 via 0x3238ac, then the selector dispatch at 0x31aed0:
bounds `cmplwi r15,0x86`, table at NK_base+0xacb8, **target = table[sel] + base +
sel*4**; out-of-bounds → r3=-4). The save goes into the **`[KDP-0x14]` ctx (the MMCB
mid-excursion)** — NOT `[KDP+0x65c]` — architecturally correct for a native-world sc;
the DEC-shim's `[KDP+0x65c]` logic does NOT transfer.

**Shim verdict: REQUIRED — exactly two SPR writes (SPRG1:=gpr(1), SPRG2:=lr) before
the bare ExcEnter transition.** Host-side glue helper called from execute_syscall's
newworld arm (the DEC-shim precedent; §2d site discipline). No KDP writes, no register
mutation, no guest-side seeds needed (everything else the handler consumes is
NK-maintained staged state, live-verified). Boot 2 (no shim) is the controlled
falsification: the round trip succeeded END-TO-END except r1 restored as 0 (from
unset SPRG1) at resume → SIGSEGV — the two SPRs are the entire missing surface.

### Q-S3 — MPLibrary's first-sc conformance vector [STATIC + PROBE✓ + FATAL-capture]

- **Stub** (file 0xd6388, raw==patched): `li r0,0x3f / sc / blr` — part of the 12-byte-
  stride syscall-stub table (sibling selectors 6, 7, 8, 9, 0xa, 0xc, 0xd, 0xe, 0x63 in
  the window; a selector family is coming — M4's warning live).
- **Caller**: 0x500cf100 `lwz r3,4(r25)` → 0x500cf104 `bl 0x500d6388` (LR=0x500cf108,
  matches the capture). **Consumption** (LR target, 0x500cf108ff): `nop / cmpwi r3,0 /
  bne 0x500cf3e4` where 0x500cf3e4 = `extsh r3` + epilogue return (error propagates as
  the function result); success falls through to 0x500cf114.
- **Live vector** ([PROBE✓] block-entry 0x500d6388 + the FATAL capture at the sc):
  **r0=0x3f, r3=0x00050001 (kernel ID, type 2 — exists in the staged ID directory),
  r4=0x10026710 (RAM ptr; the value the service stores at obj+0xec)**; r5=0xf04d6163,
  r6=0x80000000, r7=0x68ffef20, r8=0x103ffa50, r9=0x5046de08, r10=0x00000001
  (live-through, not consumed by the 0x3f body).
- **Return protocol (the Task-B predicate, register→value form): result register r3;
  SUCCESS ⇔ r3 == 0 at the resume (0x500d6390); error convention r3≠0 → branch to
  0x500cf3e4.** The legacy-spin datum is closed: the spin polled the r3 status the
  no-op never produced; boot 2 produced r3=0 live.
- **Service semantics** (selector 0x3f body at NK_base+0xf288 = 0x5031f288 [STATIC],
  executed [PROBE✓]): lock `[KDP-0xb50]` → ID lookup 0x325380 (directory at
  `[KDP-0xa98]`: two-level, 8-byte entries {type:1, gen:2, objptr:4}) → require
  type==2 → `stw r4,0xec(obj)` → unlock → r3=0. One store into an existing kernel
  object; boot 2 ran it to completion (0x5031b124 probe: r3=0).

### Q-S4 — exit path + SRR0 ownership [PROBE✓ + STATIC]

**BLOCKING half — pinned:**
- **Exit mechanism**: common exit 0x5031b124 (`crset cr2eq`) → `b 0x503242dc` — the
  scheduler/dispatch-restore route, ending in the §2.2 tail (`mtlr r12 / mtctr r10 /
  bctr`): resume PC = r10 = **SRR0 = sc+4, never re-incremented** ([PROBE✓] boot 2:
  resume block entry exactly 0x500d6390). No rfi needed on the success path (the rfi
  exits exist on the fast negative-selector paths); SRR0/SRR1 are NOT re-consumed at
  exit — nothing in between may clobber them is satisfied trivially (they're read once
  into r10/r11 at save time).
- **Preserved-register rows for Task B's gates** ([PROBE✓] boot 2 + [STATIC] restore
  protocol): **r3 = result (0)** live-through (NOT restored from ctx); **r1 = caller r1**
  (restored from ctx+0x10c ← `[KDP+4]` ← SPRG1 — the shim row); **LR = caller LR**
  (restored via ctx ← r12 ← SPRG2 — the shim row); **r4..r10 = caller values** (ctx
  slots 0x13c..0x16c + 0x114..0x134; boot 2: r4/r10 verified preserved at resume).
  Task B gate (c)'s r1=0x103ffb50 is therefore GATEABLE (r1 is sc-preserved), and
  LR=0x500cf108 likewise.

**RECORDABLE RESIDUES (per rev 2 P-M6, not blocking):**
- R-7 (ctx SRR1/EE for the MixedMode ctx): the syscall path writes the oris-0x20000
  copy of SRR1 through r11 into the ctx CR/SRR family during the heavy path; the EE
  state the eventual NK rfi uses remains unresolved — unchanged residue.
- `[0x2810]` on the syscall path: not touched by the 0x3f service ([STATIC] body has
  no XLM access); [PROBE✓] =1 at the wall (T-M2 upgrade, below). Whether other
  selectors touch run-mode: next-milestone recon.
- MSR-translation question: IR/DR-off delivery affirmatively evidenced (M3A Task 7 +
  boot 2's full round trip at the ExcEnter mask); IR/DR behaviorally inert in V=P.
- The exact MSR value at resume (bctr route vs rfi): boot 2 resumed and executed
  guest code correctly; the restore tail's MSR protocol not instruction-pinned —
  residue, falsifiable at Task B's resume probe if it ever matters.
- `[KDP+0x65c]` at the *sc instant*: sampled ECB (0x68fff000) at TVector and
  warm-entry block entries (pre-flip instants); the W2 flip's MMCB value at the exact
  sc was not separately sampled — immaterial to this milestone (the syscall save path
  consumes `[KDP-0x14]`, probed = MMCB), recorded for honesty.

### Q-S5 — staged-surface audit + the stop-rule verdict [STATIC + PROBE✓]

Bounded walk (≤2 levels, 12 functions): stub template 0x300c08 → entry 0x314ac0 →
save 0x313d40 → dispatcher 0x31aca0/0x31aed0 → nonvol-save 0x3238ac → lock 0x312700 /
unlock 0x3272e0 → ID-lookup 0x325380 → body 0x31f288 → exits 0x31af38/0x31b124 →
restore route 0x3242dc. NK structures consumed: the `KDP+0x360` handler table;
`[KDP+0x64c]` base; `[KDP-0x14]/[KDP-0x10]/[KDP-4]` ctx/flags/kernel-r1;
`[KDP+4]/[KDP+0x18]` scratch save slots; the ctx save record (MMCB); the kernel lock
`[KDP-0xb50]`; the ID directory `[KDP-0xa98]`; counters `[KDP+0xe40/0xe60/0xee4]`
(+optional `[KDP+0xef4]` table); the selector-dispatch table NK_base+0xacb8
(0x87 entries, in-image); SPRG0..3. **Every one is staged NK-init state — and boot 2
is the empirical seal: with ONLY the entry resolved (no shim, no seeds), selector
0x3f executed to completion on staged state and returned r3=0.**

**STOP-RULE VERDICT: does NOT fire.** The syscall surface requires entry resolution
+ the two-SPR shim — nothing unstaged, no vector-page code, no kernel-init-only
structures. Seed-class fix list for Task A: NONE beyond the shim.

### Blocking-answer table (plan rev 2 P-C1)

| Answer | Status | Consumer |
|---|---|---|
| Q-S1 entry address + copy + 0xC00 classification | **PINNED** (0x50314ac0 primary; page junk/in-scope) | Task A |
| Q-S2 ABI table + shim verdict | **PINNED** (table above; shim = SPRG1/SPRG2, host-side) | Task A |
| Q-S5 verdict + seed-class list | **PINNED** (go; no seeds beyond shim) | Task A |
| Q-S3 conformance vector + return predicate | **PINNED** (r0=0x3f r3=ID r4=ptr; r3==0 at 0x500d6390) | Task B |
| Q-S4 blocking half (exit mechanism + non-re-increment + preserved rows) | **PINNED** | Task B |
| Q-S5 full structure enumeration | **PINNED** (list above) | Task C |

ALL blocking answers pinned — implementation may start. Residues: the four Q-S4
recordables above (none blocking).

### Probe pack disposition + T-M2 upgrade

- PROBE-S1 (boot 1, `/tmp/sc0_1.log`): [0xC00..0xC3C] dumped (junk); publication
  slots `[KDP+0x390]`/siblings/`[KDP+0x64c]` pinned. **T-M2 upgrade: `[0x2810]`=1
  [PROBE✓] at the wall** (mid-excursion confirmed); `[KDP+0x65c]`=0x68fff000 sampled
  (instant caveat above); `[KDP-0x14]`=0x68fff400 MMCB [PROBE✓].
- PROBE-S2: superseded by the better instruments — block-entry 0x500d6388 register
  dump + the FATAL capture (both in boot 1) sampled the args at-bl AND at-sc
  (identical values; the at-bl/at-sc distinction is moot for r3..r10).
- PROBE-S3 (boot 2, `/tmp/sc0_2.log`, `SS_EXC_ENTRY=0x50412b1c,0x50314ac0`):
  classified — handler-entry conformance EXACT per the Q-S2 table minus the two shim
  rows; the divergence (resume r1=0 → SIGSEGV) is exactly the missing-shim
  prediction. Signal, not failure.
- Boot budget: **2/8 used** (co-scheduled per P-C3); no question went to residue for
  lack of boots.

### Task B results (2026-06-11) — first-sc round trip PASS; bounded selector map; frontier captured

> Method: 2 fresh diagnostic boots, env-on (`SS_NW_SC_SURFACE=1`), `/tmp/m2accept.prefs`,
> 65s each — B1 (`/tmp/taskB1.log`, probes: handler 0x50314ac0 + per-selector resumes
> 0x500d6390/0x500d6514/0x500d64bc/0x500d6414 + cold 0x50429d00 + warm 0x50429d3c +
> slot-15 0x50429cf0; guest[0]/[4] WATCH + trace ring) and B2 (`/tmp/taskB2.log`,
> SIGTERM kill for the atexit dumps: R24RING tail + blocks/comp + MMIO/CUDA/VCLK).
> Per-selector resume PCs located statically (raw==patched verified per window):
> the full `li r0,SEL / sc` stub table at file 0xd6298..0xd6d3c enumerates selectors
> 0x0..0x84 (+0xfffd/e/f); resume(SEL) = sc+4. No source changes — evidence-only task.
> Gate set per plan: build-ss + machine suite (12/12 ALL PASS) + batch test-jit
> (353/353 score=100); plain test-jit not re-run (no source file changed, stated per
> the task's smaller-set rule).

**Round-trip sub-contract (all PASS, boot B1):**
- (a) No `[EXC] FATAL` sc line anywhere in either boot. PASS.
- (b) Handler-entry probe 0x50314ac0 visit=1 conforms EXACTLY to the Q-S2 table:
  r0=0x3f, r3=0x00050001, r4=0x10026710, r5=0xf04d6163, r6=0x80000000, r7=0x68ffef20,
  r8=0x103ffa50, r9=0x5046de08, r10=0x00000001, r1=0x103ffb50 (=caller r1),
  LR=0x500cf108. PASS (Task A's gate re-asserted on fresh evidence).
- (c) Resume probe 0x500d6390 visit=1: r1=0x103ffb50 (pinned preserved — gate applies
  per P-m3), LR=0x500cf108, r4..r10 byte-identical to the at-entry caller values
  (the Q-S4 preserved rows). PASS.
- (d) **r3=0x00000000 at the resume** (the Q-S3 success predicate) AND the legacy-spin
  signature ABSENT: comp=3836 at term dump (not the 3672-class freeze; +164 blocks past
  the wall) and no 52M/s comp-frozen HB plateau (no [HB] plateau exists — see frontier
  note on heartbeat silence). PASS.

**Invariant carry-over (all PASS, boots B1+B2, byte-identical between them):**
- Cold exactly once: trampoline guest[0]/[4] write pair WATCH record #4666
  (pc=0x50429b40) — same record number as the rung-2 Task X/Y evidence (deterministic);
  cold probe 0x50429d00 visit=1 with the known cold signature (r24=0x5000002a,
  CTR=0x5046e8c0); warm 0x50429d3c visit=1 with the completion signature (r3=0xff,
  r1=0x103ffa2c, r28=scratch 0x68ff6080).
- guest[0]/[4] stable: the ONLY writes are the known classes — NK cold-init's own
  guest[4] toggles (pc=0x503109bc/dc, records #213/#215), the trampoline cold pair
  (#4666), the guest's own 68k vector install (pc=0x50490e00). No reset transitions;
  the 68k reset vector 0x5000002a appears exactly ONCE in the R24 ring (initial cold
  dispatch).
- Slot-15 exhaust stop 0x50429cf0: zero visits (probed, no output).
- Delivered-DEC stays 0: **field-index verification (P-m1)** — `SheepExcStats` out[0] =
  delivered_dec = the FIRST field of the `exc=` tuple; delivered_sc is the FIFTH
  (glue:1177–1183). Observable used: any first DEC delivery prints
  `[EXC] DEC delivered #1:` to stderr (glue:909) — ZERO such lines in 65s × 2 boots
  ⇒ delivered_dec=0 throughout. (The conditional form per contracts-m1 — delivery
  legal if a handler raises EE — never arose: no delivery at all.) PASS.

**Bounded selector map (diagnostic, 2/2 boots — P-M4(3) budget):**

| # | selector r0 | caller LR | caller r1 | args (at handler entry) | r3 at resume |
|---|---|---|---|---|---|
| 1 | 0x3f | 0x500cf108 | 0x103ffb50 | r3=0x00050001 (kernel ID) r4=0x10026710 | **0** (probe 0x500d6390) |
| 2 | 0x19 | 0x500d2fec | 0x103ffb10 | r4=0x000d0001 r5=0x80000000 r6=1 r7=0xaa7f | **0** (probe 0x500d6514) |
| 3 | 0x14 | 0x500d2d7c | 0x103ffb00 | r5=0x000e0001 r6=0x1001a258 r7=0x80000000 | **0** (probe 0x500d64bc) |
| 4 | 0x19 | 0x500d2fec | 0x103ffb10 | (repeat of #2's call site) | not sampled (visit 2 — log-sampled probes print 1/10/100) |
| 5 | 0xf | 0x500d29d4 | 0x103ffac0 | r4=0x00100001 r5/r7=0x80000000 r6=1 | **0** (probe 0x500d6414) |

Total delivered sc count: ≥5 (the 5 stderr-printed deliveries) and ≤9 (handler probe
never reached its visit=10 sample in 65s). Every sampled resume returned r3=0
(success). The ID-shaped args (0x5/0xd/0xe/0x10:0001) suggest the same kernel-ID
directory family as selector 0x3f. Per P-M4(3), selectors beyond this window are the
next milestone's recon.

**Frontier capture at 60s (the P-M4 artifact — captured, NOT chased):**
- After sc #5 the excursion RETURNS and execution goes 68k-side. R24RING: 839,284 68k
  transitions total (ring never wrapped — 68k execution is sparse/slow or ended early).
  Tail = a 68k ROM loop cycling 0x5000dfa2..0x5000e43c (two full iterations visible in
  the last 140 entries, with an inner alternation through 0x5000e3f0/f4 vs e3f8 exit
  legs), ending at `… 5000e43e → 5000f242 → 5000f246 → 5000f248` — byte-identical
  final signature to Task A's 60s boot (`/tmp/taskA_probe.log`). The dominant tail
  blocks (≈60 occurrences/8K-entry window each) span 0x50049f86..9f9e, 0x50014db6..dc2,
  0x5003xxxx and 0x5000exxx — a wide 68k poll/scan loop, not a tight 2-block spin.
- Heartbeat silence is part of the signature: ZERO `[HB`/`[JIT 5.0s]` lines in 65s
  (the post-sc execution regime never re-enters the dispatch-loop heartbeat path) —
  the comp/MMIO/CUDA baseline therefore comes from the SIGTERM term dump (SS_TERM_DUMP;
  note SIGALRM kills skip ALL dumps — use SIGTERM for capture boots).
- Term-dump baseline (B2): session 64s, blocks=3836 complete=3836 (100%), hit=18110
  miss=399; MMIO macio reads=65539, scc reads=65540 writes=2 idle_sleeps=256, via
  reads=70183 writes=307 (IER=65539, ORB=3848) — same class as the sc-wall baseline;
  CUDA packets=13 responses=13 i2c=9 all-absent (addrs 41,4F,B5,91,80,C1,28,71,9D)
  pram_rd=3 — quiet, unchanged class; VCLK mfspr_dec=0 mtspr_dec=4 dec_expiries=1
  pending=1 (the deferred cold-init expiry, unchanged).
- Honest naming: the new frontier is **a 68k-side ROM poll loop ending in a parked
  state at 0x5000f248** (68k transitions stop; wall-clock continues in a non-dispatch
  execution regime). Whether MPLibrary's full init RETURNED in the CFM sense and what
  0x5000f248 waits on are Task C's diagnostic / the next milestone's recon.

**Falsifications: NONE.** No pinned contract was falsified; the one-iteration rule was
not invoked. Boot budget: 2 boots (the P-M4(3) cap), round-trip gates co-scheduled on
boot B1.

### Task C results (2026-06-11) — acceptance PASS pre/post-flip; `SS_NW_SC_SURFACE` is the newworld DEFAULT; the M3a descope formally CLOSED

**Fix budget consumed: ZERO** — no contract falsified; no fix iterations.

**Pre-flip battery (env `SS_NW_SC_SURFACE=1`; boots `/tmp/taskC1.log` env-on 65s
SIGTERM, `/tmp/taskC2_off.log` gated-off):**
- (a) Canonical full gates: build-ss OK; batch + plain test-jit **353/353 score=100
  (plain re-run — Task B had skipped it)**; machine suite **12/12 RESULT: ALL PASS**
  (12 binaries incl. the committed `test_dev_openpic`, 206 checks; 4794 checks on
  test_exc_core line); e2e-test 122 passed; paravirtual `make e2e` PASS (clean
  lifecycle, exit 0; the whole table-finalization block sits inside the
  `MachineProfileIsNewWorld()` arm — paravirtual structurally byte-identical).
- (b) Handler-entry conformance (Task A's gate): 0x50314ac0 visit=1 exact Q-S2 row —
  r0=0x3f, r3=0x00050001, r4=0x10026710, r5=0xf04d6163, r6=0x80000000, r7=0x68ffef20,
  r8=0x103ffa50, r9=0x5046de08, r10=0x00000001, r1=0x103ffb50, LR=0x500cf108. PASS.
- (c) Round trip + result conformance (Task B's gates): no `[EXC] FATAL`; resume
  0x500d6390 visit=1 with **r3=0**, r1=0x103ffb50, LR=0x500cf108, r4..r10 preserved;
  no legacy-spin signature (blocks=3836 complete=3836, not 3672-class; no 52M/s
  plateau). 5 SC deliveries, selectors 0x3f/0x19/0x14/0x19/0xf (= Task B's map). PASS.
- (d) Rung-2 invariant carry-over: cold-once (WATCH pair #4665, trampoline pc=50429b40,
  exactly once); guest[0]/[4] stable (only the known write classes: NK cold-init
  #213/#215, the cold pair, the guest's own 68k vector install pc=50490e00; reset
  vector 0x5000002a exactly once in the ring); slot-15 stop 0x50429cf0 zero visits;
  **the MM round trip itself green per the Task Y recipe**: TVector 0x500cef8c visit=1
  (r25=0x5000fcf2), slot-1 flip region 0x50429d80 visit=1 (r3=0x68fff400 MMCB,
  r4=0x00200000), warm completion 0x50429d3c visit=1 (r3=0xff, r28=0x68ff6080),
  completion resume 0x5046e1f4 visit=1 with r6=0x000000ff and
  **[saveblk+0x3c]=0x50033776** ([r5:0x60] dump, = the Task Y value). delivered-DEC=0
  (zero `[EXC] DEC delivered` lines). PASS.
- (e) Gated-off boot (`/tmp/taskC2_off.log`): both `[EXC] FATAL` lines (incl. the
  780bbc34 r0/r3..r10 capture) **byte-identical** to the Task A baseline
  (`/tmp/taskA_offAB.log`, diff-verified), exit 134 (SIGABRT), pre-heartbeat death
  (zero `[HB]` lines) — the P-m2 signature fields (raw mmio counters excluded). PASS.

**FLIP:** `SS_NW_SC_SURFACE` promoted to newworld profile default-ON
(sheepshaver_glue.cpp table finalization; opt-out `SS_NW_SC_SURFACE=0`, polarity
mirroring `SS_NW_MM_SWITCH` — explicit-"0"-only opt-out, NOT MachineEnvFlag). The
`[NW-SC]` armed line now states the default + opt-out; an explicit opt-out logs its
own loud `[NW-SC] ... OFF` line. Env-matrix comment updated (override × gate 2×2;
`SS_EXC_ENTRY` precedence unchanged; no-comma form still preserves the default).
Paravirtual/OldWorld untouched.

**Post-flip battery (NO env vars — true default; boots `/tmp/taskC3_flip.log` 65s
SIGTERM, `/tmp/taskC4_optout.log` opt-out):**
- (a) Full gates re-run post-edit: build-ss OK; batch + plain test-jit 353/353
  score=100; machine 12/12 ALL PASS; e2e-test 122; paravirtual `make e2e` PASS. ✓
- (b) Handler-entry conformance: byte-identical to the pre-flip dump (visit=1, exact
  Q-S2 row). ✓
- (c) Round trip: no FATAL; resume r3=0, r1/LR/r4..r10 preserved; same 5 selectors;
  blocks=3836 complete=3836. ✓
- (d) Invariants: WATCH pair #4666 cold-once; only known guest[0]/[4] write classes;
  slot-15 zero visits; TVector r25=0x5000fcf2; slot-1/warm/e1f4 signatures exact;
  [saveblk+0x3c]=0x50033776; delivered-DEC=0; heartbeat silence (0 `[HB]`). ✓
- (e) Opt-out A/B (`SS_NW_SC_SURFACE=0`): FATAL lines byte-identical to the Task A
  baseline (diff-verified), exit 134, pre-heartbeat death; plus the loud opt-out
  announcement line. ✓

**FLIP STATUS: LANDED** (no gate failure; no revert). **The M3a syscall_entry
descope/carry-forward is formally closed** — vector 0xC00 resolves through real NK
code on the default newworld config; `SS_EXC_SC=abort/legacy` now applies only to
the opted-out path.

**Frontier capture on the default config (the P-M4 artifact — recorded, NOT chased;
stop-rule trigger 2):**
- **Nothing NEW vs Task B's boots**: ring tail byte-identical
  (`… 5000e43e → 5000f242 → 5000f246 → 5000f248`, 839,284 transitions, ring never
  wrapped); same 5 sc deliveries (handler-probe r0s: 0x3f/0x19/0x14/0x19/0xf, every
  sampled resume r3=0); same term-dump baseline class — blocks=3836 complete=3836,
  MMIO macio reads=65539 / scc reads=65540 writes=2 idle_sleeps=256 / via reads=70183
  writes=307; CUDA 13 pkts/13 resp, 9 i2c all-absent, pram_rd=3 (quiet); VCLK
  mtspr_dec=4 dec_expiries=1 pending=1; heartbeat silence throughout (capture via
  SIGTERM + SS_TERM_DUMP — SIGALRM skips atexit).
- **The parked loop NAMED (one bounded capstone-M68K look at `/tmp/rom901.bin`,
  md5-verified [PATCH], file 0xdfa2/0xe3e0/0xf240):**
  - 0x5000dfa2 is the ROM's **68k A-line trap dispatcher** (reads the trap word,
    `cmpi.w #$a800` / `subi.w #$ac00`, dispatches through the trap tables at
    $400/$e00/$1e00) — the wide 0x5000dfa2..0x5000e43c "loop" is repeated A-trap
    dispatch on behalf of a CFM-prep routine.
  - The 0x5000e3e0..e43c leg is that routine: trap-availability check via two
    `_GetToolTrapAddress` ($A746) calls (selector $AA7F — the MixedMode dispatch
    trap — vs the unimplemented baseline; mismatch ⇒ error 0xffff8d8e), then an
    id→index lookup (5-entry table at 0x5000e3a0) into an **ExpandMem-anchored
    pointer array (`([$2b6],$310)`)**; an empty slot (`tst.l (a3)` == 0) calls
    0x5000f240(&slot).
  - 0x5000f240: `link / moveq #$31,d0 / dc.w $FE1F / move.l d0,$c(a6) …` — an
    **F-line nanokernel/DR service trap `$FE1F` with selector d0=0x31**, result
    expected back in d0 and stored through the slot pointer. *(CORRECTED by FE1F plan
    rev 2 T-C2: the tail was misread — `202e 0008` reloads the caller's POINTER ARG
    from 8(a6), the `beq.s` is a null-arg guard not a result test; **A0** (the success
    token — live: the NK kernel-object ID handle) lands in the slot via
    `movea.l d0,a1; move.l a0,(a1)`, while **D0** is the status stored at 0xc(a6),
    0 = noErr. See M6A-ONGOING-ENTRY-DESIGN.md Q-F3 + Task B results.)*
  - **The park: the ring's final 68k PC 0x5000f248 is the instruction immediately
    after that `$FE1F` trap** — the 68k issues FE1F selector 0x31 and never records
    another transition while wall-clock continues in a non-dispatch regime.
  - **Poll-target class, named (not fixed): the FE-trap service surface — FE1F
    selector 0x31, an ExpandMem/CFM accelerator-slot fill request.** The next
    milestone's named frontier is this FE1F service (and whatever NK/DR surface
    backs it), NOT another sc selector and NOT an MMIO poll.

**Falsifications: NONE** (zero across the pre- and post-flip batteries). Boots used:
4 (C1 env-on, C2 gated-off, C3 post-flip default, C4 opt-out) + 2 paravirtual e2e
gate boots (outside the diagnostic budget per P-C3).

### Notes for Task A (carried)

- The `SS_EXC_ENTRY=0xINT` no-comma form currently ZEROES syscall_entry (glue parse) —
  rev 2 P-M1: change to PRESERVE the default before the flip (post-flip trap).
- P-m5 asymmetry: glue comment must state interrupt=staged-copy / syscall=primary-copy
  per NK publication, deliberately.
- The vector-handler-table fact generalizes M3a: any future vector resolves as
  `[KDP+0x360 + (vector>>6)]` — one probe word each (DEC's published handler
  0x50313200 ≠ the M3a delivery target 0x50412b1c, which works via the KDP shim;
  reconciling those two is explicitly NOT this milestone's scope).

## W2-4 step 0 (2026-06-11) — DEC delivery re-pointed to the NK-published handler (gated)

> The "reconciling those two is NOT this milestone's scope" note above is now
> discharged: EE-CHAIN-RECON.md D-3/D-4 (coordinator sign-off item 2 GRANTED at
> `81b79bb9`) re-points DEC delivery from the save-and-switch body 0x50412b1c to the
> NK-published DEC handler — harmonizing all four exception classes (DEC/sc/program/EXT)
> on the published-handler + 2-SPR-shim pattern. Label: w2-4-step0.

**Hook-contract change (the M3a delivery hook):**

- `SS_NW_DEC_PUBLISHED=1` (default OFF; the flip is W2-4's FINAL acceptance, not step 0)
  re-points `interrupt_entry` to **`0x50313200`** = `[KDP+0x384]` — **[PROBE✓ re-verified
  this task** (boot 1, sc-entry anchor 0x50314ac0: `[0x68ffe384]=0x50313200`, siblings
  `[KDP+0x390]`=0x50314ac0 / `[KDP+0x374]`=0x50314880, `[KDP+0x64c]`=0x50310000**)]** —
  PRIMARY copy per the publication precedent (the staged-copy asymmetry recorded in
  Q-S1 #3 is retired on this route, not silently "fixed" on the legacy one).
- **Shim shape follows the GATE, not the entry value:** gate ON ⇒ the 2-SPR shim
  (SPRG1:=caller r1, SPRG2:=caller LR — W2S-2 Q-W2 verdict: 0x50313200 opens with the
  shared save prologue 0x313d40, self-contained, r9-free; retires the W2S-R1 r9 hazard
  and the cr6/cr7 composition hazards). Gate OFF ⇒ the KDP register-save shim, unchanged
  (the proven shape for 0x50412b1c — kept as the fallback/override path). `SS_EXC_ENTRY`
  precedence is unchanged and overrides the ENTRY VALUE only; an override pointing back
  at 0x50412b1c needs the gate OFF to get its KDP shim. `SS_EXC_BARE` is moot on the
  published route (two SPR writes, no guest memory) — deliberately ignored there,
  matching the EXT-branch precedent.

**Evidence (this task):**

- Harness (the end-to-end channel — delivered_dec=0 on today's boot path, no EE riser
  yet per W2L-3, so the boot CANNOT observe a DEC delivery): run-exc.sh **H8** — gate ON,
  `SS_EXC_BARE=0` (a KDP-shim regression would fault on unmapped KDP ⇒ no REGDUMP),
  extended capture stub `SS_TEST_EXC_STUB=2` (mfspr r23,sprg1; mfspr r24,sprg2) pins the
  2-SPR rows exactly: GPR23=10ffc000 (=caller r1), GPR24=10008000 (=caller LR), plus
  delivered_dec=1 and the "(2-SPR)" delivery tag (H1 control stays untagged). Lane
  12/12, interp=JIT REGDUMP byte-diff green. HONEST SCOPE: the harness entry value is
  the SS_EXC_ENTRY stub — 0x50313200 itself is not executable without the NK; execution
  THROUGH 0x50313200 lands with W2-4 step 1's riser.
- Live A/B (3 slot boots total: probe + gate-on + gate-off): gate-OFF boot signature
  ([EXC]/[CUDA]/[MACHINE]/SIGSEGV lines) **md5-identical to the pre-change baseline**;
  gate-ON differs ONLY in the `[NW-DEC]`/entry-table lines, terminal tuple identical
  (`delivered_dec=0 deferred_ee=5 … delivered_sc=169 delivered_program=4`) — live-inert
  as designed.
- test_exc_chain: NOT extended — the decision logic (ExcDeliveryDecision/ExcEnter) is
  untouched; step 0 changes only the entry VALUE and the host-side shim, both outside
  exc_core. The harness H-vectors (override channel) are unaffected by the default.

**Residue:** retiring the legacy KDP-shim path entirely (and this table's dual-mode row)
is the W2-4 final-acceptance flip's cleanup, after the riser proves deliveries through
0x50313200 live.
