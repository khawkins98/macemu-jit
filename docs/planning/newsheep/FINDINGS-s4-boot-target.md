# NewSheep — S4 recon: the 9.2.x boot target (what a bootable disk requires)

**Status:** COMPLETE (2026-06-15) · READ-ONLY recon (parallel fan-out). Characterized from both 9.2.x ISOs;
structures identical across 9.2.1 and 9.2.2. No src/**, no commits.

## System Folder layout (blessed folder), root files
| File | type/creator | Notes |
|---|---|---|
| `System` | `zsys/MACS` | The System suitcase. 7.19 MB data (≈100 `Joy!peff` PPC fragments) + 6.98 MB rsrc. Carries `krnl 0` (replacement NanoKernel — the CGRP producer, see FINDINGS-s4-im-init-cgrp), `boot 1/2/3` (boot loader), `nlib`×32 native libs, `gbly` (gibbly/enabler resource). |
| `System Resources` | `zsyr/MACS` | Companion (data fork empty; rsrc-only). |
| `Mac OS ROM` | `tbxi/chrp` | The NewWorld boot ROM file (2.55 MB on 9.2.1, 2.78 MB on 9.2.2; the project's active "9.0.1" ROM is 2.76 MB ≈ 9.2.2-era — consistent with ASSETS R2). OF loads this; contains Trampoline + builtin NanoKernel parcels. |
| `Finder` | `FNDR/MACS` | |
| `MacTCP DNR` | `cdev/mtcp` | |

**No separate "System Enabler" file.** 9.2.1/9.2.2 do NOT ship a standalone enabler in the System Folder
root; machine support is folded into the System file (`gbly` resource id −16385 = the "gibbly", plus
`gpch`/`ptch` patch resources) and the `Mac OS ROM` file. **S4 should not expect/require a separate Enabler.**

**`vers` confirms exact OS:** System `vers 1` = `"9.2.1/9.2.1, Copyright Apple Computer, Inc. 1983-2001"`
(resp. `"9.2.2/9.2.2"`). Genuine retail 9.2.x system software — the R2 asset gap is truly closed.

**Boot blocks (HFS, the `boot 1` resource = first 1024 bytes of the volume):** standard signature `0x4c4b`
('LK'), entry `0x60000086`, system name "System", shell "Finder", debugger "MacsBug", screen
"StartUpScreen". The secondary boot loader is the embedded `boot 2` (1946 B) / `boot 3` (55048 B, 68k)
code. Standard classic-HFS boot chain — nothing 9.2-specific in the boot blocks themselves.

**Disk-driver / partition expectations:** the CD images are Apple-partitioned (APM, 512-byte units) with
`Apple_Driver43` / `Apple_Driver43_CD` / `Apple_Driver_ATAPI` driver partitions, an `Apple_Patches` ("Patch
Partition", 512 blocks), and the `Apple_HFS` data partition. For SheepShaver these driver partitions are
irrelevant — SS supplies its own block device; what matters is a valid APM + a blessed System Folder + HFS
boot blocks on the target volume. (Note: the volumes are HFS **standard**, not HFS+, which is why modern
macOS `hdiutil` cannot mount them — relevant for host-side asset prep; use `machfs` or a classic-HFS tool.)

## Boot-chain dependency summary (what must be present on the S4 boot disk)
1. APM with a blessed `Apple_HFS` volume; valid HFS boot blocks (`boot 1`).
2. System Folder containing: `System` (with `krnl 0`, `boot 2/3`, `gbly`, `nlib`s, the ≈100 PPC fragments),
   `Mac OS ROM` (`tbxi/chrp`), `Finder`.
3. The `Mac OS ROM` file (Trampoline + builtin NanoKernel) — but note S4 runs *our* NewWorld ROM
   (`MachineProfileIsNewWorld()`); whether the disk `Mac OS ROM` file or our ROM provides the builtin NK is
   an S3/S4 reconciliation question, NOT a boot-block question.
4. The replacement NanoKernel (`krnl 0`) — the CGRP producer — is inside the `System` file; loading/entering
   it is the S4 integration target.

## Falsifiable boot-target claims
- **BT1.** Copying the `System Folder` (System + Mac OS ROM + Finder) from the 9.2.2 ISO onto an APM/HFS
  volume with valid `boot 1` boot blocks yields a structurally bootable 9.2.2 disk — no separate Enabler
  needed. **Refuted if** boot requires an additional file absent from the System Folder root.
- **BT2.** The `System` file's `krnl 0` and the `Mac OS ROM` file's builtin NK are distinct NanoKernels
  (disk "replacement" vs ROM "builtin" — both log "Created motherboard coherence group"); the disk one wins
  post-handoff. **Refuted if** only one NK exists at runtime.
- **BT3.** SS_M18 can ignore the `Apple_Driver*` partitions (SS provides the block backend); only APM + HFS
  boot blocks + blessed System Folder are load-bearing. **Refuted if** the OF/Trampoline path requires
  reading a driver partition.

## Recommended S4 asset action (unchanged from ASSETS R2, now structure-confirmed)
Prime target = 9.2.2 (`macos-922-uni.iso`); copy/convert its `System Folder` to an HFS volume image SS can
mount. Because the volumes are HFS-standard, asset-prep tooling must use `machfs` (demonstrated working)
rather than `hdiutil`.

**★ Strategic note (BT2 — the two-NanoKernel question):** this is a genuine S3/S4 reconciliation item. Our
current bringup runs the ROM **builtin** NK (NanoKernel-v02.27, the one now executing). The real 9.2.x boot
hands off from that builtin NK to the disk **replacement** NK (`krnl 0`), which is the actual CGRP producer.
So the eventual 9.2 boot involves BOTH — the builtin NK we're bringing up is the *first* stage; the disk
replacement NK is the *second*. S4's integration target is the handoff to + execution of the disk `krnl 0`.
