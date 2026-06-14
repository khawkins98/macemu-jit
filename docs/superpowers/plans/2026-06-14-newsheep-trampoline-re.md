# Operation NewSheep — Trampoline RE (Task-0) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Decide HOW to integrate the NewWorld Trampoline into SheepShaver to boot Mac OS 9.2 — by reverse-engineering what the Trampoline reads (OpenFirmware calls) and writes (nanokernel interrupt setup), using two cross-checked instruments, and committing an A/B/C route decision with an SS-integration sketch.

**Architecture:** RE-only recon (no SheepShaver code, no boots beyond QEMU tracing). Two instruments — static (`tbxi` dump + capstone disasm of the Trampoline parcel) and dynamic (QEMU mac99 9.2.1 gdbstub trace) — answer the route forks Q0-A…D, **gated on agreement** (divergence → its own investigation). Output feeds the next, code-writing milestone.

**Tech Stack:** Python `tbxi` (Mac OS ROM unpacker), capstone (PPC BE disasm), QEMU mac99 + gdb (gdbstub tracer), the repo's `SheepShaver/tools/qemu-rig.sh` + `qemu-mon.py`.

**Spec:** `docs/superpowers/specs/2026-06-14-newsheep-trampoline-re-design.md`
**Effort charter:** `docs/planning/newsheep/README.md` · **Forks:** `docs/planning/newsheep/DECISIONS.md`

**Standing rules:** branch `macos-arm64`; never push unprompted; `-F-` heredoc commits (no backticks); explicit-path staging (never `git add -A`); budgets are caps, partial-findings-beat-stalling; one-iteration rule. **No SheepShaver code in this milestone** (RE-only); the route decision feeds the next milestone, which writes the SS integration.

---

## File structure

This milestone creates/modifies (all docs — no source):
- **Create:** `docs/planning/newsheep/FINDINGS-trampoline-re.md` — the recon record: blocking-answer table (Q0-A…E) → per-instrument evidence → reconciliation → route decision + SS-integration sketch.
- **Modify:** `docs/planning/newsheep/DECISIONS.md` (close Q0-A…D), `RESEARCH-LOG.md` (dated entries).
- **Working artifacts (NOT committed; referenced by path):** `tbxi` dump trees under `/tmp/newsheep/`, capstone disasm output, QEMU gdb trace logs.

**Tool/identity facts carried from the brainstorm (implementers re-verify):**
- ROM collection at `/Users/Shared/macemu/newworld-roms/` + `MANIFEST.txt`. The 9.2-era candidates: `Mac OS ROM 8.4` (2001-07-30, md5 `f97d4382…`, ≈9.2/9.2.1) and `Mac OS ROM 9.0.1` (2001-12-19, md5 `66210b4f…`, ≈9.2.2, == our active project ROM).
- Filename version = ROM *file* version, NOT the Mac OS system version (verify internal version via `tbxi` in T0.0).
- QEMU rig boots **9.2.1**; guest→host address add NATMEM is SheepShaver-only (QEMU is its own address space — behavioral oracle only).

---

## Prior-art: banked SS-milestone learnings to LEVERAGE (do not re-derive)

The M8→M17 + machine-layer work already pinned much of the target environment. Read these first;
the RE tasks below cross-reference them so the Trampoline's reads/writes can be *recognized* against
known structures rather than re-discovered. **Implementers re-verify against the chosen 9.2.x ROM.**

**Read-first docs:**
- `docs/AGENT-CONTEXT.md` "Constants" + "NK-published exception entries" — the probe-ready absolutes.
- `docs/planning/M16-FINDINGS-oracle-forge.md` Q5/Q6 — the CGRP descriptor + service-routine RE.
- `docs/planning/M14-FINDINGS-cuda-delivery.md` §7 — the IM-init chicken-and-egg + the Execute68k pair.
- `SheepShaver/docs/DIAGNOSTICS.md` — probe/trace knobs; `docs/planning/machine/CORE99-MACHINE-DESCRIPTION.md` — device tree.
- The existing synthesis we are deciding whether to replace: `SS_NW_TRAMPOLINE` writes in
  `SheepShaver/src/rom_patches.cpp` + `sheepshaver_glue.cpp` (grep `NW-TRAMP` / `SS_NW_TRAMPOLINE`).

**Known facts to match the Trampoline's behavior against (SheepShaver's *synthesized* guest space):**
- `KDP = 0x68ffe000`; `ROMBase = 0x50000000`; NK primary `0x5031xxxx`; mirror emulator base `0x50460000`;
  DR dispatch table `0x50480000`; entry-vector table `0x5046e8c0`.
- **CGRP / the structure the Trampoline must populate (M16):** `*(KDP-0x338) = [0x68ffdcc8] = 0x68ffc1c0`
  (tag `"CGRP"` at +0x04); gate `[CGRP+0x20]` (needs ≥2); handler table `[CGRP+0x38]` guard /
  `[CGRP+0x3c]` descriptor-base / `[CGRP+0x40]` stack-base / `[CGRP+0x44]` count — **all 0 today**
  (the exact writes the Trampoline would make); service routine `0x503148e0`; SPRG0 = KDP.
- **NK exception entries:** EXT `0x50314880` (`[KDP+0x374]`), SC `0x50314ac0`, DEC `0x50313200`, PROGRAM `0x50314700`.
- **Trampoline-staged globals we ALREADY write** (so Route A/C must reconcile/replace, not duplicate):
  `'Hnfo' record @0x68ff4f00` (`[KDP+0xfd0]`); PIC-rail staging `[[KDP-0x20]+0xf18]`; `[KDP+0xf2c]`
  TimebaseSpeed; the Execute68k emulator pair `[KDP+0x1074]=0x50480000` / `[KDP+0x1078]=0x50460000`.

**⚠️ THREE address spaces — keep them distinct (load-bearing):**
1. **ROM-file / Trampoline-ELF vaddr** (static RE, Task 2) — the parcel's own load address.
2. **QEMU runtime guest addresses** (dynamic RE, Task 3) — QEMU's own map; MacIO `0x80000000`, NOT ours.
   The CGRP/KDP *values* above will NOT appear at the same QEMU addresses — match by structure/tag, not address.
3. **SheepShaver synthesized guest addresses** (the M-series facts above) — the **target** for the
   SS-integration sketch (Task 4). The route decision maps the Trampoline's behavior into THIS space.

---

## Task 0 — BINDING blocking-answer table (scaffold first)

**Files:** Create `docs/planning/newsheep/FINDINGS-trampoline-re.md`

- [ ] **Step 1: Write the findings scaffold with the blocking-answer table**

Create `docs/planning/newsheep/FINDINGS-trampoline-re.md` with this content:

```markdown
# NewSheep — Trampoline RE (Task-0) Findings

**Status:** IN PROGRESS · spec `docs/superpowers/specs/2026-06-14-newsheep-trampoline-re-design.md`
**Method:** two instruments (static tbxi+capstone / dynamic QEMU gdbstub), gated on agreement; both on 9.2.

## Blocking-answer table
| # | Question | Static finding | Dynamic finding | Agree? | Status |
|---|----------|----------------|-----------------|--------|--------|
| Q0-D | Canonical 9.2.x ROM + internal version (the "9.0.1=9.2.2" reframe) | _pending_ | n/a | — | — |
| Q0-A | OF client-interface call set the Trampoline makes; bounded-and-stubbable vs open-ended | _pending_ | _pending_ | — | — |
| Q0-B | Interrupt-setup writes: constants/relocations vs computed-from-OF-tree | _pending_ | _pending_ | — | — |
| Q0-C | Can Route B be honest re-binding (relocation), not value-hardcoding? | _pending_ | _pending_ | — | — |
| Q0-E | Route decision (A run / B patch / C reproduce) + SS-integration sketch | _pending_ | _pending_ | — | — |

## Evidence
_(filled per task)_
```

- [ ] **Step 2: Commit the scaffold**

```bash
git add docs/planning/newsheep/FINDINGS-trampoline-re.md
git commit -F- <<'EOF'
docs(newsheep): Task-0 findings scaffold — Trampoline RE blocking-answer table
EOF
```

**Acceptance:** the findings doc exists with the Q0-A…E blocking-answer table, all rows `_pending_`.

---

## Task 1 — T0.0: tbxi tooling + ROM identity (resolve the version reframe)

**Files:** Modify `docs/planning/newsheep/FINDINGS-trampoline-re.md`, `RESEARCH-LOG.md`

- [ ] **Step 1: Install tbxi into an isolated venv**

```bash
python3 -m venv /tmp/newsheep/venv
/tmp/newsheep/venv/bin/pip install tbxi
/tmp/newsheep/venv/bin/tbxi --help
```
Expected: tbxi help text listing `dump` and `build` subcommands. (If `tbxi --help` is unsupported, run `/tmp/newsheep/venv/bin/tbxi dump` with no args to see usage.)

- [ ] **Step 2: Dump the two 9.2-era candidate ROMs**

```bash
mkdir -p /tmp/newsheep
/tmp/newsheep/venv/bin/tbxi dump "/Users/Shared/macemu/newworld-roms/2001-12-19 - Mac OS ROM 9.0.1.rom" /tmp/newsheep/dump-9.0.1
/tmp/newsheep/venv/bin/tbxi dump "/Users/Shared/macemu/newworld-roms/2001-07-30 - Mac OS ROM 8.4.rom"   /tmp/newsheep/dump-8.4
ls -R /tmp/newsheep/dump-9.0.1 | head -60
```
Expected: a tree containing a Bootscript, a Parcelfile + parcel binaries, a Configfile-1 (4 MB PPC ROM) + binaries, and a Romfile (3 MB 68k ROM) + binaries. (If `tbxi dump` wants `dump <rom>` with the output dir derived, adjust to the tool's actual signature observed in Step 1.)

- [ ] **Step 3: Read internal version/build strings to resolve the reframe**

```bash
# tbxi dump writes self-describing text files; the bootscript/parcel text names versions.
grep -rinE "9\.0\.1|9\.2|version|build|trampoline|nanokernel|nki|parcel" /tmp/newsheep/dump-9.0.1/*.txt 2>/dev/null | head -40
# Also scan the raw decompressed sections for OS version strings:
strings -a /tmp/newsheep/dump-9.0.1/*/* 2>/dev/null | grep -iE "Mac OS 9\.|v9\.2|9\.2\.[0-9]" | sort -u | head
```
Expected: evidence of which Mac OS version this ROM file serves. Record whether "Mac OS ROM 9.0.1" (Dec 2001) is the **9.2.2-era** ROM (the reframe). Pick the canonical 9.2.x ROM to carry forward (default: the one whose internal strings name 9.2.x).

- [ ] **Step 4: Record Q0-D in the findings doc + log**

Fill the Q0-D row (canonical 9.2.x ROM + internal version + reframe verdict). Append a `RESEARCH-LOG.md` entry. Commit:
```bash
git add docs/planning/newsheep/FINDINGS-trampoline-re.md docs/planning/newsheep/RESEARCH-LOG.md
git commit -F- <<'EOF'
docs(newsheep): T0.0 — tbxi up; 9.2.x ROM identity + version reframe verdict (Q0-D)
EOF
```

**Acceptance:** `tbxi dump` produced a parcel tree; Q0-D is answered with the internal OS version of the chosen ROM (the "9.0.1=9.2.2" reframe confirmed or refuted from ROM strings, not just the date).

---

## Task 2 — T0.1: static RE of the Trampoline (OF calls + write classification)

**Files:** Modify `docs/planning/newsheep/FINDINGS-trampoline-re.md`

- [ ] **Step 1: Identify and extract the Trampoline parcel**

```bash
# The Parcelfile lists named parcels; find the Trampoline (an ELF/PEF bootloader parcel).
cat /tmp/newsheep/dump-9.0.1/Parcels/Parcelfile 2>/dev/null || find /tmp/newsheep/dump-9.0.1 -iname "*parcel*" -o -iname "*tramp*" | head
# Determine the Trampoline binary's path and format:
file $(find /tmp/newsheep/dump-9.0.1 -type f | grep -iE "tramp|boot|elf") 2>/dev/null
```
Expected: a parcel binary identified as the Trampoline; `file` reports ELF (PPC) or PEF. Record its path + format. (If parcel naming is opaque, the Trampoline is the ELF among the parcels — `file` on each parcel binary disambiguates.)

- [ ] **Step 2: Disassemble the Trampoline (entry + body)**

```bash
# If ELF: use the ELF entry/sections. Generic capstone disasm of the code section:
python3 - <<'PY'
import capstone, subprocess, sys
path = "PATH_TO_TRAMPOLINE_FROM_STEP1"
data = open(path,"rb").read()
md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_BIG_ENDIAN)
# Disassemble the whole file as a first pass (refine to the .text range once ELF headers are read):
for i in md.disasm(data, 0):
    print(f"{i.address:#010x}  {i.mnemonic:8} {i.op_str}")
PY
```
Expected: a PPC instruction stream. (Refine: read the ELF program headers to disassemble only executable segments at their correct vaddr — `readelf -h/-l` on the parcel if ELF.)

- [ ] **Step 3: Enumerate the OF client-interface call sites (Q0-A)**

The OF client interface is reached through a single entry pointer (the "OF client interface handler" the bootloader is handed). In the disasm, find where that pointer is loaded and every indirect call through it (`mtctr`/`bctrl` or `mtlr`/`blrl` on the OF entry). For each call, recover the OF service name from the argument array the call builds (OF calls pass a packed `{name, n_args, n_rets, args…}` struct; the `name` is a C-string pointer).

```bash
# Heuristic scan for the OF service-name strings the Trampoline references:
strings -a PATH_TO_TRAMPOLINE | grep -iE "finddevice|getprop|callmethod|claim|instance-to-package|peer|child|interpret|package-to-path|map|translate" | sort -u
```
Expected: the set of OF services the Trampoline calls (e.g. `finddevice`, `getprop`, `call-method`, `claim`, `interpret`…). Classify the set: **bounded-and-stubbable** (a small, fixed list of pure tree/property queries) vs **open-ended** (e.g. `interpret` running arbitrary Forth, `call-method` into device drivers). Record per-service the call count + arg shape.

- [ ] **Step 4: Classify the interrupt-setup writes (Q0-B)**

Find the writes that populate the nanokernel interrupt structures (the CGRP descriptor + handler table — `*(KDP-0x338)` family per M16; in the Trampoline these are writes to the structures it builds before handoff). **Cross-reference the Prior-art CGRP map** (`[CGRP+0x20]` gate, `[CGRP+0x38/+0x3c/+0x44]` guard/base/count, the `0x503148e0` service-routine layout) so the Trampoline's writes are *recognized* as building exactly the structures M16 found empty — match by structure/field role, not by literal address (the Trampoline-ELF vaddr ≠ SheepShaver's `0x68ffc1c0`). For each write, trace the stored value's provenance: **immediate/relocation** (constant — `lis/ori`, or a reloc against a known base) vs **computed-from-OF-read** (the value derives from a `getprop`/device-tree result). Record each setup write as `const | reloc | computed(<which OF input>)`, and map each to its SheepShaver-space target field (the integration handle for Task 4).

- [ ] **Step 5: Record the static findings**

Fill the Q0-A and Q0-B **static** columns. Commit:
```bash
git add docs/planning/newsheep/FINDINGS-trampoline-re.md
git commit -F- <<'EOF'
docs(newsheep): T0.1 static RE — Trampoline OF-call set (Q0-A) + write classification (Q0-B)
EOF
```

**Acceptance:** Q0-A static = the enumerated OF-service set with a bounded/open-ended judgment; Q0-B static = every interrupt-setup write classified const/reloc/computed, with the computed ones naming their OF input.

---

## Task 3 — T0.2: QEMU mac99 Trampoline tracer (dynamic) — REQUIRED, open budget

**Files:** Modify `docs/planning/newsheep/FINDINGS-trampoline-re.md`, `RESEARCH-LOG.md`

> **R1 (top risk, open budget per the brainstorm):** locating + breakpointing the Trampoline as it runs is the hard part. Use the static disasm (Task 2) to predict the Trampoline's entry signature, and QEMU's gdbstub for real breakpoints (the monitor alone is too weak).
>
> **Address-space discipline (Prior-art §3):** QEMU's guest map is its own — the CGRP/KDP/exception-entry
> values from the M-series will NOT sit at the same QEMU addresses. Match the Trampoline's writes to
> structures by **tag/shape/field-role** (the `"CGRP"` tag, the gate/guard/base/count layout), not by
> literal address. Use the rig's captured `info qtree`/`info mtree`/device-tree (`<rundir>/device-tree.txt`)
> to find the Trampoline's QEMU load address. Standing QEMU caveat: behavioral/structural only.

- [ ] **Step 1: Boot 9.2.1 under QEMU with the gdbstub open**

```bash
# Start the rig; if it doesn't expose -s/-S, launch QEMU directly with gdbstub:
bash SheepShaver/tools/qemu-rig.sh --timeout 600 &   # boots 9.2.1 mac99, monitor socket up
# Confirm the rig's QEMU line; if needed, relaunch QEMU with `-s -S` (gdbstub on :1234, halted at reset).
python3 SheepShaver/tools/qemu-mon.py --sock /tmp/qemu-rig-*/mon.sock 'info roms' 2>/dev/null | head
```
Expected: a running 9.2.1 mac99 guest with a gdb stub reachable (`:1234`). Record the exact QEMU invocation used (note in RESEARCH-LOG for repeatability).

- [ ] **Step 2: Locate the Trampoline's load/run address**

```bash
# The Trampoline is loaded by OF before the nanokernel. Use the rig's captured device-tree/info,
# and the static ELF entry (Task 2) to predict the load vaddr. Confirm via gdb:
gdb -q -ex 'target remote :1234' \
    -ex 'monitor info registers' \
    -ex 'set pagination off'
# In gdb: set a breakpoint at the predicted Trampoline entry, continue, confirm PC + a known
# instruction word from the static disasm matches at that address.
```
Expected: a confirmed breakpoint at the Trampoline entry (the instruction bytes at the break PC match the static disasm head). This step is the budgeted hard part; record the method that worked.

- [ ] **Step 3: Trace the OF service calls (dynamic Q0-A)**

In gdb, breakpoint the OF client-interface entry the Trampoline calls through (the pointer found statically in Task 2 Step 3). Each hit: dump the OF call struct (read the `{name,n_args,n_rets,args}` from the arg register/stack) to recover the service name + args. Log every call.
```bash
# gdb script sketch (refine addresses from Steps 1-2):
gdb -q -ex 'target remote :1234' \
    -ex 'break *OF_CLIENT_ENTRY' \
    -ex 'commands' -ex 'silent' -ex 'x/s *(int*)$r3' -ex 'printf "OF call\n"' -ex 'continue' -ex 'end' \
    -ex 'continue'
```
Expected: the runtime OF-service call sequence — the dynamic counterpart to Q0-A's static set.

- [ ] **Step 4: Trace the interrupt-setup writes (dynamic Q0-B)**

Watch the guest addresses the nanokernel interrupt structures live at (the CGRP/handler-table region the Trampoline builds). For each write, capture value + the PC. Compare values against the static const/computed classification: confirm which writes carry computed (OF-derived) values at runtime.
```bash
# gdb hardware watchpoints on the structure region (addresses from Task 2 Step 4 / the static build):
gdb -q -ex 'target remote :1234' -ex 'watch *STRUCT_ADDR' -ex 'commands' -ex 'printf "wrote %x at pc %x\n", *STRUCT_ADDR, $pc' -ex 'continue' -ex 'end' -ex 'continue'
```
Expected: the runtime write values + PCs for the interrupt-setup region.

- [ ] **Step 5: Record the dynamic findings + tear down**

Fill the Q0-A and Q0-B **dynamic** columns. Kill the QEMU rig (its own process — NOT a SheepShaver slot; do not use ss-reap). Commit:
```bash
git add docs/planning/newsheep/FINDINGS-trampoline-re.md docs/planning/newsheep/RESEARCH-LOG.md
git commit -F- <<'EOF'
docs(newsheep): T0.2 dynamic RE — QEMU Trampoline trace: OF calls + setup writes (Q0-A/B)
EOF
```

**Acceptance:** the QEMU tracer ran; Q0-A dynamic = the runtime OF-call sequence; Q0-B dynamic = runtime setup-write values+PCs. (If bring-up stalls, per R5: record progress honestly and surface a decision point to the user — do not silently grind. The budget is open but the stall must be visible.)

---

## Task 4 — T0.3 + T0.4: reconcile (agreement gate) and decide the route

**Files:** Modify `docs/planning/newsheep/FINDINGS-trampoline-re.md`, `DECISIONS.md`, `RESEARCH-LOG.md`, `docs/planning/newsheep/README.md` (status)

- [ ] **Step 1: Apply the agreement gate (T0.3)**

For Q0-A and Q0-B, compare static vs dynamic. Fill the `Agree?` column. For each **divergence**, open an investigation block in the findings doc (static says X, dynamic says Y, hypothesis, resolution) — resolve it or mark it OPEN-BLOCKING. Q0-A/B close ONLY when static and dynamic corroborate (or the divergence is explained). Never average.

- [ ] **Step 2: Settle Q0-C (Route-B honesty)**

From the Q0-B classification: if the interrupt-setup writes are constants/relocations, Route B (re-bind/relocate) and Route C (reproduce) are honest. If they are OF-tree-computed, record that **Route C collapses into Route A** (you must reproduce the computation + supply its OF inputs) and **Route B is only honest as a relocation fix, never a value-hardcode**. Fill Q0-C.

- [ ] **Step 3: Decide the route + write the SS-integration sketch (T0.4 / Q0-E)**

Apply the decision logic:
- **Q0-A bounded-and-stubbable** → **Route A viable** (run the Trampoline against a stubbed OF). SS sketch: execute the Trampoline parcel at the `SS_NW_TRAMPOLINE` handoff, stubbing the bounded OF-call set, behind `SS_M18_*` + `MachineProfileIsNewWorld()`.
- **Q0-A open-ended** → Route A is "implement OpenFirmware" → prefer **B/C**. If Q0-B = constants/relocs → **Route C** (informed host-reproduction) or **Route B** (tbxi-patch + relocate). SS sketch names the integration point accordingly.
- **Q0-A open-ended AND Q0-B computed** → escalate: document the integration cost; recommend park-or-narrow to the user (DoD-negative is valid).

Write the route decision + the **SS-integration sketch** (integration point, env-gate, what `SS_NW_TRAMPOLINE` becomes) into the findings doc Q0-E row + an `## SS-integration sketch` section. **Reconcile against the existing synthesis (Prior-art):** the sketch must state, field by field, which of the current `SS_NW_TRAMPOLINE` writes (`'Hnfo'@0x68ff4f00`, the PIC-rail staging, `[KDP+0xf2c]`, the emulator pair `[KDP+0x1074/0x1078]`) the chosen route **keeps, replaces, or makes redundant** — and map the Trampoline's interrupt-setup writes (Task 2 Step 4) to their SheepShaver-space target fields (the CGRP `[0x68ffc1c0+…]` family). This is what turns the RE into an actionable SS integration for the next milestone.

- [ ] **Step 4: Close the forks + update effort docs**

Mark Q0-A…E resolved in `DECISIONS.md` (move to its Decision Log); append `RESEARCH-LOG.md`; flip `README.md` §9 status (Task-0 done; next milestone = the chosen route's SS integration). Commit:
```bash
git add docs/planning/newsheep/FINDINGS-trampoline-re.md docs/planning/newsheep/DECISIONS.md docs/planning/newsheep/RESEARCH-LOG.md docs/planning/newsheep/README.md
git commit -F- <<'EOF'
docs(newsheep): T0.3/T0.4 — agreement-gate reconciliation + A/B/C route decision + SS sketch
EOF
```

**Acceptance:** Q0-A/B/C/D answered with the `Agree?` column filled (divergences resolved or flagged); a committed route decision (A/B/C, or DoD-negative escalation) with a concrete SS-integration sketch; effort docs updated; `make test-jit` untouched (no code changed this milestone).

---

## Self-review notes (plan vs spec)
- **Spec DoD §1.1** (Q0-A/B/C/D cross-instrument, agreement) → Tasks 2 (static) + 3 (dynamic) + 4 Step 1 (gate). ✓
- **Spec DoD §1.2** (route decision + SS-integration sketch) → Task 4 Steps 3. ✓
- **Spec DoD §1.3** (findings doc) → Task 0 + filled throughout. ✓
- **Spec DoD §1.4 / Scope OUT** (no SS code, no boots beyond QEMU) → respected; only docs created/modified; QEMU is RE-only; SS integration is explicitly the NEXT milestone (Task 4 Step 3 sketch). ✓
- **Spec §3 method locks** (both instruments / agreement gate / 9.2 version / tracer required-open-budget) → Tasks 1 (9.2 ROM), 2 (static), 3 (dynamic, open budget + R5 stall-visibility), 4 (gate). ✓
- **Spec §4 risks** R1→Task 3 (top risk, open budget), R2→Task 4 Step 3 (open-ended is a finding), R3→Task 4 Step 1 (divergence→investigation), R4→Task 4 Step 2 (Route-C honesty), R5→Task 3 Step 5 (stall visibility). ✓
- No placeholders except the deliberately-parameterized addresses (`PATH_TO_TRAMPOLINE`, `OF_CLIENT_ENTRY`, `STRUCT_ADDR`) which are *outputs* of earlier steps and cannot be known before the dump/disasm — each is defined by the step that produces it.
