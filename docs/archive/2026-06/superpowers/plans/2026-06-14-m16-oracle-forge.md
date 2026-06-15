> **ARCHIVED 2026-06-14** — Moved to archive during the Reku doc-archive sweep.
> May contain outdated assumptions, resolved questions, or superseded plans.
> Current state: `docs/HANDOFF.md` · `docs/planning/ROADMAP.md`
> **Reason:** M8→M17 forge arc CLOSED (banked NO-GO); superseded by Operation NewSheep. Verdict banked in the paired `docs/planning/M1x-FINDINGS-*.md`.

# M16 — NewWorld CGRP Handler-Table Synthesis Implementation Plan

> **CLOSED — DoD-3 NO-GO (2026-06-14). DO NOT EXECUTE.** A pre-implementation red-team round
> (SHA `df627fe0`) fired the pre-authorized early NO-GO: the descriptor FORMAT is RE-tractable but
> the synthesis target value (`[entry+0]` SRR0 chaining to `0x5000ec50`) is **ROM-absent**
> (0 ROM refs to `0x5000ec50`; `"CGRP"` tag ROM-absent → runtime/disk-built), scratch ownership
> needs an occupancy-map extension, and a populated table re-opens the M10 DR-reentry crash class.
> Verdict + evidence: `docs/planning/M16-FINDINGS-oracle-forge.md` Q7. Milestone closed; frontier
> pivoted to compatibility-payoff. The host-owned EXT-handler-stub route (the only surviving path)
> is documented in FINDINGS as the re-entry point if NewWorld 9.x becomes a hard requirement.

> **Rev 4 (2026-06-14) — RE-SCOPE to CGRP-table synthesis (user decision, go/no-go = B).**
> The rev-3 RE (committed `41041d72`→`9a3cdd28`, findings Q5/Q6) proved the minimal one-word
> forge (`*(r8-0x338)+0x20 ≥ 2`) is **SAFE but INERT**: the CGRP handler table it gates is
> empty, so the service routine `0x503148e0` `beqlr`s without dispatching. The milestone is
> re-scoped from "seed the deciding field" to **synthesize the full CGRP handler table** (an
> M10-class forge) whose descriptor entries `rfi` toward the 68k L1 handler `0x5000ec50`. Task 0
> is replaced: it is no longer "oracle viability + value extraction" but **"obtain the CGRP
> handler-descriptor format + the source→handler mapping that lands on `0x5000ec50`"** — primary
> via static RE of `rom901.bin` (`0x503148e0` + the guest IM-init construction code), QEMU as a
> format-only tiebreaker. The rev-1/2/3 routing-triplet content (`hnfo+0x14`/`hnfo+0x28`/
> `KDP+0x674`, the `r11` bit) is **superseded** — the `r11` gate is already satisfied and the
> triplet was never the decider. Spec rev-2: `docs/superpowers/specs/2026-06-14-m16-oracle-forge-design.md`.
> The historical rev-3 banner below is retained for provenance.

> **Rev 3 (2026-06-14) — RE-FIRST pivot (user decision).** Task-0 recon (commit `41041d72`) found the onward-route decider is a LOCAL field in our own 9.0.1 ROM — `*(r8-0x338)+0x20 ≥ 2` (r8=SPRG0 per-CPU base) — NOT the M14 §7 forge triplet, and the `r11`/SRR1 gate is already satisfied. Combined with the red-team showing the QEMU oracle is weak (512MB/9.2.1), the user chose **static-RE-first**: resolve the deciding field + the `5031489c` service-path structures by RE of `rom901.bin` + live register-snapshot probes (`SS_JIT_TRACE_RING` + `ring-walk --regs-at`), and forge the minimal correct value/structure so the dispatcher takes the service path to `0x5000ec50`. **QEMU (Task 0 Step 3/3a) is demoted to a tiebreaker** used only if the ROM RE is ambiguous. Task 0's remaining work is the RE in M16-FINDINGS "Next RE step", not the QEMU read. The seed-target list updates to whatever the RE identifies (likely `*(SPRG0-0x338)+0x20` + its indexed source table), superseding the M14 triplet.

> **Rev 2 (2026-06-14)** — folded the pre-implementation red-team: (B2) `qemu-rig.sh` hardcodes `-m 512` and (B3) boots **9.2.1 from CD**, not our 9.0.1 — so QEMU is a *weak* value oracle; the address-transfer comparison is now an explicit, mandatory blocking-answer row with a SYNTHESIZE fallback as the *expected* outcome, not the exception. (SF1) SS_SEED_MEM has an MMIO-range guard that silently refuses writes — the three target addresses must be seed-probe-verified before Task 0 trusts them. (NIT) Task 2 acceptance is now conditional on the `r11`-bit disposition from Task 0 Step 4.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Synthesize a host-owned **CGRP handler table** so the NewWorld 9.x EXT dispatcher's
service routine (`0x503148e0`), reached via the published EXT entry `0x50314880`, actually
dispatches the EXT edge — its `rfi` chaining toward the 68k level-1 handler `0x5000ec50` —
advancing the boot without the M10-class 0xDEADBEEF crash. The table's format and source→handler
mapping come from RE of our exact 9.0.1 ROM (the service routine + the guest IM-init construction
code), not blind guesses.

**Architecture:** Front-loaded by a BINDING Task-0 RE recon that can fork the milestone (the
foundational risk is whether the CGRP handler-descriptor format + source→handler mapping can be
pinned within budget from static RE of `rom901.bin`, with QEMU as a format-only tiebreaker). If
the format is obtained, a single env-gated `SS_M16_FORGE=1` synthesis (applied at NW-trampoline-end
behind `MachineProfileIsNewWorld()`): resolve the live CGRP base from `[0x68ffdcc8]`, write a
host-owned handler descriptor + stack into NK-owned scratch, wire `[CGRP+0x3c/+0x40/+0x44/+0x38]`
and the `[CGRP+0x20]` gate field. Falsifiable ring-confirmed acceptance (reach `0x5000ec50`);
revert-on-red. The service routine self-guards (M16 Q6), so a wrong table fails clean, not crashing.

**Tech Stack:** SheepShaver aarch64 JIT; slot boot protocol (`ss-slot-boot.sh`); QEMU differential rig (`SheepShaver/tools/qemu-rig.sh` + `qemu-mon.py`); diagnostics (`SS_SEED_MEM`, `SS_NW_PIC`, `SS_NW_IRQ_CONSUME`, `SS_DR_R24_RING`, `SS_JIT_TRACE_RING`, `SS_JIT_WATCH_ADDR`, `SS_PROBE_PC`); `tools/ring-walk.py`; capstone disasm of `rom901.bin`.

**Spec:** `docs/superpowers/specs/2026-06-14-m16-oracle-forge-design.md`
**Parent strategy:** `docs/planning/MACHINE-LAYER-PLAN.md`. **Predecessors:** `docs/planning/M14-FINDINGS-cuda-delivery.md` §7 · `docs/planning/M15-FINDINGS-consumption-recon.md` "Verdict (Task 4)" + "Addendum — misroute-why diagnostic".

**Standing rules:** branch `macos-arm64`; never push unprompted; slot boots only (never global pkill); reap strays with `SheepShaver/tools/ss-reap.sh`; `-F-` heredoc commits (no backticks); explicit-path staging (never `git add -A`); struct fields appended LAST; budgets are caps, partial-findings-beat-stalling; one-iteration rule (falsified contract → dated addendum → ONE re-pin → resume; second falsification → stop).

---

## File structure

This milestone creates/modifies:
- **Modify (append):** `docs/planning/M16-FINDINGS-oracle-forge.md` — add a "Task 0b — CGRP-table-synthesis recon" section (descriptor-format table → source→handler mapping → scratch-ownership → GO/NO-GO). The existing Q1–Q6 RE stays as the predecessor evidence.
- **Modify (only if Task 0 returns GO):** one env-gated synthesis site behind `SS_M16_FORGE=1` + `MachineProfileIsNewWorld()` (NW-trampoline-end path near the `SS_SEED_MEM` hook): resolve live CGRP base, write host-owned descriptor+stack into NK scratch, wire the CGRP fields. Paravirtual byte-identical; gated-off A/B required.
- **Capture (not committed; referenced by path):** slot rundirs under `/tmp/ss-slots/`, QEMU rundirs under `/tmp/qemu-rig-*/`, ring dumps, `ring-walk.py` output; capstone disasm of `rom901.bin` around `0x503148e0`.

---

## Task 0: BINDING recon — CGRP handler-descriptor format + source→handler chain (can fork the milestone)

**Files:**
- Modify (append): `docs/planning/M16-FINDINGS-oracle-forge.md` — a "Task 0b — CGRP-table-synthesis recon" section (descriptor-format table → source→handler chain → scratch-ownership → GO/NO-GO). Q1–Q6 stay as predecessor evidence.

Task 0 exists because the synthesis premise is unproven: that the CGRP handler-descriptor format
the service routine `0x503148e0` consumes, AND the source→handler chain that eventually reaches the
68k L1 handler `0x5000ec50`, can both be pinned within budget from RE of our exact 9.0.1 ROM. Four
facts are load-bearing. **Primary tool: static RE of `rom901.bin` (capstone, BE)** — QEMU is a
format-only tiebreaker, not a value/address oracle.

> **Known starting facts (from M16 Q6 + disasm — verify exactly, do not assume):**
> The service routine indexes by source #, then `if [CGRP+0x38]==0: beqlr` (guard), `if idx >= [CGRP+0x44]: bgelr`
> (count), else loads the entry from base `[CGRP+0x3c]`. The **entry layout is unambiguous from disasm**:
> ```
> 503149b0  lwzx  r20, r8, idx     ; entry ptr = [CGRP+0x3c] + idx*stride
> 503149b4  lwz   r18, 0(r20)      ; r18 = [entry+0]
> 503149bc  mtspr 0x1a, r18        ; SRR0 = [entry+0]   ← handler entry PC
> 503149c8  lwz   r2,  4(r20)      ; r2(TOC) = [entry+4] ← r2/TOC, NOT SRR1
> ```
> **SRR1 is sourced separately from r19** (`mtspr 0x1b, r19`), not from the entry word. So the
> synthesized entry must set `[entry+0]=handler PC`, `[entry+4]=the correct r2/TOC`. Getting `+4`
> wrong (TOC vs SRR1) mis-sets the handler's globals pointer. Task 0 must pin the exact stride
> (`idx*?`), the index source (which source # the EXT edge presents), `[CGRP+0x40]` stack-array
> semantics, and the r19/SRR1 source.

- [ ] **Step 1: Statically RE `0x503148e0` end-to-end against `rom901.bin`**

Disassemble the full service routine (capstone PPC BE; ROMBase `0x50000000`, so file offset =
PC − `0x50000000`). Pin, with cited instruction addresses written into the findings table:
(a) the **descriptor stride** (the `lwzx`/scaling — is the entry array 8-byte `[SRR0,TOC]` pairs or wider?);
(b) the **index source** — which source # / value indexes `[CGRP+0x3c]` for the EXT edge (trace back from `idx` in `503149b0` to its origin: a hardware source #, a `[CGRP+…]` field, or the EXT frame);
(c) the **stack model** — what `[CGRP+0x40]` (stack-array base) supplies and whether a synthesized entry needs a matching stack slot;
(d) the **SRR1/r19 source** — what `mtspr 0x1b,r19` loads (so the synthesized dispatch sets a sane MSR for the handler);
(e) the guard/count semantics exactly (`+0x38`, `+0x44`).
```bash
python3 - <<'PY'
import capstone
code=open('/Users/Shared/macemu/dumps/rom901.bin','rb').read()
md=capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_BIG_ENDIAN)
base=0x50000000
for i in md.disasm(code[0x3148e0:0x314a80], base+0x3148e0):
    print(f'{i.address:#010x}  {i.mnemonic}  {i.op_str}')
PY
```

- [ ] **Step 2: Trace the source→handler CHAIN (the `rfi` does NOT land directly on `0x5000ec50`)**

The `rfi` from the CGRP entry lands at `SRR0=[entry+0]`, which is almost certainly an **NK/PPC-space
handler**, not the 68k address `0x5000ec50` directly. The real path is: `rfi` → NK-level EXT handler
→ (eventually) crosses into the DR/68k emulator → 68k L1 handler `0x5000ec50`. Task 0 must RE this
**full chain** and pin the SRR0 the synthesized entry needs so the chain terminates at `0x5000ec50`:
- On a working IM-init boot, the entry would point at the NK handler the guest registered. Find that
  handler: grep the decompressed ROM for the IM-init **construction code** (stores to `[CGRP+0x38]`,
  `[CGRP+0x3c]`, `[CGRP+0x44]` off the CGRP / `KDP-0x338` base) — the value it stores into the
  descriptor array entry is the ground-truth handler PC.
```bash
# find stores to the CGRP table fields (idiomatic: stw rX, 0x3c(rBase) etc.) — scan for the
# +0x38/+0x3c/+0x44 displacement constants near a base that traces to *(KDP-0x338):
python3 - <<'PY'
import capstone
code=open('/Users/Shared/macemu/dumps/rom901.bin','rb').read()
md=capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_BIG_ENDIAN); base=0x50000000
for i in md.disasm(code, base):
    if i.mnemonic.startswith('stw') and any(d in i.op_str for d in ('0x38(','0x3c(','0x44(')):
        print(f'{i.address:#010x}  {i.mnemonic}  {i.op_str}')
PY
```
- If construction code is absent from the ROM (the table is built by data the IM-init reads, not
  inline), fall back to RE the NK EXT handler the *non-empty* path expects, and/or the QEMU tiebreaker (Step 3).
- Record the full chain `entry.SRR0 → … → 0x5000ec50` with each hop's PC, OR document that the chain
  cannot be closed from static RE (→ informs the GO/NO-GO and DoD-3).

- [ ] **Step 3: QEMU format tiebreaker (only if Steps 1–2 leave the layout/chain ambiguous)**

```bash
bash SheepShaver/tools/qemu-rig.sh --timeout 60   # rig is 512MB / 9.2.1 — FORMAT ONLY, never addresses
python3 SheepShaver/tools/qemu-mon.py --sock /tmp/qemu-rig-*/mon.sock '<read a populated CGRP descriptor + entry STRUCTURE at ~5s>'
```
> **Load-bearing caveat:** QEMU is 512MB/9.2.1 (≠ our 256MB/9.0.1). Read the descriptor **structure
> and semantics** (stride, field roles, whether entries hold NK-PPC handler PCs) — NEVER literal
> addresses or values; those do not transfer. Read at the *earliest* IM-init-complete point (~5s,
> pre-System-customization). The primary format source is the RE of our own ROM (Steps 1–2); QEMU
> only breaks ties.

- [ ] **Step 4: Identify a safe scratch region for the synthesized descriptor + stack (R4)**

The host-owned handler-descriptor array (and any stack slot per Step 1c) must live in NK-owned memory
the NK will not reclaim or overwrite. Candidate: the trampoline scratch `68ff5000` (host-asserted in
`[NW-TRAMP] W2 'Hnfo' record @68ff4f00 … scratch=68ff5000`). Verify the chosen region is (a) not
MMIO-guarded for `SS_SEED_MEM`/host writes, (b) not touched by the NK between trampoline-end and the
EXT edge (watch it across a baseline boot). Record the region + size budget.

- [ ] **Step 5: Record the blocking-answer table + GO/NO-GO**

Append to `M16-FINDINGS-oracle-forge.md` (Task 0b section): the descriptor **stride** + **entry layout**
(`[entry+0]=SRR0`, `[entry+4]=TOC`, confirmed); the **index source**; the **stack model** (`+0x40`);
the **SRR1/r19 source**; the **source→handler chain** (`entry.SRR0 → … → 0x5000ec50`, each hop, OR
"chain not closeable from static RE"); the **scratch region**; the gate field (`[CGRP+0x20]≥2`) +
guard/count values to write; per-field GO/NO-GO; boot budget. Apply the **viability fork**: format +
chain GO → proceed to Task 1; format/chain unobtainable within budget (RE intractable AND QEMU
tiebreaker yields no usable structure) → write the DoD-3 infeasibility verdict and STOP (the
gate-field probe remains as the safe/inert RE-confirmation artifact).
```bash
git add docs/planning/M16-FINDINGS-oracle-forge.md
git commit -F- <<'EOF'
docs(m16): Task-0b recon — CGRP handler-descriptor format + source->handler chain
EOF
```

**Acceptance:** filled blocking-answer table with the exact descriptor stride + entry layout, the
index source, the stack/SRR1 model, the traced source→handler chain to `0x5000ec50` (or a documented
"not closeable" verdict), the scratch region, and an explicit GO/NO-GO viability fork. No synthesis written yet.

---

## Task 1: Implement the env-gated CGRP-table synthesis (GO path only)

**Files:**
- Modify: the NW-trampoline-end synthesis site (behind `SS_M16_FORGE=1` + `MachineProfileIsNewWorld()`)
- Modify: `docs/planning/M16-FINDINGS-oracle-forge.md` (implementation notes)

- [ ] **Step 1: Add the `SS_M16_FORGE` gate + the synthesis writes**

At NW-trampoline-end, behind `ExcM16ForgeEnabled() && MachineProfileIsNewWorld()`:
1. **Resolve the live CGRP base** from `[0x68ffdcc8]` (= `*(KDP-0x338)`) — never hardcode `0x68ffc1c0`.
2. **Build the host-owned handler descriptor array** in the Task-0 scratch region: write `[entry+0]=`
   the handler PC from the Task-0 source→handler chain (the NK-PPC EXT handler that chains to
   `0x5000ec50`), `[entry+4]=`the correct r2/TOC (Task-0 value), plus any per-entry stride padding /
   stack slot (`[CGRP+0x40]` model) Task 0 pinned.
3. **Wire the CGRP descriptor**: `[CGRP+0x3c]=`descriptor-array base, `[CGRP+0x40]=`stack-array base,
   `[CGRP+0x44]=`count (≥ the EXT source index+1), `[CGRP+0x38]=`guard (non-zero per Task-0 semantics),
   `[CGRP+0x20]=`gate field (≥2).
All writes must respect the SS_SEED_MEM MMIO guard or go through a host path that bypasses it (Task-0
Step 4 decides). Prefer the minimal gated host-code seed; record exact addresses/values written.

- [ ] **Step 2: Per-commit gate (inner tier) + gated-off A/B**

```bash
cd SheepShaver && make build-ss && SS_HARNESS_BATCH=1 make test-jit   # expect score=100
```
Show a gated-off (`SS_M16_FORGE` unset) byte-identical A/B boot to prove paravirtual/default-path
inertness (every new line inside the gate).
```bash
git add <the gated synthesis file> docs/planning/M16-FINDINGS-oracle-forge.md
git commit -F- <<'EOF'
docs(m16)+feat: SS_M16_FORGE gated CGRP handler-table synthesis (RE'd format)
EOF
```

**Acceptance:** `make test-jit` score=100; gated-off A/B byte-identical; synthesis applies only under
`SS_M16_FORGE=1` + newworld; CGRP base resolved live (never hardcoded).

---

## Task 2: Synthesis smoke boot — falsifiable acceptance

**Files:**
- Modify: `docs/planning/M16-FINDINGS-oracle-forge.md` (smoke evidence)

- [ ] **Step 1: Run the synthesis smoke boot**

```bash
SheepShaver/tools/ss-slot-boot.sh --label m16-cgrp-smoke --timeout 45 \
  --env 'SS_M16_FORGE=1 SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1 SS_DR_R24_RING=1'
```
(`--env` splits on WHITESPACE, not `;`. Do NOT use `SS_JIT_WATCH_DUMPS=0` — it suppresses the ring dump.)

- [ ] **Step 2: Ring-confirm the dispatch chain**

```bash
python3 tools/ring-walk.py <RUNDIR> --find-pc 0x5000ec50     # GOAL: now > 0 hits (was 0/510)
# Also confirm each intermediate hop of the Task-0 chain (entry.SRR0 → NK handler → DR → 0x5000ec50):
python3 tools/ring-walk.py <RUNDIR> --find-pc <entry.SRR0 PC>
python3 tools/ring-walk.py <RUNDIR> --r24-flow | tail -60
grep -aE 'EXT delivered|SIGSEGV|SIGTRAP|DEADBEEF|\[ALARM\]|\[NW-PROG' <RUNDIR>/boot.log
```
Gate: the service routine `0x503148e0` **dispatches** (the `rfi` fires — entry.SRR0 PC appears in the
ring, not a `beqlr`/`bgelr` early-return) AND the chain reaches `0x5000ec50` AND no 0xDEADBEEF/SIGSEGV.
Run `ring-walk.py` from the **repo root** (`tools/ring-walk.py`, never `SheepShaver/tools/`). Bonus:
`[NW-PROG]` advances past the prior park.

- [ ] **Step 3: Record verdict (DoD 1 vs 2)**

DoD-1 (dispatched + chain reaches `0x5000ec50`, no crash) → success; capture the boot-progress delta.
DoD-2 (dispatch fails) → record the failure mode: clean early-return (which guard: `+0x38` beqlr or
`+0x44` bgelr — wrong guard/count) vs wrong-target `rfi` (entry.SRR0 wrong → chain diverges before
`0x5000ec50`). Probe `[CGRP+0x20/+0x38/+0x3c/+0x44]` and the entry words at the branch. Isolate via Task 3.
```bash
git add docs/planning/M16-FINDINGS-oracle-forge.md
git commit -F- <<'EOF'
docs(m16): CGRP synthesis smoke boot — dispatch-to-0x5000ec50 acceptance
EOF
```

**Acceptance:** `0x503148e0` dispatches and the chain reaches `0x5000ec50` with no crash (DoD 1), OR
the failing descriptor field / chain hop isolated with ring/probe evidence (DoD 2).

---

## Task 3: Reflight / single-field bisect (only if Task 2 = DoD-2) + close-out

**Files:**
- Modify: `docs/planning/M16-FINDINGS-oracle-forge.md`, `docs/HANDOFF.md`, `docs/AGENT-CONTEXT.md`

- [ ] **Step 1: Vary one descriptor field at a time (bounded)**

If the smoke failed, flip one synthesized value at a time to find which the service routine read wrong:
guard (`+0x38`) vs count (`+0x44`) vs base (`+0x3c`) vs gate (`+0x20`) vs `[entry+0]` SRR0 vs `[entry+4]`
TOC vs the stack model (`+0x40`). One-iteration rule applies: a falsified Task-0 value gets ONE re-pin
from the RE/oracle, then stop and document.

- [ ] **Step 2: Standing gate + revert-on-red**

```bash
cd SheepShaver && make test-jit    # authoritative, score=100
```
If no DoD-1/DoD-2 outcome is reachable within budget, leave the gate OFF-green (code in-tree,
default-off, inert) — never ship a crashing default.

- [ ] **Step 3: Close-out — update resume docs**

Update `docs/HANDOFF.md` (headline + resume prompt) and `docs/AGENT-CONTEXT.md` (current frontier) to
M16's outcome (DoD 1/2/3) and the next entry.
```bash
git add docs/planning/M16-FINDINGS-oracle-forge.md docs/HANDOFF.md docs/AGENT-CONTEXT.md
git commit -F- <<'EOF'
docs(m16): close-out — CGRP-table-synthesis verdict + next-milestone handoff
EOF
```

**Acceptance:** a stated DoD-1/2/3 verdict with cited evidence; `make test-jit` score=100; gate OFF-green if unproven; HANDOFF + AGENT-CONTEXT updated.

---

## Self-review notes (plan vs spec)

- **Spec §1 DoD-1/2/3** → Task 2 (DoD 1/2) + Task 0 (DoD 3 viability fork) + Task 3 (close-out states the outcome). ✓
- **Spec §3 method** (format RE → format oracle → synthesis-smoke → reflight) → Task 0 Steps 1–3 / Task 2 / Task 3 one-to-one. ✓
- **Spec §4 risks** R1(format RE-intractable)→Task0 Steps1–2, R2(source→handler chain)→Task0 Step2, R3(crash)→gating+self-guard+revert-on-red (Tasks 1/3), R4(scratch ownership)→Task0 Step4, R5(QEMU demoted)→Task0 Step3. ✓
- **Spec §2 OUT** (no non-gated delivery change; no IM-init root-cause; no hardcoded base/QEMU addresses) → respected; all changes behind `SS_M16_FORGE`, CGRP base resolved live. ✓
- **Spec §1 entry layout** (`[entry+0]`=SRR0, `[entry+4]`=TOC, SRR1 from r19) → Task 0 known-facts box + Task 1 Step 1. ✓
- **Workflow:** Task 0 BINDING with blocking-answer table + GO/NO-GO fork; flip-last/revert-on-red; gated-off A/B for the structural-inertness substitution; budgets are caps. ✓
