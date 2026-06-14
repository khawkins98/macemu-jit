# SS_M18 Stage 2a — OF-CI callback + Core99 device-tree: Task-0 RECON findings

**Status:** COMPLETE (2026-06-14) — **RESIDUE-PASS** (the *expected* case: Q-S2a.3 tuple-shape +
§5-Q8 input numbers are a flagged-residue-to-S2b; Q-S2a.1 and Q-S2a.2 close affirmatively with no
*blocking* residue, so S2a-impl dispatch + DT-query work is cleared to start).
**Authority:** `docs/superpowers/plans/2026-06-14-ss-m18-s2a-ofci-dt-task0.md` (rev-2, amendments
P-M1..M4 / A1 folded). **Method:** static doc/source reads + grep + md5 — **ZERO boots, ZERO NK
disasm** (the OF inventory is statically closed 177/177 + 24/24; the DT shape is a doc read).
**Inputs cross-walked:** `FINDINGS-trampoline-re.md` Q0-A/B (OF inventory) × `CORE99-MACHINE-DESCRIPTION.md`
§1/§2/§3/§5 × `DONOR-NOTES.md` Donor 3.

**Evidence-tag legend:** `[STATIC-INV]` = the static Trampoline RE inventory (FINDINGS-trampoline-re
Q0-A/B, `tramp.asm`); `[QEMU-BEHAVIORAL]` = the runtime OpenBIOS trace (FINDINGS-trampoline-re T0.2);
`[CORE99]` = `CORE99-MACHINE-DESCRIPTION.md` (file:line); `[OF-STD]` = IEEE-1275 / CHRP standard node
(scaffolding the device skeleton doesn't tabulate). All addresses re-verified against source this run.

> **rev-2 (2026-06-14) — 3-reviewer round folded (spec+tech UPHOLD · adversary OPTIMISTIC-BUT-OK).**
> Verdict RESIDUE-PASS upheld; S2a-impl dispatch/DT-query cleared. **One BINDING amendment (ADV-1):**
> the path-divergence "RECONCILED via `/aliases`+`canon`" claim in Q-S2a.1 below is the **wrong
> mechanism** — the adversary (re-deriving from `tramp.asm`/`ofcalls.json` + CORE99 grep) showed real OF
> `finddevice` resolves `/pci/mac-io/...` against `/pci@f2000000/mac-io@c/...` by **per-component
> node-name-vs-unit-address matching built into finddevice**, NOT an `/aliases` table lookup (CORE99 has
> 0 `aliases` hits; `canon` is called ×1). Non-blocking for the recon, but a **false-green trap for
> S2a-impl** — so it is a BINDING S2a-impl design constraint: (a) the unit-test battery MUST issue the
> **short static spellings verbatim** (`/pci/mac-io/interrupt-controller`, etc.), NOT canonical/aliased
> forms; (b) `finddevice` MUST implement OF **component-wise, unit-address-insensitive matching** — not
> exact-string + alias lookup. Else G2a.c goes green while the real Trampoline's short form misses at
> first integration boot. Minor dent (non-blocking): `/rom/macos` is not an IEEE-1275 node; the DT just
> needs a node spelled `/rom/macos` to answer the observed query. Spec+tech: 2 cosmetic minors only
> (the `@40000` unit-address is a synthesis to confirm at first boot).

---

## Gate-item-0 — parcel provenance (re-verified this run)

| Artifact | Expected md5 | Verified md5 | Source |
|---|---|---|---|
| Canonical 9.0.1 ROM | `66210b4f71df8a580eb175f52b9d0f88` | `66210b4f71df8a580eb175f52b9d0f88` ✓ | `/Users/Shared/macemu/2001-12-19 - Mac OS ROM 9.0.1.rom` |
| NanoKernel-v02.27 parcel (105280 B) | `61c176e90b6365e84e5c660d703e56af` | `61c176e90b6365e84e5c660d703e56af` ✓ | `/tmp/newsheep/dump-9.0.1/Parcels.src/MacROM.src/NanoKernel-v02.27` |

Both match. The Trampoline `MacOS.elf` + the Q3 trace derive from this md5-verified parcel set.
(The *active project* boot ROM is the 1.1 ROM `e0fc03faa589ee066c411b4603e0ac89`; the RE binary is the
staged 9.0.1, both instruments analyze the same `66210b4f…`.)

---

## Q-S2a.1 — DT read set + order (cross-walk of the OF inventory to CORE99 §3) — **CLOSED (with named over-provision residue)**

The Trampoline's full DT-query surface, mapped to the node/property the synthesized DT must answer.
Every static finddevice path and getprop key resolves to a node or property; the gaps are (a) OF-standard
*scaffolding* nodes the device skeleton (§3) doesn't tabulate, and (b) the named-dynamic residue (P-M3).

### finddevice paths → node (11 static `[STATIC-INV]` FINDINGS L84-86 + 2 dynamic)

| Path (as issued) | Count | DT node it must resolve to | Provenance |
|---|---|---|---|
| `/chosen` | ×4 | OF standard `/chosen` (holds `bootpath`, `stdin/stdout`, `mmu`/`memory` ihandles) | `[OF-STD]` — **NOT in §3**; impl must add |
| `/aliases` | ×3 | OF standard `/aliases` (short-path → canonical map) | `[OF-STD]` — **NOT in §3**; load-bearing for the path divergence (below) |
| `/` | ×3 | root `/` | `[CORE99]` §3 L92 |
| `/rtas` | ×3 | RTAS node (paired with `call-method instantiate-rtas`) | `[OF-STD]` — **NOT in §3**; minimal RTAS stub node |
| `/cpus/@0` | ×1 | the cpu node `/PowerPC,G4` reached via a `/cpus` container | `[CORE99]` §3 L93 (node), `[OF-STD]` (`/cpus` container path) |
| `/cpus/@0/l2-cache` | ×1 | l2-cache child of the cpu | `[CORE99]` §3 L93 (cache props `name_registry.cpp:298-328`) |
| `/cpus/@0/l2-cache/l2-cache` | ×1 | nested l2 (unified) child | `[CORE99]` §3 L93 |
| `/options` | ×1 | OF standard `/options` (nvram-backed config vars) | `[OF-STD]` — **NOT in §3** |
| `/rom/macos` | ×1 | the ROM/parcels node (`AAPL,toolbox-parcels` lives here; cf. `/AAPL,ROM`) | `[CORE99]` §3 L95 (`/AAPL,ROM`) + `[OF-STD]` (`/rom/macos` sub-path) |
| `/pci/mac-io/interrupt-controller` | ×1 | the OpenPIC node — **see 3-way divergence below** | `[CORE99]` §3 L97 (`/mac-io/interrupt-controller`, `open-pic`) |
| dynamic #1 (runtime concrete) | — | `/pci@f2000000/mac-io@c/ata-3@20000/cdrom@0` (boot disk path) | `[QEMU-BEHAVIORAL]` FINDINGS L156-159 |
| dynamic #2 | — | observed-but-unnamed in the static `None`-slot set | `[STATIC-INV]` "+2 dynamic" L86 — residue (P-M3) |

**Node-set obligation flagged:** the device skeleton (CORE99 §3) tabulates the *hardware* nodes
(`/`, cpu, `/memory`, `/AAPL,ROM`, `/mac-io` + children, `/video`, `/ethernet`) but NOT the OF-standard
*scaffolding* the Trampoline finddevices first — `/chosen`, `/aliases`, `/options`, `/rtas`, the `/cpus`
container, `/rom/macos`. These are IEEE-1275 standard nodes (answerable without a boot), so this is an
**S2a-impl build obligation, NOT a coverage-UNKNOWN** — it does not block. The DT model owns them.

### getprop / getproplen keys → property (37 static `[STATIC-INV]` FINDINGS L89-92 + 8 dynamic)

All 37 named static keys are standard Core99 DT properties, sourced from CORE99 §3 (which cross-checks
them to `name_registry.cpp:85-388`, the props SheepShaver already fakes). Representative mapping:

| Key family | Resides on | Provenance |
|---|---|---|
| `reg`, `device_type`, `compatible`, `name`, `model` | every node | `[CORE99]` §3 (per-node `reg` from §1; names `name_registry.cpp:92-385`) |
| `#address-cells`, `#interrupt-cells`, `interrupt-parent`, `ranges` | bus nodes (`/`, `/pci`, `/mac-io`) | `[CORE99]` §3 + §2 interrupt-parent chain |
| **`interrupt-map`, `interrupt-map-mask`** | `/mac-io/interrupt-controller` | `[CORE99]` §3 L97 / §2 — **the Q-S2a.3 residue; shape NOT tabulated** |
| `assigned-addresses`, `AAPL,address`, `slot-names` | `/pci` device nodes (MMIO layout) | `[CORE99]` §1 addr map |
| `AAPL,toolbox-parcels`, `AAPL,reserved-memory-space`/`-io-space` | `/rom/macos`, `/chosen` | `[STATIC-INV]` setprop edits L97-98 (Trampoline also *writes* these) |
| `cache-unified`, cpu clock/cache keys | cpu / l2-cache | `[CORE99]` §3 L93 (`name_registry.cpp:131-328`) |
| `display-type`, `screen` | `/video` | `[CORE99]` §3 L101 |
| `bootpath` | `/chosen` | `[OF-STD]` |

**P-M3 — the +8 dynamic getprop / +2 dynamic finddevice NAMES (BINDING disposition):** the static
inventory records "+8 dynamic" getprop keys and "+2 dynamic" finddevice paths *without individually
enumerating all of them*. The `[QEMU-BEHAVIORAL]` trace (FINDINGS L161-162) partially names the dynamic
getprop family as **PCI-config-probe keys** (`vendor-id`, `device-id`, `class-code`, …) and names one
dynamic finddevice path (`/pci@f2000000/mac-io@c/ata-3@20000/cdrom@0`). The remaining individual names
are **observed-but-unnamed** and obtaining the exact set would require re-running the trace — **forbidden
(zero-boot is LAW; Stop-rule #3).** Per P-M3, recorded as:

> **8 getprop keys + 2 finddevice paths observed-but-unnamed** (PCI-config-probe family partially named;
> one concrete cdrom boot-path named). → The DT **over-provisions the full CORE99 property set + the
> PCI-config-probe family per node**; named confirmation of the exact dynamic set is a **flagged residue,
> NOT closed here**. Sampled only against QEMU OpenBIOS (≠ Apple OF) — full real-Core99 name closure is
> owed to the first integration boot (A1, below). This is NOT a backdoor to re-run the trace.

### 3-way path divergence on the interrupt-controller node — **RECONCILED**

| Source | Path |
|---|---|
| static inventory `[STATIC-INV]` | `/pci/mac-io/interrupt-controller` (FINDINGS L86) |
| CORE99 §3 `[CORE99]` | `/mac-io/interrupt-controller` (L97) |
| runtime canonical `[QEMU-BEHAVIORAL]` | `/pci@f2000000/mac-io@c/…` (FINDINGS L159; OpenBIOS fully-qualified unit-address form) |

**Closure (binding per the rev-2 minor):** pin the **canonical path** as the OpenBIOS/Apple fully-qualified
unit-address form `/pci@f2000000/mac-io@c/interrupt-controller@40000` (the `@40000` = OpenPIC offset within
MacIO, CORE99 §1 L47). The other two are **alias / short-path spellings** that the DT must resolve via:
- an **`/aliases` node** mapping `mac-io` (and friends) to the canonical path, AND
- the **`canon` service** (one of the 21 direct verbs, `[STATIC-INV]` L76) which normalizes any
  short/alias path to canonical.

→ **Path-divergence closure criterion met:** canonical path pinned + `/aliases`+`canon` handling decided +
every static finddevice path resolves under the published canonical-or-alias set. `/aliases` and `canon`
are therefore **in-scope for S2a's DT** (red-team self-review tension #3 confirmed in-scope, not deferred).

**Q-S2a.1 verdict:** CLOSED. The read-set + order is pinned; the static surface maps fully; the only
residue is the **named-dynamic over-provision** (P-M3) which is answered by over-provisioning, not a
DT-coverage-UNKNOWN → **non-blocking** for S2a-impl dispatch/DT-query.

---

## Q-S2a.2 — CHRP entry register contract — **CLOSED (r5 confirmed; r3/r4 = flagged residue to S2b)**

Confirmed from `[STATIC-INV]` (FINDINGS L59-67, L222-238) and cross-checked at the dynamic launch
(FINDINGS L145-148, PC + `r2` identical at runtime):

- **`r5` = OF-CI callback pointer — CONFIRMED.** Handed to the Trampoline in **`r5`** at launch; the
  Trampoline stashes it at **`[r2-0x60]`** (`r2 = 0x1001e8`, small-data base) and calls it *only* through
  the 3 wrappers (`0x20dbec` / `0x20dcc0` / `0x20ddb4`) via indirect glue `0x21024c`. Entry **`0x20f078`**
  (PIC self-reloc stub); `MacOS.elf` = `ET_EXEC`, 2 `PT_LOAD` @ vaddr `0x200000` (exec) / `0x100000` (data).
- **`r3` / `r4` — NOT pinned in the inventory → flagged residue.** The findings do not establish r3/r4 at
  launch. Per the plan + the rev-2 minor: the **S2a unit-test launch double asserts NOTHING about r3/r4
  (injects `r5` only)**, so a `r3/r4 = 0` default CANNOT congeal into a hidden S2b contract. r3/r4 is
  **handed to S2b's loader** to confirm against the trace; it is NEVER a guessed ABI.

**Q-S2a.2 verdict:** CLOSED. r5 affirmatively confirmed (the only entry dependency the dispatch contract
needs); r3/r4 carried as a non-blocking residue to S2b. Does **not** block S2a dispatch/DT work; blocks
only any S2a *claim* about r3/r4.

---

## Q-S2a.3 — interrupt-map / interrupt-map-mask tuple SHAPE — **FLAGGED-RESIDUE-TO-S2b (the expected discovery)**

Window: CORE99 §2 (interrupt tree), §3 (interrupt-controller row), §5 Q6/Q8 — **ONLY**. No QEMU value
reconstruction, no NK disasm (Stop-rule #3/#6).

**Finding — CORE99 has ZERO `interrupt-map` tuple cell-shape.** §3 L97 names the node
(`/mac-io/interrupt-controller`, `device_type=open-pic`, `reg=(0xF3040000, see §5 Q6)`) and §2 (L63-72)
tabulates *per-device PIC input numbers*, but **neither tabulates the `interrupt-map`/`-mask` tuple cell
layout** (cells-per-row, parent-phandle encoding, child-unit-address spec, `#interrupt-cells` width). The
technical reviewer independently confirmed ZERO `interrupt-map` hits in the doc. The residue is therefore
**unavoidable**, not an oversight.

**P-M4 (binding) — the residue covers BOTH:**
1. **The tuple cell-SHAPE** — missing entirely from CORE99. What is missing, precisely: the per-row cell
   count, the child interrupt-specifier width (`#interrupt-cells`), the parent phandle encoding, and the
   child unit-address mask in `-mask`.
2. **The §5-Q8 PIC input-number CONTENTS** — `0x24` (ESCC ch B) / `0x25` (ESCC ch A) / `0x19` (VIA-Cuda)
   are **QEMU-only** (`macio.h:51-54`, CORE99 §2 L65-67), explicitly flagged in §5 Q8 (L194-196) as
   *not yet cross-checked against DingusPPC or Apple KeyLargo*. These values feed the `interrupt-map` rows
   and may NOT be fabricated into a "half-closed" tuple.

Neither the shape nor the input numbers may be fabricated. Both defer to S2b with a **named confirmatory-boot
owner** (the first integration boot). The OpenPIC region size (§5 Q6, L188-190) stays an M3 question, not
sized here (per the technical-reviewer minor).

**Disposition (per plan PASS-cond #3 + blocking table):** the DT node **exists and answers the `getprop
interrupt-map` / `-mask` query**, but the returned value is marked **provisional / NON-FINAL**. Per the
**P-M2 carve-out**, these rows are **EXCLUDED from the unit-test value-coverage**: the test asserts only
(a) the query dispatches/resolves and (b) `getproplen` returns a NON-FINAL length — it NEVER asserts the
tuple cell content as a PASS criterion (which would let the provisional self-validate — the M8→M17
false-green pattern).

**Q-S2a.3 verdict:** FLAGGED-RESIDUE-TO-S2b — **the expected "one real discovery."** Does NOT block S2a
dispatch / DT-query work; DOES block treating the `interrupt-map`/`-mask` VALUE as final and hands the
shape + the §5-Q8 input numbers to S2b.

---

## OF-CI / DT unit-test SPEC (dispatch-resolution + DT-coverage; no boot, no real backends)

> Task-0 produces the SPEC + I/O contract + pinned oracle dependency. The S2a-impl milestone builds it at
> `SheepShaver/src/machine/test_openfirmware_ci.cpp` (Makefile `TESTS` target pre-wired by the coordinator
> at impl-kickoff, matching the verified `test_dev_openpic` pattern — `machine/Makefile:6`).

**Harness shape.** A standalone, no-boot, no-`kpx_cpu`-link test, identical in posture to the existing
`test_dev_cuda` / `test_adb_stub` / `test_dev_openpic` (verified standalone in `machine/Makefile:6`). Drives
`of_ci_callback()` directly.

**Battery (P-M1 BINDING — anti-vacuity).** The call battery is a **committed fixture derived from the
Q-S2a.1 inventory above** (the 21 verbs / 14 method names / 3 literals + their representative trace args),
**NEVER iterated from the dispatch table's registered handlers** — else `unresolved=0` is a test-double
tautology. Battery provenance is asserted in the SPEC. Battery composition:
- (a) each of the **21 direct services** (`getprop`, `getproplen`, `finddevice`, `close`, `setprop`, `open`,
  `seek`, `parent`, `exit`, `read`, `package-to-path`, `instance-to-package`, `write`, `instance-to-path`,
  `peer`, `test`, `canon`, `quiesce`, `nextprop`, `child`, `claim`) with a representative arg set;
- (b) each of the **14 `call-method` names** (`read-blocks`, `dimensions`, `set-hybernot-flag`, `claim`,
  `get-key-map`, `set-colors`, `fill-rectangle`, `draw-rectangle`, `size`, `instantiate-rtas`, `block-size`,
  `write-blocks`, `translate`, `map`) against an opened ihandle test-double;
- (c) the **3 `interpret` literals** (`key?`, `key`, `reset-all`);
- (d) the **adversarial DT-coverage case** — the full runtime `nextprop` tree-walk of `/mac-io` + children
  (the runtime ×397 breadth, NOT the static ×1) + the PCI-config-probe getprop family (the P-M3
  over-provision). Stop-rule #6: dropping the static-vs-runtime divergence (treating `nextprop`×1 as the
  surface) is forbidden — the runtime breadth is the DT-coverage obligation.

**`call-method` backends are INJECTED test doubles** (the `dev_cuda`/`adb_stub` decoupling). The test
supplies callbacks for `read-blocks`/`write-blocks`/`block-size` (disk → S2b), `/mmu` `claim`/`translate`/
`map` (→ S1/S2b), and the display methods (→ M5/fb); asserts the backend was *invoked with the decoded
args*. It does NOT exercise real disk/mmu/fb. S2a owns the **seam + doubles ONLY** — real backends are
S1/S2b (Stop-rule #5: claiming backend completeness is a false-clean).

**Predicates.**
- **G2a.b** — `unresolved_calls == 0` across all 21 + 14 + 3. A missing handler FAILS by construction.
- **G2a.c (rev-2 REFRAMED, A1+P-M2)** — `dt_answered(every finddevice/getprop/nextprop in the
  OpenBIOS-observed query set) == true`, including the runtime `nextprop` full-tree-walk breadth. This
  proves **DT-answers-the-observed-set, NOT "real-Core99 coverage proven"**: the +8/+2 dynamic queries are
  data-dependent on the values our DT returns and were sampled only against QEMU OpenBIOS (≠ Apple OF), so
  **real-Core99 query closure is a NAMED hand-off to the first integration boot** — NOT an S2a claim.
- **P-M2 carve-out** — the `interrupt-map`/`-mask` rows are EXCLUDED from value-coverage: assert only that
  the query resolves and `getproplen` returns a length **marked NON-FINAL**; NEVER assert the tuple cell
  content as a PASS criterion.
- each injected `call-method` double observed its decoded args.

**Oracle dependency.** The Q3 `[QEMU-BEHAVIORAL]` Trampoline trace = the expected-call-sequence ground
truth. **QEMU OpenBIOS is the DISPATCH-IDIOM oracle ONLY** (the CHRP `{name,n_args,n_rets,args…}` array
decode at `[r2-0xc]`), NEVER an address oracle (Stop-rule #6 / Donor-3 caveat): no QEMU MMIO address is a
reference value for our DT (our MacIO is fixed `0xF3000000`; QEMU's is a firmware-assigned PCI BAR).

**What it retires vs. does not.** Retires "the OF-CI surface is unresolvable / the DT can't answer the
producer" as a *dispatch-and-coverage* property. Does NOT prove the real Trampoline boots (needs S2b
loader + S1 `/mmu` + a real boot) and does NOT validate the interrupt-routing VALUE end-to-end (the
NanoKernel→CGRP path, owed to the program-level boot after S1+S2b+S3).

**File location / pre-wire.** `SheepShaver/src/machine/test_openfirmware_ci.cpp`; add to `machine/Makefile`
`TESTS` (line 6) at impl-kickoff. **G2a.e** uses the §6 structural-inertness substitute (genuinely-new
files; no shared runtime-gated path edited — confirmed all four target files absent this run) + `make
test-jit` score=100 (353/353 vectors); S2a does NOT require the `make e2e` soak.

---

## VERDICT — **RESIDUE-PASS**

| Question | Closure | Blocks S2a-impl? |
|---|---|---|
| Q-S2a.1 (DT read-set + order, path divergence) | CLOSED — full static map; canonical path + `/aliases`/`canon` pinned; named-dynamic over-provision residue (P-M3) | **No** |
| Q-S2a.2 (CHRP entry contract) | CLOSED — r5 confirmed; r3/r4 residue → S2b loader (r5-only injection) | **No** |
| Q-S2a.3 (interrupt-map/-mask tuple shape + §5-Q8 inputs) | FLAGGED-RESIDUE-TO-S2b (the expected "one real discovery"); DT answers the query with a NON-FINAL provisional | **No** (blocks treating the VALUE as final only) |
| OF-CI/DT unit-test SPEC | SPEC'd to I/O-contract + pinned-fixture level (P-M1 battery provenance, P-M2 carve-out, A1 reframe) | — |
| Gate-item-0 provenance | Both md5s re-verified ✓ | — |

This is **RESIDUE-PASS, not GREEN-PASS** (Q-S2a.3 closed with a conservative residue) — and per the plan's
PASS-taxonomy that is the **expected** case: the Q-S2a.3 tuple-shape residue is "the one real discovery,"
is non-blocking for S2a dispatch/DT-query, and is a named hand-off to S2b + the first integration boot.
**No residue lands on Q-S2a.1 or Q-S2a.2** (the two that would *block*), so S2a-impl dispatch + DT-query
work is cleared to start; the `interrupt-map`/`-mask` VALUE and r3/r4 remain explicit S2b owings.

**Residues handed forward:**
1. **Q-S2a.3 → S2b:** `interrupt-map`/`-mask` tuple cell-shape (cells-per-row, `#interrupt-cells`,
   parent-phandle encoding, child-unit-address mask) + the §5-Q8 PIC input numbers (`0x24`/`0x25`/`0x19`,
   QEMU-only) — confirmatory-boot owner = first integration boot.
2. **Q-S2a.2 → S2b loader:** r3/r4 at CHRP entry (S2a injects r5 only).
3. **Q-S2a.1 (P-M3) → first integration boot:** exact names of the 8 dynamic getprop keys + 2 dynamic
   finddevice paths (DT over-provisions the full CORE99 + PCI-config-probe set in the meantime).
4. **DT scaffolding-node build obligation (non-residue, S2a-impl task):** `/chosen`, `/aliases`,
   `/options`, `/rtas`, `/cpus` container, `/rom/macos` — OF-standard nodes the device skeleton (§3)
   doesn't tabulate; answerable without a boot.

**Scope-guard:** docs-only — no `SheepShaver/src/**` written, no builds, no boots, no NK disasm.
Falsifications this Task-0: 0 (re-pins: 0).
