# Mac OS 9.x System File Boot Gates

> **Created:** 2026-06-10 · **Scope:** Mac OS 9.2.1 on 1.1 NewWorld ROM ·
> **Applies to:** any Mac OS 9.x System file running on a ROM older than its target

The Mac OS 9 System file contains boot-time compatibility checks in its `boot` resource
that reject unsupported hardware/ROM configurations. This document maps those checks as
discovered through binary analysis of the 9.2.1 System file.

The gates are generic Mac OS mechanisms — not SheepShaver-specific. Any emulator or
real hardware running a System file on a ROM it doesn't expect will hit these same checks.

---

## 1. Boot resource architecture

The System file's resource fork contains several `boot` resources that execute during
early startup (after the nanokernel hands off to the 68k emulator):

| Resource | Size | Role |
|----------|------|------|
| `boot` id=2 | ~small | Early init |
| `boot` id=3 | 55,048 bytes | **Main startup code** — compatibility checks, Toolbox init, extension loading |
| `boot` id=$57BC | 16,352 bytes | Additional boot code (5 `_SysError` calls, none are compatibility gates) |

`boot` id=3 is where the compatibility gates live. It contains 39 `_SysError` (trap A9C9)
call sites, each with a specific DSAT error code.

## 2. _SysError and the DSAT resource

`_SysError` (68k trap word `A9C9`) is the Mac Toolbox call for fatal system errors. It:
1. Reads error code from d0
2. Looks up the error text in the **DSAT** (Deep System Alert Table) resource, id=0
3. Displays a modal dialog with the stop-hand icon and a Restart button
4. Halts the system (only Restart is possible)

### DSAT resource format (id=0, 2354 bytes in 9.2.1)

```
Header:
  uint16  count               # number of error code entries (22 in 9.2.1)

Entry table (count × 14 bytes each):
  uint16  error_code          # the _SysError code (e.g., $63, $76)
  uint16  unknown_1           # always 0x000A in observed data
  uint16  text_marker         # 0xb1XX — links to text section
  uint16  unknown_2
  uint16  unknown_3
  uint16  unknown_4
  uint16  unknown_5

Text section (variable length, after entry table):
  Entries, each:
    uint16  marker            # 0xb1XX (matches entry table)
    uint16  text_length       # byte length of dialog text
    uint16  param_1           # display parameter (0x005E in all observed)
    uint16  param_2           # display parameter (0x0072 in all observed)
    char[]  text              # null-terminated, '/' = line break in dialog
```

### Error code → text mapping (9.2.1 System file)

| Code | Marker | Dialog text |
|------|--------|-------------|
| $7FFF | b174 | "Sorry, a system error occurred." (default) |
| $0063 | b1DE | "The "System" file on this startup disk may be damaged..." |
| $0066 | b179 | "This startup disk will not WORK on this Macintosh model..." |
| $0068 | b178 | "This disk must be unlocked in order to perform one-time housekeeping..." |
| $0069 | b176 | "System 9.2.1 needs more memory to start up." |
| $0074 | b175 | "This startup disk will not work on this computer. A Power PC based computer is required." |
| $0076 | b173 | "The system software on the startup disk only functions on the original media, not if copied to another drive." |
| $0078 | b172 | "The "Mac OS ROM" file on this startup disk is too old to be used with Mac OS 9.2..." |

Additional entries exist for codes $29A, $29B, $29C (3-digit codes used by late-boot
error paths) and several others. The table above covers the boot-gate-relevant codes.

## 3. Compatibility gates in boot id=3

### Gate 2: Model check — `_SysError($63)` at offset 0x0404

```
0x03FA: 4eba 7a28    jsr   (+$7a28,pc)      ; call subroutine at ~0x7E24
0x03FE: 321f         move.w (a7)+,d1        ; pop result
0x0400: 6704         beq.s  +4              ; skip if result == 0 (OK)
0x0402: 7063         moveq  #$63,d0         ; error code $63
0x0404: a9c9         _SysError              ; FATAL — model rejection
```

The subroutine at ~0x7E24 probes the ROM environment. Returns 0 if acceptable, non-zero if
rejected. The 1998 v1.1 ROM fails this check.

> **RESOLVED by Spike S1 (2026-06-10, `docs/planning/spikes/SPIKE-S1-QEMU-GATE-CHECK.md`):**
> the probe is a **CFM boot-fragment audit**, not a model/version check. It selects a
> checklist via `Gestalt('mach')` (short 3-entry list for machineType 406), then verifies
> fragments (DebugLib, InterfaceLib, Math64Lib, MPLibrary, …) via
> `GetResource('fovr'/'sfvr'/'nlib', id)` + CFM `GetMemFragment`/`GetSharedLibrary('pwpc')`.
> The "version words" previously noted here are actually **resource IDs**. The 9.0.1 ROM's
> `prcl` parcels provide this state natively (unpatched 9.2.1 boots to Finder on it under
> QEMU mac99); the 1.1 ROM (no parcels, no fragment names) **structurally cannot pass** —
> which is why identity patches never worked.

**Bypass:** NOP the A9C9 at offset 0x0404 (`4E71`). The `moveq` still executes but the
_SysError never fires — d0 is subsequently overwritten.

**Note on error code vs. dialog text:** DSAT maps $63 to "The System file may be damaged,"
NOT "won't work on this Macintosh model" (which is $66). Yet the dialog that appears when
this gate fires is a model rejection. The subroutine at ~0x7E24 may override the dialog
text before `_SysError` renders it, or the error-display path has a fallback. The empirical
observation is clear: NOP at 0x0404 suppresses the model-rejection dialog.

### Gate 2a: btst check — `_SysError($66)` at offset 0x03CA (CD boot only)

```
0x03C0: 0801 0002    btst  #2,$0B20         ; test bit 2 of byte at $0B20
0x03C4: 6706 0B20    beq.s +6               ; skip if bit clear
0x03C8: 7066         moveq #$66,d0
0x03CA: a9c9         _SysError($66)
```

This gate fires on **CD/ISO boot** but **not on HD boot** — the bit at $0B20 is set when
booting from removable media but clear when booting from a hard disk. On real hardware this
distinguishes installer CD boots from installed-system HD boots.

**Bypass (CD boot only):** NOP the A9C9 at offset 0x03CA (`4E71`). Not needed for HD boot.

### Gate 3: Original media check — `_SysError($76)` at offset 0x70A4

```
0x709A: 1e03         move.b d3,d7           ; copy check result
0x709C: 4a07         tst.b  d7              ; test it
0x709E: 660a         bne.s  +10             ; skip if non-zero (OK)
0x70A0: 7076         moveq  #$76,d0         ; error code $76
0x70A2: (branch?)
0x70A4: a9c9         _SysError              ; FATAL — original media rejection
```

This check detects that the System file has been copied to a different disk/volume than
the one it was originally installed on. On real hardware this prevents casual piracy of
the System Folder. In the emulator context, we always "copy" the System to a disk image,
so this fires unconditionally.

**Bypass:** NOP the A9C9 at offset 0x70A4 (`4E71`).

### Ruled out: _Alert (A985) at offset 0x599C

`_Alert` (trap A985) at boot id=3 offset 0x599C was investigated as a potential non-fatal
gate mechanism (alerts return to the caller, unlike `_SysError` which halts). NOP-ing it
alongside Gate 2 did not suppress the "original media" dialog — the dialog is produced
exclusively by `_SysError($76)` at 0x70A4.

### Full error code catalog (boot id=3, 39 call sites)

| Offset | Error | Offset | Error | Offset | Error |
|--------|-------|--------|-------|--------|-------|
| 0x011E | $19 | 0x01D0 | (dynamic) | 0x0272 | $69 |
| 0x03CA | $66 | 0x0404 | **$63** ★ | 0x0978 | $62 |
| 0x153C | $19 | 0x1894 | $63 | 0x204E | $0C |
| 0x236C | $19 | 0x24B2 | $62 | 0x24F6 | $66 |
| 0x2528 | $62 | 0x2546 | $62 | 0x2576 | $63 |
| 0x25FA | $63 | 0x2684 | $62 | 0x26FA | $62 |
| 0x2750 | $62 | 0x278A | $62 | 0x2792 | $0C |
| 0x5436 | $28 | 0x5D5C | (table) | 0x5D64 | $0B |
| 0x5D82 | $0C | 0x70A4 | **$76** ★ | 0x7BD6 | $1B |
| 0x7C6A | $66 | 0x83B4 | $0F | 0x8944 | $78 |
| 0x8C0A | $69 | 0x8D36 | $69 | 0x8E82 | $69 |
| 0x9380 | $7A | 0x9A5E | $29A | 0x9A64 | $29B |
| 0x9A6A | $29C | 0xC656 | $66 | 0xCB1E | $0F |

★ = compatibility gate (must NOP for 9.2.1 on 1.1 ROM)

**Elimination testing:** Error code groups $62 (8 sites), $66 (4 sites), and $69 (4 sites)
were tested by selectively NOP-ing all sites for each code simultaneously. None produced the
"original media" dialog — that dialog is exclusively produced by $76 at offset 0x70A4.

Two sites use dynamic error codes:
- 0x01D0: error set by preceding trap (`a01f`) return value
- 0x5D5C: table lookup — `lea (0x12,pc),a0; move.b (0,a0,d0.w),d0; _SysError`
  Table at boot3+0x5D68 maps input d0 values to DSAT codes ($01–$10)

## 4. Disk-level patching

### Finding the live resource copy on an HFS disk

HFS disks may contain multiple copies of resource data due to allocation block reuse.
Only the live copy (referenced by the catalog B-tree) matters. For the 9.2.1 System file
on `pathB_upgrade.dsk`:

| Resource | Live disk offset | Size |
|----------|-----------------|------|
| `boot` id=3 | 0x18F39748 | 55,048 bytes |
| `boot` id=$57BC | 0x18F35764 | 16,352 bytes |

Gate offsets are relative to the resource start. Disk offset = resource disk offset + gate offset.

**MacBinary vs. HFS disk offsets:** When the System file is extracted to the host filesystem
(e.g., via `hmount`/`hcopy`), the result is a MacBinary-wrapped file: data fork at byte 128,
resource fork at byte 7,191,424 (for the 9.2.1 System, 14,176,128 bytes total). The gate
offsets above are HFS on-disk offsets — for offline analysis of an extracted MacBinary file,
locate the `boot` resource within the resource fork instead.

### Minimal 4-byte patch

To bypass both compatibility gates on disk, replace 2 bytes at each site:

```
Disk offset 0x18F39748 + 0x0404 = 0x18F39B4C:  A9 C9 → 4E 71
Disk offset 0x18F39748 + 0x70A4 = 0x18FA07EC:  A9 C9 → 4E 71
```

Result: Mac OS 9.2 splash screen appears (Happy Mac + "Mac OS 9.2 / Welcome to Mac OS").
Boot then stalls at a deeper ROM/System initialization mismatch (see §5).

## 5. Post-gate stall — SCC serial polling (identified)

After bypassing both gates, boot reaches the Mac OS 9.2 splash screen but stalls.
SS_PROBE_PC analysis identified the cause: ROM serial initialization code polling SCC hardware.

### Symptoms

- HOT-PC at 0x50484038, inside the 68k opcode handler mirror region (ROMBase+0x480000)
- ~2M blocks/s execution rate, no new block compilations (comp frozen ~10733)
- Same stall pattern with ALL A9C9 NOP'd — not another _SysError gate
- Identical stall on both HD and ISO boot — ROM-structural, not disk-related

### Root cause

The 68k PC stabilizes at **0x500cc998** (ROM offset 0xcc998) — serial init code polling
the SCC (Zilog 8530 Serial Communications Controller):

```
0x500cc998: btst.b  #$11,d7          ; test SCC status bit
0x500cc99c: btst.b  #$0,$2(a3)       ; read SCC status register directly
0x500cc9c0: jmp     (a6)             ; dispatch back to loop
```

Key register state at stall *(labels CORRECTED by Spike S3, 2026-06-10 —
`docs/planning/spikes/SPIKE-S3-STALL-DEVICE-PROBE.md`)*:
- r18 = 0xF3016000 — **VIA 6522 base** (NOT SCC ch A; IFR/T2 at 0x200-stride offsets)
- r19 = 0xF3012000 — **SCC base** (ch A control at +2, data at +6)
- r24 = 0x500cc998 — 68k PC (ROM **factory serial test monitor**, "STM 2.2/CTE 2.1")

Per S3: the loop polls SCC RR0 bit 0 ("Rx char available") at 0xF3012002, and the monitor's
designed escape is a **VIA T2 timeout** + Cuda handshake — the VIA timer is plausibly the
actual un-stick mechanism, since an honest "no Rx char" SCC never terminates the blocking
read. Note also (Spike S1): even past this stall, 9.2-on-1.1 is structurally capped by the
missing CFM boot fragments (§ Gate 2 above) — this path is a device-model testbed, not a
route to a 9.2 boot.

### Options

1. **Improve SCC emulation** in `serial.cpp` — make the SCC status register return
   appropriate values during serial init polling
2. **Patch ROM serial init** at 0x500cc998 — NOP the polling loop or force the branch
3. **Intercept SCC I/O** in the memory map to return "ready" status

This stall is the current frontier for the Upgrade Card (Path B) approach.

## 6. Applicability to other Mac OS versions

The `boot` resource architecture and DSAT mechanism are shared across Mac OS 9.x releases.
Other versions will have:
- Different `boot` id=3 code (different gate offsets, possibly different checks)
- Different DSAT entries (different error codes/text)
- But the same overall pattern: `moveq #$NN,d0; _SysError` after a conditional test

The binary search methodology (§ in LEARNINGS.md 2026-06-10) and DSAT parsing approach
are reusable for any System file version.
