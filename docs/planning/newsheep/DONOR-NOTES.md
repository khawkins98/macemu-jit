# SS_M18 Donor Notes

> **Purpose.** Make Stages **S1** (paged MMU) and **S4** (Cuda IFR/IER) start *warm*. This is a
> read-only donor study per the program plan's "Donor read-only studies" parallel workstream
> (`docs/superpowers/plans/2026-06-14-ss-m18-trampoline-lle-program.md`). Donors are **NOT vendored** —
> we reimplement/port to our style and cite at the porting site (backport-hygiene memory: repo + file +
> **full pinned SHA** in a code comment, plus "needs validation" in the comment AND the CHANGELOG).
>
> **SHAs pinned at study time (2026-06-14):**
> - **Dolphin** — `144d19433aa734c19c34e5978a1b817d2aa12663` (master, 2026-06-14). GPLv2.
> - **DingusPPC** — `b2660e29201730efc2179a43ec6a0a5fb22ad120` (master, 2026-06-11). GPLv3.
> - **QEMU** — `de5d8bfd6105d3dd3ae668df9762df244a6d1506` (from `M3-PIC-CUDA-DONOR-STUDY.md`). GPLv2.
>
> Line numbers below are approximate (resolved against the pinned SHA via `raw.githubusercontent.com`);
> re-confirm at port time.

---

## Donor 1 — Dolphin "Dynamic BAT" shadow-arena (for **S1, paged MMU**) — PRODUCTION PORT target

### ⚠ Plan-vs-reality divergence (read first)
The plan (rev-4, S1 "Donor (C1)" line + the citation ritual) cites
`Source/Core/Core/PowerPC/JitArm64/Memmap.cpp :: UpdateDBATMappings`. **That path/owner is wrong in
current Dolphin.** The logic has been refactored and split across two files, and `UpdateDBATMappings`
is a method of the *memory manager*, not the ARM64 JIT:

| Concept | Actual location (SHA `144d1943…`) | Symbol |
|---|---|---|
| BAT-table rebuild (decode `mtspr` BAT writes → translation table) | `Source/Core/Core/PowerPC/MMU.cpp` | `MMU::DBATUpdated()` (~L1535), `MMU::UpdateBATs()` (~L1470), `MMU::UpdateFakeMMUBat()` (~L1530) |
| **Arena remap** (the part we actually port) | `Source/Core/Core/HW/Memmap.cpp` | `MemoryManager::UpdateDBATMappings(const BatTable&)` (~L233–305) |
| Host-page-size classification | `Source/Core/Core/HW/Memmap.cpp` ~L48 | `GetHostPageTypeForPageSize()` |
| Large-page (16 KB) feasibility test | `Source/Core/Core/HW/Memmap.cpp` ~L343 | `CanCreateHostMappingForGuestPages()`, `TryAddLargePageTableMapping()` (~L309) |
| Arena reservation + view mapping primitives | `Source/Core/Common/MemArena.{h,cpp}` | `MemArena::ReserveMemoryRegion / MapInMemoryRegion / UnmapFromMemoryRegion / GrabSHMSegment` |

The JIT (`JitArm64`) is **not involved in the remap** — that is the whole point of the technique and the
reason it fits us: access-time codegen never changes. Port from `MMU.cpp` + `Memmap.cpp`, ignore the
plan's `JitArm64/Memmap.cpp` path.

### Mechanism summary
Dolphin keeps a single reserved virtual **arena** and never translates at access time. Guest fast accesses
are bare loads/stores into `physical_base + guest_addr` (our exact RMEMBASE/NATMEM model). When the guest
reprograms address translation (BAT writes via `mtspr`), Dolphin **remaps sub-ranges of the arena** so the
bare access lands on the right backing page. The cost is paid on the (rare) map-change, not the access.

1. **Reservation** — `MemoryManager::InitFastmemArena()` (`Memmap.cpp` ~L163–219):
   - `m_fastmem_arena = m_arena.ReserveMemoryRegion(memory_size)` (~L199), one contiguous reservation.
   - Layout: **two 4 GiB views** (physical + logical) with three 2 GiB guard regions:
     `memory_size = ppc_view_size*2 + guard_size*3` (~L197).
   - `m_physical_base = arena + guard_size`; `m_logical_base = arena + ppc_view_size + guard_size*2`
     (~L207–208). Physical regions are mapped in with `MapInMemoryRegion(shm_pos, size, base, writeable)`
     (~L210–225) — the backing is a **shared-memory (SHM) segment** (`GrabSHMSegment`), so the *same*
     physical RAM can be aliased into multiple virtual offsets (this is the trick that lets logical/BAT
     views and the physical view share pages without copying).

2. **Map-change hook** — `MMU::DBATUpdated()` (`MMU.cpp` ~L1535) is the trigger. It runs when the guest
   writes the data-BAT SPRs (`SPR_DBAT0U/L … SPR_DBAT3U/L`, +HID4 extended BATs on Wii) via `mtspr`.
   It rebuilds `m_dbat_table` via `UpdateBATs()` (iterates the 4/8 BAT pairs; for each enabled entry
   enumerates the block via `for (u32 j=0; j<=batu.BL; ++j)` and writes
   `physical_address = (batl.BRPN | j) << BAT_INDEX_SHIFT` into `bat_table[va >> BAT_INDEX_SHIFT]`), then
   calls `m_memory.UpdateDBATMappings(m_dbat_table)` (~L1545). (`ReloadPageTable()` is also reloaded if a
   hashed page table is in use, ~L1541.)

3. **The remap itself** — `MemoryManager::UpdateDBATMappings()` (`Memmap.cpp` ~L233–305): for each BAT
   logical region it intersects against the registered `m_physical_regions`:
   ```
   intersection_start = max(mapping_address, translated_address);
   intersection_end   = min(mapping_end, translated_address + logical_size);
   if (intersection_start < intersection_end) { /* MapInMemoryRegion(...) the overlap */ }
   ```
   Each created view is recorded in `m_dbat_mapped_entries`; teardown/refresh uses
   `UnmapFromMemoryRegion(entry.mapped_pointer, entry.mapped_size)`. So a context switch = unmap old
   entries, remap new ones — **no codegen, no per-access walk.**

### Apple-Silicon / 16 KB-page specifics (our Discriminator-A risk)
- `GetHostPageTypeForPageSize()` (~L48): `page_size > PowerPC::HW_PAGE_SIZE (4 KB) ? LargePages : SmallPages`.
  On Apple Silicon the host page is **16 KB**, so Dolphin is permanently on the `LargePages` path.
- `CanCreateHostMappingForGuestPages()` (~L343–363) is the decisive gate when host > guest page:
  ```
  if ((translated_address & (m_page_size-1)) != 0) return false;      // must be 16 KB-aligned
  for (i=1; i<m_guest_pages_per_host_page; ++i)                       // all 4 constituent 4 KB
      if (entries[i] != translated_address + i*HW_PAGE_SIZE) return false;  // guest pages must be
  return true;                                                        // physically contiguous
  ```
  i.e. a 16 KB host page can only be fast-mapped if its **four** 4 KB guest pages are aligned AND
  contiguous AND share permissions. Any misalignment, fragmentation, or **mixed per-4 KB-page
  permission** → returns false → that range falls off the fastmem path (slow path).
- **This is exactly the S1 Discriminator-A fork in the plan's runbook:**
  - **Coarse** NK mapping (segment/BAT, ≥256 MB blocks) ⇒ trivially 16 KB-aligned & contiguous ⇒
    the shadow-arena port is **bounded and 16 KB-host-safe**. Take this path.
  - **Fine** NK mapping (dense 4 KB hashed-PTE with mixed per-page perms) ⇒ `CanCreateHostMappingForGuestPages`
    fails pervasively ⇒ Dolphin itself falls back to slow path; for us that means softmmu, fast path lost,
    S1 balloons. Re-band and escalate.
  Run the Discriminator-A QEMU/gdbstub probe (plan §"Discriminator-A runbook") **before** writing S1 code.
- **W^X / mach_vm.** The arena is RW *data* (not `MAP_JIT`), so the Apple-Silicon W^X JIT constraint does
  NOT apply to the remap — the dead-end "`vm_remap` from `MAP_JIT` → `KERN_PROTECTION_FAILURE`" noted in the
  plan is about code pages and is irrelevant here. The SHM-alias mechanism (`shm_open` + `mmap MAP_FIXED`
  on Unix; `mach_make_memory_entry` / `vm_map` on macOS) is abstracted in `MemArena.cpp` behind
  `MapInMemoryRegion`. The header carries the macOS members (`vm_address_t`, `vm_size_t`,
  `mem_entry_name_port_t`) — read `MemArena.cpp` (the macOS `#ifdef`) at port time for the exact
  `mach_make_memory_entry_64` / `vm_map` calls; this study did not disassemble that file in full
  (**partial — confirm at port**). PR #9441 (W^X on Apple Silicon) is the historical reference for the
  map-change hook shape but applies to the JIT code arena, not the data arena we remap.

### Mapping to OUR seams
| Dolphin element | Our seam |
|---|---|
| `ReserveMemoryRegion` + `m_physical_base` arena | NATMEM reservation in `SheepShaver/src/Unix/main_unix.cpp` (~:2079, the `MachineUsesMMIOBus()` mmap block reserving `NATMEM_OFFSET + 0xF3000000`). The paged-MMU arena/shadow is reserved alongside this. |
| Bare access into `physical_base + addr` (codegen untouched) | The **80 `RMEMBASE` JIT sites** in `SheepShaver/src/kpx_cpu/src/cpu/jit/aarch64/ppc-jit.cpp` (count confirmed = 80). These **must stay bit-identical** — the whole port is predicated on not touching them. |
| `MMU::DBATUpdated()` trigger | Our `mtspr` BAT / `mtspr` SDR1 / `mtsr`/`mtsrin` handlers (already exist from Wave 0, "honor-the-write" largely done). Hook the shadow-remap call here. SR/SDR1 state lives in `SheepShaver/src/kpx_cpu/src/cpu/ppc/ppc-registers.hpp` (`sdr1` ~L266, `bat[16]` ~L269, `sprg[4]` — supervisor block near :258). |
| `UpdateDBATMappings()` remap body | New shadow-remap routine in the NATMEM/`vm_alloc` layer (`main_unix.cpp`), called from the `mtspr` handlers. Mirror the intersect-and-remap loop; gate strictly behind `MachineProfileIsNewWorld()` so paravirtual stays V=P byte-identical (the paged path is structurally unreachable when `!MachineProfileIsNewWorld()`). |
| `CanCreateHostMappingForGuestPages` 16 KB gate | Our equivalent feasibility check before each remap; if it fails on real NK mappings → Discriminator-A says "fine" → softmmu. |

**Oracle (do NOT port):** PearPC / QEMU softmmu PPC translator as a reference to *diff* our PA outputs on
identical `(SR/BAT/SDR1, EA)` inputs. Never port a per-access walker — that is the perf-fatal anti-pattern.
DingusPPC is **not** a usable S1 oracle (no KeyLargo/OpenPIC / no full MMU model for our purpose).

---

## Donor 2 — DingusPPC VIA-Cuda (for **S4**, the expected next wall: Cuda IFR/IER) — EXTRACT-PROTOCOL (do NOT wrap)

> **Why S4 needs this.** Per M14-FINDINGS the expected next wall after live IM-init is the NK polling the
> VIA **IER** while `sr_int_pending` never reaches the VIA **IFR**. The Dingus model shows the precise
> IFR/IER + SR-interrupt handshake to reimplement in our style. **Extract the protocol; do not wrap their
> core.**

**Files:** `devices/common/viacuda.cpp` + `viacuda.h` @ DingusPPC SHA `b2660e29201730efc2179a43ec6a0a5fb22ad120`.
(The M3 study cited prefix `92bb6d10…`; that is now stale — use the full SHA above, or re-pin at port time.)

### State machine + handshake
Dingus does **not** use an explicit `enum` of named states — state is implicit in line flags
(`old_tip`, `old_byteack`, `treq`, `is_sync_state`). The driver is `ViaCuda::write(uint8_t new_state)`
(~L520–600), decoding **Port B** bits `CUDA_TIP` (Transaction In Progress) and `CUDA_BYTEACK`, plus the
ACR shift-direction bit `via_acr & 0x10` (host→Cuda vs Cuda→host):
```
if (new_tip)      { byteack ? /* idle/reset */ : /* enter sync */ }
else              { (via_acr & 0x10) ? /* Host→Cuda xfer */ : /* Cuda→Host xfer */ }
```
Transfers shift one byte per SR handshake; completion calls `process_packet()`.

### Command dispatch
`process_packet()` (~L667–685) routes on the first byte:
- `CUDA_PKT_ADB` → `process_adb_command()`
- `CUDA_PKT_PSEUDO` → `pseudo_command()`

`pseudo_command()` switch (~L731–885) — the boot-relevant command set:
| Constant | Use for boot |
|---|---|
| `CUDA_START_STOP_AUTOPOLL` (0x01) | ADB autopoll enable/disable |
| `CUDA_GET_REAL_TIME` / `CUDA_SET_REAL_TIME` | RTC — boot reads clock |
| `CUDA_READ_PRAM` / `CUDA_WRITE_PRAM` | **256-byte PRAM** (`addr <= 0xFF` guard, ~L793/L809; backing `pram.bin`, 256 B, ~L86). QEMU lacks PRAM; Dingus has it — this is why Dingus is the PRAM oracle. |
| `CUDA_SET_AUTOPOLL_RATE` / `CUDA_GET_AUTOPOLL_RATE` | ADB poll rate |
| `CUDA_ONE_SECOND_MODE` | 1 Hz tick |
| `CUDA_POWER_DOWN` / `CUDA_RESTART_SYSTEM` | power/restart |
| `CUDA_READ_MCU_MEM` / `CUDA_WRITE_MCU_MEM`, `CUDA_READ_WRITE_I2C`, `CUDA_COMB_FMT_I2C` | not boot-critical |

Device bitmap / ADB device enumeration comes via the ADB path (`process_adb_command()` → autopoll
response framing).

### IFR/IER interrupt behavior (the S4 crux)
VIA IFR bits in `_via_ifr`: `VIA_IF_SR` (shift register), `VIA_IF_T1`, `VIA_IF_T2`, `VIA_IF_CA1/CA2/CB1/CB2`.
- **Assert** (~L440–490): `assert_sr_int()` sets `VIA_IF_SR`; `assert_t1_int()/assert_t2_int()`;
  `assert_ctrl_line(ViaLine)`.
- **Notify** — `update_irq()` (~L416–428):
  ```
  active_ints = _via_ifr & _via_ier & 0x7F;
  int_ctrl->ack_int(irq_id, active_ints ? 1 : 0);   // bit-7 = composite IRQ to CPU
  ```
  i.e. an interrupt reaches the CPU **only when the corresponding IFR bit AND its IER enable are both set**.
  This is exactly our M14 wall: the NK enables `VIA_IF_SR` in IER and polls; if our Cuda completion never
  calls the equivalent of `assert_sr_int()` to set IFR bit, `active_ints` stays 0 and the NK spins.
- **Scheduling** — `schedule_sr_int(timeout_ns)` (~L495–509) posts a one-shot timer that later raises the
  SR interrupt (models the byte-shift completion latency). Our reimpl must raise IFR.SR after the
  packet/response is staged, then let the IFR&IER gate fire the PIC.

### Mapping to OUR seams
| Dingus element | Our seam (reimplement in our style) |
|---|---|
| `ViaCuda` register file + `write()` handshake | `SheepShaver/src/machine/dev_via6522.cpp` (our VIA surface; `test_dev_via6522` exists) — our VIA register/handshake decode. |
| `process_packet` / `pseudo_command` dispatch | `SheepShaver/src/machine/dev_cuda.cpp` (`test_dev_cuda` exists) — our Cuda command engine. |
| `update_irq()` IFR&IER gate → PIC | Our VIA→PIC wiring: `dev_via6522.cpp` raising IFR, routed through `dev_openpic.cpp`. **This is the S4 fix point** — ensure SR-int completion sets IFR.SR and the IFR&IER mask actually signals the OpenPIC source the NK polls. |
| `assert_sr_int` + `schedule_sr_int` | Our SR-completion → IFR.SR set, scheduled via `event_sched.cpp`/`virt_clock.cpp`. |
| PRAM read/write (`pram.bin`, 256 B) | Our NVRAM/PRAM backing (note: QEMU has no PRAM; use Dingus as the PRAM behavioral oracle). |

**Oracles:** Dingus (has PRAM) + QEMU `hw/misc/macio/cuda.c` (no PRAM, but a second behavioral reference).
Use both to cross-check command framing; reimplement, don't import.

### License (IMPORTANT)
DingusPPC is **GPLv3**. Any Dingus-derived code in our tree makes the **distributed binary GPLv3** — record
this in `docs/UPSTREAM-LINEAGE-SYNC.md` at port time. Cite repo + `viacuda.cpp/.h` + full SHA
`b2660e29201730efc2179a43ec6a0a5fb22ad120` at the porting site, mark "needs validation" in the comment AND
the CHANGELOG. **Never PR anything back upstream to DingusPPC (their AI-contribution ban).** (We reimplement
to-spec / extract-protocol rather than copy, but cite the behavioral source regardless.)

---

## Donor 3 — QEMU OpenBIOS / OpenPIC + MacIO (for **S2**, OF-CI + interrupt controller) — oracle + reimplement-to-spec

Cross-reference, do **not** re-derive — the region map is already pinned in
`docs/archive/2026-06/machine/M3-PIC-CUDA-DONOR-STUDY.md`:
- **`hw/intc/openpic.c`** @ QEMU `de5d8bfd6105d3dd3ae668df9762df244a6d1506` — `openpic_init` reserves the
  region `memory_region_init(&opp->mem, …, 0x40000)` (~L1498), added to the MacIO BAR at **offset `+0x40000`**
  (`macio_newworld_realize`); KeyLargo model `OPENPIC_MODEL_KEYLARGO` lays out the glb/src/cpu sub-regions
  via `map_list`. Full sub-region table is in M3-DONOR-STUDY §1.2.
- **`hw/misc/macio/*`** (`macio.c`, `cuda.c`) — same SHA — MacIO container wiring (the BAR that hosts
  OpenPIC + Cuda + SCC) and a second Cuda behavioral reference.
- The captured **Q3 `[QEMU-BEHAVIORAL]` Trampoline trace** is the expected OF-call / DT-read sequence oracle
  for S2a (`of_ci_callback` must resolve all 21 direct services + `call-method` set + the 3-word `interpret`
  shim against the synthesized Core99 DT).

Our seams: new `src/openfirmware_ci.{cpp,h}` (S2a) + `dev_openpic.cpp` (already present;
`test_dev_openpic` exists). License: QEMU is GPLv2 (compatible). **Address-oracle caveat:** never cite a
QEMU MMIO address as a reference value for *our* machine layer (QEMU MacIO base differs); use QEMU only for
*behavior* and *relative* region layout, with the Core99 DT (`CORE99-MACHINE-DESCRIPTION.md`) as the
authoritative shape source — especially the `interrupt-map`/`-mask` tuple (plan Q-S2a.3).

---

## Quick-start checklist
- **S1 dev:** read `MMU.cpp::DBATUpdated/UpdateBATs` + `Memmap.cpp::UpdateDBATMappings` +
  `CanCreateHostMappingForGuestPages` (NOT the plan's `JitArm64/Memmap.cpp`). Run Discriminator-A FIRST.
  Hook remap at our `mtspr` BAT/SDR1/SR handlers; keep the 80 RMEMBASE sites untouched; gate on
  `MachineProfileIsNewWorld()`.
- **S4 dev:** read `viacuda.cpp::update_irq / assert_sr_int / pseudo_command`. The fix is making
  IFR.SR set on Cuda completion so the IFR&IER mask signals the OpenPIC source the NK polls. Reimplement in
  `dev_via6522.cpp` + `dev_cuda.cpp`; mind the GPLv3 propagation.
