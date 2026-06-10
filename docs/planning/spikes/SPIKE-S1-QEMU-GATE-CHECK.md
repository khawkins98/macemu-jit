# Spike S1 — Does Mac OS 9.2.x pass its boot gates on the 9.0.1 "Mac OS ROM"?

> **Status:** ✅ Answered (2026-06-10) · **Plan:** `MACHINE-LAYER-PLAN.md` §3 pre-M0 spikes ·
> **Decides:** milestone **M7's path** (native 9.0.1 ROM vs newer family ROM vs 4-byte bypass)
>
> **Verdict: the gates PASS natively on the 9.0.1 ROM.** Mac OS 9.2.1 with the 2001-12-19
> "Mac OS ROM 9.0.1" file swapped into its System Folder boots to the Finder desktop under
> QEMU `mac99` with **all `_SysError` gate sites unpatched** (A9C9 intact at boot3+0x0404
> `$63`, +0x03CA `$66`, +0x70A4 `$76`, +0x8944 `$78`). No model-rejection or original-media
> dialog. **M7 recommendation: native 9.0.1 ROM path; keep the 4-byte bypass as fallback;
> the newer-family-ROM rung is unnecessary.**

Both angles from the spike brief were run: A (QEMU empirical) and B (static RE of the
gate-2 probe at boot3+0x7E24). They agree.

---

## 1. Angle A — QEMU empirical test

### 1.1 Setup

- Host: macOS arm64, QEMU 11.0.1 (`/opt/homebrew/bin/qemu-system-ppc`), hfsutils, capstone 5.0.7.
- Machine: `qemu-system-ppc -M mac99 -m 512 -drive file=<img>,format=raw,media=cdrom -boot d
  -vnc 127.0.0.1:9 -monitor unix:/tmp/s1/mon.sock,server,nowait -display none`.
  Screenshots via QEMU monitor `screendump` (PPM → PNG with `sips`).
- Boot medium: **the Mac OS 9.2.1 installer CD image**
  (`/Users/Shared/macemu/Apple Mac OS 9.2.1/macos_921_ppc.iso` — note: this asset is a
  *directory* containing the iso). Its System file's `boot` id=3 is **unpatched** (verified
  A9C9 at all four gate sites) — unlike `pathB_upgrade.dsk`, whose live System is pre-patched
  (4E71 at +0x0404 and +0x70A4); see confounds (§3).
- ROM swap procedure (on copies in `/tmp/s1/`, originals untouched): `hmount <iso> 1` →
  `hdel ':System Folder:Mac OS ROM'` → `hcopy -m <MacBinary tbxi>` → `humount`. The
  replacement is a data-fork-only MacBinary (name "Mac OS ROM", type `tbxi`, creator `chrp`)
  built from the raw `.rom` file — a resource fork proved unnecessary.

### 1.2 Run matrix and raw observations

| Run | Config | Result | Screenshot |
|---|---|---|---|
| A (control-pass) | unmodified ISO, shipped "Mac OS ROM" (1120-byte rsrc fork, data 2,552,522 B, dated 1991-07-30 → the 9.2.1-era ROM, presumably v8.4) | **Boots to Finder desktop in ~45 s.** No gate dialog. Validates the whole QEMU/OpenBIOS/CD path. | `/tmp/s1/runA_t45.png` |
| B (**the test**) | ISO copy, ROM file replaced by `2001-12-19 - Mac OS ROM 9.0.1.rom` (2,763,530 B), gates **unpatched** | **Boots to Finder desktop in ~30 s.** No `$63` model dialog, no `$66` CD-boot dialog, no `$76` original-media dialog. | `/tmp/s1/runB_t30.png` |
| C (control-fail) | ISO copy, ROM file replaced by the 1998 v1.1 ROM | **Fails before the gates**: the 1.1 Trampoline aborts in Open Firmware — `MacOS: Boot Failure! (0xF3C481F6)` / `"/rtas" not found!`. OpenBIOS provides no RTAS node; the 1998 Trampoline requires one, the 2001 ones tolerate its absence. | `/tmp/s1/runC_t45.png` |
| D (HD-boot attempt) | `pathB_upgrade.dsk` copy, gate patches **reverted** to A9C9, 9.0.1 ROM swapped in, hand-wrapped in an Apple Partition Map | ROM loads and runs (OpenBIOS→Trampoline OK) but the Mac OS startup-disk search rejects the volume — blinking **?-floppy**. Most likely cause: no `Apple_Driver_ATA` partitions in the hand-built APM. Gate code never reached. | `/tmp/s1/runD_t40.png` |

Side findings while getting there:

- **Bare HFS images (SheepShaver-style) are not OpenBIOS-bootable** — `Trying hd:,\\:tbxi`
  fails outright with no partition map (`/tmp/s1/run0_t20.png`). An APM wrapper gets the
  tbxi loaded and the ROM running, but without driver partitions the OS-side startup scan
  still rejects the disk (Run D). Real Apple CD/HD images (DDM + `Apple_Driver*` + `Apple_HFS`)
  work as-is. Any future QEMU-oracle work should boot from real-format media, not our
  raw `.dsk` files.
- **`macos921.dsk` does not contain Mac OS 9.2.1.** Its System file's `vers` 1 is **8.6**
  (volume "Mac Test", no "Mac OS ROM" file in the System Folder). The asset name is
  misleading; it was useless for this spike.

### 1.3 What Run B does and does not establish

Establishes: with the *same* System file, gate code, and boot media, swapping the ROM file
from the shipped 9.2.1-era one to the 9.0.1 file changes nothing — every compatibility gate
the System runs at boot is satisfied by the environment the 9.0.1 ROM constructs. The probe
checks ROM-file-delivered state (Angle B confirms: CFM boot fragments and override
resources), and that state travels with the ROM file we'd use in SheepShaver.

Does NOT establish: that SheepShaver can *reach* the gates on the 9.0.1 ROM — that is
exactly the Machine Layer's M1–M6 work (nanokernel handoff, DR Emulator cold start, etc.).
S1 only removes M7's gate-compatibility unknown.

## 2. Angle B — static RE of the gate-2 probe (boot3+0x7E24)

Source: `boot` id=3 (55,048 B) extracted from `pathB_upgrade.dsk` at disk offset
0x18F39748 (SYSTEM-BOOT-GATES §4); the CD's System carries byte-identical gate code.
Disassembled with capstone (M68K_040, BE). The call site is `0x03FA: jsr (+$7a28,pc)`
→ target 0x7E24; nonzero return → `moveq #$63; _SysError`.

### 2.1 What the probe actually checks

It is a **boot-time CFM-environment audit**: verify that the fragments the rest of boot
depends on can be found/instantiated, at specific override-resource IDs. Structure:

1. **`Gestalt('mach')`** (sub at 0x889E, trap A1AD): if `gestaltMachineType` is **406**
   (0x196, the generic NewWorld ID — what SheepShaver reports) or **1206** (0x4B6), use a
   short 3-entry checklist (plus one extra init via `CodeFragmentMgr` /
   `CFragInitializationRDesc`, sub 0x7C7A); otherwise an 11-entry checklist.
2. Each checklist entry is a 0x48-byte record `{name[0x40]; OSType kind @+0x40;
   int16 resID @+0x44; uint16 flags @+0x46}`. Full (11-entry) list, in iteration order:

   | Name | kind | resID | flags | Check performed |
   |---|---|---|---|---|
   | DebugLib | `nlib` | 0xBFF1 (−16399) | $4000 | `GetResource('nlib',id)` → CFM **GetMemFragment** (trap $AA5A sel 3) → register (sel $FFFC) |
   | Math64Lib | `nlib` | 0xBFDC | $8001 | bit0 ⇒ **GetSharedLibrary**(name, arch `'pwpc'`, $AA5A sel $FFFE) |
   | BootStdCLib | `nlib` | 7 | $8001 | GetSharedLibrary |
   | InterfaceLib | `fovr` | 51 | $8000 | `GetResource('fovr',51)` → version/instantiation helper ($9F3E) |
   | MPSharedGlobals | `nlib` | 3 | $4000 | GetResource + GetMemFragment |
   | MPLibrary | `nlib` | 2 | $4000 | GetResource + GetMemFragment |
   | MixedMode | `sfvr` | 1 | $8000 | GetResource('sfvr',1) → $9F3E |
   | InterfaceLib | `fovr` | 50 | $8000 | GetResource → $9F3E |
   | CodeFragmentMgr | `sfvr` | 0 | $8000 | GetResource → $9F3E |
   | ProcessMgrSupport | `sfvr` | 8 | $8000 | GetResource → $9F3E |
   | PrivateInterfaceLib | `fovr` | 6 | $8000 | GetResource → $9F3E |

   Short list (machine type 406/1206): DebugLib (`nlib` 0xBFF1, **$4001** ⇒ GetSharedLibrary),
   InterfaceLib (`fovr` 50), PrivateInterfaceLib (`fovr` 6).
3. The kind dispatcher (0x7DC2/0xA5C4) recognizes exactly the NewWorld override-resource
   family: **`sfvr`→8, `fovr`→9, `nlib`→10, `ntrb`→11, `ncod`→12** — the same types used by
   the System-file↔ROM-parcels override mechanism (`ntrb` = device tree, `ncod` = native
   code). The "version" words in SYSTEM-BOOT-GATES' description are actually **resource IDs**.
4. On any entry failing, the probe consults low-mem `$BFF` / `$120` (debug-flag path,
   trap $ABFF) and returns the error → `_SysError($63)`. If all pass, a final check (0x7D16)
   GetSharedLibrary("MPLibrary") + FindSymbol("MPSecondaryInitializeAPI") (sel 5), requires
   a TVector (symClass 2), wraps it in a routine descriptor (trap $AA59) and calls it; its
   status is the probe's return value.

So the dialog is "System file damaged" ($63) because the probe's literal meaning is *"the
boot fragment set is broken"* — the model check is indirect: only a ROM of sufficient
vintage assembles the fragment environment 9.2.1's checklist demands.

### 2.2 Why 1.1 fails and 9.0.1 passes (cross-reference)

- The **9.2.1 System file itself contains every queried resource** (`fovr` {6,10,35,50,**51**,…},
  `sfvr` {0,1,8,−16401}, `nlib` incl. 2,3,7,0xBFF1,0xBFDC — verified from the CD's System).
  So a bare `GetResource` presence test cannot be what fails on the 1.1 ROM. The failing
  predicate must live in the post-GetResource instantiation/version helpers ($9F3E/$A2EC,
  *not fully decoded — see §3 honesty*) and/or in `GetSharedLibrary` against the boot-time
  CFM registry, both of which depend on what the **ROM parcels** registered.
- ROM-file contents (raw byte scan): the **9.0.1 ROM has a `prcl` parcel directory
  (@0x1BFC0) with `nlib` parcel entries and carries the fragment names** InterfaceLib,
  ProcessMgrSupport, CodeFragmentMgr, Math64Lib, MPLibrary in cleartext. The **1.1 ROM has
  none of those names and no `prcl` tag** (its 1998 parcel format predates them). The 8.6
  System (which 1.1 boots happily) wants `fovr` 50 but has no concept of `fovr` 51 —
  consistent with 9.2.1 demanding a 2001-vintage fragment set the 1998 ROM never provides.
- For comparison, the older 8.6 System file ships `fovr` {6,10,35,50} but **no id 51** —
  id 51 is the new-in-9.x demand.

This static picture predicts exactly what Run B observed.

### 2.3 Gate 3 ($76, original media) and gate 2a ($66, CD boot)

Under QEMU with the 9.0.1 ROM (CD boot), neither fired. Note the prior SheepShaver/1.1
data (UPGRADE-CARD §2.6) NOP'd all four sites at once on the ISO, so it was never isolated
whether $76 fires on CD boot under 1.1. What S1 shows is that in a CD-boot context with a
proper-vintage ROM, the $76 check's accept path is taken with no patching. The
HD-with-copied-System context was **not** reproduced under QEMU (Run D's disk was rejected
pre-gate), so "$76 never fires on an HD-copied System under 9.0.1" remains *probable but
unproven* — the 2-byte NOP at +0x70A4 stays in the toolbox if it resurfaces.

## 3. Confounds and honesty

1. **OpenBIOS ≠ Apple OF.** The control-fail run (1.1 ROM) died at the Trampoline's RTAS
   dependency, *before* the gates — so QEMU never reproduced the SheepShaver gate-2 failure,
   and the matrix lacks a "gates visibly fire under QEMU" cell. The positive result (Run B)
   is still well-controlled by Run A (same everything, only the ROM file differs), and
   Angle B independently explains the mechanism. But strictly: we proved "9.0.1 passes",
   not "QEMU can show a ROM failing these gates".
2. **CD-boot context only.** The HD-copied-System context (where SheepShaver originally saw
   $63 then $76) was not bootable under QEMU (bare-HFS / hand-built-APM driver issue), so
   gate behavior on an *installed* 9.2.1 was inferred from identical gate code, not re-run.
3. **`pathB_upgrade.dsk` is pre-patched** (live boot3: 4E71 at +0x0404 and +0x70A4 —
   verified). Any experiment using it must revert those first (Run D did). The CD's System
   is clean and is the right unpatched source going forward.
4. **$9F3E/$A2EC not fully decoded.** The per-entry version/instantiation predicate for
   `fovr`/`sfvr` entries is characterized (GetResource → helper, dependent on CFM/parcel
   state), not instruction-level proven. The empirical result makes finishing that RE
   unnecessary for the M7 decision.
5. **QEMU device models ≠ SheepShaver's machine layer.** Run B says the *gate logic* is
   satisfied by the 9.0.1 ROM's payload; it says nothing about SheepShaver's ability to get
   that ROM through nanokernel/DR-emulator/device init (M1–M6 territory, HANDOFF §2.8).
6. Asset hygiene: all modifications happened on `/tmp/s1/` copies. Caveat: `hmount` was run
   once against the original ISO (read-only operations — `hls`/`hcopy` out), and hfsutils
   may bump the volume's last-modified timestamp on mount; content untouched.
7. Screenshot files live in `/tmp/s1/` (ephemeral): `runA_t45.png` (control Finder),
   `runB_t30.png` (**9.0.1 ROM Finder, gates unpatched**), `runC_t45.png` (1.1 RTAS abort),
   `runD_t40.png` (?-floppy), `run0_t20.png` (bare-HFS OpenBIOS failure).

## 4. Recommendation for M7

**Take the native 9.0.1 ROM path.** The fallback ladder simplifies:

1. ~~Newer family ROM (9.6.1/9.8.1)~~ — not needed; 9.0.1 itself satisfies 9.2.1's gates.
2. **4-byte bypass** stays as the tactical fallback, primarily for the residual $76-on-HD
   uncertainty (§2.3) and for any 9.2.**2**-specific delta (S1 tested the 9.2.1 System;
   9.2.2's boot 3 should be re-checked with the same DSAT/offset methodology when the
   media is in hand — the gate architecture is the same family).

Bonus for M0/M5: the probe's dependence on **CFM boot fragments registered from ROM
parcels** makes the parcels/override handshake ('fovr'/'sfvr'/'nlib'/'ntrb'/'ncod') a
first-class part of the machine description — the Trampoline-handoff component (§2e of the
plan) must preserve it, and `gestaltMachineType=406` selecting the *short* checklist is a
small, welcome break: on SheepShaver's identity the gate audits only DebugLib +
InterfaceLib(fovr 50) + PrivateInterfaceLib(fovr 6).
