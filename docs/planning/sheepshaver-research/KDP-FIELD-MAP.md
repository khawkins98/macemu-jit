# KDP (Kernel Data Page) Field Map

The NanoKernel's per-CPU data structure, addressed via SPRG0 (= r1 in kernel code).
Fields at positive offsets from KDP base. Source: `Init.s`, `Reset.s`, `Emulate.s`,
`Defines.s` from elliotnunn/NanoKernel disassembly.

## Source Material

- **elliotnunn/OldKern** `KDP.h` — the most complete public field map (hex offsets,
  field names, types). Covers NanoKernel versions through ~9.2.
- **elliotnunn/OldKern** `InfoRecords.h` — `NKConfigurationInfo`, `NKSystemInfo`,
  `NKProcessorInfo` structs (the firmware→nanokernel handoff records).
- **NanoKernel source** (`Init.s`, `Reset.s`, `Defines.s`, `Emulate.s`) — field
  definitions and boot-time initialization flow.

---

## Complete Init/Reset KDP Field Map (0x500–0x700 focus)

### All KDP fields in 0x500–0x700 range (Init.s + Reset.s + Emulate.s)

r1 = KDP throughout. rCI = r3 (ConfigInfo ptr). rED = r8 (EDP = KDP + 0x1000).
r13 = KernelMemoryBase. r14 = HTAB mask. r15 = total kernel memory size.

| Offset | Name | Init.s line | Source register | Value comes from | Read by Reset.s? | Purpose |
|--------|------|-------------|-----------------|------------------|-------------------|---------|
| 0x5A0:0x5A8 | FloatScratch | — (zeroed) | r0 | zero (bulk wipe) | no | FP scratch area |
| 0x5A8:0x5B0 | (reserved padding) | — (zeroed) | r0 | zero (bulk wipe) | no | Gap between FloatScratch and IntHandlerPtr; no named field; wall-dump values are leftover noise |
| 0x5B0 | IntHandlerPtr | 200 | r7 | `GetExtIntHandler` return (line 196) | yes (line 49): loaded → VecTblSystem.External | External interrupt handler address |
| 0x5B8 | InstEmControl | 78 (Emulate.s) | r23 | Probed perf-monitor SPR flags (EmAlways1\|2 ± EmHasMMCR0/1/SDA) | no (runtime only) | Instruction emulation control flags |
| 0x5BC | InstEmTimebaseScale | 112 (Emulate.s) | r24 | Long-division: 0x80587ff3_d62611e3 / DecClockRateHz | no (runtime only) | TB→RTC nanosecond scaling factor |
| 0x5C8 | SupervisorSpace.SegMapPtr | Reset 268 | r23 | `r1 + KDP.SupervisorSegMap` (computed) | — (set in Reset) | Supervisor address space record |
| 0x5CC | SupervisorSpace.BatMap | Reset 270 | r23 | `ConfigInfo.BatMap32SupInit` | — | BAT mapping for supervisor |
| 0x5D0 | UserSpace.SegMapPtr | Reset 273 | r23 | `r1 + KDP.UserSegMap` | — | User address space record |
| 0x5D4 | UserSpace.BatMap | Reset 275 | r23 | `ConfigInfo.BatMap32UsrInit` | — | BAT mapping for user |
| 0x5D8 | CpuSpace.SegMapPtr | Reset 278 | r23 | `r1 + KDP.CpuSegMap` | — | CPU address space record |
| 0x5DC | CpuSpace.BatMap | Reset 280 | r23 | `ConfigInfo.BatMap32CPUInit` | — | BAT mapping for CPU |
| 0x5E0 | OverlaySpace.SegMapPtr | Reset 283 | r23 | `r1 + KDP.OverlaySegMap` | — | Overlay address space record |
| 0x5E4 | OverlaySpace.BatMap | Reset 285 | r23 | `ConfigInfo.BatMap32OvlInit` | — | BAT mapping for overlay |
| 0x5F0:0x630 | KCallTbl | Reset 97–133 | r23 | `_kaddr` of each KCall handler | — | Trap dispatch table (16 entries) |
| 0x630 | ConfigInfoPtr | 198 | rCI (r3) | Entry register r3 (passed by firmware) | yes (line 7): loaded into rCI=r26 | Pointer to NKConfigurationInfo |
| 0x634 | EDPPtr | 206 | rED (r8) | `r1 + 0x1000` (KDP + 4096) | yes (line 224): loaded for PMDT patching | Emulator Data Page pointer |
| 0x638 | **KernelMemoryBase** | 208 | r13 | Computed: start of kernel alloc in chosen bank | yes (line 315): free-list exclusion range start | Lowest phys addr of nanokernel memory |
| 0x63C | **KernelMemoryEnd** | 210 | r12 | `r13 + r15` (base + total kernel size) | yes (line 316): free-list exclusion range end | End of nanokernel memory |
| 0x640 | LowMemPtr | 213 | r12 | `ConfigInfo.PA_RelocatedLowMemInit` | no (Init reads it back at lines 358, 367) | Physical addr of MacOS Low Memory |
| 0x644 | SharedMemoryAddr | 216 | r12 | `ConfigInfo.SharedMemoryAddr` | no | Mac/Smurf shared message memory |
| 0x648 | EmuTrapTableLogical | 221 | r12 | `ConfigInfo.LA_EmulatorCode + ConfigInfo.KernelTrapTableOffset` | no (Init reads it at lines 354, 399) | Emulator trap table logical address |
| 0x64C | CodeBase | 226 | r12 | `bl *+4; mflr; addi` (NanoKernel code start, self-located) | yes (line 9): loaded into rNK=r25 | NanoKernel code base address |
| 0x650 | MRBase | 229 | r12 | `_kaddr` from CodeBase to MRBase label | no | MemRetry code base address |
| 0x654 | SysContextPtrLogical | 234 | r12 | `ConfigInfo.LA_EmulatorData + ConfigInfo.ECBOffset` | no | Logical addr of system ContextBlock |
| 0x658 | SysContextPtr | 237 | r12 | `rED + ConfigInfo.ECBOffset` (physical) | no | Physical addr of system ContextBlock |
| 0x65C | ContextPtr | 238 | r12 | Same as SysContextPtr (initial) | no | Currently active ContextBlock |
| 0x660 | Flags | 396 | r7 | `GlobalFlagSystem \| MQ-probe result` | no | NanoKernel flags (GlobalFlagSystem + MQ detect) |
| 0x664 | Enables | — (zeroed) | r0 | zero (bulk wipe) | no (runtime: Exceptions.s, RTAS.s) | Runtime enable flags |
| 0x668 | OtherContextDEC | 420 | r8 | `ProcInfo.DecClockRateHz` (1 second of ticks) | no | Decrementer ticks for inactive context |
| 0x66C | PageMapFreePtr | 262 | r13 | `PageMapPtr + ConfigInfo.PageMapInitSize` | no | First free byte in PageMap |
| 0x670 | TestIntMask | 241 | r12 | `ConfigInfo.TestIntMaskInit` | no | CR mask for testing pending interrupt |
| 0x674 | PostIntMask | 245 | r12 | `ConfigInfo.PostIntMaskInit` | no | CR flags set when posting interrupt |
| 0x678 | ClearIntMask | 243 | r12 | `ConfigInfo.ClearIntMaskInit` | no | CR mask for clearing interrupt |
| 0x67C | EmuIntLevelPtr | 249 | r12 | `rED + ConfigInfo.IplValueOffset` | no | Physical ptr to emulator IPL global |
| 0x680 | DebugIntPtr | 253 | r12 | `ConfigInfo.SharedMemoryAddr + 0x7C` | no | Debug shared memory pointer |
| 0x684 | PageMapPtr | 260 | r13 | `r1 + KDP.PageMap` (= KDP + 0x920) | yes (line 11): loaded into rPgMap=r18 | Start of PageMap buffer |
| 0x688 | **PageAttributeInit** | 256 | r12 | `ConfigInfo.PageAttributeInit` | yes (line 321): default PTE lower-word attrs | Default WIMG/PP for new Page Table Entries |
| 0x68C | HtabSingleEA | — (zeroed) | r0 | zero | no (runtime: PageTable.s) | Last single-page PMDT EA in HTAB |
| 0x690 | HtabSinglePTE | — (zeroed) | r0 | zero | no (runtime: PageTable.s) | Ptr to that PTE |
| 0x694 | HtabLastEA | — (zeroed) | r0 | zero | no (runtime: MRInts.s) | Last EA looked up in HTAB |
| 0x698 | HtabLastPTE | — (zeroed) | r0 | zero | no (runtime: MRInts.s) | Ptr to last PTE |
| 0x69C | HtabLastOverflow | — (zeroed) | r0 | zero | no | HTAB overflow tracking |
| 0x6A0 | **PTEGMask** | Reset 176 | r22 | `(SDR1 HTABMASK << 16) \| 0xFFC0` | — (set in Reset) | PTEG mask for HTAB lookups |
| 0x6A4 | **HTABORG** | Reset 175 | r8 | `SDR1 & 0xFFFF0000` (upper 16 bits) | — (set in Reset) | Hash table base address |
| 0x6A8 | **VMLogicalPages** | Reset 374 | r19 | `(free_page_count + 1) << 10 >> 12` | no | VM address space size in pages |
| 0x6AC | **VMPhysicalPages** | Reset 375 | r19 | Same as VMLogicalPages (initially equal) | no | Physical pages available to VM |
| 0x6B0 | **VMPageArray** | Reset 379 | r21 | `KernelMemoryBase` (r21 set at line 315, unchanged through 379; r29 = KMB-4 is the iteration pointer, not the stored value) | no | Base of 68k Page Descriptor array |
| 0x6B4 | **VMMaxVirtualPages** | Reset 306 | r22 | Counted from PMDT_Paged segments in PageMap | yes (line 359): caps logical area size | Max VM area the PageMap allows |
| 0x6B8 | PowerHID0Select | Reset 145 | r23 | `HID0SelectTable[PVR>>16]` (lookup table) | no | HID0 power mode selector |
| 0x6B9 | PowerHID0Enable | Reset 147 | r23 | `HID0EnableTable[PVR>>16]` (lookup table) | no | HID0 power enable mode |
| 0x6C0:0x700 | **PhysicalPageArray[16]** | Reset 389 | r21 | Per-segment ptrs into 68k PD array (4 entries used, incremented by 0x40000) | no | Per-segment 68k Page Descriptor pointers |

### Fields read by Reset.s (must be already set by Init.s)

| Offset | Name | Reset.s line | Used for |
|--------|------|-------------|----------|
| 0x630 | ConfigInfoPtr | 7 | Load rCI for all ConfigInfo accesses |
| 0x64C | CodeBase | 9 | Load rNK for `_kaddr` handler address computation |
| 0x684 | PageMapPtr | 11 | Load rPgMap for PageMap initialization |
| 0x5B0 | IntHandlerPtr | 49 | Stored into VecTblSystem.External |
| 0x634 | EDPPtr | 224 | Patch EDP PMDT physical address |
| 0x638 | KernelMemoryBase | 315 | Free-list exclusion range (lower bound) |
| 0x63C | KernelMemoryEnd | 316 | Free-list exclusion range (upper bound) |
| 0x688 | PageAttributeInit | 321 | Default WIMG/PP attrs for 68k PD construction |
| 0x6B4 | VMMaxVirtualPages | 359 | Caps logical area (read-after-write within Reset) |

### Fields set by InitEmulation (Emulate.s, called from Reset.s line 419)

| Offset | Name | Emulate.s line | Source | Purpose |
|--------|------|---------------|--------|---------|
| 0x5B8 | InstEmControl | 78 | Probed SPR availability flags | Which instructions to emulate |
| 0x5B8 byte 0 | InstEmControl (high byte) | 113 | TB shift exponent | Left-shift count for TB→RTC |
| 0x5BB byte 3 | InstEmControl+3 | 116 | `32 - exponent` | Right-shift count for TB→RTC |
| 0x5BC | InstEmTimebaseScale | 112 | Long-division quotient | Timebase scaling mantissa |

---

## Fields NOT set by Init/Reset (set at runtime or by Trampoline)

These fields appear in Defines.s but are not initialized during boot — they are either
zeroed by the bulk wipe, set at runtime, or must be seeded by the Trampoline:

| Offset | Name | Where set | Notes |
|--------|------|-----------|-------|
| 0x000:0x080 | EWA (r0–r31) | Runtime (exception entry) | Scratch save area |
| 0x080:0x280 | SegMaps (4 × 16 entries) | Reset.s:ResetSegMaps (bulk copy from ConfigInfo) | Segment map arrays |
| 0x280:0x300 | BatRanges | Reset.s:ResetBatRanges (from ConfigInfo) | BAT range entries |
| 0x300:0x340 | CurIBAT/CurDBAT | Runtime (SetSpace) | Current BAT register shadows |
| 0x340:0x360 | NCBPointerCache | Reset.s:161 (`_clrNCBCache`) | Zeroed |
| 0x5B4 | NatContextPtrLogical | SoftInts.s:116 | Set when alternate (native) context is created |
| 0x5E8:0x5F0 | CurSpace | Runtime (SetSpace) | Current address space |
| 0x908 | RTASDispatch | Init.s:153/165 | From `r8` if RTAS present, else 0 |
| 0x90C | RTASData | Init.s:155/166 | From `HWInfo.RTAS_PrivDataArea` if RTAS, else 0 |
| 0x910 | (legacy PageMap word) | Init.s:264 | Zeroed for backward compat |

---

## Critical Fields for Trampoline Seeding (0x500–0x700)

These are the fields that **must** be correctly populated for the NanoKernel's Reset flow
to succeed. Init.s sets them; our trampoline must replicate:

| Priority | Offset | Name | How to seed |
|----------|--------|------|-------------|
| **P0** | 0x630 | ConfigInfoPtr | Point to our synthetic ConfigInfo |
| **P0** | 0x638 | KernelMemoryBase | Start of HTAB/kernel region we allocated |
| **P0** | 0x63C | KernelMemoryEnd | End of kernel region |
| **P0** | 0x64C | CodeBase | NanoKernel code start in ROM |
| **P0** | 0x684 | PageMapPtr | `KDP + 0x920` (standard offset to PageMap area) |
| **P0** | 0x688 | PageAttributeInit | Copy from `ConfigInfo.PageAttributeInit` (runtime value; do not hardcode) |
| **P1** | 0x634 | EDPPtr | `KDP + 0x1000` |
| **P1** | 0x640 | LowMemPtr | Physical addr of MacOS Low Memory |
| **P1** | 0x5B0 | IntHandlerPtr | External interrupt handler address |
| **P1** | 0x648 | EmuTrapTableLogical | Emulator trap table |
| **P1** | 0x650 | MRBase | MemRetry code base |
| **P1** | 0x654 | SysContextPtrLogical | System ContextBlock logical addr |
| **P1** | 0x658 | SysContextPtr | System ContextBlock physical addr |
| **P1** | 0x65C | ContextPtr | Initially = SysContextPtr |
| **P2** | 0x644 | SharedMemoryAddr | From ConfigInfo |
| **P2** | 0x660 | Flags | `GlobalFlagSystem` (0x00800000, bit 8); optionally OR `GlobalFlagMQReg` (0x00040000, bit 13) if MQ present |
| **P2** | 0x668 | OtherContextDEC | DecClockRateHz value |
| **P2** | 0x670 | TestIntMask | From ConfigInfo |
| **P2** | 0x674 | PostIntMask | From ConfigInfo |
| **P2** | 0x678 | ClearIntMask | From ConfigInfo |
| **P2** | 0x67C | EmuIntLevelPtr | EDP + IplValueOffset |
| **P2** | 0x680 | DebugIntPtr | SharedMemoryAddr + 0x7C |
| **P2** | 0x66C | PageMapFreePtr | PageMapPtr + PageMapInitSize |

---

## Memory Layout (SheepShaver NW Trampoline)

```
Guest address space (not to scale):

0x10000000  ┌──────────────────────┐
            │   Guest RAM (256MB)  │  Pages here go into the free list
            │                      │  (excluding kernel memory range)
0x1FFFFFFF  └──────────────────────┘

0x50000000  ┌──────────────────────┐
            │   ROM (5 MB)         │  NanoKernel code lives here
0x504FFFFF  └──────────────────────┘

0x68FB0000  ┌──────────────────────┐  ← KernelMemoryBase (r13)
            │ Page descriptors     │  Free list grows UPWARD via stwu
            │  (256 KB, 65536 pgs) │  rom_patches.cpp `desc_create` skip
0x68FF0000  ├──────────────────────┤
            │   (safety gap 16KB)  │
0x68FF4000  ├──────────────────────┤  ← sub_kdp_base = IRP base
            │ sub-KDP pool (32 KB) │  Pool/heap for spinlocks etc.
            │  IRP+0xDF0 = bank0   │  Bank entries read by bank scan
0x68FFBFFF  ├──────────────────────┤
            │ KDP-0x1000..KDP      │  Negative-offset region (zeroed)
0x68FFDFE0  │   [KDP-0x20] = IRP  │  Info Record Page base pointer
0x68FFDFF0  │   [KDP-4] = KDP     │  Self-pointer
0x68FFE000  ├──────────────────────┤  ← KDP = SPRG0 = KernelDataAddr
            │   KDP (8 KB)         │  Per-CPU data page
            │   +0x638 = KMemBase  │
            │   +0x63C = KMemEnd   │
            │   +0x688 = PgAttrInit│
            │   +0x6B0 = (copy)    │
            │   +0x6B4 = VMMaxVPgs │
            │   +0x6C0 = PhysPgArr │
            │   +0x900 = WorkQueue │  Checked by idle-loop (0→no work)
            │   +0xC00 = SysInfo   │  Copied from GPR5 by Init.s
0x68FFFFFF  ├──────────────────────┤
0x69000000  │   HTAB (64 KB)       │  ← htab_base (SDR1), at KDP+0x2000
0x6900FFFF  └──────────────────────┘  ← KernelMemoryEnd (r13+r15)
```

### Negative-offset fields (below KDP)

| Offset from KDP | Guest Address | Value | Purpose |
|---|---|---|---|
| -0x04 | 0x68FFDFF0 | KDP (self-pointer) | `lwz r1, -4(r1)` chase pattern |
| -0x20 | 0x68FFDFE0 | IRP base (0x68FF4000) | Info Record Page — the skipped cold-init at 0x5031008C normally computes `KDP - 0xA000`. Bank scan reads bank entries at `IRP + 0xDF0..IRP + 0xEBC` (26 eight-byte {start,size} pairs). |
| -0x110 | 0x68FFDEF0 | saved LR | Context save area for interrupt handlers |
| -0x10C | 0x68FFDEF4 | saved CR | Context save area for interrupt handlers |
| -0x108..-0x80 | 0x68FFDEF8.. | saved r24-r31 | Context save area (stmw/lmw) |
| -0x900 | 0x68FFD700 | work queue | Idle loop at 0x50326880 checks this; 0 = no pending work |
| -0xAF0 | 0x68FFD510 | lock word | Spinlock; the first lock acquire at 0x50312700 |

## Wall Status (RESOLVED — 2026-06-08)

**All three walls in the free-list / page-table subsystem have been broken.** The fixes:

1. **IRP pointer `[KDP-0x20]`:** seeded in trampoline (`WriteMacInt32(kdp - 0x20, irp_base)`).
   Without this, bank scan reads guest low memory (all zeros) → no pages → `r22=0xFFFFFFFC`.
2. **`desc_create` ROM patch:** skipped under `g_rom_904_lenient` so the real `stwu r31,4(r29)`
   executes. Without this, the free-list store is NOPped → no descriptors written.
3. **Layout:** KernelMemoryBase lowered to `sub_kdp_base - pgdesc_size` (0x68FB0000) so the 256KB
   of descriptors (growing upward) fit below the sub-KDP pool.

**Result:** 65536 pages correctly added to free list, 568 compiled blocks, 153M blocks/s.
Nanokernel reaches idle loop at `0x5032751C` (waiting for `[KDP-0x900]` work queue).
Next wall is the PPC→68k handoff — see HANDOFF-NEWWORLD-SUPERVISOR-MMU.md.
