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

## ⚠️ R2 — the 9.2 asset GAP (blocking for the actual 9.2 boot)

- We do **not** currently have genuine Mac OS 9.2.x media. `/Users/Shared/macemu/macos921.dsk` is
  **mislabeled — it actually contains Mac OS 8.6** (verified earlier in the project; see the
  `[[machine-layer-pivot]]`/asset memory + CLAUDE.md asset table caveat).
- **Need:** a genuine Mac OS 9.2.1 (or 9.2.2) install ISO/CD. From it we extract the 9.2 "Mac OS ROM"
  file (System Folder) and `tbxi dump` it — **no hardware ROM extract required at any point**.
- The Trampoline RE can **start now on 9.0.x** (Task-0); the 9.2-specific boot and any 9.2-only
  Trampoline behavior are blocked on this ISO. Track acquisition explicitly.

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
