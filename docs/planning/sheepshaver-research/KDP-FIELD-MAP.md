# KDP (Kernel Data Page) Field Map

The NanoKernel's per-CPU data structure, addressed via SPRG0.
Fields at positive offsets from KDP base; the area below KDP (negative offsets)
is a pool/heap region used for spinlocks and per-CPU structures.

## Source Material

- **elliotnunn/OldKern** `KDP.h` — the most complete public field map (hex offsets,
  field names, types). Covers NanoKernel versions through ~9.2.
- **elliotnunn/OldKern** `InfoRecords.h` — `NKConfigurationInfo`, `NKSystemInfo`,
  `NKProcessorInfo` structs (the firmware→nanokernel handoff records).
- **NanoKernel source** (`Init.s`, `Reset.s`, `Defines.s`) — field definitions and
  boot-time initialization flow.

## Fields Mapped (SheepShaver parcels boot)

| Offset | Size | Name | Set by | Read by | Notes |
|--------|------|------|--------|---------|-------|
| -0x1000..0 | 4 KB | Pool/heap region | Init.s (zeroed) | Various (spinlocks, per-CPU) | Must be mapped+zeroed; trampoline does this |
| -0x4 | 4 | Self-pointer | Trampoline (`[KDP-4] = KDP`) | NanoKernel init | Convention: `[SPRG0-4]` = KDP base |
| +0x5A8 | 4 | (unknown — read during init) | ? | Reset.s | Observed in wall-12250 dump; zero in our setup |
| +0x5AC | 4 | (unknown — read during init) | ? | Reset.s | Observed in wall-12250 dump; zero in our setup |
| +0x5B0 | 4 | (unknown — read during init) | ? | Reset.s | Value 0x50325F00 (ROM address); likely a code pointer |
| +0x638 | 4 | **KernelMemoryBase** | Init.s (from ConfigInfo) | Reset.s:315 (free-list exclusion) | Lowest address of the nanokernel's memory allocation (HTAB base in our layout) |
| +0x63C | 4 | **KernelMemoryEnd** | Init.s (`base + total_size`) | Reset.s:316 (free-list exclusion) | End of nanokernel memory; `KernelMemoryEnd = KernelMemoryBase + total_alloc` |
| +0x640 | 4 | LowMemPtr | Init.s | ? | |
| +0x644 | 4 | SharedMemoryAddr | Init.s | ? | |
| +0x688 | 4 | (unknown — read during setup) | ? | Reset.s ~0x5031216c area | Observed loaded into r23 |
| +0x6B0 | 4 | (free-list related) | Reset.s priming loop | ? | Written by `stw r??, 0x6b0(r1)` during page counting |
| +0x6B4 | 4 | (free-list page count) | Reset.s priming loop | ? | Stores final page count |
| +0x6C0 | 4 | **PhysicalPageArray** | Reset.s:377 | 68k page-descriptor mapping | `KDP + 0x6C0` = base of per-segment PhysicalPageArray pointers |

### SysInfo Bank Entries (embedded in KDP)

The `NKSystemInfo` structure (within KDP) contains physical memory bank descriptors:

| Relative offset | Field | Purpose |
|----------------|-------|---------|
| `SysInfo.Bank0Start` | start addr | Physical start of RAM bank 0 |
| `SysInfo.Bank0Size` | size | Size of RAM bank 0 |
| ... | ... | Up to 26 banks (NanoKernel Defines.s) |

Reset.s iterates these at ~0x503121A4 to discover total physical memory extent.

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

0x68FE0000  ┌──────────────────────┐  ← htab_base (SDR1)
            │   HTAB (64 KB)       │  KernelMemoryBase → here
0x68FEFFFF  ├──────────────────────┤
            │   (gap to sub-KDP)   │
0x68FF4000  ├──────────────────────┤  ← sub_kdp_base
            │ sub-KDP pool (32 KB) │  Pool/heap for spinlocks etc.
0x68FFBFFF  ├──────────────────────┤
            │ KDP-0x1000..KDP      │  Negative-offset region (zeroed)
0x68FFDFF0  │   [KDP-4] = KDP     │  Self-pointer
0x68FFE000  ├──────────────────────┤  ← KDP = SPRG0 = KernelDataAddr
            │   KDP (8 KB)         │  Per-CPU data page
            │   +0x638 = KMemBase  │
            │   +0x63C = KMemEnd   │
            │   +0x6C0 = PhysPgArr │
0x68FFFFFF  └──────────────────────┘  KernelMemoryEnd → ~here
```

## Wall Status

The free-list priming loop at ROM 0x50312244 reads KDP+0x638 and KDP+0x63C to
determine which pages to exclude from the free list. Without seeding these fields,
the nanokernel reads garbage (0xDEAD0000 / 0xDEBD0000) → computes ~524K phantom
pages → infinite loop reading unmapped memory via ignoresegv.

**Root cause proven** (R29-TRACE at block 0x5031216c). Fix requires seeding
KernelMemoryBase/End in the trampoline AND mapping ~256KB below htab_base for
the page descriptor array. This is bespoke bootloader emulation, not a general
CPU-model fix — see stop-rule discussion in NEW-WORLD-ROM-SUPPORT-PLAN.md.
