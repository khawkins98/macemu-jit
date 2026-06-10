# KDP (Kernel Data Page) Field Map

> **Research reference (2026-06-10).** Originally built for Path A (NW trampoline).
> Still actively useful — the **[Machine Layer](../MACHINE-LAYER-PLAN.md)** hits KDP
> fields during nanokernel boot (e.g. the KDP+0x6b4 ceiling fix in `d8932203`).

The NanoKernel's per-CPU data structure, addressed via SPRG0 (= r1 in kernel code).
Fields at positive offsets from KDP base. Source: `Init.s`, `Reset.s`, `Emulate.s`,
`Defines.s` from elliotnunn/NanoKernel disassembly, plus reverse-engineering of the
parcels 9.0.1 ROM's DR Emulator dispatch and 68k init path.

## Source Material

- **elliotnunn/OldKern** `KDP.h` — the most complete public field map (hex offsets,
  field names, types). Covers NanoKernel versions through ~9.2.
- **elliotnunn/OldKern** `InfoRecords.h` — `NKConfigurationInfo`, `NKSystemInfo`,
  `NKProcessorInfo` structs (the firmware→nanokernel handoff records).
- **NanoKernel source** (`Init.s`, `Reset.s`, `Defines.s`, `Emulate.s`) — field
  definitions and boot-time initialization flow.
- **Parcels 9.0.1 ROM** (cksum ec86128e) — DR Emulator cold-start dispatch
  (ROM+0x36e964), ECB init (ROM+0x36db94), decode loop (ROM+0x366080), interrupt
  check (ROM+0x36d114). Disassembled from `/tmp/rom_decompressed.bin`.

> **Cross-ROM applicability (confirmed 2026-06-10):** This field map applies to both the
> parcels 9.0.1 ROM and the 1998-07-21 v1.1 ROM. Both are ROMTYPE_NEWWORLD (type 5) and share
> the same NanoKernel architecture — the nanokernel ID string in both is "NewWorld". The 1.1
> ROM was previously misidentified as OldWorld; `rom_detect_type()` confirms it is NewWorld.
> The Path A trampoline work (SegMap spike, VIA spike, IRP seeding, free-list fixes) advanced
> the 1.1 ROM's NanoKernel boot to DR Emulator entry, confirming the KDP layout documented
> here is correct for both ROMs.

## Table of Contents

- [Master Field Inventory (0x000–0x920)](#master-field-inventory-0x0000x920)
  - [EWA / Exception Work Area (0x000–0x080)](#ewa--exception-work-area-0x0000x080)
  - [Segment Maps and BAT Ranges (0x080–0x360)](#segment-maps-and-bat-ranges-0x0800x360)
  - [FP Scratch and Instruction Emulation (0x5A0–0x5BC)](#fp-scratch-and-instruction-emulation-0x5a00x5bc)
  - [Address Space Records (0x5C8–0x5F0)](#address-space-records-0x5c80x5f0)
  - [KCall Dispatch Table (0x5F0–0x630)](#kcall-dispatch-table-0x5f00x630)
  - [Core Pointers and Configuration (0x630–0x660)](#core-pointers-and-configuration-0x6300x660)
  - [Scheduler, Flags, and Interrupt Masking (0x660–0x680)](#scheduler-flags-and-interrupt-masking-0x6600x680)
  - [Memory Management: PageMap, HTAB, VM (0x684–0x6C0)](#memory-management-pagemap-htab-vm-0x6840x6c0)
  - [Power Management (0x6B8–0x6B9)](#power-management-0x6b80x6b9)
  - [Physical Page Array (0x6C0–0x700)](#physical-page-array-0x6c00x700)
  - [Miscellaneous / Late-Init Fields (0x900–0x920)](#miscellaneous--late-init-fields-0x9000x920)
- [Negative-Offset Fields (below KDP)](#negative-offset-fields-below-kdp)
  - [SPRG0 per-task frame (negative offsets from per-task frame pointer)](#sprg0-per-task-frame-negative-offsets-from-per-task-frame-pointer)
- [Fields Read by Reset.s (consumer digest)](#fields-read-by-resetsmust-be-set-by-inits)
- [Fields Set by InitEmulation (Emulate.s)](#fields-set-by-initemulation-emulates)
- [Fields NOT Set by Init/Reset](#fields-not-set-by-initreset-set-at-runtime-or-by-trampoline)
- [Critical Fields for Path A Trampoline Seeding](#critical-fields-for-path-a-trampoline-seeding-0x5000x700)
- [Memory Layout (SheepShaver NW Trampoline)](#memory-layout-sheepshaver-nw-trampoline)
- [DR Emulator Dispatch Fields](#dr-emulator-dispatch-fields)
- [ECB (EmulatorData / EDP) Structure](#ecb-emulatordata--edp-structure-kdp0x1000)
- [NEWWORLD-Specific KDP Fields (main.cpp)](#newworld-specific-kdp-fields-maincpp)
- [Trampoline Seeding Summary (Path B)](#trampoline-seeding-summary-ss_nw_synth_entry-path)
- [Wall Status](#wall-status-updated--2026-06-10)

---

## Master Field Inventory (0x000–0x920)

r1 = KDP throughout. rCI = r3 (ConfigInfo ptr). rED = r8 (EDP = KDP + 0x1000).
r13 = KernelMemoryBase. r14 = HTAB mask. r15 = total kernel memory size.

### EWA / Exception Work Area (0x000–0x080)

| Offset | Name | Init.s line | Source register | Value comes from | Purpose |
|--------|------|-------------|-----------------|------------------|---------|
| 0x000:0x080 | EWA (r0–r31) | — | — | Runtime (exception entry) | Scratch save area for exception handlers |

### Segment Maps and BAT Ranges (0x080–0x360)

| Offset | Name | Init.s line | Source register | Value comes from | Purpose |
|--------|------|-------------|-----------------|------------------|---------|
| 0x080:0x280 | SegMaps (4 x 16 entries) | — | — | Reset.s:ResetSegMaps (bulk copy from ConfigInfo) | Segment map arrays |
| **0x080:0x100** | **SegMap32SupInit** | — | — | Reset.s:ResetSegMaps → `addi r27, r1, 0x78; lwzu r25, 8(r27)` pattern | 16 segments × 8 bytes (ptr, flags). Pointers to PMDT chains. **Spike-verified (2026-06-09):** corrupted during page-init loop — the page-init loop overwrites the value NKInit copied (`0x68FFE920` → `0x0000FFFF`) via an indirect store. CreateAreasFromPageMap reads these via `addi r27, r1, 0x78; lwzu r25, 8(r27)`. Our SegMap spike writes fresh data at ROM+0x30d600 immediately before CAFPM, bypassing the corruption. |
| 0x280:0x300 | BatRanges | — | — | Reset.s:ResetBatRanges (from ConfigInfo) | BAT range entries |
| 0x300:0x340 | CurIBAT/CurDBAT | — | — | Runtime (SetSpace) | Current BAT register shadows |
| 0x340:0x360 | NCBPointerCache | — | — | Reset.s:161 (`_clrNCBCache`), zeroed | Non-cacheable block pointer cache |

### FP Scratch and Instruction Emulation (0x5A0–0x5BC)

| Offset | Name | Init.s line | Source register | Value comes from | Read by Reset.s? | Purpose |
|--------|------|-------------|-----------------|------------------|-------------------|---------|
| 0x5A0:0x5A8 | FloatScratch | — (zeroed) | r0 | zero (bulk wipe) | no | FP scratch area. **Note:** at dispatch time, the trampoline repurposes +0x5A0 as a context pointer (see [DR Emulator Dispatch Fields](#dr-emulator-dispatch-fields)). |
| 0x5A8:0x5B0 | (reserved padding) | — (zeroed) | r0 | zero (bulk wipe) | no | Gap between FloatScratch and IntHandlerPtr; no named field; wall-dump values are leftover noise |
| 0x5B0 | IntHandlerPtr | 200 | r7 | `GetExtIntHandler` return (line 196) | yes (line 49): loaded → VecTblSystem.External | External interrupt handler address |
| 0x5B4 | NatContextPtrLogical | — | — | SoftInts.s:116 | no | Set when alternate (native) context is created |
| 0x5B8 | InstEmControl | 78 (Emulate.s) | r23 | Probed perf-monitor SPR flags (EmAlways1\|2 +/- EmHasMMCR0/1/SDA) | no (runtime only) | Instruction emulation control flags |
| 0x5B8 byte 0 | InstEmControl (high byte) | 113 (Emulate.s) | — | TB shift exponent | no | Left-shift count for TB→RTC |
| 0x5BB byte 3 | InstEmControl+3 | 116 (Emulate.s) | — | `32 - exponent` | no | Right-shift count for TB→RTC |
| 0x5BC | InstEmTimebaseScale | 112 (Emulate.s) | r24 | Long-division: 0x80587ff3_d62611e3 / DecClockRateHz | no (runtime only) | TB→RTC nanosecond scaling factor |

### Address Space Records (0x5C8–0x5F0)

| Offset | Name | Init.s line | Source register | Value comes from | Read by Reset.s? | Purpose |
|--------|------|-------------|-----------------|------------------|-------------------|---------|
| 0x5C8 | SupervisorSpace.SegMapPtr | Reset 268 | r23 | `r1 + KDP.SupervisorSegMap` (computed) | — (set in Reset) | Supervisor address space record |
| 0x5CC | SupervisorSpace.BatMap | Reset 270 | r23 | `ConfigInfo.BatMap32SupInit` | — | BAT mapping for supervisor |
| 0x5D0 | UserSpace.SegMapPtr | Reset 273 | r23 | `r1 + KDP.UserSegMap` | — | User address space record |
| 0x5D4 | UserSpace.BatMap | Reset 275 | r23 | `ConfigInfo.BatMap32UsrInit` | — | BAT mapping for user |
| 0x5D8 | CpuSpace.SegMapPtr | Reset 278 | r23 | `r1 + KDP.CpuSegMap` | — | CPU address space record |
| 0x5DC | CpuSpace.BatMap | Reset 280 | r23 | `ConfigInfo.BatMap32CPUInit` | — | BAT mapping for CPU |
| 0x5E0 | OverlaySpace.SegMapPtr | Reset 283 | r23 | `r1 + KDP.OverlaySegMap` | — | Overlay address space record |
| 0x5E4 | OverlaySpace.BatMap | Reset 285 | r23 | `ConfigInfo.BatMap32OvlInit` | — | BAT mapping for overlay |
| 0x5E8:0x5F0 | CurSpace | — | — | Runtime (SetSpace) | no | Current address space |

### KCall Dispatch Table (0x5F0–0x630)

| Offset | Name | Init.s line | Source register | Value comes from | Read by Reset.s? | Purpose |
|--------|------|-------------|-----------------|------------------|-------------------|---------|
| 0x5F0:0x630 | KCallTbl | Reset 97–133 | r23 | `_kaddr` of each KCall handler | — | Trap dispatch table (16 entries) |

### Core Pointers and Configuration (0x630–0x660)

| Offset | Name | Init.s line | Source register | Value comes from | Read by Reset.s? | Purpose |
|--------|------|-------------|-----------------|------------------|-------------------|---------|
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
| 0x65C | ContextPtr | 238 | r12 | Same as SysContextPtr (initial) | no | Currently active ContextBlock. **Also read by DR Emulator at 0x5036f920**: `lwz r6, 0x65c(r1)` — context block for 68k emulator state. Uninitialized at current wall → crash at guest PC 0x00000000. |

### Scheduler, Flags, and Interrupt Masking (0x660–0x680)

| Offset | Name | Init.s line | Source register | Value comes from | Read by Reset.s? | Purpose |
|--------|------|-------------|-----------------|------------------|-------------------|---------|
| 0x660 | Flags | 396 | r7 | `GlobalFlagSystem \| MQ-probe result` | no | NanoKernel flags (GlobalFlagSystem + MQ detect) |
| 0x664 | Enables | — (zeroed) | r0 | zero (bulk wipe) | no (runtime: Exceptions.s, RTAS.s) | Runtime enable flags |
| 0x668 | OtherContextDEC | 420 | r8 | `ProcInfo.DecClockRateHz` (1 second of ticks) | no | Decrementer ticks for inactive context |
| 0x670 | TestIntMask | 241 | r12 | `ConfigInfo.TestIntMaskInit` | no | CR mask for testing pending interrupt |
| 0x674 | PostIntMask | 245 | r12 | `ConfigInfo.PostIntMaskInit` | no | CR flags set when posting interrupt |
| 0x678 | ClearIntMask | 243 | r12 | `ConfigInfo.ClearIntMaskInit` | no | CR mask for clearing interrupt |
| 0x67C | EmuIntLevelPtr | 249 | r12 | `rED + ConfigInfo.IplValueOffset` | no | Physical ptr to emulator IPL global |
| 0x680 | DebugIntPtr | 253 | r12 | `ConfigInfo.SharedMemoryAddr + 0x7C` | no | Debug shared memory pointer |

### Memory Management: PageMap, HTAB, VM (0x684–0x6C0)

| Offset | Name | Init.s line | Source register | Value comes from | Read by Reset.s? | Purpose |
|--------|------|-------------|-----------------|------------------|-------------------|---------|
| 0x66C | PageMapFreePtr | 262 | r13 | `PageMapPtr + ConfigInfo.PageMapInitSize` | no | First free byte in PageMap |
| 0x684 | PageMapPtr / PA_PageMapStart ptr | 260 | r13 | `r1 + KDP.PageMap` (= KDP + 0x920) — set at 0x50310874: `addi r13, r1, 0x920; stw r13, 0x684(r1)` | yes (line 11): loaded into rPgMap=r18 | Points to KDP+0x920. Start of PageMap buffer. |
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

### Power Management (0x6B8–0x6B9)

| Offset | Name | Init.s line | Source register | Value comes from | Read by Reset.s? | Purpose |
|--------|------|-------------|-----------------|------------------|-------------------|---------|
| 0x6B8 | PowerHID0Select | Reset 145 | r23 | `HID0SelectTable[PVR>>16]` (lookup table) | no | HID0 power mode selector |
| 0x6B9 | PowerHID0Enable | Reset 147 | r23 | `HID0EnableTable[PVR>>16]` (lookup table) | no | HID0 power enable mode |

### Physical Page Array (0x6C0–0x700)

| Offset | Name | Init.s line | Source register | Value comes from | Read by Reset.s? | Purpose |
|--------|------|-------------|-----------------|------------------|-------------------|---------|
| 0x6C0:0x700 | **PhysicalPageArray[16]** | Reset 389 | r21 | Per-segment ptrs into 68k PD array (4 entries used, incremented by 0x40000) | no | Per-segment 68k Page Descriptor pointers |

### Miscellaneous / Late-Init Fields (0x900–0x920)

| Offset | Name | Init.s line | Source register | Value comes from | Purpose |
|--------|------|-------------|-----------------|------------------|---------|
| 0x908 | RTASDispatch | Init.s:153/165 | r8 | From `r8` if RTAS present, else 0 | RTAS dispatch entry point |
| 0x90C | RTASData | Init.s:155/166 | — | From `HWInfo.RTAS_PrivDataArea` if RTAS, else 0 | RTAS private data area |
| **0x910** | **(scratch / possible NKPublic field)** | Init.s:264 | — | Zeroed for backward compat in Init.s | **Used by our SegMap spike stub** to save/restore LR (spike saves LR here, does bl, restores). May be a real NKPublic-exported field — verify against `OldKern/KDP.h` before using in production code. |
| **0x920** | **PA_PageMapStart (PageMap buffer start)** | Init.s:260 / 262 | r13 | `r1 + 0x920` (KDP + 0x920), also stored to `KDP+0x684` | Start of PMDT data / PageMap buffer. Set at 0x50310874: `addi r13, r1, 0x920; stw r13, 0x684(r1)`. Our spike writes PMDT entries (PMDT[0] = 256MB RAM area, PMDT[1] = sentinel) here before CreateAreasFromPageMap runs. |
| **0xedc** | **NanoKernelInfo.ConfigFlags** | — (runtime) | — | Runtime | Read/written by idle loop exit path at 0x50327540: `ori r8, r8, 2` sets "work was dispatched" flag. |

---

## Negative-Offset Fields (below KDP)

| Offset from KDP | Guest Address | Value | Purpose |
|---|---|---|---|
| -0x04 | 0x68FFDFF0 | KDP (self-pointer) | `lwz r1, -4(r1)` chase pattern |
| -0x20 | 0x68FFDFE0 | IRP base (0x68FF4000) | Info Record Page — the skipped cold-init at 0x5031008C normally computes `KDP - 0xA000`. Bank scan reads bank entries at `IRP + 0xDF0..IRP + 0xEBC` (26 eight-byte {start,size} pairs). |
| -0x110 | 0x68FFDEF0 | saved LR | Context save area for interrupt handlers. Also used as saved outer r1 in SPRG0-relative per-task frame (see SPRG0 per-task offsets below). |
| -0x10C | 0x68FFDEF4 | saved CR | Context save area for interrupt handlers |
| -0x108..-0x80 | 0x68FFDEF8.. | saved r24-r31 | Context save area (stmw/lmw). SPRG0 per-task frame: `-0x108..-0x108+32` = saved r24-r31 (stmw/lmw frame). |
| **-0x340** | 0x68FFDC60 | current task context ptr | CAS sentinel in dequeue at 0x50312700 |
| **-0x8fc** | 0x68FFD704 | (cleared field) | Zeroed during extended idle processing |
| -0x900 | 0x68FFD700 | VIA base / NoIdeaR23 | **Spike-verified (2026-06-09):** Physical base address of the 6522 VIA chip. Read by SchIdleTask's `check_work` at 0x50326880. If null, check_work returns -1 and idle loop spins. Nanokernel exits idle, VIA interrupt handler runs, scheduler dispatch reaches DR Emulator entry when populated with a fake VIA page at 0x68FAF000. Thud console (0x503263fc) also checks this. Seeded by SS_NW_TRAMPOLINE spike. |
| -0x964 | 0x68FFD69C | target MSR (SRR1) | Dispatch routine at 0x503126B4 reads this for `mtspr SRR1` before `rfi` into the DR Emulator. Trampoline seeds `0x0000D032`. |
| **-0xAF0** | 0x68FFD510 | RTASLock / work-queue head | Spinlock / pending-work check at 0x503265ac. CAS target for task dequeue at 0x50312700. |

### SPRG0 per-task frame (negative offsets from per-task frame pointer)

These offsets are relative to the **SPRG0-relative per-task frame** (the stack pointer of a
suspended task, not the static KDP). Observed in the scheduler save/restore path around
0x503265cc.

| Offset from frame | Purpose |
|---|---|
| -4 | Saved outer r1 (stack pointer of interrupted context) |
| -0x110 | Saved LR (restored at 0x503265cc) |
| -0x10c | Saved CR (restored at 0x503265cc) |
| -0x108..-0x108+32 | Saved r24-r31 (stmw/lmw frame) |
| -0x340 | Current task context ptr (CAS sentinel in dequeue at 0x50312700) |
| -0x8fc | (cleared field) — zeroed during extended idle processing |

---

## Fields Read by Reset.s (must be set by Init.s)

This is a **consumer digest** — the subset of KDP fields that Reset.s depends on having been
already initialized by Init.s. For the full field descriptions, see the
[Master Field Inventory](#master-field-inventory-0x0000x920).

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

---

## Fields Set by InitEmulation (Emulate.s)

Called from Reset.s line 419. These are set at boot by probing the processor's SPR
capabilities and computing timebase scaling. Byte-level sub-field detail is included
because the control word packs multiple values.

| Offset | Name | Emulate.s line | Source | Purpose |
|--------|------|---------------|--------|---------|
| 0x5B8 | InstEmControl | 78 | Probed SPR availability flags | Which instructions to emulate |
| 0x5B8 byte 0 | InstEmControl (high byte) | 113 | TB shift exponent | Left-shift count for TB→RTC |
| 0x5BB byte 3 | InstEmControl+3 | 116 | `32 - exponent` | Right-shift count for TB→RTC |
| 0x5BC | InstEmTimebaseScale | 112 | Long-division quotient | Timebase scaling mantissa |

---

## Fields NOT Set by Init/Reset (set at runtime or by Trampoline)

These fields appear in Defines.s but are not initialized during boot — they are either
zeroed by the bulk wipe, set at runtime, or must be seeded by the Trampoline. Fields
already listed in the master inventory above are included here to mark their
initialization path explicitly.

| Offset | Name | Where set | Notes |
|--------|------|-----------|-------|
| 0x000:0x080 | EWA (r0–r31) | Runtime (exception entry) | Scratch save area |
| 0x080:0x280 | SegMaps (4 x 16 entries) | Reset.s:ResetSegMaps (bulk copy from ConfigInfo) | Segment map arrays |
| 0x280:0x300 | BatRanges | Reset.s:ResetBatRanges (from ConfigInfo) | BAT range entries |
| 0x300:0x340 | CurIBAT/CurDBAT | Runtime (SetSpace) | Current BAT register shadows |
| 0x340:0x360 | NCBPointerCache | Reset.s:161 (`_clrNCBCache`) | Zeroed |
| 0x5B4 | NatContextPtrLogical | SoftInts.s:116 | Set when alternate (native) context is created |
| 0x5E8:0x5F0 | CurSpace | Runtime (SetSpace) | Current address space |
| 0x908 | RTASDispatch | Init.s:153/165 | From `r8` if RTAS present, else 0 |
| 0x90C | RTASData | Init.s:155/166 | From `HWInfo.RTAS_PrivDataArea` if RTAS, else 0 |
| 0x910 | (legacy PageMap word) | Init.s:264 | Zeroed for backward compat |

---

## Critical Fields for Path A Trampoline Seeding (0x500–0x700)

These are the fields that **must** be correctly populated for the NanoKernel's Reset flow
to succeed (Path A = SS_NW_TRAMPOLINE, full nanokernel boot). Init.s sets them; the
trampoline must replicate. For Path B (SS_NW_SYNTH_ENTRY, skip nanokernel), see the
[Trampoline Seeding Summary](#trampoline-seeding-summary-ss_nw_synth_entry-path) section below.

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

0x10000000  +----------------------+
            |   Guest RAM (256MB)  |  Pages here go into the free list
            |                      |  (excluding kernel memory range)
0x1FFFFFFF  +----------------------+

0x50000000  +----------------------+
            |   ROM (5 MB)         |  NanoKernel code lives here
0x504FFFFF  +----------------------+

0x68FB0000  +----------------------+  <- KernelMemoryBase (r13)
            | Page descriptors     |  Free list grows UPWARD via stwu
            |  (256 KB, 65536 pgs) |  rom_patches.cpp `desc_create` skip
0x68FF0000  +----------------------+
            |   (safety gap 16KB)  |
0x68FF4000  +----------------------+  <- sub_kdp_base = IRP base
            | sub-KDP pool (32 KB) |  Pool/heap for spinlocks etc.
            |  IRP+0xDF0 = bank0   |  Bank entries read by bank scan
0x68FFBFFF  +----------------------+
            | KDP-0x1000..KDP      |  Negative-offset region (zeroed)
0x68FFDFE0  |   [KDP-0x20] = IRP  |  Info Record Page base pointer
0x68FFDFF0  |   [KDP-4] = KDP     |  Self-pointer
0x68FFE000  +----------------------+  <- KDP = SPRG0 = KernelDataAddr
            |   KDP (8 KB)         |  Per-CPU data page
            |   +0x638 = KMemBase  |
            |   +0x63C = KMemEnd   |
            |   +0x688 = PgAttrInit|
            |   +0x6B0 = (copy)    |
            |   +0x6B4 = VMMaxVPgs |
            |   +0x6C0 = PhysPgArr |
            |   +0x900 = WorkQueue |  Checked by idle-loop (0->no work)
            |   +0xC00 = SysInfo   |  Copied from GPR5 by Init.s
0x68FFFFFF  +----------------------+
0x69000000  |   HTAB (64 KB)       |  <- htab_base (SDR1), at KDP+0x2000
0x6900FFFF  +----------------------+  <- KernelMemoryEnd (r13+r15)
```

---

## DR Emulator Dispatch Fields

These KDP fields are read by the nanokernel dispatch routine (0x503126B4) to hand off
execution to the DR (Dynamic Recompilation) Emulator — the PPC code that interprets
68k instructions. Source: disassembly of parcels 9.0.1 ROM + trampoline code in
`sheepshaver_glue.cpp`.

### Nanokernel -> DR Emulator handoff (dispatch routine at 0x503126B4)

```
lwz  r8, 0x5a0(r1)   -> mtspr SPRG0       ; context pointer
lwz  r8, 0x5a4(r1)   -> addi +0x26e8      ; code_base -> entry = code_base + 0x26e8
                        -> mtspr SRR0       ; 68k emulator entry point
lwz  r4, 0x648(r1)                        ; opcode table base (handler mirror region)
lwz  r9, -0x964(r1)  -> mtspr SRR1        ; target MSR
rfi                                        ; enters DR Emulator
```

| Offset | Name | Seeded by | Value | Purpose |
|--------|------|-----------|-------|---------|
| +0x5A0 | ContextPtr (dispatch) | Trampoline P8 | KDP (0x68FFE000) | Context pointer loaded into SPRG0 at dispatch |
| +0x5A4 | CodeBase (DR Emulator) | Trampoline P8 | 0x5046D218 | DR Emulator code base in ROM mirror region; entry = base + 0x26E8 = 0x5046F900 |
| +0x648 | EmuTrapTableLogical | Trampoline P8/P9 | ROMBase + 0x480000 | Base address of 68k opcode handler table (mirror region 0x50480000) |
| -0x900 | WorkQueue | Trampoline P8 | 1 (non-zero) | Non-zero triggers dispatch; 0 = idle loop spins |
| -0x964 | TargetMSR (SRR1) | Trampoline P9 | 0x0000D032 | MSR value for rfi into emulator (supervisor, IR/DR on) |

### DR Emulator register convention (at entry and during decode loop)

| Register | Role | Set by |
|----------|------|--------|
| r1 | KDP (from SPRG0) | nanokernel dispatch |
| r23 | Interrupt handler address (CTR target) | Cold-start: `li r23, 0` (**NULL -- see note**) |
| r24 | 68k PC (guest program counter) | Cold-start loads from guest[4] = ROMBase+0x2A |
| r25 | Address mask / flags | Cold-start: `li r25, 0x27` |
| r26 | (cleared) | Cold-start: 0 |
| r27 | Prefetched 68k opcode | `lhau r27, 2(r24)` in decode loop |
| r28 | (cleared) | Cold-start: 0 |
| r29 | Handler address (computed) | `rlwimi r29, r27, 3, 0xD, 0x1C` into base 0x50480000 |
| r30 | ROM handler source base | 0x50360000 (set by ROM patch at 0x310000) |
| r31 | ECB (EmulatorData) pointer | KDP + 0x1000 (set by ROM patch: `addi r31, r1, 0x1000`) |

> **r23=0 observation:** The cold-start dispatch at ROM+0x36e964 sets `li r23, 0`.
> In the normal (warm) boot path, r23 would hold a valid interrupt handler address.
> The interrupt check path at 0x5036D114 loads `r5 = ECB+0x814`, branches to it via
> `blrl`, then uses various CR-based dispatch. If any path reaches `bgtctr cr1` (or
> similar CTR-based interrupt dispatch) while CTR still holds the handler address from
> r29 (the last mtctr), this is safe. But if r23 were loaded into CTR as an interrupt
> handler and is 0, execution would jump to PPC address 0 -- catastrophic.
> **Status:** Under investigation as the root cause of non-deterministic r24 corruption
> during 68k init. See NEW-WORLD-ROM-SUPPORT-PLAN.md Phase 2.

### 68k decode loop (ROM+0x366080 -- standard variant)

```
0x50366080  lhau    r27, 2(r24)                 ; fetch next 68k opcode, advance PC
0x50366084  rlwimi  r29, r27, 3, 0xD, 0x1C      ; compute handler: rotl(opcode,3) & 0x7FFF8 -> r29 base
0x50366088  mtlr    r29                          ; handler address -> LR
0x5036608C  lhau    r27, 2(r24)                  ; prefetch next opcode
0x50366090  bgelr   cr2                          ; dispatch if CR2.LT=0 (normal case)
0x50366094  b       0x5036D114                   ; interrupt pending -> interrupt check path
```

Handler address computation: `r29 = (r29 & ~0x7FFF8) | (rotl(opcode, 3) & 0x7FFF8)`.
Base is 0x50480000 (mirror region). `opcode >> 6` selects the 8-byte-aligned handler
entry. Mirror region = ROM[0x300000..0x400000] copied to ROM[0x400000..0x500000],
so handler at 0x504XXXXX has source at 0x503XXXXX.

CR2 controls dispatch: CR2.LT=0 -> normal dispatch (bgelr), CR2.LT=1 -> interrupt
pending (fall through to 0x5036D114). Cold-start sets `crset cr2un` (CR2.SO=1);
all other CR2 bits are cleared.

### Cold-start dispatch (ROM+0x36E964) -- full sequence

```
li r0, 0                      ; zero register
li r23, 0                     ; interrupt handler = NULL (!)
mtcrf 0x7F, r0                ; clear CR1-CR7
bl 0x5036DB94                 ; -> ECB init (also clears CR1 explicitly)
li r8..r22, 0                 ; zero data registers
li r24, 0                     ; zero 68k PC temporarily
lwz r1, 0(r24)               ; guest[0] = initial 68k SP (= 0)
lwz r24, 4(r24)              ; guest[4] = 68k reset PC (= ROMBase + 0x2A)
li r28, 0
stw r28, 0x54(r31)           ; clear ECB+0x54
li r25, 0x27                  ; address mask
crset cr2un                   ; CR2.SO=1 (enables bgelr dispatch)
li r26, 0
mtcrf 0x80, r0                ; clear CR0
mtxer r0                      ; clear XER
lha r27, 0(r24)              ; fetch first 68k opcode
rlwimi r29, r27, 3, 0xD, 0x1C; compute handler address
mtctr r29                     ; first instruction uses CTR (not LR)
lhau r27, 2(r24)             ; prefetch next opcode
bctr                          ; -> first 68k handler
```

---

## ECB (EmulatorData / EDP) Structure (KDP+0x1000)

The EmulatorData block (also called EDP or ECB) occupies KDP+0x1000..KDP+0x1FFF
(0x400 uint32 words = 4KB). `r31 = KDP + 0x1000` throughout the DR Emulator.
`cpu_emulation.h` defines it as `struct EmulatorData { uint32 v[0x400]; }` inside
`struct KernelData`.

### ECB fields initialized by ECB init (ROM+0x36DB94)

Called by cold-start dispatch via `bl 0x5036DB94`. r31=ECB, r30=0x50360000, r0=0.

| ECB Offset | KDP Offset | Value | Purpose |
|------------|-----------|-------|---------|
| +0x054 | +0x1054 | 0 | Cleared by cold-start (`stw r28, 0x54(r31)`) |
| +0x1C8 | +0x11C8 | 0xAE0 | Set by ECB init continuation |
| +0x1D8 | +0x11D8 | (checked) | Conditional move to +0x4D8 if non-zero |
| +0x400..0x6FC | +0x1400..0x16FC | 0 | Bulk zeroed (0x60 iterations x 8 bytes from +0x700 downward) |
| +0x4D8 | +0x14D8 | (preserved) | Saved before bulk zero, restored after |
| +0x700 | +0x1700 | -> 0x5036DE08 | Error/default handler |
| +0x708 | +0x1708 | ECB+0x740 | Self-relative pointer (68k stack frame?) |
| +0x70C | +0x170C | -> 0x5036DE08 | Error/default handler |
| +0x710 | +0x1710 | stack ptr | Set after bulk zero (stack base) |
| +0x714 | +0x1714 | -> 0x5036DE08 | Error/default handler |
| +0x718 | +0x1718 | 0x21F8 | Constant (possibly buffer size or limit) |
| +0x720..72C | +0x1720..172C | 0 | Four zeroed words |
| +0x730..73C | +0x1730..173C | 0 | Four zeroed words |
| +0x7A0 | +0x17A0 | 0x80000039 | Status/mode flags (bit 31 set + 0x39) |
| +0x7A4 | +0x17A4 | -> 0x5036E48C | Handler address |
| +0x7A8 | +0x17A8 | 6 | Counter/index |
| +0x7AC | +0x17AC | 0x907 | Constant |
| +0x7B0 | +0x17B0 | 0xE00 | Constant (address space size?) |
| +0x7B4 | +0x17B4 | 0x400 | Constant (page size?) |
| **+0x800..0xA5C** | **+0x1800..0x1A5C** | **151 handler ptrs** | **68k opcode handler table** -- each entry = `r30 \| halfword` from ROM+0x3DC42 lookup table. 151 entries x 4 bytes. |
| +0xA14 | +0x1A14 | ECB+0xB00 \| 0x81AC | Derived handler address |
| +0xA18 | +0x1A18 | ECB+0xB00 \| 0x81A8 | Derived handler address |
| +0xB00 | +0x1B00 | 0x50370000 | r30 + 0x10000 (ROM handler extended base) |

### ECB handler pointer table (ECB+0x800..0xA5C)

Built by the ECB init loop at ROM+0x36DC0C:
```
r5 = r30 | 0xDC42   ; halfword table at ROM+0x3DC42
r3 = r31 + 0x7FC     ; destination = ECB+0x7FC (pre-decrement -> ECB+0x800)
CTR = 0x97 (= 151)   ; 151 entries
loop:
  lhzu r4, 2(r5)     ; read halfword offset from table
  or   r4, r4, r30   ; -> full ROM address (r30 = 0x50360000)
  stwu r4, 4(r3)     ; store to handler table, advance
  bdnz loop
```

Known handler entries (**probe-verified** from SS_PROBE_PC observations):

| ECB Offset | Handler addr | Source (mirror->src) | 68k instruction |
|------------|-------------|--------------------|----|
| +0x800 | 0x5036D780 | ROM+0x36D780 | (default / entry 0) |
| +0x804 | 0x5036D780 | ROM+0x36D780 | (entry 1) |
| +0x814 | 0x5036D780 | ROM+0x36D780 | Interrupt check loads ECB+0x814 |
| +0x818 | 0x5036C410 | ROM+0x36C410 | MOVE-to-SR related |
| +0x87C | 0x5036C714 | ROM+0x36C714 | MOVEC handler |

---

## NEWWORLD-Specific KDP Fields (main.cpp)

These fields are set by `main.cpp` for `ROMTYPE_NEWWORLD` only, after the
`Mac_memset(KERNEL_DATA_BASE, 0, sizeof(KernelData))` wipe. Not part of the
NanoKernel's Init.s/Reset.s flow -- they are SheepShaver's emulation of what
Open Firmware / BootX would provide on real hardware.

| KDP Offset | Value | Purpose |
|-----------|-------|---------|
| +0xB80 | ROMBase | ROM base address (first word after 0x3D fill) |
| +0xB80..0xBFF | 0x3D fill (128 bytes) then selective writes | OF device tree / vector tables |
| +0xB84 | of_dev_tree | Open Firmware device tree base |
| +0xB90 | vector_lookup_tbl | Interrupt vector lookup table |
| +0xB94 | vector_mask_tbl | Interrupt vector mask table |
| +0xB98 | ROMBase | OpenPIC base address |
| +0xBB0 | 0 | ADB base |
| +0xC20 | RAMSize | Physical RAM size |
| +0xC24 | RAMSize | (duplicate) |
| +0xC30 | RAMSize | (duplicate) |
| +0xC34 | RAMSize | (duplicate) |
| +0xC38 | 0x00010020 | Memory controller config |
| +0xC3C | 0x00200001 | Memory controller config |
| +0xC40 | 0x00010000 | Memory controller config |
| +0xC50 | RAMBase | RAM base address |
| +0xC54 | RAMSize | RAM size (bank 0) |
| +0xF60 | PVR | Processor Version Register |
| +0xF64 | CPUClockSpeed | clock-frequency |
| +0xF68 | BusClockSpeed | bus-frequency |
| +0xF6C | TimebaseSpeed | timebase-frequency |

### GOSSAMER-specific fields (for comparison)

| KDP Offset | Value | Purpose |
|-----------|-------|---------|
| +0xC80 | RAMSize | Physical RAM size |
| +0xC84 | RAMSize | (duplicate) |
| +0xC90 | RAMSize | (duplicate) |
| +0xC94 | RAMSize | (duplicate) |

---

## Trampoline Seeding Summary (SS_NW_SYNTH_ENTRY path)

All KDP/ECB fields written by the trampoline in `sheepshaver_glue.cpp` for the
Path B (SS_NW_SYNTH_ENTRY) synthetic entry that skips the nanokernel and enters
the DR Emulator directly.

| Phase | Offset | Name | Value | Notes |
|-------|--------|------|-------|-------|
| P6 | IRP+0x000 | NKSystemInfo.PhysicalMemorySize | ram_size | Via GPR(5) -> Init.s copy |
| P6 | IRP+0x030 | Bank0Start | RAMBase | |
| P6 | IRP+0x034 | Bank0Size | ram_size | |
| P7 | IRP+0xDF0 | bank0.start | RAMBase | Reset.s bank scan source |
| P7 | IRP+0xDF4 | bank0.size | ram_size | |
| P7 | KDP-0x04 | self-pointer | KDP | `lwz r1, -4(r1)` |
| P7 | KDP-0x20 | IRP base | 0x68FF4000 | Bank scan anchor |
| P8 | KDP+0x5A0 | dispatch ctx | KDP | mtspr SPRG0 at dispatch |
| P8 | KDP+0x5A4 | code_base | 0x5046D218 | DR Emulator base (mirror) |
| P8 | KDP-0x900 | work queue | 1 | Triggers dispatch (non-zero) |
| P9 | KDP+0x65C | ContextPtr | ECB (KDP+0x1000) | Active context block |
| P9 | KDP+0x660 | Flags | 0 | NanoKernel flags (cleared) |
| P9 | KDP+0x5F0 | KCallTbl[0] | ROMBase+0x366080 | Decode loop entry (KCall handler) |
| P9 | KDP+0x5F4 | KCallTbl[1] | ROMBase+0x366080 | Decode loop entry (duplicate) |
| P9 | KDP+0x648 | EmuTrapTableLogical | ROMBase+0x480000 | Opcode handler table base |
| P9 | KDP-0x964 | target MSR | 0x0000D032 | SRR1 for rfi |
| P9 | guest[0] | 68k SP | 0 | Overwritten by 68k code |
| P9 | guest[4] | 68k reset PC | ROMBase+0x2A | 68k cold-start vector |

---

## Wall Status (Updated -- 2026-06-10)

### Resolved (2026-06-08): Free-list / page-table subsystem

**All three walls in the free-list / page-table subsystem have been broken.** The fixes:

1. **IRP pointer `[KDP-0x20]`:** seeded in trampoline (`WriteMacInt32(kdp - 0x20, irp_base)`).
   Without this, bank scan reads guest low memory (all zeros) -> no pages -> `r22=0xFFFFFFFC`.
2. **`desc_create` ROM patch:** skipped under `g_rom_904_lenient` so the real `stwu r31,4(r29)`
   executes. Without this, the free-list store is NOPped -> no descriptors written.
3. **Layout:** KernelMemoryBase lowered to `sub_kdp_base - pgdesc_size` (0x68FB0000) so the 256KB
   of descriptors (growing upward) fit below the sub-KDP pool.

**Result:** 65536 pages correctly added to free list, 568 compiled blocks, 153M blocks/s.

### Resolved (2026-06-09): SegMap/CreateAreasFromPageMap + idle loop / VIA

4. **SegMap corruption (CreateAreasFromPageMap):** page-init loop corrupts `KDP+0x80` (SegMap32SupInit
   pointers). Fixed by a 34-instruction PPC stub at ROM+0x30d600 that writes minimal SegMap + PMDT
   data immediately before both `bl CreateAreasFromPageMap` call sites (0x3124e4, 0x312568).
   CAFPM processes the data (PC 0x5031f530 reached), boot advances 451 -> 568 compiled blocks.

5. **Idle loop / VIA base (`[KDP-0x900]`):** nanokernel's `check_work` reads `[KDP-0x900]` as the
   VIA base address. If null, returns -1 and spins forever. Spike fix: populate with a fake VIA page
   at 0x68FAF000, env-gated on `SS_NW_TRAMPOLINE`. Nanokernel exits idle, VIA interrupt handler runs,
   scheduler dispatch reaches DR Emulator.

### ⏸ PARKED (2026-06-09): Path A — DR Emulator entry (0x5046e8c0)

Nanokernel dispatch at 0x503126b4 does `rfi` to DR Emulator entry at 0x5046f900 (ROM mirror).
DR Emulator reads low-memory globals: ECB ptr from 0x2804, counter at 0x2818, context from
`KDP+0x65c`. All uninitialized -> crash at guest PC 0x00000000. Root cause: `patch_68k` /
`jump68k` diagnostically skipped (`SS_ROM_SKIP_JUMP68K`). The NW ROM's jump68k signature differs
from OldWorld (the 1.1 byte pattern is absent). **Character change**: no longer a supervisor-memory
problem — this is the PPC->68k emulator boundary, requiring 68k HLE shim infrastructure.
See `HANDOFF-NEWWORLD-SUPERVISOR-MMU.md` (full Path A state, obstacle map).

**Path A is parked as of 2026-06-09.** Path B (Upgrade Card approach) is now the active
strategy: run Mac OS 9.2 on the proven 1.1 ROM via targeted enabler shims. As of 2026-06-10,
2 of 3 boot-time System file gates have been bypassed; a post-splash stall is under
investigation. See `docs/planning/UPGRADE-CARD-PATH.md`.
