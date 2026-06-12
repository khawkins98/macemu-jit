# M10 — User-mode DR + CGRP initialization → 68k interrupt handler fires

> **Status: PLAN Rev 2 (post-red-team)** · Branch: `macos-arm64`
> Red-team: PROCESS reviewer (APPROVE WITH AMENDMENTS) + TECHNICAL reviewer (REJECT → resolved).
> Rev 2 folds all BLOCKING findings as BINDING amendments; ADVISORY items folded or noted.
> Predecessor: M9 partial-complete (stall fixed, probe criterion deferred).
> Acceptance: `SS_PROBE_68K=0x5000ed08:5` fires.

---

## Goal

Make `SS_PROBE_68K=0x5000ed08:5` fire — the 68k level-1 external interrupt handler executes via the NK's CGRP delivery path, not a host-side shortcut.

**PASS criterion (gate):** slot boot with `SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1 SS_M10_CGRP=1`
produces `SS_PROBE_68K=0x5000ed08` match(es) in the log.

**DIAGNOSTIC (not a gate):** `dec_expiries≥200 irq_fired≥1` in the `[PROGRESS]` atexit line
(already true in baseline; cited for regression detection, NOT as a M10 claim).

**Paravirtual regression:** byte-identical to current baseline when `SS_M10_CGRP` is off (the gate
is default-OFF). All changes inside `MachineProfileIsNewWorld()` + env-gate guards.

**Honest framing:** M10 owns the FIRST delivery (PR=1 plumbing + CGRP fields populated). It does
NOT promise the 68k handler returns cleanly, nor that sustained interrupt delivery works. Subsequent
walls (68k stack frame setup, MixedMode re-entry from interrupt, etc.) are named/stopped on per the
stop-rule and captured for M11+.

---

## Root cause (from S4 RE — BINDING)

**Layer 1 — NK EXT handler PR-bit gate (0x50314880):**
```
bl   0x50313d40                       ← save context; r11 = saved MSR at interrupt time
rlwinm. r9, r11, 0, 0x10, 0x10       ← extract r11 bit16 = PR (user-mode flag)
beq  0x50313ab0                       ← if PR=0 (kernel mode) → fallback; CGRP skipped
```
In all current boots: r11=0x0000000a → PR=0 → always fallback. The DR emulator runs in
kernel mode (MSR=0xa = IR|DR only). No CGRP state matters until PR=1 is established.

**Layer 2 — CGRP uninitialized (0x68ffc1c0 = `*(KDP-0x338)`):**
Mac OS normally populates this during System startup (interrupt group registration). Our
boot stalls before that. Required fields for the NK delivery function (0x503148e0):

| Field | Offset | Required | Current |
|-------|--------|----------|---------|
| function pointer | +0x20 | `NK_base+0x3da0` = 0x503143a0 | 0x00000001 (null-check: cmpwi≥2) |
| guard | +0x38 | non-zero | 0x00000000 (beqlr bails immediately) |
| TABLE_BASE | +0x3c | ptr to array of ptrs to 2-word {SRR0, r2} descriptors | 0x00000000 |
| STACK_TABLE | +0x40 | ptr to array of stack pointers (r1 per level) | 0x00000000 |
| COUNT | +0x44 | ≥10 (source index 9 is posted by NK EXT) | 0x00000000 |
| stack-match | +0x4c | compared vs r9 for stack switch branch | 0x00000000 |

Delivery function logic (static RE, `[STATIC]`):
```
0x503148e0  mfspr   r23, 0x110         ← r23 = KDP
0x503148fc  lwz     r18, -0x238(r23)   ← r18 = source_index (9 stored at 0x503148a4)
0x50314904  slwi    r20, r18, 2        ← r20 = source_index * 4 = 0x24
0x50314908  lwz     r22, -0x338(r23)   ← r22 = CGRP base (0x68ffc1c0)
0x50314910  lwz     r17, 0x38(r22)     ← r17 = guard; or. → beqlr if zero
0x50314914  lwz     r16, 0x44(r22)     ← r16 = COUNT
0x50314920  slwi    r16, r16, 2        ← r16 = COUNT * 4
0x50314928  cmplw   r20, r16           ← r20 (0x24) < r16 (COUNT*4) required
0x5031492c  bgelr                      ← bail if out of range
 ... (save context, stack switch check at +0x4c) ...
0x503149a8  lwz     r8, 0x3c(r22)      ← r8 = TABLE_BASE
0x503149ac  lwz     r9, 0x40(r22)      ← r9 = STACK_TABLE
0x503149b0  lwzx    r20, r8, r20       ← r20 = TABLE_BASE[9] = descriptor ptr
0x503149b4  lwz     r18, 0(r20)        ← r18 = descriptor[0] = RFI_target (→ SRR0)
0x503149c0  mtspr   0x1b, r19          ← SRR1 = masked interrupt MSR
0x503149c4  lwzx    r1, r9, r16        ← r1 = STACK_TABLE[?] = interrupt stack
0x503149c8  lwz     r2, 4(r20)         ← r2 = descriptor[1] (TOC/r2 for target)
0x503149cc  srwi    r3, r16, 2         ← r3 = level index
0x503149dc  addi    r6, r23, -0x318    ← r6 = KDP-0x318 (context area)
0x503149e4  beq     cr2, 0x50314a08   ← if r8==0, alternate path
 ...
0x50314a04  rfi                        ← jump to RFI_target with new r1/r2/r3/r6
```

---

## Authoritative inputs

| Doc | What it pins |
|-----|-------------|
| `docs/HANDOFF.md` §S4 | Full root cause chain, CGRP field table, layer-1/2 analysis |
| `docs/AGENT-CONTEXT.md` | Constants: CGRP=0x68ffc1c0, KDP=0x68ffe000, ECB=0x68fff000, NK EXT=0x50314880; env-gate cluster; trampoline budget note |
| `SheepShaver/src/rom_patches.cpp` :975–1022 | SS_M6A_USER_MSR block + Q-E quarantine comment + word-budget arithmetic (pool+switch+user_msr=49>48) |
| `SheepShaver/src/rom_patches.cpp` :860–910 | SS_NW_MM_SWITCH trampoline layout (current default-ON, occupies 2 of the 48 slots) |
| `docs/archive/2026-06/machine/M6A-ONGOING-ENTRY-DESIGN.md` §Q-E + R-2 | Quarantine rationale: zero-page slide NOT root-caused in-budget; R-2 = "not a blocker" for M6a but IS a blocker for M10 |
| `docs/archive/2026-06/LEARNINGS-2026-06.md` "2026-06-12 — Interrupt-chain night" | tm_task patch misalignment (`2ff7765f`) fixed one class of slide crashes; **Q1 below tests if R-2 is the same class** |
| `SheepShaver/docs/DIAGNOSTICS.md` | `SS_PROBE_68K` syntax, ring tools |
| `docs/MILESTONE-WORKFLOW.md` | Process: Task 0 gates all impl; env-gated impl; default flip LAST |

---

## Codebase facts (re-verify before editing)

- **Trampoline budget (A2, A3, TECHNICAL-2)**: region 0x429b40–0x429bff = 48 words; guard is
  `b_idx > 47` (NOT `> 48`). With pool+switch default-ON: the branch slot lands at b_idx=47
  (re-verify: `b_idx = idx+1` after fixed section + 2 pool words + 2 switch words; compute
  idx from the base `tp` assignment in `PatchROM_NW_trampoline`). Adding 3 user_msr words
  → b_idx=50 at the branch slot → OVERRUN (50 > 47). Budget must extend by ≥3 words; extend to
  ≥52 words (audit confirms region via Q2 ROM dump scan). The next region 0x429c00 onward is
  in the 222KB zero run. **BINDING: re-verify b_idx arithmetic from code before changing the
  guard constant; the plan's "49 > 48" is off-by-one but the overrun conclusion is correct.**
- **SS_M6A_USER_MSR guard**: `if (mm_switch && user_msr)` → forces user_msr=false loudly.
  Any M10 user-mode fix must either (a) remove this guard after extending the budget, or
  (b) find a different path to PR=1 that doesn't use the trampoline's mtmsr block.
- **SS_NW_IRQ_CONSUME (M8)**: fires the 68k handler via host-side `Execute68k()`, NOT via
  CGRP. M10's CGRP path is parallel and complementary. Keep M8 active in all M10 test boots
  (it provides the EXT delivery that makes the irq visible); M10 just changes HOW the NK
  routes it. Gates must verify the CGRP path fires, not just that M8 fires.
- **RFI_target format**: the NK delivery function at 0x503149b4–0x503149c8 loads
  `descriptor[0]` → SRR0 (jump target after rfi), `descriptor[1]` → r2 (TOC). On rfi:
  r1=STACK_TABLE[level] (A8: level-indexed, NOT source_index), r2=descriptor[1], r3=level,
  r6=KDP-0x318. RFI_target must be a PPC stub that, given this register state, sets r24 (68k
  PC) to the interrupt vector address and re-enters the DR dispatch loop. **Identifying this
  stub is Task 0 Q3 (primary method: static RE of mirror emulator via SS_DUMP_ROM + probes).**
- **CGRP+0x20 guard check**: `cmpwi r9, 2; blt bail` at 0x50314898 — value must be ≥2
  to pass. NK init writes 0x503143a0 there. We must write the same.
  `SS_SEED_MEM=0x68ffc1e0=0x503143a0` (CGRP+0x20). `[STATIC]` verified correct. ✓
- **STACK_TABLE indexing (A8)**: `lhz r16, -0x116(r23)` = interrupt nesting level (not
  source_index 9). STACK_TABLE[0] must be valid for first delivery. Provision ≥1 valid entry.
- **cr2 / STACK_TABLE path (A7)**: `li r8,1` before bl → `cmpwi cr2,r8,0` cr2=NE →
  `beq cr2,0x50314a08` NOT taken → rfi path at 0x50314a04 IS used → STACK_TABLE IS used. `[STATIC]`

---

## Task 0 — Recon (BINDING; gates all implementation)

**Budget: 3 boots + 2 static RE sessions (3h cap total). Partial answers beat stalling.**

All questions must be answered (or residue fallback chosen) before ANY implementation starts.
Evidence tags: `[RAW-ROM]`/`[PATCH]`/`[STATIC]`/`[PROBE✓]`. Add a dated addendum block below
for each answer.

### Rev 2 amendments (BINDING)

*From red-team round (2026-06-12). Each item is BINDING — implementers must not deviate.*

**A1 (PROCESS BLOCKING-1):** Q3 QEMU approach removed. Primary method for Q3 is static RE of
the DR emulator code — the mirror emulator (0x50460000) is derived from ROM but starts
beyond the 4MB ROM file boundary; use the `SS_DUMP_ROM` + live probe approach instead
(`SS_PROBE_PC` at the entry-vector table slots). QEMU is demoted to tertiary / out-of-scope.

**A2 (PROCESS BLOCKING-2):** Q2 scope extended. The audit must cover the combined allocation:
user_msr extension (3 words ≈ 12 bytes past 0x429bff) PLUS CGRP data (≤32 words = 128 bytes
starting at ≥0x429da0). Audit range: ROM dump 0x429c00–0x429e00. Result must confirm the
entire combined footprint sits in the zero run.

**A3 (PROCESS BLOCKING-3):** Task A gate uses `SS_PROBE_LINEAR=1 SS_PROBE_CAP=3` to capture
the first 3 visits to 0x50314884 (avoids logarithmic-sampling false-negative). Standard step
updated below.

**A4 (TECHNICAL BLOCKING-4):** Q5 residue fallback added: if `SS_PROBE_PC=0x503149a0:r1,r19`
shows the masked r19 has PR=0 or EE=0, Task B must additionally seed `*(r1-0x964)` with a
valid user-mode MSR (0xd032) via `SS_SEED_MEM` before relying on the rfi to deliver correctly.
Add the probe to Task 0's boot budget.

**A5 (PROCESS ADVISORY-4):** Q3 and Q3a moved to Task 0 static RE sessions (independent of
Q1/Q2 boot results). They can and should be answered during the ROM dump / emulator inspection
phase, before any boots are spent.

**A6 (PROCESS ADVISORY-6):** `dec_expiries≥40` threshold raised to `≥200` in acceptance.
Rationale: the M9 "done" table in HANDOFF.md uses ≥200; the baseline is ≈1577; ≥40 was
unjustified. Any failure post-rfi that parks the NK should still satisfy ≥200 if delivery
worked (the DEC timer isn't affected by a single 68k interrupt).

**A7 (PROCESS ADVISORY-7):** Q4 pre-answered by static analysis (r8=1 at call → cr2≠0 →
STACK_TABLE path taken). Moved to Codebase facts with `[STATIC]` tag. Removed from blocking
table.

**A8 (TECHNICAL ADVISORY-3):** STACK_TABLE is indexed by interrupt LEVEL (from
`lhz r16, -0x116(r23)` = nesting level), NOT source_index. Allocation of 10 entries is still
safe (conservative); the rationale updated to reflect level-based indexing. STACK_TABLE[0]
must be valid (first-delivery level is almost certainly 0).

**A9 (TECHNICAL ADVISORY-8):** Add `SS_PROBE_PC=0x50314a04` (the rfi itself) as a
DIAGNOSTIC-ONLY gate in acceptance to distinguish "delivery function reached rfi" from
"68k handler executed." Does not replace the PASS criterion.

---

### Blocking answer table

| Q | Question | Blocks | Method | Residue fallback |
|---|----------|--------|--------|-----------------|
| Q1 | Does `SS_M6A_USER_MSR=1 SS_NW_MM_SWITCH=0` boot without zero-page slide crash? | Task A | Slot boot (30s), check for SIGSEGV / panic in log | If still-crashes: root-cause in 2 additional boots; if unresolvable → pivot to Q1-alt |
| Q1-alt | If user_msr still crashes, is there a host-side path to set MSR.PR=1 before NK EXT fires? | Task A | Static search in `sheepshaver_glue.cpp` HandleInterrupt path | If no clean hook → Task A scope = trampoline-side mtmsr + budget extension with guard removed |
| Q2 (A2) | Full combined budget audit: does the zero run cover user_msr extension + CGRP allocation? Audit ROM dump 0x429c00–0x429e00 | Task A + Task B | ROM dump byte scan; verify all zeros | If collision: shrink allocation or move CGRP data to a different zero region |
| Q3 (A1) | What PPC code should TABLE_BASE[9]→descriptor[0] (RFI_target) be, such that after rfi, the DR emulator executes the 68k handler at 0x5000ed08? | Task B | (Primary) Static RE of DR emulator: use `SS_DUMP_ROM` + `SS_PROBE_PC` at entry-vector slots; (Secondary) read `[KDP+0x1074/0x1078]` Execute68k mechanism for clues | If unresolvable: write a minimal stub in trampoline space that sets r24=0x5000ed08 and branches to DR warm-entry (Q3a); gate separately |
| Q3a (A5) | Where is the DR dispatch loop's warm re-entry (not cold-start 0x5046e964)? | Task B | Static RE of mirror emulator: `SS_PROBE_PC=0x5046e964:r24` at visit 1 to find where the main loop jumps back to after dispatch | If not in budget: use cold-entry as provisional approximation |
| Q5 (A4) | What is r19 (SRR1 for the interrupt context) after masking? Is it a valid user-mode MSR? (`SS_PROBE_PC=0x503149a0:r1,r19`) | Task B | Slot boot, 30s, with probe | If r19 has PR=0 or EE=0: `SS_SEED_MEM` to patch `*(r1-0x964)` with 0xd032 before CGRP goes live |

### Residue disposition

If Q1 and Q1-alt both fail (user_msr is unresolvable):
→ Stop Task A; re-scope M10 to CGRP-only with a different PR=1 mechanism to be designed.
Document in an addendum and re-plan before starting Task B.

If Q3 is unresolvable (no RFI_target can be pinned in budget):
→ Stop-rule fires. Write a provisional stub that does `mtlr r24; blr` (returns to current DR
PC as a no-op), gate it in, capture what actually happens post-rfi, and file as M10b.

---

### Task 0 addendum (fill in during recon)

> *Results go here as dated sub-sections. Template:*
> **[DATE] Q1 answer:** ...evidence... `[PROBE✓]` / `[STATIC]`
> **Binding contract update:** ...

---

## Task A — User-mode DR (MSR PR=1 at interrupt time)

**Depends on:** Q1 + Q2 answered.

**Gate (falsifiable in advance):** slot boot with `SS_M10_CGRP=0 SS_M6A_USER_MSR=1` (plus the
budget extension), `SS_PROBE_PC=0x50314884:r11` fires and shows r11.bit16=1 (i.e., r11 & 0x8000
≠ 0). If still 0: the mtmsr didn't survive to EXT time — bisect the MSR path.

**Steps:**
- [ ] Read `rom_patches.cpp` :975–1022 to pin current b_idx arithmetic for pool+switch layout.
- [ ] Audit patch space 0x429c00–0x42nine9c (ROM dump) for any content. If zero, extend trampoline
      budget to 52 words (adjust the `if (b_idx > 47)` guard to 51, update the comment). Verify
      the extended region is zero-verified at PatchROM time (add a `[NW-TRAMP]` assertion).
- [ ] Remove the `if (mm_switch && user_msr)` guard (or change to allow both, with the extended
      budget). Keep the `SS_M6A_USER_MSR` env-gate; add `SS_M10_CGRP` as the outer gate for M10.
- [ ] Build, run `SS_HARNESS_BATCH=1 make test-jit` (353/353 gate).
- [ ] Boot with `ss-slot-boot.sh --timeout 30 --label m10-task-a --env 'SS_M6A_USER_MSR=1 SS_PROBE_LINEAR=1 SS_PROBE_CAP=3 SS_PROBE_PC=0x50314884:r11'`.
      **PASS:** first 3 visits to 0x50314884 all show r11.bit16=1 (r11 & 0x8000 ≠ 0).
      **FAIL:** re-check mtmsr timing and trampoline execution order; file in addendum.
- [ ] If PASS: confirm baseline (8.6 HD boot) still works with `SS_NW_MM_SWITCH=1` only (no user_msr).

---

## Task B — CGRP initialization

**Depends on:** Task A PASS + Q3/Q4/Q5 answered.

**Gate (falsifiable in advance):** slot boot with `SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1 SS_M10_CGRP=1`
produces at least one `SS_PROBE_68K=0x5000ed08` match. The boot must NOT SIGSEGV or park at
`dec_expiries<5`.

**Pre-implementation contracts (pin before coding):**
1. CGRP data must be allocated in trampoline patch space (above the W2 slot-1 region at
   0x429d9c; start at ≥0x429da0). Compute word count needed:
   - TABLE_BASE array: 10 pointers = 10 words
   - STACK_TABLE array: 10 pointers = 10 words (all may point to same sentinel stack)
   - Descriptor for entry[9]: 2 words {RFI_target, r2}
   - Sentinel stack (if needed): 8 words (enough for r1 reference)
   - Total: ≤32 words = 128 bytes. Verify fits in zero run; check against high-water mark.
2. All patch sites: verify-zero-first (`verify_rom_word == 0`), then write. No silent overwrites.
3. CGRP writes must happen AFTER NK cold-start init (the trampoline fires at NK cold-start END —
   any CGRP write in the trampoline fires at the right time). Alternative: `SS_SEED_MEM` for
   no-rebuild iteration during Task 0.

**Steps:**
- [ ] Using `SS_SEED_MEM`, test CGRP+0x38=1, CGRP+0x20=0x503143a0 (no-rebuild probe):
      `SS_SEED_MEM=0x68ffc1e0=0x503143a0;0x68ffc1f8=0x00000001` (check delivery guard pass;
      still exits delivery at TABLE_BASE=0 bgelr — but we can verify guard passed by watching
      which error code lands in r8 post-delivery).
- [ ] Once Q3/Q4/Q5 are answered: lay out the CGRP data structures in trampoline patch space.
      Write to patch space in `patch_68k()` or `PatchROM_NW_trampoline()`, gated on `SS_M10_CGRP`.
- [ ] Populate CGRP fields: +0x20=0x503143a0, +0x38=non-zero sentinel, +0x3c=TABLE_BASE_addr,
      +0x40=STACK_TABLE_addr, +0x44=10, +0x4c=? (from Q4; if stack-match unused, write 0xffffffff).
- [ ] Build, `SS_HARNESS_BATCH=1 make test-jit` (353/353), then slot boot with full gate env.
- [ ] **PASS:** `SS_PROBE_68K=0x5000ed08` matches. Record visit count, surrounding log.
- [ ] **FAIL:** check which delivery-function bail fired (r8 value at exit), add probe, one re-pin.

---

## Acceptance

**Env-on first:**
```bash
SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1 SS_M10_CGRP=1 SS_M6A_USER_MSR=1
```
Slot boot 30s. Check: probe match + no SIGSEGV + dec_expiries≥200 (A6: NK not idle; baseline ≈1577).
Additionally: `SS_PROBE_PC=0x50314a04` (the rfi itself) as DIAGNOSTIC — confirms delivery
function completed vs 68k execution reached separately (A9).

**Paravirtual regression (8.6 HD boot):**
`make e2e-test` (offline) + `make e2e` (boot gate) must PASS. This is REQUIRED because the
trampoline budget extension touches code reachable in the paravirtual profile (shared
`PatchROM_NW_trampoline` function — even if the new user_msr/CGRP blocks are inside
`MachineProfileIsNewWorld()`, the budget arithmetic and the branch-slot are not).

**Default flip:** `SS_M10_CGRP` flips to default-ON for newworld profile ONLY AFTER:
1. Env-on gate passes
2. Paravirtual regression passes
3. `SS_M6A_USER_MSR` confirmed NOT needed for paravirtual (leave it as explicit-1 opt-in)

Any gate failure post-flip → revert flip in-task (keep impl code, revert gate default).

**Fix budget:** one telemetry-fix (no rebuild required: `SS_SEED_MEM` iteration) plus one
small in-scope code fix per falsified contract. Second falsification of same contract → stop.

---

## Stop-rule

Triggers — stop and re-scope if:
1. Q1 AND Q1-alt fail: user-mode mechanism is blocked; pause and re-design before coding.
2. Q3 is not resolved within budget AND the provisional no-op stub doesn't make the probe fire:
   the RFI_target design is non-trivial; stop and file M10b with the new information.
3. Task B FAILS twice on the same contract (e.g., delivery function still bails, same r8 code):
   static RE budget is exhausted; stop, document the bail path, file for next session.
4. **Tempting wrong fix (A5 + PROCESS-5)**: any mechanism that causes 0x5000ed08 to execute
   WITHOUT the NK delivery function (0x503148e0) completing its rfi is a bypass and must NOT
   be used. This includes: (a) writing the 68k handler address directly into SRR0 from host
   code, (b) calling `Execute68k(0x5000ed08)` from the host-side interrupt path (M8's existing
   mechanism — keep it active but it doesn't count for M10), (c) patching the NK EXT handler's
   `beq 0x50313ab0` branch to NOP to skip the PR-bit check. The acceptance criterion
   specifically requires the CGRP delivery path to fire.

---

## Self-review

**Tensions flagged for red team:**

1. **Q3 is the highest-risk unknown.** The RFI_target mechanism requires finding or writing a PPC
   stub that bridges the NK's rfi and the DR emulator's 68k dispatch. If this stub doesn't exist
   in ROM, we must write one — increasing impl scope significantly. The plan's provisional fallback
   (a no-op stub to capture what happens post-rfi) may not fire the probe at 0x5000ed08, meaning
   the task goal is not met. Red team: is there evidence the RFI_target exists in ROM already?
   (Check: mirror emulator entry stubs, QEMU rig observation.)

2. **SS_M6A_USER_MSR and SS_NW_MM_SWITCH are currently mutually exclusive** by code guard. If Q1
   says user_msr NOW works (tm_task fix resolved the crash), we're unblocking it by removing a
   guard that was placed deliberately. Red team: verify the Q-E rationale for the guard is fully
   addressed — not just the slide crash, but also the word-budget concern and the ctx-MSR concern
   (R-3 from M6A ONGOING: "sustained PR=1 execution under the JIT remains untested at scale").
   R-3 fires at scale — but the Task A gate (`r11.bit16=1 at EXT entry`) would validate one
   delivery cycle, which is the minimum for M10. Scale validation is M11+.

3. **CGRP data in trampoline patch space** must not collide with existing W2 slot-1 region
   (0x429d80–0x429d9c). The plan says "start at ≥0x429da0" but the exact upper bound of the zero
   run must be audited in Task 0 (Q2 partially covers this). Red team: enforce that Q2 explicitly
   bounds the CGRP allocation, not just the user_msr extension.

4. **The probe criterion (SS_PROBE_68K=0x5000ed08) may not fire even if CGRP delivers correctly.**
   `SS_PROBE_68K` hooks the DR dispatch loop (the 68k PC transition at the DR dispatch hook). If
   the RFI_target stub re-enters the DR loop via a path that doesn't expose r24 to the hook
   (e.g., a cold-entry path that resets r24), the probe won't match. Red team: assess whether the
   acceptance criterion needs a backup (e.g., a PPC probe at the delivery function's rfi or a
   direct OP in the RFI_target stub).

---

## Red-team record

*Empty — to be filled by two parallel reviewers (PROCESS + TECHNICAL/CONTRACTS).*

---

## Implementation notes (carry forward to agent prompts)

- Gate name: `SS_M10_CGRP` (new env var, default OFF, opt-in with `=1`)
- Trampoline budget extension: update the `if (b_idx > 47)` guard to account for extended region;
  add a `[NW-TRAMP] BUDGET:` log line showing b_idx vs limit.
- CGRP seeds in `PatchROM_NW_trampoline()` (same function as other NW seeds): use the `tp[]`
  array write idiom or `SS_SEED_MEM` during Task 0 iteration.
- Evidence tags required in all comments: `[STATIC]` for ROM disassembly facts, `[PROBE✓]` for
  boot-verified values. Oracle SHAs cited at the call site.
- **Never `pkill` by name.** All boots via `SheepShaver/tools/ss-slot-boot.sh`.
