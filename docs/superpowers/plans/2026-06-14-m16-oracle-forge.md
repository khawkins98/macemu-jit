# M16 — Oracle-First NK Routing-Struct Forge Implementation Plan

> **Rev 2 (2026-06-14)** — folded the pre-implementation red-team: (B2) `qemu-rig.sh` hardcodes `-m 512` and (B3) boots **9.2.1 from CD**, not our 9.0.1 — so QEMU is a *weak* value oracle; the address-transfer comparison is now an explicit, mandatory blocking-answer row with a SYNTHESIZE fallback as the *expected* outcome, not the exception. (SF1) SS_SEED_MEM has an MMIO-range guard that silently refuses writes — the three target addresses must be seed-probe-verified before Task 0 trusts them. (NIT) Task 2 acceptance is now conditional on the `r11`-bit disposition from Task 0 Step 4.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Seed a correct, well-informed set of NK routing-struct values so the NewWorld 9.x EXT dispatcher (`0x50314880`) routes onward through `0x50314660`/the service path to the 68k level-1 handler `0x5000ec50` — advancing the boot without the M10-class 0xDEADBEEF crash. The seed values come from a boot where IM init genuinely runs (oracle-first), not blind guesses.

**Architecture:** Front-loaded by a BINDING Task-0 oracle-viability recon that can itself fork the milestone (the foundational risk is whether *any* probeable boot populates these structs — paravirtual HLEs the ROM and likely has no NK structs, leaving QEMU mac99 as the only oracle, with an address-transfer caveat). If oracle is viable, a single env-gated `SS_M16_FORGE=1` seed (applied at NW-trampoline-end behind `MachineProfileIsNewWorld()`) plus an optional MODE_68K CR-injection un-fence; falsifiable ring-confirmed acceptance; revert-on-red.

**Tech Stack:** SheepShaver aarch64 JIT; slot boot protocol (`ss-slot-boot.sh`); QEMU differential rig (`SheepShaver/tools/qemu-rig.sh` + `qemu-mon.py`); diagnostics (`SS_SEED_MEM`, `SS_NW_PIC`, `SS_NW_IRQ_CONSUME`, `SS_DR_R24_RING`, `SS_JIT_TRACE_RING`, `SS_JIT_WATCH_ADDR`, `SS_PROBE_PC`); `tools/ring-walk.py`; capstone disasm of `rom901.bin`.

**Spec:** `docs/superpowers/specs/2026-06-14-m16-oracle-forge-design.md`
**Parent strategy:** `docs/planning/MACHINE-LAYER-PLAN.md`. **Predecessors:** `docs/planning/M14-FINDINGS-cuda-delivery.md` §7 · `docs/planning/M15-FINDINGS-consumption-recon.md` "Verdict (Task 4)" + "Addendum — misroute-why diagnostic".

**Standing rules:** branch `macos-arm64`; never push unprompted; slot boots only (never global pkill); reap strays with `SheepShaver/tools/ss-reap.sh`; `-F-` heredoc commits (no backticks); explicit-path staging (never `git add -A`); struct fields appended LAST; budgets are caps, partial-findings-beat-stalling; one-iteration rule (falsified contract → dated addendum → ONE re-pin → resume; second falsification → stop).

---

## File structure

This milestone creates/modifies:
- **Create:** `docs/planning/M16-FINDINGS-oracle-forge.md` — Task-0 blocking-answer table → per-boot evidence → verdict/handoff.
- **Modify (only if Task 0 returns GO):** one env-gated seed site behind `SS_M16_FORGE=1` + `MachineProfileIsNewWorld()` (likely in the NW-trampoline-end path near the `SS_SEED_MEM` hook); optionally the MODE_68K CR-injection un-fence (`sheepshaver_glue.cpp` ~line 3441) behind the same gate. Paravirtual byte-identical; gated-off A/B required.
- **Capture (not committed; referenced by path):** slot rundirs under `/tmp/ss-slots/`, QEMU rundirs under `/tmp/qemu-rig-*/`, ring dumps, `ring-walk.py` output.

---

## Task 0: BINDING recon — oracle viability + value extraction (can fork the milestone)

**Files:**
- Create: `docs/planning/M16-FINDINGS-oracle-forge.md` (scaffold + blocking-answer table)

Task 0 exists because the milestone's foundational premise is unproven: that a probeable boot exists where IM init populates these structs AND its values transfer to us. Three facts are load-bearing.

- [ ] **Step 1: Resolve the live `hnfo` base (do NOT hardcode `0x68ff4f00`)**

Boot once, read the live `hnfo` pointer from the NK PIC descriptor at `0x68ffefd0` (KDP+0xFD0):
```bash
SheepShaver/tools/ss-slot-boot.sh --label m16-hnfo-resolve --timeout 30 \
  --env 'SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1 SS_PROBE_PC=0x50314880:[0x68ffefd0]'
```
> **Carried caveat (re-verify):** `SS_PROBE_PC` may not fire at the vector-entry PC `0x50314880` (M15 Task-1 found vector-dispatch bypasses the block-entry hook). If it doesn't fire, fall back to a `SS_JIT_WATCH_ADDR`/ring read of `0x68ffefd0`, or probe a PPC PC that is genuinely block-entered near the dispatcher. Record the **live** `hnfo` base; compute `+0x00/+0x14/+0x28` from it.

- [ ] **Step 2: Confirm paravirtual is NOT the oracle (R1)**

Probe a normal paravirtual boot for any NK struct presence at the `hnfo`/`KDP+0x674` addresses. Expected: absent / zero (paravirtual HLEs the ROM; no 9.0.1 NK). Record GO/NO-GO: if paravirtual unexpectedly DOES populate them, that is the cheaper oracle — use it and skip QEMU.

- [ ] **Step 3: Stand up QEMU mac99 as the value oracle (R2) — expect SYNTHESIZE, not literal transfer**

```bash
bash SheepShaver/tools/qemu-rig.sh --timeout 60   # NOTE (red-team B2/B3): the rig hardcodes -m 512
                                                  # and boots 9.2.1 from CD, NOT our 9.0.1/256MB.
python3 SheepShaver/tools/qemu-mon.py --sock /tmp/qemu-rig-*/mon.sock 'x/4xw 0x68ffefd0'   # hnfo base?
# then read hnfo+0x14 (source-table ptr), hnfo+0x28 (pending bits), KDP+0x674 (CR mask),
# and follow hnfo+0x14 to dump the source-table FORMAT (this is the transferable artifact).
```
> **Load-bearing caveat (rev-2, hardened):** QEMU is a *value oracle for NK lowmem only*, NEVER an MMIO-address oracle. **Two confounds make literal value-transfer unlikely:** the rig runs **512 MB** (≠ our 256 MB) and **Mac OS 9.2.1** (≠ our 9.0.1) — both perturb NK heap allocation, so `hnfo`'s guest address and the literal pointer in `hnfo+0x14` almost certainly do NOT transfer. **The transferable artifact is the source-table FORMAT and the CR-mask bit pattern (`KDP+0x674`)** — read QEMU at the *earliest* IM-init-complete point (~5 s, per `qemu-rig.sh` notes; pre-System-customization), not at Finder. **Default assumption: R2 applies (synthesize a host-owned source table, point `hnfo+0x14` at it).** Literal transfer is the exception, allowed only if Step 3a proves address match.

- [ ] **Step 3a: Explicit address-transfer decision (red-team B1/SF2 — mandatory)**

Compare the QEMU `hnfo` base (Step 3) against our **live** `hnfo` base (Step 1). Write the verdict to the blocking-answer table: `hnfo address transfer: <QEMU base> vs <our live base> → LITERAL-OK | SYNTHESIZE-TABLE`. If DIFFER (expected), the seed builds a host-owned table; only the format/CR-mask pattern is borrowed. Do not proceed past Task 0 without this row filled.

- [ ] **Step 3b: Seed-target MMIO-safety probe (red-team SF1 — mandatory)**

`SS_SEED_MEM` silently refuses writes in MMIO range (`vm_is_mmio` guard, `ppc-cpu.cpp`). Before trusting any seed, prove each target accepts a write:
```bash
SheepShaver/tools/ss-slot-boot.sh --label m16-seedprobe --timeout 20 \
  --env 'SS_NW_PIC=1 SS_SEED_MEM=0x68ff4f14=0x12345678;0x68ff4f28=0x12345678;0x68fff674=0x12345678'
grep -a '\[SEED\]' <RUNDIR>/boot.log    # expect 'applied' x3, NOT 'REFUSED (MMIO range)'
```
(Use the LIVE `hnfo` base from Step 1, not the literal `0x68ff4f14`, if Step 1 differs.) If any target is REFUSED, the seed must be host-code at a path that bypasses the SS_SEED_MEM MMIO guard — record that in the table.

- [ ] **Step 4: Capture the `r11` bit `0x8000` gate semantics (R4)**

The dispatcher branches away at `50314884 rlwinm. r9,r11,0,16,16` / `50314888 beq 0x50313ab0` *before* the struct read. On the QEMU oracle (or by static RE of `bl 0x50313d40` prologue + `0x50313ab0`), determine what sets bit `0x8000` of the saved `r11` on a frame that routes onward. Record whether the seed alone is sufficient or the `r11` bit must also be forced.

- [ ] **Step 5: Record the blocking-answer table + GO/NO-GO and commit the scaffold**

Write into `M16-FINDINGS-oracle-forge.md`: live `hnfo` base; the **hnfo address-transfer row** (Step 3a: LITERAL-OK | SYNTHESIZE-TABLE); the **seed-MMIO-safety row** (Step 3b: applied | REFUSED→host-code); the three field values (literal or "synthesize"); `*(r8-0x338)+0x20` source-count target (≥2 to take the service path, per `cmpwi r9,2; blt`); `r11` bit disposition (Step 4); per-field GO/NO-GO; boot budget. Apply the **viability fork**: all fields GO → proceed to Task 1; oracle infeasible (e.g. QEMU 9.2.1/512MB yields no usable format AND static RE of the source-table format is intractable within budget) → write the DoD-3 infeasibility verdict and STOP.
```bash
git add docs/planning/M16-FINDINGS-oracle-forge.md
git commit -F- <<'EOF'
docs(m16): Task-0 recon scaffold — oracle viability + struct-value extraction
EOF
```

**Acceptance:** filled blocking-answer table with a live-resolved `hnfo` base, per-field oracle values (or a documented "synthesize" decision), the `r11`-bit disposition, and an explicit GO/NO-GO viability fork. No seed written yet.

---

## Task 1: Implement the env-gated seed (GO path only)

**Files:**
- Modify: the NW-trampoline-end seed site (behind `SS_M16_FORGE=1` + `MachineProfileIsNewWorld()`)
- Modify: `docs/planning/M16-FINDINGS-oracle-forge.md` (implementation notes)

- [ ] **Step 1: Add the `SS_M16_FORGE` gate + seed writes**

Apply the Task-0 values at NW-trampoline-end: `KDP+0x674` (CR mask), `hnfo+0x28` (pending bits), `hnfo+0x14` (source-table ptr — to a host-owned table if Task 0 said synthesize). All writes guarded by `ExcM16ForgeEnabled() && MachineProfileIsNewWorld()`. If Task 0 found the seed expressible purely via existing `SS_SEED_MEM`, prefer that (no new code) and record it; otherwise add the minimal gated host-code seed.

- [ ] **Step 2: Optional MODE_68K CR-injection un-fence (only if Task 0 said required)**

Behind the same gate, lift the `if (!MachineProfileIsNewWorld())` guard at `sheepshaver_glue.cpp` ~3441 (M14 §7 step 2). Skip if Task 0 showed routing completes without it.

- [ ] **Step 3: Per-commit gate (inner tier) + gated-off A/B**

```bash
cd SheepShaver && make build-ss && SS_HARNESS_BATCH=1 make test-jit   # expect score=100
```
Show a gated-off (`SS_M16_FORGE` unset) byte-identical A/B boot to prove paravirtual/default-path inertness (every new line inside the gate).
```bash
git add <the gated seed file> docs/planning/M16-FINDINGS-oracle-forge.md
git commit -F- <<'EOF'
docs(m16)+feat: SS_M16_FORGE gated NK routing-struct seed (oracle values)
EOF
```

**Acceptance:** `make test-jit` score=100; gated-off A/B byte-identical; seed applies only under `SS_M16_FORGE=1` + newworld.

---

## Task 2: Forge smoke boot — falsifiable acceptance

**Files:**
- Modify: `docs/planning/M16-FINDINGS-oracle-forge.md` (smoke evidence)

- [ ] **Step 1: Run the forge smoke boot**

```bash
SheepShaver/tools/ss-slot-boot.sh --label m16-forge-smoke --timeout 45 \
  --env 'SS_M16_FORGE=1 SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1 SS_DR_R24_RING=1'
```
(`--env` splits on WHITESPACE, not `;`. Do NOT use `SS_JIT_WATCH_DUMPS=0` — it suppresses the ring dump.)

- [ ] **Step 2: Ring-confirm the route**

```bash
python3 tools/ring-walk.py <RUNDIR> --find-pc 0x5000ec50     # GOAL: now > 0 hits
python3 tools/ring-walk.py <RUNDIR> --r24-flow | tail -60
grep -aE 'EXT delivered|SIGSEGV|SIGTRAP|DEADBEEF|\[ALARM\]|\[NW-PROG' <RUNDIR>/boot.log
```
Gate (red-team NIT — conditional on Task 0 Step 4): `0x5000ec50` reached (the 68k L1 handler — previously 0/510) AND no 0xDEADBEEF / SIGSEGV, where the seed set is **(a)** the three struct fields alone if Task 0 Step 4 found the `r11` bit `0x8000` is already set on the EXT frame, OR **(b)** the three fields PLUS the Task 1 Step 2 CR-injection/`r11`-forcing if Step 4 found the bit must also be forced. Run `ring-walk.py` from the **repo root** (`tools/ring-walk.py`, never `SheepShaver/tools/`). Bonus: `[NW-PROG]` advances past the prior park.

- [ ] **Step 3: Record verdict (DoD 1 vs 2)**

DoD-1 (routed, no crash) → success; capture the boot-progress delta. DoD-2 (gate fails) → record which field the dispatcher read wrong (probe `*(r8-0x338)+0x20` and the `r11` bit at the branch), isolate via Task 3.
```bash
git add docs/planning/M16-FINDINGS-oracle-forge.md
git commit -F- <<'EOF'
docs(m16): forge smoke boot — route-to-0x5000ec50 acceptance
EOF
```

**Acceptance:** `0x5000ec50` reached with no crash (DoD 1), OR the failing field isolated with ring/probe evidence (DoD 2).

---

## Task 3: Reflight / single-field bisect (only if Task 2 = DoD-2) + close-out

**Files:**
- Modify: `docs/planning/M16-FINDINGS-oracle-forge.md`, `docs/HANDOFF.md`, `docs/AGENT-CONTEXT.md`

- [ ] **Step 1: Vary one field at a time (bounded)**

If the smoke failed, flip one seed value at a time (CR mask vs pending bits vs source-table ptr vs `r11` bit) to find which controls the `50314898 blt`. One-iteration rule applies: a falsified Task-0 value gets ONE re-pin from the oracle, then stop and document.

- [ ] **Step 2: Standing gate + revert-on-red**

```bash
cd SheepShaver && make test-jit    # authoritative, score=100
```
If no DoD-1/DoD-2 outcome is reachable within budget, leave the gate OFF-green (code in-tree, default-off, inert) — never ship a crashing default.

- [ ] **Step 3: Close-out — update resume docs**

Update `docs/HANDOFF.md` (headline + resume prompt) and `docs/AGENT-CONTEXT.md` (current frontier) to M16's outcome (DoD 1/2/3) and the next entry.
```bash
git add docs/planning/M16-FINDINGS-oracle-forge.md docs/HANDOFF.md docs/AGENT-CONTEXT.md
git commit -F- <<'EOF'
docs(m16): close-out — oracle-forge verdict + next-milestone handoff
EOF
```

**Acceptance:** a stated DoD-1/2/3 verdict with cited evidence; `make test-jit` score=100; gate OFF-green if unproven; HANDOFF + AGENT-CONTEXT updated.

---

## Self-review notes (plan vs spec)

- **Spec §1 DoD-1/2/3** → Task 2 (DoD 1/2) + Task 0 (DoD 3 viability fork) + Task 3 (close-out states the outcome). ✓
- **Spec §3 boots** (oracle-viability → forge-smoke → reflight) → Tasks 0/2/3 one-to-one. ✓
- **Spec §4 risks** R1→Task0 Step2, R2→Task0 Step3, R3→gating+revert-on-red (Tasks 1/3), R4→Task0 Step4 + Task3 bisect. ✓
- **Spec §2 OUT** (no non-gated delivery change; no IM-init root-cause) → respected; all changes behind `SS_M16_FORGE`. ✓
- **Workflow:** Task 0 BINDING with blocking-answer table + GO/NO-GO fork; flip-last/revert-on-red; gated-off A/B for the structural-inertness substitution; budgets are caps. ✓
