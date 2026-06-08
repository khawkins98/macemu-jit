# patch_68k() HLE Shim Inventory

**Source:** `SheepShaver/src/rom_patches.cpp`, function `patch_68k()` (approx. line 1588–2491)

**Generated:** 2026-06-08 by read-only analysis of the source + `rom-patch-sizing.py` run against:
- Target: `/Users/Shared/macemu/2001-12-19 - Mac OS ROM 9.0.1.rom` (decoded to `/tmp/rom901_inventory.bin`, 4 MB)
- Control: `/tmp/rom11.bin` (Mac OS ROM 1.1, the baseline the patterns were calibrated against)

**Tool command:**
```
python3 SheepShaver/tools/rom-patch-sizing.py /tmp/rom901_inventory.bin --control /tmp/rom11.bin
```

**Column definitions:**
- **Name** — `*_dat` variable name in source (or descriptive label for non-pattern patches)
- **Concept** — what the shim does
- **EMUL_OP / patch type** — the M68K_EMUL_OP installed, or NOP/branch/data patch
- **Search Range** — `[start, end)` hex offsets passed to `find_rom_data`
- **NW-only?** — `Y` = inside `if (ROMType == ROMTYPE_NEWWORLD)`, `N` = all types, `!NW` = explicitly excluded for NewWorld
- **Already Lenient?** — `Y` = site already has a `g_rom_904_lenient` skip guard (won't hard-abort on miss)
- **9.0.1 Status** — result from `rom-patch-sizing.py` against the 9.0.1 parcels ROM

---

## Summary Counts (patch_68k only, 9.0.1 ROM)

| Status | Count |
|--------|-------|
| in_range (pattern found in declared range) | 28 |
| relocated (found elsewhere via whole-image scan) | 31 |
| absent (pattern not present anywhere in image) | 25 |
| **Total find_rom_data calls** | **84** |

"Applicable absent" (absent AND not guarded by a ROM-type that excludes NewWorld): **17 patterns** — these are the hard abort points that will reject the 9.0.1 ROM without `SS_ROM_LENIENT`.

---

## How to use this table

This inventory maps every `find_rom_data` pattern in `patch_68k()` to its ROM-porting
status. Use it during incremental ROM bring-up:

1. **Run with lenient + trace enabled:**
   ```bash
   SS_ROM_LENIENT=1 SS_ROM_PATCH_TRACE=1 ./SheepShaver --config <new-rom.prefs> 2>/tmp/rompatch.log
   ```
2. **Grep for failures:** `grep -E 'MISS|SKIP' /tmp/rompatch.log`
3. **Look up each name below.** The "Concept" column tells you what the shim does, "EMUL_OP"
   tells you what it installs, and "9.0.1 Status" tells you whether the pattern is in-range,
   relocated (found by whole-image fallback), or absent (needs real RE to re-locate or confirm
   unnecessary).
4. **For `relocated` patterns:** verify offset arithmetic -- the pattern matched at a different
   address, so any `base+N` writes after it may land wrong if surrounding code shifted.
5. **For `absent` patterns:** disassemble the equivalent region in both the working ROM and the
   new ROM (capstone + `SS_DUMP_ROM`), find the rewritten routine, and either add a
   version-specific pattern or confirm the shim is unnecessary under emulation.

See `SheepShaver/docs/DIAGNOSTICS.md` ("ROM patching diagnostics") for the full log format
reference.

---

## Full Shim Table

| Name | Concept | EMUL_OP / Patch | Search Range | NW-only? | Already Lenient? | 9.0.1 Status |
|------|---------|-----------------|-------------|----------|-----------------|--------------|
| `reset_dat` | Remove 68k RESET instruction | NOP | `[0xc8, 0x120)` | N | No | relocated (@0xba) |
| `powermac_id_dat` | Fake reading PowerMac ID via Universal | Inline data patch (move.l #id,d0) | `[0xe000, 0x15000)` | N | No | **absent** |
| `univ_info_dat` | Patch UniversalInfo struct | Data patch (struct fields) | `[0x14000, 0x18000)` | NW only | No | relocated (@0xe198) |
| `via_init_dat` | Don't initialize VIA (via Universal) | branch (bra skip) | `[0xe000, 0x15000)` | N | No | relocated (@0xaad8) |
| `via_init2_dat` | Don't initialize VIA (alt entry) | `jmp (a6)` | `[0xa000, 0x10000)` | N | No | relocated (@0x9584) |
| `via_init3_dat` | Don't initialize VIA (third entry) | `jmp (a6)` | `[0xa000, 0x10000)` | N | No | relocated (@0x9630) |
| `run_diags_dat` (NW) | Don't RunDiags, get BootGlobs directly | Inline `lea RAMTop-0x1c,a6` | `[0x110, 0x128)` | NW only | Yes (skip w/ warning) | relocated (@0xde) |
| `run_diags_dat` (OW) | Don't RunDiags (OldWorld path) | Inline `lea RAMTop-0x1c,a6` | `[0xd0, 0xf0)` | not NW | No | not applicable (NW ROM) |
| `nvram1_dat` | Replace NVRAM read routine (all types) | `M68K_EMUL_OP_XPRAM1` + RTS | `[0x7000, 0xc000)` | N | No | in_range |
| `nvram2_dat` (NW) | Replace NVRAM read (NewWorld variant 2) | `M68K_EMUL_OP_XPRAM2` + `jmp (a3)` | `[0xa000, 0xd000)` | NW only | No | **absent** |
| `nvram3_dat` (NW) | Replace NVRAM write (NewWorld variant 3) | `M68K_EMUL_OP_XPRAM3` + `jmp (a3)` | `[0xa000, 0xd000)` | NW only | No | **absent** |
| `nvram4_dat` (NW) | Patch NVRAM multi-byte op (NW) | `M68K_EMUL_OP_NVRAM3` + epilogue | `[0xa000, 0xd000)` | NW only | No | **absent** |
| `nvram5_dat` (NW) | NOP out NVRAM check | NOP | `[0xa000, 0xd000)` | NW only | No | **absent** |
| `nvram6_dat` (NW) | Stub NVRAM clear function | Inline zero + RTS | `[0x9000, 0xb000)` | NW only | No | **absent** |
| `nvram7_dat` (NW) | Patch NVRAM exit path (optional) | RTS | `[0x9000, 0xb000)` | NW only | No (soft: `if (base)`) | **absent** |
| `nvram2_dat` (OW) | Replace NVRAM read (OldWorld variant) | `M68K_EMUL_OP_XPRAM2` + `jmp (a3)` | `[0x7000, 0xb000)` | not NW | No | not applicable (NW ROM) |
| `nvram3_dat` (OW) | Replace NVRAM write (OldWorld variant) | `M68K_EMUL_OP_XPRAM3` + `jmp (a3)` | `[0x7000, 0xb000)` | not NW | No | not applicable (NW ROM) |
| `nvram4_loc` (OW) | Patch OW NVRAM read by hardcoded offset | `M68K_EMUL_OP_NVRAM1` | hardcoded per ROMType | not NW | N/A | not applicable |
| `nvram5_loc` (OW) | Patch OW NVRAM write by hardcoded offset | `M68K_EMUL_OP_NVRAM2` | hardcoded per ROMType | not NW | N/A | not applicable |
| `mem_top_dat` | Fix MemTop/BootGlobs during startup | `M68K_EMUL_OP_FIX_MEMTOP` + NOP | `[0x120, 0x180)` | N | No | in_range |
| `scc_init_caller_dat` | Locate SCC init caller | (indirect — used to find `scc_init_dat`) | `[0x180, 0x1f0)` | N | No | in_range |
| `scc_init_dat` | Don't initialize SCC | `M68K_EMUL_OP_RESET` + RTS | relative to `scc_init_caller_dat` | N | No | in_range |
| `ext_cache_dat` | Don't EnableExtCache / DisableIntSources | RTS (at indirect targets) | `[0x1d0, 0x230)` | N | No | relocated (@0x1b6) |
| `timek_dat` | Fake CPU speed test (SetupTimeK) | Inline constant writes + RTS | `[0x400, 0x500)` | N | No | **absent** |
| `jump_tab_dat` | Relocate jump tables `$2000..` | Data relocation (ROM pointer fixup) | `[0x3000, 0x6000)` | N | No | in_range |
| `sys_zone_dat` | Create SysZone at start of Mac RAM | Data patch (RAMBase addresses) | `[0x600, 0x900)` | N | No | in_range |
| `boot_stack_dat` | Set boot stack + fix logical/physical RAM size | `M68K_EMUL_OP_FIX_MEMSIZE` + inline + RTS | `[0x580, 0x800)` | N | No | in_range |
| `page_size_dat` | Get PowerPC page size (InitVMemMgr) | Inline `move.l #$1000,d0` + NOPs | `[0xb000, 0x12000)` | N | Yes (skip w/ warning) | **absent** |
| `page_size2_dat` | Gestalt PPC page size / CPU type / RAM size | Inline constants + NOPs | `[0x50000, 0x70000)` | N | Yes (skip w/ warning) | **absent** |
| `gc_mask_dat` | Don't write to GC interrupt mask register | NOP (x4 locations) | `[0x13000, 0x20000)` | not NW | No | **absent** (excluded for NW) |
| `gc_mask2_dat` | Don't write to GC interrupt mask register (alt) | NOP (x5+ entries) | `[0x13000, 0x20000)` | not NW | No | **absent** (excluded for NW) |
| `cuda_init_dat` | Don't initialize Cuda (via 0x274) | NOP x7 | `[0xa000, 0x12000)` | N | No | relocated (@0x9be2) |
| `cpu_speed_dat` | Fake CPU speed (GetCPUSpeed) | Inline `move.l #MHz,d0` + RTS | `[0x6000, 0xa000)` | N | Yes (skip w/ warning) | **absent** |
| `time_via_dat` | Don't poke VIA in InitTimeMgr | Inline `movem.l` + RTS (early return) | `[0x30000, 0x40000)` | N | Yes (skip w/ warning) | **absent** |
| `open_firmware_dat` | Don't read from 0xff800000 (OF/Name Registry) | Inline deadbeef + NOP x2 | `[0x48000, 0x58000)` | N | Yes (skip w/ warning) | **absent** |
| `ext_cache2_dat` | Don't EnableExtCache (via 0x2b2) | RTS | `[0x13000, 0x20000)` | N | No | relocated (@0xe6a6) |
| `tm_task_dat` (NW/Gossamer) | Don't install 60Hz TM interrupt task | NOP x6 | `[0x2a0, 0x320)` | NW+Gossamer | No | relocated (@0x10586) |
| `tm_task_dat` (other) | Don't install 60Hz TM interrupt task (OW) | NOP x3 | `[0x280, 0x300)` | not NW, not Gossamer | No | relocated (@0x10586) |
| `dsl_pvr_dat` (Zanzibar) | Don't read PVR from 0x5fffef80 (DriverServicesLib) | Inline `lis r4,PVR` | relative to nlib resource | not NW, not Gossamer | No | not applicable |
| `dsl_pvr_dat` (other OW) | Don't read PVR from 0x5fffef80 (DriverServicesLib) | Inline `lis r4,PVR` | relative to nlib resource | not NW, not Gossamer | No | not applicable |
| `dsl_bus_dat` (Zanzibar) | Don't read bus clock from 0x5fffef88 (DSLib) | Inline `lwz r8,(bus clock speed)` | relative to nlib resource | not NW, not Gossamer | No | not applicable |
| `dsl_bus_dat` (other OW) | Don't read bus clock from 0x5fffef88 (DSLib) | Inline `lwz r4,(bus clock speed)` | relative to nlib resource | not NW, not Gossamer | No | not applicable |
| `hpchk_dat` | Don't read from MacPgm (StdCLib WipeOutMACPGMINFO) | Inline `lwz r4,(zero page)` | relative to nlib:10 resource | N (always) | No | in_range |
| `name_reg_dat` | Patch Name Registry | `M68K_EMUL_OP_NAME_REGISTRY` | `[0x300, 0x380)` | N | No | relocated (@0x2fa) |
| `scsi_mgr_a_dat` / `scsi_mgr_b_dat` | Fake SCSI Manager (`#if DISABLE_SCSI`) | `M68K_EMUL_OP_SCSI_ATOMIC` + `M68K_EMUL_OP_SCSI_DISPATCH` | `[0x1c000, 0x28000)` | N | No | scsi_mgr_a relocated (@0x18e30); scsi_mgr_b absent |
| `scsi_var_dat` (NW) | Don't access SCSI variables (`#if DISABLE_SCSI`) | branch | `[0x1f500, 0x1f600)` | NW only (soft) | No (soft: `if (base)`) | relocated (@0x1921c) |
| `scsi_var2_dat` (NW) | Don't access SCSI var2 (`#if DISABLE_SCSI`) | moveq #0,d0 + RTS | `[0x1f700, 0x1f800)` | NW only (soft) | No (soft: `if (base)`) | **absent** |
| `scsi_var_dat` (Gossamer) | Don't access SCSI variables, Gossamer | branch | `[0x1d700, 0x1d800)` | Gossamer only (soft) | No (soft: `if (base)`) | relocated (@0x1921c, wrong range) |
| `scsi_var2_dat` (Gossamer) | Don't access SCSI var2, Gossamer | moveq #0,d0 + RTS | `[0x1d900, 0x1da00)` | Gossamer only (soft) | No (soft: `if (base)`) | **absent** |
| `adb_init_dat` | Don't wait in ADBInit (via 0x36c) | NOP | `[0x31000, 0x3d000)` | N | No | relocated (@0x2b780) |
| `init_res_dat` | Fix InitResources() for addr > 0x80000000 | byte patch (0x6e→0x66) | `[0x78000, 0x8c000)` | N | No | relocated (@0x650aa) |
| `check_load_dat` | Patch vCheckLoad() to allow resource patching | `M68K_EMUL_OP_CHECKLOAD` via trampoline | `[0x78000, 0x8c000)` | N | No | relocated (@0x66328) |
| `drvr_install_dat` | Patch driver install routine | `M68K_EMUL_OP_INSTALL_DRIVERS` + RTS | `[0xb00, 0xd00)` | N | No | relocated (@0x9bc) |
| (trap table) | Replace Time Manager | `M68K_EMUL_OP_INSTIME/RMVTIME/PRIMETIME/MICROSECONDS` | trap table entries (a058/a059/a05a/a093) | N | N/A | in_range (trap-table based) |
| `egret_dat` | Disable Egret Manager | moveq #0,d0 + RTS | `[0xa000, 0x10000)` | N | No | relocated (@0x98ac) |
| `shutdown_dat` | Don't call FE0A in Shutdown Manager | RTS or bra (varies) | `[0x30000, 0x40000)` | N | No | relocated (@0x2c76e) |
| (trap table) | Patch PowerOff() → host exit | `M68K_EMUL_OP_POWEROFF` | trap a05b | N | N/A | in_range |
| `via_int_dat` | Patch VIA interrupt handler (Level 1) | Inline moveq #2,d0 + NOPs | `[0x13000, 0x1c000)` | N | No | relocated (@0xef2c) |
| `via_int2_dat` | Patch 60Hz VIA handler | `M68K_EMUL_OP_IRQ` + tst/beq | `[0x10000, 0x18000)` | N | No | relocated (@0xbbc8) |
| `via_int3_dat` | Redirect CHRP Level 1 handler | `M68K_JMP` to level1_int | `[0x15000, 0x19000)` | NW only | No | relocated (@0xec50) |
| (trap table) | Patch ZeroScrap / PutScrap / GetScrap | `M68K_EMUL_OP_ZERO_SCRAP/PUT_SCRAP/GET_SCRAP` | trap a9fc/a9fe/a9fd | N | N/A | in_range |
| (trap) | Patch SynchIdleTime | `M68K_EMUL_OP_IDLE_TIME` or `IDLE_TIME_2` | trap abf7+4 | N (if `idlewait` pref) | N/A | in_range |

---

## Applicable-Absent Detail (17 patterns — hard abort for 9.0.1 without SS_ROM_LENIENT)

These are the patterns that are (a) absent in the 9.0.1 ROM image and (b) NOT protected by a ROM-type exclusion or existing lenient guard. Each is a hard `return false` for a 9.0.1 boot attempt:

| Name | Range | First 4 bytes | Why absent / notes |
|------|-------|---------------|---------------------|
| `powermac_id_dat` | `[0xe000, 0x15000)` | `45 f9 5f ff` | Function eliminated or inlined in 9.0.1 Universal |
| `nvram2_dat` (NW) | `[0xa000, 0xd000)` | `48 e7 1c e0` | Rewritten NVRAM path in 9.0.1 |
| `nvram3_dat` (NW) | `[0xa000, 0xd000)` | `48 e7 dc e0` | Rewritten NVRAM path in 9.0.1 |
| `nvram4_dat` (NW) | `[0xa000, 0xd000)` | `4e 56 ff a8` | Rewritten NVRAM path in 9.0.1 |
| `nvram5_dat` (NW) | `[0xa000, 0xd000)` | `0c 80 03 00` | Rewritten NVRAM path in 9.0.1 |
| `nvram6_dat` (NW) | `[0x9000, 0xb000)` | `2f 0a 24 48` | Rewritten NVRAM path in 9.0.1 |
| `timek_dat` | `[0x400, 0x500)` | `0c 38 00 04` | SetupTimeK may be absent/NOP in 9.0.1 (no hardware clock) |
| `page_size_dat` | `[0xb000, 0x12000)` | `20 30 81 f2` | InitVMemMgr rewritten in 9.0.1; already has lenient guard |
| `page_size2_dat` | `[0x50000, 0x70000)` | `26 79 5f ff` | InitGestalt rewritten in 9.0.1; already has lenient guard |
| `cpu_speed_dat` | `[0x6000, 0xa000)` | `20 30 81 f2` | GetCPUSpeed absent in 9.0.1; already has lenient guard |
| `time_via_dat` | `[0x30000, 0x40000)` | `40 e7 00 7c` | InitTimeMgr rewritten in 9.0.1; already has lenient guard |
| `open_firmware_dat` | `[0x48000, 0x58000)` | `2f 79 ff 80` | OF read absent in 9.0.1; already has lenient guard |
| `scsi_var2_dat` (NW) | `[0x1f700, 0x1f800)` | `4e 56 fc 58` | Soft guard (`if (base)`) — won't abort |
| `nvram7_dat` (NW) | `[0x9000, 0xb000)` | `42 2a 00 04` | Soft guard (`if (base)`) — won't abort |

Note: `gc_mask_dat` and `gc_mask2_dat` are absent for 9.0.1 but guarded by `if (ROMType != ROMTYPE_NEWWORLD)` — they are not applicable for the NewWorld ROM at all.

**Hard aborts without lenient mode:** `powermac_id_dat`, `nvram2_dat` (NW), `nvram3_dat` (NW), `nvram4_dat` (NW), `nvram5_dat` (NW), `nvram6_dat` (NW), `timek_dat`. These 7 are the primary unguarded blockers for 9.0.1 ROM.

---

## Relocated Patterns (need range widening for 9.0.1)

These patterns exist in the 9.0.1 ROM but outside their declared search windows. With `g_rom_904_lenient` the whole-image fallback finds them, but the patch is applied at the wrong site relative to surrounding code if the offset arithmetic after `base` is ROM-version-dependent.

Key relocated patterns to audit for correctness (offset arithmetic may differ):

| Name | Declared Range | Found At | Notes |
|------|---------------|----------|-------|
| `reset_dat` | `[0xc8, 0x120)` | @0xba | Very close — likely fine |
| `via_init_dat` | `[0xe000, 0x15000)` | @0xaad8 | Relocated earlier; patch offset `+4` is relative — may still be correct |
| `via_init2_dat` | `[0xa000, 0x10000)` | @0x9584 | Just before window start — offset 0 patch likely fine |
| `via_init3_dat` | `[0xa000, 0x10000)` | @0x9630 | As above |
| `run_diags_dat` (NW) | `[0x110, 0x128)` | @0xde | Offset –6 patch writes before pattern; needs careful verification |
| `univ_info_dat` | `[0x14000, 0x18000)` | @0xe198 | Relocated by ~0x6000 earlier; struct layout may differ in 9.0.1 |
| `ext_cache_dat` | `[0x1d0, 0x230)` | @0x1b6 | Indirect — patch follows runtime pointer from `base+6`; may work |
| `adb_init_dat` | `[0x31000, 0x3d000)` | @0x2b780 | Large relocation; `+6` offset patch likely still correct |
| `tm_task_dat` (NW) | `[0x2a0, 0x320)` | @0x10586 | Large relocation — `+28` NOPs may land wrong |
| `via_int_dat` | `[0x13000, 0x1c000)` | @0xef2c | Relocated — `level1_int` pointer computed from this; downstream `via_int3_dat` uses it |
| `via_int2_dat` | `[0x10000, 0x18000)` | @0xbbc8 | Relocated — patch offset 0, likely fine |
| `trap_return_dat` | `[0x312000, 0x320000)` | @0x32451c | PPC nanokernel patch — outside original range |

---

## Notes

1. The `#if DISABLE_SCSI` block wraps `scsi_mgr_*_dat` and `scsi_var_*_dat` — these only activate when compiled with `DISABLE_SCSI` defined. They are conditionally included in the analysis above.

2. OldWorld-only patches (`nvram2_dat`/`nvram3_dat`/`nvram4_loc`/`nvram5_loc` in the else branch, `gc_mask_dat`/`gc_mask2_dat`, `dsl_pvr_dat`/`dsl_bus_dat`) are excluded from NewWorld ROM runs by the surrounding `if/else (ROMType == ...)` guards and are listed here for completeness only.

3. Several patches target PPC code (nanokernel, DSLib PPC resources) rather than 68k ROM code. They appear in `patch_68k()` for historical reasons but operate on PPC instruction words at fixed resource offsets.

4. For the 9.0.1 ROM porting work, see `docs/planning/NEW-WORLD-ROM-SUPPORT-PLAN.md` for the overall strategy and `HANDOFF-NEWWORLD-SUPERVISOR-MMU.md` for current status.
