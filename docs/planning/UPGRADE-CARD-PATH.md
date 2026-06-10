# The Upgrade Card Path — Mac OS 9.2 on the 1.1 ROM

> **⚠️ CLOSED — dead end (2026-06-10).** Superseded by [`MACHINE-LAYER-PLAN.md`](MACHINE-LAYER-PLAN.md).
> The 1.1 ROM is structurally incompatible with Mac OS 9.2.1: (1) the CFM boot fragment
> audit fails (missing DebugLib/InterfaceLib/etc. — Spike S1), and (2) the Memory Manager
> heap mismatch corrupts the A-line vector at guest `$28` (confirmed in both JIT and
> interpreter). Gate-bypass findings and the DSAT/boot-resource methodology remain useful
> reference for other System file versions. The SCC device model built for M1 was informed
> by the serial polling stall discovered here.
>
> **Status:** 🔴 Closed — structural dead end ·
> **Created:** 2026-06-09 · **Updated:** 2026-06-10
>
> **Key finding:** The 1.1 ROM is actually **NewWorld (type 5)**, not OldWorld. The "upgrade
> card" metaphor (keep OldWorld ROM, shim CPU identity) doesn't describe the real situation.
> We have a 1998 NewWorld ROM trying to boot a 2001 System — the gap is ROM vintage, not
> ROM type. See §2.1 for full experiment results.
>
> **Supersedes:** Path A (NewWorld ROM port), which is **parked**.

---

## 0. The metaphor — and where it breaks down

In the late 1990s, companies like Sonnet, NewerTech, and XLR8 sold **processor upgrade cards**
for Power Macintosh hardware. The original idea was that our 1.1 ROM is the "motherboard" —
keep it, shim the CPU identity, and Mac OS 9.2 should accept the hardware.

### Why the metaphor is wrong

**The 1.1 ROM is already NewWorld.** `rom_detect_type()` identifies it as ROMTYPE_NEWWORLD
(type 5) — its nanokernel ID string is `"NewWorld"`, not `"Boot Gossamer"`. The file is a
NewWorld CHRP/parcels ROM from 1998, version 1.1, designed for the first-generation NewWorld
machines (iMac G3). All prior references to this as "OldWorld" were incorrect.

Real G3→G4 upgrade cards **didn't bypass ROM version checks**. They upgraded the CPU on
machines that already had sufficient ROMs. OldWorld machines (beige G3, 9500) topped out
at Mac OS 9.1 regardless of CPU upgrade — the ROM was the limiting factor, not the CPU.
NewWorld machines (B&W G3, iMac G3) could run Mac OS 9.2.x natively because their ROMs
were already NewWorld.

**Our real situation:** a 1998 NewWorld ROM (v1.1) trying to boot a 2001 System (9.2.1).
The gap is ROM vintage/version, not ROM type or CPU identity. The System file expects
ROM features/version that the v1.1 ROM doesn't provide.

### What remains useful from this framing

The architecture-first principle is still right — define what 9.2 needs, build exactly that.
But the "enabler" won't be simple identity patches. It's either:
- **Deeper ROM RE** to find and patch the specific version/feature check in the System file
- **A newer ROM** (e.g., the 9.0.1 ROM) that satisfies 9.2.1's requirements natively
- **Path A revisited** with the obstacle map as a guide

### Why this is still better than Path A bug-chasing

Even with the metaphor broken, the experiment yielded clear data about what doesn't work
and a concrete characterization of the barrier (§2.1). Path A was chipping away without
a unifying design; this experiment at least established the shape of the problem.

---

## 1. What we know now (post-Experiment 1)

**The 1.1 ROM boots Mac OS 9.0.4 perfectly.** This is the baseline.

**The 1.1 ROM is NewWorld (ROMTYPE_NEWWORLD, type 5).** `rom_detect_type()` matches
`"NewWorld"` in the nanokernel ID string. NOT OldWorld/GOSSAMER as previously assumed.

**Mac OS 9.2.1 has THREE compatibility gates, all rejecting this ROM:**
1. **Installer gate:** "This program cannot run on your computer" — CFM/launch-time check,
   refuses to run even from Mac OS 9.0.4.
2. **Model check gate:** `_SysError($63)` at boot id=3 offset 0x0404 — 68k subroutine probes
   ROM environment, rejects the 1.1 ROM. **Bypassed** (2026-06-10).
3. **Original media gate:** `_SysError($76)` at boot id=3 offset 0x70A4 — detects System
   copied to a different volume. **Bypassed** (2026-06-10).

Full gate anatomy in `docs/planning/sheepshaver-research/SYSTEM-BOOT-GATES.md`.

**Identity patches tried and FAILED:**
- `SS_NW_MODEL=1` device-tree model/compatible → no effect
- UniversalInfo gestaltMachineType → 406 (0x196) → no effect (all NewWorld is already 406)

**Real-world parallel: OS 9 Helper.** The community tool that enabled 9.2 on unsupported
Macs patched **3 resources in the installer** (gate 1). But we bypassed gate 1 by manually
copying the System Folder. We're stuck at gate 2 — the System file's own boot-time check.

**Real upgrade cards never solved this.** They only enabled CPU features (AltiVec), not OS
version compatibility. The ROM was always the limiting factor.

---

## 2. Experiment 1: Path B probe — results (2026-06-09)

> **Status:** 🔴 Blocked at the System file's boot-time compatibility check. Identity-field
> patches are insufficient. The check probes for ROM environment characteristics.

### 2.1 What we tried

| # | Patch | Result |
|---|---|---|
| 1 | `SS_NW_MODEL=1` → device-tree model/compatible | No effect — same dialog |
| 2 | UniversalInfo[0x60] → 0x196 (gestaltMachineType=406) | No effect — all NewWorld is already 406 |
| 3 | `SS_FORCE_ALTIVEC=1` → gestalt 'ppcf' AltiVec bit | No effect (was already set) |

### 2.2 Setup used

- ROM: `/Users/Shared/macemu/1998-07-21 - Mac OS ROM 1.1.rom` (NewWorld, version 1.1)
- Disk: `/Users/Shared/macemu/pathB_upgrade.dsk` (9.0.4 installed, 9.2.1 System Folder swapped in)
- Prefs: `/tmp/exp_921_on_11.prefs` (HD boot, no CD)
- Env: `SS_NW_MODEL=1 SS_FORCE_ALTIVEC=1`

### 2.3 Observations

- Boot reaches 10,169 compiled PPC blocks, then stalls in DR Emulator 68k dispatch
- Hot PCs: 0x504a51c0 (58.7%), 0x504b37d0 (40.5%) — DR Emulator opcode dispatch tables
- Guest is actively running 68k code (modal dialog event loop), not truly hung
- The error dialog is rendered by standard Dialog Manager — Toolbox is fully initialized
- Error string found in System file on disk (NOT in ROM), at multiple disk offsets
- The `[ALARM]` boot-stall watchdog fires at 15s ("comp frozen"), but the 68k VM is active

### 2.4 Conclusion

The System file's compatibility check is NOT about machine identity (gestaltMachineType,
device-tree model). It probes for ROM environment characteristics — likely ROM file version,
ROM feature flags, or nanokernel version — that the 1998 v1.1 ROM doesn't provide.

**The "upgrade card enabler" approach of patching identity fields cannot work.** The delta
between what 9.2.1 expects and what the v1.1 ROM provides is structural, not cosmetic.

### 2.5 Resolution: System file gates found and bypassed (2026-06-10)

**Both System file compatibility gates have been identified and bypassed.** Full analysis
in `docs/planning/sheepshaver-research/SYSTEM-BOOT-GATES.md`.

The solution was **option 3 (OS 9 Helper approach)** — patch the System file's `boot` id=3
resource to NOP the two `_SysError` call sites:

| Gate | Error | boot id=3 offset | Check |
|------|-------|-------------------|-------|
| 2 | $63 | 0x0404 | Model/ROM version (subroutine probes ROM environment) |
| 3 | $76 | 0x70A4 | Original media (detects System copied to different volume) |

**4 bytes total** (2 × `A9C9` → `4E71`). DSAT resource parsing was the key — it maps error
codes to dialog text, identifying $76 as the "original media" code with exactly one call site.

**Result:** Mac OS 9.2 splash screen appears. Boot then stalls at SCC serial init (see §2.7).

### 2.6 ISO discriminator experiment

**Goal:** determine if the post-splash stall is disk-related (e.g., filesystem upgrade
mismatch) or ROM-structural.

**Setup:** 9.2.1 installer ISO patched with 4 gates NOP'd (Gate 2a at 0x03CA + Gate 2 at
0x0404 + Gate 3 at 0x70A4 + Gate 4 at 0x8944). CD boot required the extra Gate 2a patch —
the `btst #2,$0B20` check at 0x03CA fires on CD/ISO boot but not HD boot.

**Results:**
- Unpatched ISO fires Gate 2a ($66) — different from HD which fires Gate 2 ($63)
- Fully patched ISO reaches partial splash (black + "Welcome to Mac OS" progress bar)
- Stalls at identical HOT-PC (0x50484038) with identical rate (~2.6M blocks/s)
- **Conclusion: stall is ROM-vintage-structural, NOT disk-related**

### 2.7 Post-splash stall: A-line vector corruption (resolved)

The observable symptom was SCC serial polling at `0x500cc998`, but deeper investigation
(2026-06-10) identified the real root cause: **guest `$28` (A-line exception vector)
corruption**.

The Memory Manager's heap block-split routine writes through a corrupt free-list
backward-link (`0x1F`), clobbering the LSB of the longword at `$28` — changing the A-line
vector from `0x50015570` (valid Trap Dispatcher) to `0x50015500` (Name Registry data).
All subsequent A-line traps dispatch to non-code, triggering illegal-instruction exceptions
that land in the ROM serial debug monitor, which polls SCC forever.

**Confirmed NOT a JIT bug** — interpreter mode reproduces identical corruption. The 1.1 ROM's
heap/low-memory initialization is structurally incompatible with 9.2.1's Memory Manager.

Full analysis in `docs/planning/sheepshaver-research/SYSTEM-BOOT-GATES.md` §5.

### 2.8 Conclusion: 1.1 ROM cannot boot 9.2.1

The Upgrade Card path is **closed as a dead end**. Two independent structural barriers:
1. **CFM boot fragment audit** (Gate 2, Spike S1) — 9.2.1 requires named boot fragments
   that the 1.1 ROM's parcels don't provide. The 9.0.1 ROM provides them natively.
2. **Memory Manager heap incompatibility** (§2.7) — the 1.1 ROM's heap setup produces a
   corrupt free-list when 9.2.1's Memory Manager runs, clobbering the A-line vector.

Both are ROM-vintage problems, not emulation bugs. Identity patches and gate bypasses
cannot address them. The successor approach is the **Machine Layer** architecture
(`MACHINE-LAYER-PLAN.md`) — dual machine profiles with proper device models.

---

## 3. Experiment 2: QEMU oracle — reference behavior for supervisor state

> **Goal:** Boot the same 9.0.1 NewWorld ROM under QEMU, capture supervisor state at key
> checkpoints, and use it as a reference oracle for our emulator's behavior.

### Why this helps even on the Upgrade Card path

Even if we stay on the 1.1 ROM, understanding what the NewWorld nanokernel *actually does*
with SDR1/HTAB/SPRG/KDP is valuable:
- It tells us which of our supervisor fakes are wrong vs just different.
- It validates whether the MMU rung-ladder (HANDOFF §3) assumptions are correct.
- If we ever resume Path A, the oracle data is directly reusable.

### Setup

- Install QEMU: `brew install qemu` (qemu-system-ppc)
- ROM: The same 9.0.1 ROM (`/Users/Shared/macemu/2001-12-19 - Mac OS ROM 9.0.1.rom`)
- QEMU config: OpenBIOS + the ROM file + `-d` trace flags for MMU/exception state
- Capture: SDR1, SPRG0–3, KDP contents, HTAB entries at boot checkpoints

### Key questions for QEMU

1. Does the nanokernel build only identity (V=P) PTEs, or any non-identity mappings?
2. What's in SPRG0–3 at idle-loop entry?
3. What does the KDP look like when the scheduler dispatches a task?
4. Does `sc` actually vector to a syscall handler, and what does it return?

### Priority

**Lower than Experiment 1.** The QEMU oracle is "nice to have" — it answers theoretical
questions about the NewWorld nanokernel. Experiment 1 (Path B probe) answers the practical
question: can we run 9.2 on the 1.1 ROM? Do Experiment 1 first.

---

## 4. The enabler architecture — what an "upgrade card" looks like in code

> **This section is speculative until Experiment 1 gives us data.** The architecture below is
> the framework; the specific enablers depend on what 9.2 actually demands.

### Principle: minimal, env-gated, additive

Like the real upgrade card enablers, our code should be:
- **Minimal:** only fake what the OS actually checks.
- **Env-gated:** `SS_UPGRADE_CARD=1` or similar — default off, no regression risk.
- **Additive:** patches on top of the working 1.1 path, not changes to it.

### Likely enabler categories (speculative)

| Category | What it does | Precedent |
|---|---|---|
| **CPU identity** | PVR = G4, gestalt `'ppcf'` with AltiVec bit | `SS_FORCE_ALTIVEC=1` already exists |
| **Machine identity** | Gestalt `'mach'`, ROM version fields, Name Registry | `SS_NW_MODEL` probe (insufficient alone) |
| **System version** | Gestalt `'sys1'`/`'sys2'`, System file version | May need patching in the installed System |
| **Manager versions** | Memory Manager, File Manager version gestalts | Simple gestalt responses |
| **NewWorld-only APIs** | Any API that 9.2 calls that doesn't exist on 8.x | Need to identify from the 9.2 delta |

### What we keep from Path A

The general correctness fixes harvested during Path A are permanent:
- SPRG0–3 real registers (benefits all guests)
- fctiw dynamic FPSCR rounding (benefits all guests)
- SDR1 real register (benefits all guests)
- sc double-increment bug (identified, fix pending)

The NW-specific scaffolding (trampoline, SegMap spike, etc.) stays env-gated and parked.

---

## 5. Relationship to existing docs

| Doc | Relationship |
|---|---|
| `HANDOFF-NEWWORLD-SUPERVISOR-MMU.md` | Path A (parked). §2.8 obstacle map is the reference for why we pivoted. §0 mission statement (forcing function) still applies — the Upgrade Card path is a different forcing function targeting the same goal (JIT correctness hardening via OS 9.2). |
| `ROADMAP.md` D3 | Needs updating — D3 should reflect the Upgrade Card pivot. |
| `NEW-WORLD-ROM-SUPPORT-PLAN.md` | Historical context for Path A. Still valid as reference. |
| `PATCH-68K-SHIM-INVENTORY.md` | Path A artifact. Not needed for Upgrade Card path (1.1 ROM has working shims). |

---

## 6. Decision log

| Date | Decision | Rationale |
|---|---|---|
| 2026-06-09 | Park Path A, adopt Upgrade Card (Path B) as primary | Path A's obstacle map shows diminishing forcing-function ROI (shifting from general PPC bugs to ROM-specific byte-pattern work). The 1.1 ROM is proven infrastructure; the Upgrade Card approach is architecture-first rather than bug-chase-first. |
| 2026-06-09 | Two experiments before implementation | Experiment 1 (9.2 delta probe) decides feasibility. Experiment 2 (QEMU oracle) is optional supporting data. No implementation until Experiment 1 reports. |
| 2026-06-09 | Experiment 1 blocked — identity patches insufficient | gestaltMachineType=406 and device-tree model/compatible both failed. The 1.1 ROM is NewWorld (not OldWorld as assumed). The System file checks ROM characteristics, not identity fields. Real upgrade cards never solved OS version gates either. |
