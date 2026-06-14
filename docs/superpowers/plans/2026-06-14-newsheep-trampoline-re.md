# Operation NewSheep — Trampoline RE (Task-0) Implementation Plan

> **Rev 2 (2026-06-14) — pre-implementation red-team folded** (3 reviewers, all GO-WITH-FIXES; one
> empirically installed `tbxi 0.13` + dumped our ROM). Load-bearing changes:
> - **Producer reframe (new fork Q0-F):** the Trampoline is the top-level **`MacOS.elf`** (ELF PPC BE,
>   entry `0x20f078`, segs vaddr `0x200000`/`0x100000`) — NOT a parcel; `Parcels/` are PEF device
>   drivers; the **NanoKernel is the `NanoKernel-v02.27` parcel**. The string `CGRP` is **absent from
>   `MacOS.elf`** — the Trampoline does **device-tree + MMU bring-up** (copies OF nodes incl.
>   `interrupt-map`, `claim`/`translate`, `InitializePageMapTable`, then `NanoKernelEntry`), and the
>   **NanoKernel builds CGRP from the device tree.** So the producer is likely **Trampoline + NanoKernel**,
>   not the Trampoline alone. Q0-B is reframed around this; "find the CGRP writes in the Trampoline" is
>   retired (it would hunt writes that don't exist).
> - **Tracer:** no PPC `gdb` exists on this host → use a **Python gdb-remote client** to QEMU's stub;
>   add **`-S`** (halt at reset) or the one-shot Trampoline is missed. Static & dynamic analyze the
>   **same ROM binary** (md5 `66210b4f…`, verified) — version skew is a non-issue; OpenBIOS ≠ Apple OF
>   is the real cross-space difference.
> - **Agreement gate is MECHANISM-LEVEL** (services / write-classes / provenance), never literal
>   values/addresses/counts.
> - **Route-decision honesty:** Q0-A split into call-surface-bounded + stub-data-tractable; full 4-cell
>   truth table; a mechanical-feasibility check per route; DoD-negative is a first-class *costed* outcome.
> - **tbxi facts:** `tbxi dump -o <dir> <rom>`; Trampoline = `MacOS.elf`; NanoKernel = `NanoKernel-v02.27`.

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

**Trampoline artifact identity (empirically verified in red-team, `tbxi 0.13` on our ROM):**
- The Trampoline is the **top-level `MacOS.elf`** in the `tbxi dump` tree — `ELF 32-bit MSB PowerPC`,
  entry `0x20f078`, PT_LOAD segs at vaddr `0x200000` (0x10260) and `0x100000` (memsz 0x19920). ~94 KB.
- `Parcels/` (or `Parcels.src/`) are **PEF device-driver fragments** (`via-cuda…pef`, `keylargo-ata…pef`),
  NOT the Trampoline. The **NanoKernel** is the `MacROM.src/NanoKernel-v02.27` parcel (separate disasm
  target — it, not the Trampoline, is where the `0x5031xxxx` / CGRP / `0x503148e0` code lives).
- **`CGRP` does not appear in `MacOS.elf`.** The Trampoline does device-tree + MMU bring-up; the
  NanoKernel builds CGRP downstream. The "producer" is likely **Trampoline + NanoKernel** (Q0-F).

**Known facts to match behavior against (SheepShaver's *synthesized* guest space — the NanoKernel/SS layer, NOT the Trampoline ELF):**
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
| Q0-F | **Who builds CGRP — the Trampoline directly, or the NanoKernel from the device tree the Trampoline produces?** (decides whether the producer is Trampoline-alone or Trampoline+NanoKernel) | _pending_ | _pending_ | — | — |
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
# SIGNATURE (verified): tbxi dump [-o <outdir>] <input>  — a positional outdir ERRORS.
/tmp/newsheep/venv/bin/tbxi dump -o /tmp/newsheep/dump-9.0.1 "/Users/Shared/macemu/newworld-roms/2001-12-19 - Mac OS ROM 9.0.1.rom"
/tmp/newsheep/venv/bin/tbxi dump -o /tmp/newsheep/dump-8.4   "/Users/Shared/macemu/newworld-roms/2001-07-30 - Mac OS ROM 8.4.rom"
ls -R /tmp/newsheep/dump-9.0.1 | head -60
```
Expected (verified shape): `Bootscript` (CHRP ASCII), **`MacOS.elf`** (the Trampoline — top level), `Parcels.src/` (PEF device drivers + `MacROM.src/{NanoKernel-v02.27, Configfile-1, Mac68KROM, …}`). The Trampoline is `MacOS.elf`, NOT a parcel; the NanoKernel is the `NanoKernel-v02.27` parcel.

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

- [ ] **Step 1: Confirm the Trampoline = `MacOS.elf` (top level), and note the NanoKernel parcel**

```bash
file /tmp/newsheep/dump-9.0.1/MacOS.elf            # expect: ELF 32-bit MSB executable, PowerPC
readelf -h -l /tmp/newsheep/dump-9.0.1/MacOS.elf   # entry ~0x20f078; PT_LOAD vaddr 0x200000 / 0x100000
ls /tmp/newsheep/dump-9.0.1/*Parcels*/*NanoKernel* 2>/dev/null   # the CGRP CONSUMER (separate target)
```
Expected: `MacOS.elf` is the Trampoline (ELF PPC BE). The `Parcels` are PEF drivers; the `NanoKernel-vNN`
parcel is a SEPARATE disasm target (the CGRP builder — see Q0-F, Step 4). Record the Trampoline's entry
+ PT_LOAD vaddrs (disassemble at the correct vaddr, not offset 0).

- [ ] **Step 2: Disassemble the Trampoline (entry + body)**

```bash
# Disassemble the PT_LOAD executable segments AT THEIR ELF vaddr (from readelf -l in Step 1).
python3 - <<'PY'
import capstone
from elftools.elf.elffile import ELFFile   # pip install pyelftools (add to the venv)
md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_BIG_ENDIAN)
elf = ELFFile(open("/tmp/newsheep/dump-9.0.1/MacOS.elf","rb"))
for seg in elf.iter_segments():
    if seg['p_type']=='PT_LOAD' and (seg['p_flags'] & 0x1):   # executable
        va, data = seg['p_vaddr'], seg.data()
        for i in md.disasm(data, va):
            print(f"{i.address:#010x}  {i.mnemonic:8} {i.op_str}")
PY
```
Expected: a PPC instruction stream at the correct vaddr (`0x200000`/`0x100000`), so call targets/relocs read true.

- [ ] **Step 3: Enumerate the OF client-interface call sites (Q0-A)**

The OF client interface is reached through a single entry pointer the bootloader is handed. In the disasm, find where that pointer is loaded and every indirect call through it (`mtctr`/`bctrl`). For each call, recover the OF service name from the packed `{name, n_args, n_rets, args…}` struct it builds (`name` is a C-string ptr).

**CRITERION (falsifiable — NOT string-grep; string presence ≠ on-boot-path call).** Classify on the
**normal disk-boot call path only** — call sites reachable from entry, *excluding* netboot/switch-boot
and any branch guarded by a `'<svc>' not implemented` probe (these Trampolines probe for optional OF
methods and degrade gracefully; `interpret`/`call-method` strings appear precisely on those non-disk
paths). Then split Q0-A into TWO judgments, BOTH required:
1. **Call-surface bounded?** — every boot-path OF call is a pure tree/property/memory query
   (`finddevice`/`getprop`/`getproplen`/`peer`/`child`/`claim`/`translate`/`instance-to-package`/
   `package-to-path`), with NO boot-path `interpret` (arbitrary Forth) or `call-method`-into-a-driver
   whose result is consumed.
2. **Stub-data tractable?** — enumerate the **queried KEYS** (the actual device-tree paths/property
   names, not just the verbs), and for each, can we supply the value from
   `docs/planning/machine/CORE99-MACHINE-DESCRIPTION.md` + the QEMU device-tree oracle?

```bash
strings -a /tmp/newsheep/dump-9.0.1/MacOS.elf | grep -iE "not implemented|finddevice|getprop|call-method|claim|translate|interrupt-map|/chosen|/memory" | sort -u   # candidate-finder ONLY
```
Expected deliverable: the boot-path OF-call list with, per call, the **queried key** + arg shape +
count; and the two-part judgment (surface-bounded? + data-tractable?). "Stubbable" = data-tractable,
NOT merely "short list."

- [ ] **Step 4: Classify the interrupt-setup writes (Q0-B)**

**Q0-F first (falsifiable, with a real null — do NOT assume the Trampoline writes CGRP).** The string
`CGRP` is absent from `MacOS.elf`; the Trampoline's strings point to device-tree + MMU work. So test the
hypothesis both ways: **does the Trampoline write any NK interrupt-routing structure DIRECTLY, OR does
it only build the OF device tree (notably the `interrupt-map` property) + page map that the NanoKernel
later consumes to build CGRP?** Both outcomes are first-class and route-changing:
- If the Trampoline writes NK structures directly → classify those writes (below).
- If it only produces the device tree → **the producer is Trampoline + NanoKernel**; CGRP is built by
  the `NanoKernel-vNN` parcel (a separate disasm target). Record this; it changes Q0-E + the SS sketch
  (we must run/reproduce the NanoKernel's device-tree→CGRP step, not a Trampoline write).

For whichever writes ARE the relevant producer step (Trampoline writes, or — if Q0-F says so — the
NanoKernel's CGRP construction), classify each value's provenance: **`const`** (`lis/ori` immediate) /
**`reloc`** (self-relocation / PC- or base-relative — still reproducible) / **`computed(OF-input)`**
(derives from a `getprop`/device-tree read). Only `computed(OF-input)` is OF-dependent. Map each to its
SheepShaver-space target field by structure/role (NOT literal address — the producer's vaddr ≠ SS's
`0x68ffc1c0`). **Do NOT look for `0x503148e0`/the CGRP field layout inside the Trampoline disasm — that
code is in the `NanoKernel-vNN` parcel, a different address space; analyze it there if Q0-F sends you to it.**

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

> **R1 (top risk, open budget per the brainstorm):** locating + breakpointing the Trampoline as it runs
> is the hard part. **No PPC-capable `gdb` exists on this macOS-arm64 host** (only Apple `lldb`, which
> has no PowerPC support). So do NOT use `gdb`: drive QEMU's gdbstub with a **small Python gdb-remote
> client** (TCP `:1234`, the standard serial protocol: `$Z0` sw-breakpoint, `$c` continue, `$g`/`$p`
> read regs in BE PPC32 order, `$m` read memory). This is in-scope (we already ship `qemu-mon.py`).
> **And the rig must halt at reset:** `qemu-rig.sh --gdbstub` adds only `-s` (stub) — NOT `-S` (freeze).
> Without `-S` the one-shot Trampoline has already run by the time you attach. Add `-S` (patch the rig
> or relaunch QEMU reusing the rig's prebuilt ISO `/tmp/qemu-rig-*/cd_test_901.iso` + its exact
> `QEMU_ARGS`), connect the client, set the Trampoline breakpoint, THEN continue from the reset halt.
>
> **Address-space discipline (Prior-art §3):** QEMU's guest map is its own — the CGRP/KDP/exception-entry
> values from the M-series will NOT sit at the same QEMU addresses. Match the Trampoline's writes to
> structures by **tag/shape/field-role** (the `"CGRP"` tag, the gate/guard/base/count layout), not by
> literal address. Use the rig's captured `info qtree`/`info mtree`/device-tree (`<rundir>/device-tree.txt`)
> to find the Trampoline's QEMU load address. Standing QEMU caveat: behavioral/structural only.

- [ ] **Step 1: Boot 9.2.1 under QEMU with the gdbstub open**

```bash
# The rig's --gdbstub adds -s but NOT -S. We need BOTH (stub + halt-at-reset). Either patch the rig
# to add -S, or relaunch QEMU by hand reusing the rig's prebuilt ISO + QEMU_ARGS:
bash SheepShaver/tools/qemu-rig.sh --gdbstub --timeout 5 2>&1 | tee /tmp/newsheep/rig.log   # capture its QEMU line + ISO path
# Then relaunch with the SAME -M mac99 -m 512 -drive/-cdrom ... PLUS  -s -S  (stub on :1234, CPU halted).
```
Expected: a 9.2.1 mac99 guest **halted at reset**, gdbstub on `:1234`. Record the exact QEMU line in
RESEARCH-LOG. (Build the Python gdb-remote client now if not present — see R1.)

- [ ] **Step 2: Locate the Trampoline's load/run address**

Using the **Python gdb-remote client** (R1): from the reset halt, set a sw-breakpoint (`$Z0`) at the
Trampoline's predicted run address and continue (`$c`). Note: OpenBIOS *claims/relocates* the tbxi
into RAM at runtime, so the ELF vaddr (`0x20f078`) may not be the live address — `info roms`/the
device-tree dump show devices, not the claimed code blob. Realistic method: from `-S`, single-step out
of OpenBIOS to the client-entry handoff (OpenBIOS `go`), or breakpoint OpenBIOS's launch of the tbxi.
Confirm the break by matching the instruction bytes at the break PC against the static disasm head.
Expected: a confirmed breakpoint at the Trampoline entry. **This is the budgeted hard part (R1/R5):
record the method that worked; if it stalls, surface to the user — do not silently grind.**

- [ ] **Step 3: Trace the OF service calls (dynamic Q0-A)**

With the Python client, set a breakpoint (`$Z0`) at the OF client-interface entry the Trampoline calls
through (found statically in Task 2 Step 3). On each hit: read the arg register (`$g`/`$p` → r3 points
at the `{name,n_args,n_rets,args}` struct), then `$m` the `name` C-string to recover the service +
args; continue (`$c`). Log every call. Expected: the runtime OF-service call **sequence** — the dynamic
counterpart to Q0-A's static set. (Watch for OpenBIOS-vs-AppleOF path divergence — see the agreement
gate; a different device-tree shape can send the Trampoline down a different branch.)

- [ ] **Step 4: Trace the interrupt-setup writes (dynamic Q0-B)**

**Locate the structure in QEMU space FIRST** (its QEMU runtime address ≠ Task 2's Trampoline-ELF /
SheepShaver-space address — find it by tag/shape, e.g. scan for the `"CGRP"`/interrupt-map structure in
the claimed region per Q0-F). Then set a write-watchpoint (`$Z2 addr,len`) there with the Python client;
on each hit capture value + PC; continue. Compare provenance (NOT values) against the static
classification: confirm which writes are `computed(OF-input)` at runtime. If Q0-F found the Trampoline
only builds the device tree, watch the `interrupt-map`/device-tree region it writes (and, if pursuing
the consumer, the NanoKernel's CGRP construction). Expected: runtime write PCs + provenance for the
producer step.

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

**Agreement is MECHANISM-LEVEL, not literal (B1 — the gate is unsatisfiable otherwise):**
- **Agreement =** same OF *services* invoked (by name/role); same *kinds* of writes to the same
  *structural fields* (by tag/role); same *provenance class* (`const`/`reloc`/`computed(OF-input)`) per write.
- **NOT criteria (divergence here is EXPECTED, never a gate failure):** literal addresses, literal
  values, exact call counts, OF-tree contents, instruction offsets. (Static = our ROM file; dynamic =
  the *same* ROM binary, md5 `66210b4f…`, but run against **OpenBIOS**, not Apple OF — so values,
  addresses, and possibly a branch or two differ by construction.)
- **A divergence is BLOCKING only when mechanism-level:** e.g. static says a write is `const`, the
  runtime trace proves it `computed(OF-input)`; or the dynamic trace invokes an OF service static
  missed. Pre-declared expected-divergence class: OpenBIOS-vs-AppleOF path differences — log, don't block.

Fill the `Agree?` column at the mechanism level. For each mechanism-level divergence, open an
investigation block (static says X, dynamic says Y, hypothesis, resolution); resolve or mark
OPEN-BLOCKING. Q0-A/B/F close only on mechanism-level corroboration. Never average values.

- [ ] **Step 2: Settle Q0-C (Route-B honesty)**

From the Q0-B classification: if the interrupt-setup writes are constants/relocations, Route B (re-bind/relocate) and Route C (reproduce) are honest. If they are OF-tree-computed, record that **Route C collapses into Route A** (you must reproduce the computation + supply its OF inputs) and **Route B is only honest as a relocation fix, never a value-hardcode**. Fill Q0-C.

- [ ] **Step 3: Decide the route + write the SS-integration sketch (T0.4 / Q0-E)**

**First branch on Q0-F (who builds CGRP):** if the producer is **Trampoline + NanoKernel** (Trampoline
only builds the device tree), the route applies to *that* producer — "run/reproduce" now means the
Trampoline's device-tree build AND the NanoKernel's device-tree→CGRP step. Note this in the sketch.

**Full route truth table — `bounded` means BOTH call-surface-bounded AND stub-data-tractable (B2):**

| Q0-A | Q0-B (the producer's writes) | Route | Honest cost (state in the sketch) |
|---|---|---|---|
| bounded | const / reloc | **A** (run, OF stubbed); **C** also honest | A: stub the (small, tractable) key set. C: reproduce const/reloc writes directly. |
| **bounded** | **computed(OF-input)** | **A** — but C ≡ A here | A cost = **synthesize the device tree the producer reads**: enumerate the exact `getprop` keys the computed writes consume + where each value comes from in our env (CORE99 + QEMU oracle). This is the real cost — the `SS_NW_TRAMPOLINE` problem one level up; do NOT mark "cheap." |
| open | const / reloc | **B or C** (A = "implement OpenFirmware") | tiebreak: **B** if writes are pure relocs repackable via `tbxi build`; else **C**. |
| open | computed(OF-input) | **DoD-negative escalate** | document each route's cost; recommend park-or-narrow. Still a costed, first-class outcome (S2), not a bare "park." |

**Mechanical-feasibility check for the chosen route (S3 — GO only if it passes):** one falsifiable assertion —
- **A:** the Trampoline (`MacOS.elf`) parcel is relocatable into our guest space + its entry ABI (what
  registers / OF-callback pointer it expects on entry, from Task 2's ELF headers/disasm) is one we can supply.
- **B:** `tbxi build` round-trips the dump back to a SheepShaver-loadable ROM (do the repack, even unmodified, as proof).
- **C:** every input the computed writes consume is available in our env (the B2 enumeration is complete).

Write the route decision + the **SS-integration sketch** (integration point, env-gate `SS_M18_*` +
`MachineProfileIsNewWorld()`, what `SS_NW_TRAMPOLINE` becomes) into the Q0-E row + an `## SS-integration
sketch` section. **Reconcile field-by-field against the existing synthesis (Prior-art):** which current
`SS_NW_TRAMPOLINE` writes (`'Hnfo'@0x68ff4f00`, PIC-rail staging, `[KDP+0xf2c]`, the emulator pair
`[KDP+0x1074/0x1078]`) the chosen route **keeps / replaces / makes redundant**; and map the producer's
interrupt-setup writes to their SS-space target fields (the CGRP family) by role.

- [ ] **Step 4: Close the forks + update effort docs**

Mark Q0-A…E resolved in `DECISIONS.md` (move to its Decision Log); append `RESEARCH-LOG.md`; flip `README.md` §9 status (Task-0 done; next milestone = the chosen route's SS integration). Commit:
```bash
git add docs/planning/newsheep/FINDINGS-trampoline-re.md docs/planning/newsheep/DECISIONS.md docs/planning/newsheep/RESEARCH-LOG.md docs/planning/newsheep/README.md
git commit -F- <<'EOF'
docs(newsheep): T0.3/T0.4 — agreement-gate reconciliation + A/B/C route decision + SS sketch
EOF
```

**Acceptance:** Q0-A/B/C/D/F answered with the `Agree?` column filled at the mechanism level (divergences resolved or flagged; OpenBIOS-vs-AppleOF logged not blocked); Q0-C answered for-the-record regardless of chosen route; a committed route decision (A/B/C, **or** the first-class *costed* DoD-negative) that **passed its mechanical-feasibility check (S3)**, with a concrete SS-integration sketch; effort docs updated. **Scope guard (real invariant): no source files staged in any commit this milestone** (RE-only) — verify `git log --stat newsheep-baseline..HEAD` shows only docs.

---

## Self-review notes (plan vs spec)
- **Spec DoD §1.1** (Q0-A/B/C/D cross-instrument, agreement) → Tasks 2 (static) + 3 (dynamic) + 4 Step 1 (gate). ✓
- **Spec DoD §1.2** (route decision + SS-integration sketch) → Task 4 Steps 3. ✓
- **Spec DoD §1.3** (findings doc) → Task 0 + filled throughout. ✓
- **Spec DoD §1.4 / Scope OUT** (no SS code, no boots beyond QEMU) → respected; only docs created/modified; QEMU is RE-only; SS integration is explicitly the NEXT milestone (Task 4 Step 3 sketch). ✓
- **Spec §3 method locks** (both instruments / agreement gate / 9.2 version / tracer required-open-budget) → Tasks 1 (9.2 ROM), 2 (static), 3 (dynamic, open budget + R5 stall-visibility), 4 (gate). ✓
- **Spec §4 risks** R1→Task 3 (top risk, open budget), R2→Task 4 Step 3 (open-ended is a finding), R3→Task 4 Step 1 (divergence→investigation), R4→Task 4 Step 2 (Route-C honesty), R5→Task 3 Step 5 (stall visibility). ✓
- No placeholders except the deliberately-parameterized addresses (`PATH_TO_TRAMPOLINE`, `OF_CLIENT_ENTRY`, `STRUCT_ADDR`) which are *outputs* of earlier steps and cannot be known before the dump/disasm — each is defined by the step that produces it.
