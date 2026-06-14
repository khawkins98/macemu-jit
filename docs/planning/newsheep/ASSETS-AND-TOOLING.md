# Operation NewSheep — Assets & Tooling

> The real dependencies the effort needs, and their current state. Update as items are sourced/installed.

## ROMs we hold (the "Mac OS ROM" disk-image files — type `tbxi`)

| File | Size | Notes |
|---|---|---|
| `/Users/Shared/macemu/1998-07-21 - Mac OS ROM 1.1.rom` | 1.8 MB | NewWorld v1.1 (1998). Pre-parcels era. |
| `/Users/Shared/macemu/2001-12-19 - Mac OS ROM 9.0.1.rom` | 2.6 MB | Active diagnostic ROM; all M-series RE is against this. |
| `/Users/Shared/macemu/MacOS-ROM-9.0.4-G4-extracted.rom` | 2.3 MB | 9.0.4 extracted. |

Decompressed dumps + manifest: `/Users/Shared/macemu/dumps/` (`rom901.bin` PATCHED, `rom901_inventory.bin` RAW).

**Key fact:** these are System-Folder *files*, not hardware ROM extracts. NewWorld's hardware boot
ROM (OpenFirmware) is synthesized by SheepShaver and is NOT needed as a file. The Trampoline ELF is a
**parcel inside these files** (parcels-based for 9.1+; the 9.0.x layout differs — Task-0 confirms).

## ⭐ NewWorld "Mac OS ROM" file collection (found 2026-06-14)

`~/Downloads/New_World_Mac_Roms/New World ROM/` holds the **full ROM-file progression 1998→2003**.
**Load-bearing fact: the filename version is the "Mac OS ROM" *file* version, NOT the Mac OS system
version** (separate version tracks). Dates place the late ones in the 9.2 era:

| ROM file | Date | md5 | Likely OS era (verify via `tbxi`) |
|---|---|---|---|
| Mac OS ROM 8.4 | 2001-07-30 | `f97d4382…` | Mac OS **9.2 / 9.2.1** |
| **Mac OS ROM 9.0.1** | 2001-12-19 | `66210b4f…` | Mac OS **9.2.2** — **== our active project ROM** (byte-identical) |
| Mac OS ROM 9.1.1 | 2002-04-08 | `c5f7aaaf…` | post-9.2.2 |
| Mac OS ROM 9.6.1 | 2002-09-03 | `3c08de22…` | post-Apple / community-rebuilt (cf. elliotnunn/newworld-rom builds "9.6.1") |
| (1.1 … 7.5.1) | 1998–2001 | — | 8.1 → 9.1; useful for the Q0-D parcels-layout diff |

**Reframe to verify (`tbxi` internal version strings, Task-0):** the project's "9.0.1 ROM" is named
by its *file* version and dated **Dec 2001 = Mac OS 9.2.2 era** — so it is almost certainly the
**9.2.2-era ROM**, not the original Mac OS 9.0.1 ROM (~Sept 1999, file version ~3.0). If confirmed,
**we have been running a 9.2-era ROM all along**, and the entire M-series ran against it.

**Impact on the version strategy (Q0-D / brainstorm):** "source the 9.2 ROM first" is **effectively
solved** — we hold multiple 9.2-era ROM files AND the cross-version set to answer the parcels-layout
transfer question by direct diff. Both Task-0 instruments can run on 9.2 (static RE on the in-hand
9.2-era ROM; QEMU already boots 9.2.1). **Action:** copy this set into the project asset area + a
manifest; `tbxi`-verify each ROM's internal OS version; pick the canonical 9.2.x ROM for the RE.

## ⚠️ R2 — the remaining 9.2 gap: SYSTEM SOFTWARE (not the ROM, and not needed for Task-0)

- The ROM file ≠ the OS. To eventually *boot* 9.2 in SheepShaver we still need genuine **Mac OS 9.2.x
  system software** (install media / a bootable disk). `/Users/Shared/macemu/macos921.dsk` is
  **mislabeled — it actually contains Mac OS 8.6** (verified earlier; see `[[machine-layer-pivot]]`).
- **Not on Task-0's critical path:** Task-0 is static `tbxi` RE of the ROM (in hand) + QEMU trace
  (QEMU has its own 9.2.1 CD). The 9.2 system software is needed only for the later *boot* milestones.

## Tooling

| Tool | Repo | Role for us | Installed? |
|---|---|---|---|
| `tbxi` | github.com/elliotnunn/tbxi (PyPI: `tbxi`) | `tbxi dump <rom>` → tree exposing OF bootinfo, **Parcels (incl. Trampoline ELF)**, 4 MB PPC ROM, 3 MB 68k ROM; `tbxi build` repacks. Operates on the Mac OS ROM file we hold. **Primary effort tool.** | ☐ `pip install tbxi` |
| `tbxi-patches` | github.com/elliotnunn/tbxi-patches | Library of scripts that patch the Mac OS ROM — the Route-B enabler (patch the Trampoline/nanokernel parcels offline). | ☐ |
| `newworld-rom` | github.com/elliotnunn/newworld-rom | Builds a Mac OS ROM *from* a 4 MB hardware Power Mac ROM + PEFs. **NOT a no-extract path** — reference for structure / repack only, not for obtaining a ROM. | n/a (reference) |
| capstone (PPC BE) | (already used) | Disassemble the extracted Trampoline ELF / nanokernel. | ✅ in use |
| QEMU mac99 rig | `SheepShaver/tools/qemu-rig.sh` + `qemu-mon.py` | Behavioral oracle: QEMU boots real 9.2 via the real OF+Trampoline+nanokernel. Format/semantics only — NEVER an address oracle (MacIO 0x80000000 ≠ ours; OpenBIOS ≠ Apple OF pre-NK). | ✅ in repo |

## Code touchpoints (where NewSheep work lands)
- `SS_NW_TRAMPOLINE` synthesis + `SS_NW_*` gates — `SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp`,
  `rom_patches.cpp`, `src/machine/`.
- Machine description / device tree: `docs/planning/machine/CORE99-MACHINE-DESCRIPTION.md`.
- All new code behind an env gate + `MachineProfileIsNewWorld()`; paravirtual byte-identical;
  `make test-jit` = 100.
