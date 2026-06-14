# Operation NewSheep — Glossary & Orientation

> One read to onboard a fresh agent. Terms, then the M8→M17 lineage that produced this effort.

## Terms

- **NewWorld architecture** — Macs from 1998 (iMac) on. Replaces the old hardware Toolbox ROM with
  (a) a small **OpenFirmware boot ROM in hardware** + (b) a **"Mac OS ROM" file** loaded into
  write-protected RAM from the System Folder ("ROM-in-RAM").
- **OpenFirmware (OF)** — IEEE-1275 boot environment in the hardware boot ROM. Initializes hardware,
  builds the **device tree**, runs CHRP boot scripts, then loads/launches the Mac OS ROM. SheepShaver
  **synthesizes** OF (does not run Apple's); QEMU mac99 uses **OpenBIOS** (an OF reimplementation —
  ≠ Apple OF pre-nanokernel, a load-bearing caveat for the oracle).
- **"Mac OS ROM" file** (type code `tbxi`) — the System-Folder file containing the Toolbox + the
  nanokernel + (9.1+) the parcels and the Trampoline. This is what our `.rom` files are.
- **Parcels / tbxi container** — the packaging of the Mac OS ROM file from ~9.1 on. `tbxi dump`
  exposes: OF bootinfo/Bootscript, the **Parcels** (incl. the Trampoline ELF), the 4 MB PPC ROM
  section, the 3 MB 68k ROM section.
- **Trampoline** — an **ELF bootloader, a parcel inside the Mac OS ROM file**. On real hardware OF
  loads and runs it; it **copies/modifies the OF device tree AND sets up the interrupts for the
  nanokernel**, then hands off. *This is the producer Operation NewSheep targets.* We do not run it;
  `SS_NW_TRAMPOLINE` fakes a partial substitute.
- **Nanokernel (NK)** — the low-level PowerPC kernel under the Toolbox; owns exceptions/interrupts,
  the KDP, the vector tables. Staged into guest memory at `0x5031xxxx`. **It is a separate ROM parcel
  (`NanoKernel-vNN`) from the Trampoline (`MacOS.elf`); per the Task-0 red-team it is the NanoKernel —
  not the Trampoline — that builds CGRP, from the device tree the Trampoline produces** (so the
  "producer" is likely Trampoline + NanoKernel; fork Q0-F).
- **IM init (Interrupt Manager init)** — the guest code (driven off the Trampoline + Cuda init) that
  populates the NK interrupt-routing structures. **Runs downstream of Cuda init and never completes
  in our emulation** — the chicken-and-egg at the heart of M14→M17.
- **CGRP** — the "interrupt-group" descriptor the NK EXT dispatcher reads (`*(KDP-0x338)=0x68ffc1c0`,
  tag `"CGRP"`). Its handler table is **empty** because IM init never built it. The most-probed
  symptom of the disease.
- **KDP** — nanokernel data page (`0x68ffe000`); base for many NK structures (`[KDP+0x360]` vector
  table, `[KDP+0x374]` EXT entry `0x50314880`, etc.).
- **Forge (forge-class)** — host-side writing of guest NK structures the host doesn't own.
  M10/M16/M17's approach. Banked as NO-GO: blind or inert, re-opens the M10 `0xDEADBEEF` crash class.
- **The producer-vs-product reframe** — forging *products* (CGRP table, handler PCs) is O(N walls),
  each guessed/blind. Running/reproducing the *producer* (Trampoline) is O(1) and yields real values.

## The M8→M17 lineage (why we're here) — CANONICAL

> **This table is the SINGLE SOURCE OF TRUTH for the M8→M17 lineage.** HANDOFF, AGENT-CONTEXT, the
> charter, and ROADMAP keep only a one-line frontier statement + a pointer here — do not restate the
> per-milestone detail elsewhere (that drift caused the M14-wall contradiction). Update here first.

| Milestone | Result | What it added to the picture |
|---|---|---|
| M8 | consumption rail green env-on; default flip refused (`SC#1=0x0d`) | the consume path exists but stalls |
| M13 | retraction | NewWorld 68k interrupt **delivery WORKS**; the wall is downstream (the "`0x5000ED08` never runs" keystone was a probe-granularity artifact) |
| M14 | parked | DEC doesn't signal the DR; `KDP+0x674`/hnfo zero **because IM init runs downstream of Cuda init**; all fixes "collapse to forge". **VERDICT:** the `[ALARM]` stall is a **Cuda device-model IFR/IER bug** (`sr_int_pending` never reaches VIA IFR; NK polls IER) — NOT a "model-rejection gate". This is the expected **post-NewSheep next wall**. |
| M15 | FORGE verdict (verified) | structs frozen-zero across obs=1e9; **CGRP is the first of N** |
| M16 | DoD-3 NO-GO | one-word forge safe-but-inert (empty table self-guards); handler PC `0x5000ec50` **ROM-absent** → built at runtime |
| M17 | red-team BLOCKED | the "sanctioned cross" needs MODE_EMUL_OP but the EXT regime is **MODE_68K**; **series tripwire fired on wall 1** → per-wall forging is bankrupt |

(Also COMPLETE, not interrupt-arc: **M10/M11/M11a** — 16 MB aperture @`0x81000000` + OF "cofb"
display node; r24 never NK-clobbered. **M12 PARTIAL** — NW lowmem 1MB→32MB + anon-zero high EAs.)

**Throughline:** the milestones are one disease — *the Trampoline never runs, so the structures it
would build stay empty.* Operation NewSheep targets the producer.

## Pointers
- Charter: `README.md` · Log: `RESEARCH-LOG.md` · Assets/tools: `ASSETS-AND-TOOLING.md`
- Banked RE: `docs/planning/M14-FINDINGS-cuda-delivery.md`, `…/M15-FINDINGS-consumption-recon.md`,
  `…/M16-FINDINGS-oracle-forge.md`; strategy: `docs/planning/MACHINE-LAYER-PLAN.md`
- Externals: elliotnunn/{tbxi, tbxi-patches, newworld-rom}; 68kMLA "Picking apart the NewWorld ROM";
  E-Maculation "Using QEMU to explore the Mac OS Nanokernel"; Apple Wiki "New World ROM".
