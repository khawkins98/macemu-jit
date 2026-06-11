# Disk-Path Recon — how do boot-volume blocks reach the newworld fidelity profile?

> **Status:** recon complete (2026-06-11) · Stream C, M7 risk-retirement
> **Question:** Mac OS 9.2.x on real Core99 reaches its boot volume through the NewWorld
> driver stack (ATA Manager + `.ndrv` interface drivers + DBDMA under KeyLargo). SheepShaver
> paravirtual bypasses all of it with HLE EMUL_OP drivers. When the fidelity profile boots a
> real 9.2.x System from a disk image — **through what?**
> **Method:** static + docs only (no boots, no source edits). All ROM-dump offsets are from
> `/tmp/rom901_decompressed.bin` (SS_DUMP_ROM of the 2001-12-19 "Mac OS ROM 9.0.1") and the
> raw parcels file `/Users/Shared/macemu/2001-12-19 - Mac OS ROM 9.0.1.rom`.
> **Headline:** today the newworld profile has **no disk path at all** — the paravirtual HLE
> install chain statically cannot complete on the 9.0.1 parcels ROM (§1.4), and no LLE
> ATA/DBDMA model exists (M4 ships abort-loudly stubs only). The recommended default is an
> **HLE block device behind the real NDRV framework** (option c, §4/§5), which is the
> mechanism MACHINE-LAYER-PLAN §2f already chose in principle ("disk … stays HLE on purpose")
> — this recon supplies the missing *how*.

---

## §1. The paravirtual disk path, as built

### 1.1 The replacement drivers (what 68k surface is replaced)

`SheepShaver/src/rom_patches.cpp` carries three hand-assembled 68k `DRVR` images:

| Driver | Bytes | Entry points |
|---|---|---|
| `.Sony` (floppy) | `sony_driver[]`, rom_patches.cpp:291–337 | `M68K_EMUL_OP_SONY_OPEN/PRIME/CONTROL/STATUS` (lines 302–316) |
| `.Disk` (generic HD) | `disk_driver[]`, rom_patches.cpp:339–385 | `M68K_EMUL_OP_DISK_OPEN/PRIME/CONTROL/STATUS` (lines 350–364) |
| `.AppleCD` | `cdrom_driver[]`, rom_patches.cpp:387–413 | `M68K_EMUL_OP_CDROM_*` (lines 398–412) |

Each is a classic Device Manager `DRVR` whose Open/Prime/Control/Status vectors are 2-byte
EMUL_OP opcodes (`M68K_EMUL_BREAK = 0xfe43` + op, `include/emul_op.h:58,72`). They replace
the **entire 68k Device-Manager driver level**: the guest's `.ATALoad`/`.ATADisk`/SCSI/Sony
stack never runs; `PBRead/PBWrite` on these refnums lands host-side.

### 1.2 Execution path of one block read

1. Guest 68k code (under the ROM's DR emulator) executes the `0xFE43+n` opcode.
2. The DR emulator's opcode dispatch table — patched by `patch_68k_emul()` at fixed image
   offset **0x380000 + (opcode << 3)** with PPC `POWERPC_EMUL_OP | (i+3)` instructions
   (rom_patches.cpp:1987–1999) — funnels into the JIT/interpreter EMUL_OP seam.
3. `sheepshaver_cpu` marshals 68k registers and calls `EmulOp()`
   (`kpx_cpu/sheepshaver_glue.cpp:414`; JIT trampoline init at :2307–2330).
4. `EmulOp()` dispatches `OP_DISK_PRIME` → `DiskPrime(pb, dce)` (`emul_op.cpp:669–684`).
5. `DiskPrime` (`disk.cpp:313–356`) does `Mac2HostAddr(ioBuffer)` + `Sys_read`/`Sys_write`
   at `position + start_byte` — direct host file I/O into guest RAM, 512-byte granularity.
   `DiskOpen` (`disk.cpp:249–306`) allocates `DrvSts` via `Execute68kTrap(0xa71e)` and
   inserts the drive into the guest drive queue with `AddDrive` (`Execute68kTrap(0xa04e)`).

So the HLE drivers still *participate* in the guest's Device Manager/drive-queue protocol —
they are guest-visible DRVRs whose bodies are host code, not a parallel universe.

### 1.3 Where the drivers get installed (two hooks, both ROM patches)

- **Bodies into ROM:** `patch_68k()` locates an anchor resource — `find_rom_resource('DRVR', 4)`
  (second occurrence on NewWorld; first DRVR 4 is `.MFMFloppy`), falling back to
  `find_rom_resource('ndrv', -20196)` ("PCFloppy" in NewWorld 1.1) — and `memcpy`s the three
  drivers over it at `sony_offset`, `+0x100`, `+0x200` (rom_patches.cpp:3351–3369). Serial
  drivers and icons follow at `+0x300…+0xe00`. **If the anchor is not found, `patch_68k()`
  returns false at this point — there is no lenient skip for this block** (line 3359).
- **Install trigger:** `drvr_install_dat` (`{a7 1e 21 c8 01 1c 4e 75}`, search window
  `[0xb00, 0xd00)`) is patched so the ROM's boot-time driver-install routine executes
  `M68K_EMUL_OP_INSTALL_DRIVERS` (rom_patches.cpp:3388–3396). That EMUL_OP calls
  `InstallDrivers()` (`emul_op.cpp:756–758` → rom_patches.cpp:3603–3720), which runs
  `DrvrInstallRsrvMem` (`Execute68kTrap(0xa43d)`) + `Open()` for `.Sony`, `.Disk`,
  `.AppleCD` and the four serial drivers, wiring DCEs into the Unit Table (`0x11c`).
- **SCSI:** the real SCSI Manager is *faked out* under `DISABLE_SCSI` (`scsi_mgr_a/b_dat`,
  rom_patches.cpp:3234ff; fake SCSIGlobals in `InstallDrivers()`, :3611–3617). SCSI is a
  dead surface on every profile; MESH at MacIO+0x10000 is a stub
  (CORE99-MACHINE-DESCRIPTION §1).

### 1.4 Does any of this apply to the 9.0.1 parcels ROM? **No — statically broken, three ways**

This is the recon's central negative result.

**(i) `find_rom_resource()` cannot walk the 9.0.1 combined resource map.**
The walker (rom_patches.cpp:154–180) reads the next-link at **entry+0**, then applies
`header_size` (byte at `map+5`) to find the data/type/id fields. On the 9.0.1 image the
combined-map entry layout moved the next-link **into** the 8-byte header:

```
entry+0:  78 00 00 00   (header word — NOT a link)
entry+4:  00 00 00 00
entry+8:  next-link     ← the code reads entry+0 instead
entry+12: data offset
entry+16: type / +20 id / +23 pname
```

Evidence: head at image offset `0xaa2d0` (pointed to by the map pointer at offset `0x1a`),
`header_size`=8; first entry `0x257370` = `78 00 00 00 | 00 00 00 00 | 00 24 80 f0 |
00 24 81 20 | 'ndrv' | 0xadfe …` — the field arithmetic parses entry #1 correctly
(`ndrv -20994 "sbp609e,104d8"`, data `0x248120` = `Joy!peff`), then the next-link read at
entry+0 yields `0x78000000` and the walk leaves the ROM. A corrected walk (next at +8)
enumerates the full map: **157 resources, 39 types** — the format diagnosis, not the ROM,
is what's broken. (Probed 2026-06-11 against `/tmp/rom901_decompressed.bin`; the same
breakage explains why every `find_rom_resource`-relative patch — `dsl_offset`, `hpchk`,
`SERD` — has always no-op'd on 9.0.1.)

**(ii) Even with the walk fixed, the anchor resources don't exist.** The 9.0.1 map's full
`DRVR` set is `.ATALoad` (id -20175, data `0x19c6e0`), `.ATADisk` (id 53, `0x198a60`),
`.EDisk` (id 48, `0xb60d0`) — **no `DRVR` 4 at all**, and no `ndrv -20196` (its `ndrv`s are
`sbp609e,104d8`, `fw609e,10483`, `USBUnitTableStorageDriver`, `pciclass,0c0310`,
`media-bay`, `pccard-ata`, `DefaultPCCardEnabler`, `.BCScreen`). The sony anchor strategy
needs a redesign for parcels ROMs (e.g. overwrite `.EDisk`, or claim free ROM space and
synthesize a map entry).

**(iii) Because the sony block hard-fails, everything after it never runs.** `patch_68k()`
returns false at rom_patches.cpp:3359; the lenient wrapper tolerates this
(`[ROMPATCH] parcels: patch_68k incomplete — boot continues`, rom_patches.cpp:654–664) but
the `drvr_install` hook at :3388 — which the whole-image lenient fallback *would* find
relocated at `0x9bc` (PATCH-68K-SHIM-INVENTORY row 127; fallback in `find_rom_data`,
rom_patches.cpp lenient retry loop) — is **never reached**. Net: on every 9.0.1 boot to
date, no HLE disk/floppy/CD driver exists in the guest, and `InstallDrivers()` never runs.

A fourth, unverified dependency: the EMUL_OP dispatch table write at fixed offsets
`0x36e600`/`0x380000` (`patch_68k_emul()`) is applied **unconditionally** to the parcels
image; whether that region is layout-identical there has never been proven
(HANDOFF-NEWWORLD-SUPERVISOR-MMU:587 flags exactly this risk). No 9.0.1 boot has yet
demonstrably executed *any* 68k EMUL_OP. Tripwire T2 (§5) answers this cheaply.

---

## §2. What the 9.0.1 ROM and its parcels actually provide

### 2.1 The parcels TOC (raw ROM file, parcels-offset `0x1bfc0`, size `0x259c8c`, magic `prcl`)

28 parcels (probed 2026-06-11; offsets are within the parcels blob):

| # | type | name | size | contents |
|---|---|---|---|---|
| 0 | `node` | CodePrepare Node Parcel | 0x2730 | `nlib` CFM fragments prepared at boot (`AAPL,prepare_order`) |
| 1 | `node` | CodeRegister Node Parcel | 0x1592c | registered `nlib`s (NativePowerMgrLib, AGPLib, …) — the gate-2 CFM-fragment surface from spike S1 |
| 2 | `rom ` | Mac OS ROM Parcel | 0x1d3684 | LZSS → the 4 MB image (the only parcel SheepShaver consumes) |
| 3 | `psum` | Property Checksum | 0x254 | checksum over injected properties |
| 4–27 | `prop` | per-device-node payloads | — | see below |

Disk-relevant `prop` parcels — each carries a `driver,AAPL,MacOS,PowerPC` property whose
payload is an LZSS-compressed PEF (`Joy!peffpwpc`) `.ndrv`:

- **`cmd646-ata`** (0x4420, PEF @+0x95) — PCI CMD646 IDE interface ndrv
- **`heathrow-ata`** ×2 (0x33c0 + a 0x94 alias) — Heathrow/Paddington ATA ndrv
- **`keylargo-ata`** (0x3890, PEF @+0x95) — **the Core99 boot-disk interface driver**
- plus `mac-io`/`nvram,flash` (nvram ndrv), `via-cuda`, `pmu`, `cofb` (display), network
  (`bmac+`, `gmac`, `apple21143`), cardbus, firewire (`pciclass,0c0010`).

On real hardware the **Trampoline** matches `prop` parcels to OF device-tree nodes by
node name/compatible and grafts the properties in; Mac OS's NDRV loading (DriverLoaderLib)
later instantiates `driver,AAPL,MacOS,PowerPC` via CFM. **SheepShaver discards all of
this:** `decode_parcels()` extracts only `'rom '` parcels (`include/rom_decode.hpp:86–100`)
— the `keylargo-ata` ndrv never reaches guest memory today. Injecting prop parcels is
named M5 territory (Trampoline handoff publishes the device tree;
CORE99-MACHINE-DESCRIPTION §3 "Parcels/override handshake").

### 2.2 The 68k image's own disk stack (decompressed image offsets)

- `.ATALoad` DRVR (-20175, data `0x19c6e0`; name string at `0x5d9e3`) — boot-time INIT that
  probes ATA buses through the ATA Manager and spawns drives.
- `.ATADisk` DRVR (53, `0x198a60`) — the ATA hard-disk Device-Manager driver.
- `ATAManager` strings at `0x5da6b`, `0x1cd512`, `0x1d210a`, `0x1d7e98`; `ATAPI` at
  `0x19d2ed` — the ATA Manager trap layer is fully present in ROM.
- `.EDisk` (48) — RAM-disk driver. No `.Sony`, no `.AppleCD` in ROM (CD support comes from
  the System file / extensions era drivers).
- `USBUnitTableStorageDriver`, `fw`/`sbp` ndrvs — 9.x can also boot mass storage via
  USB/FireWire ndrv stacks, an existence proof that **9.x boot volumes need not sit behind
  the ATA Manager** (relevant to option (c)).

### 2.3 The real Core99 chain at the stage M7 heads toward

OF loads the ROM file (we skip this — prefs `rom` path) → Trampoline builds the device
tree + injects parcels → nanokernel → 68k Start Manager → ROM DRVR init → `.ATALoad` /
ATA Manager probe the `ata` nodes (KeyLargo IDE cells, DBDMA channel per bus) → `.ATADisk`
drives enter the drive queue → Start Manager picks the boot volume (PRAM DefaultStartup +
drive-queue scan) → HFS+ mounts → System file loads, replacing/augmenting ROM drivers
(chain documented in HANDOFF-NEWWORLD-SUPERVISOR-MMU:198; SYSTEM-BOOT-GATES for the
System-side gates). The hardware under that: ATA register file + DBDMA channel engine in
MacIO, interrupts via KeyLargo OpenPIC inputs `0xd`/`0xe` (cmd) and `0x2`/`0x3` (DMA)
(CORE99-MACHINE-DESCRIPTION §2, QEMU `macio.h:55–58`).

---

## §3. The Machine-Layer frame (what's already decided)

- **§2f of MACHINE-LAYER-PLAN is binding:** "Video …, **disk**, ethernet, sound, file
  system … are *runtime* paths where paravirtualization is the feature, not the
  compromise. … **LLE for what the ROM probes, HLE for what the OS uses.**" Plus the
  constraint: HLE-backed nodes must not advertise capabilities we don't model ("no phantom
  DBDMA channels on nodes we service by HLE"). Disk = HLE is *decided*; the open question
  is purely the **delivery mechanism** on a parcels boot.
- **M4** ships NVRAM + MacIO container + **DBDMA stubs that abort loudly** — "real channel
  engine only when a milestone demands it" (plan M4 row; deliberately not started, no live
  consumer). DBDMA channel block pinned at MacIO+0x08000 (CORE99 §1, QEMU `macio.c:102`).
- **IDE register window is disputed:** our AddrMap says MacIO+0x18000 (`rom_patches.cpp:1890`,
  `lp[24]`), QEMU NewWorld places macio-ide at +0x1f000+(n+1)*0x1000 → +0x20000/+0x21000
  (`macio.c:199`; CORE99 §1 row "IDE" + §5 Q3). Unresolved; must be settled before any LLE work.
- **The published device tree has no disk node.** The HLE Name Registry tree
  (`name_registry.cpp:85–388`) creates `device-tree`, `AAPL,ROM`, `PowerPC,…`, `memory`,
  `video` (+ndrv stub property, :371), `ethernet` (+ndrv stub, :382) — nothing
  disk-shaped. The CORE99 §3 device-tree skeleton (M5's contract) likewise lists mac-io /
  open-pic / escc / via-cuda / nvram / video only. **A disk node is an unallocated line item.**
- **S1 is the existence proof for the full LLE chain:** unpatched 9.2.1 boots to Finder
  under QEMU mac99 with this exact 9.0.1 ROM (SPIKE-S1-QEMU-GATE-CHECK.md; plan M7 row) —
  i.e. the ROM's keylargo-ata ndrv + ATA Manager + DBDMA path *works* when the hardware is
  really there. That bounds option (b)'s risk: it is expensive, not speculative.

---

## §4. The options, costed

| | (a) Revive the 68k EMUL_OP DRVRs | (b) LLE: ATA register model + DBDMA engine | (c) HLE block ndrv behind the real NDRV framework |
|---|---|---|---|
| **What it is** | Fix the paravirtual install chain so `.Disk`/`.AppleCD` EMUL_OP DRVRs install on the parcels ROM | Model KeyLargo IDE cell + DBDMA channel + OpenPIC inputs behind the M1 bus; let the ROM's own `keylargo-ata` ndrv + ATA Manager drive it | Publish a disk node in the M5 device tree whose `driver,AAPL,MacOS,PowerPC` property is **our** PEF stub (NativeOp → host block server reusing `disk.cpp`'s `Sys_read/Sys_write` layer) |
| **What exists** | Drivers, EMUL_OP dispatch, `disk.cpp` backend — all proven on paravirtual/1.1 | M1 bus + fault decode + backpatch; MacIO container map; OpenPIC model (unwired); S1 oracle; loud-stub seam at +0x08000 | **Precedent in-tree:** video + ethernet already work exactly this way (`VideoDriverStub.i`/`EthernetDriverStub.i`, planted at `name_registry.cpp:371,382`, loaded by the guest's own DriverLoaderLib); `disk.cpp` host backend |
| **What's missing** | `find_rom_resource` 9.x-layout fix (§1.4-i, next-link@+8 — needed by ~6 other patches anyway); a new anchor (§1.4-ii, no DRVR 4); confirmation the EMUL_OP table works on parcels (§1.4 residue); reaching the 68k Start-Manager driver-install stage at all (M6 frontier) | ATA-4 register file/state machine; DBDMA channel-program engine (the big one); IDE address dispute (§3); parcels-prop injection (M5) so keylargo-ata loads; OpenPIC wiring (M3b W2-3); media error/ATAPI surface for CD | A disk `.ndrv` PEF stub (DoDriverIO + Driver Gestalt: `vers`/`sync`/`intf`/boot support); the M5 device-tree node (name/compatible TBD — must NOT claim DBDMA per §2f); NativeOp plumbing (pattern exists: `thunks.cpp` NATIVE_* table); drive-queue/partition behavior now done by the NDRV framework instead of `DiskOpen` |
| **Donors** | none needed (own code) | QEMU `hw/ide/macio.c` + `hw/misc/macio/mac_dbdma.c` @ `de5d8bfd6105d3dd3ae668df9762df244a6d1506` (the pinned M1/M3 fetch SHA, M3-PIC-CUDA-DONOR-STUDY:35); DingusPPC `devices/common/ata/*` + KeyLargo @ `92bb6d10549529f9f4031a85c2bc136149535bdc` (GPLv3-combined, per M2 precedent) | Apple "Designing PCI Cards & Drivers" ndrv/DoDriverIO spec; our own EthernetDriverStub as the PEF template; `disk.cpp` |
| **Fidelity** | lowest — bypasses Device Manager *and* NDRV framework alike; invisible to Driver Gestalt-era System code | highest — the guest runs its own entire stack | middle — **real** CFM/NDRV/DriverGestalt/drive-queue machinery runs; only the bottom block transport is host code (same posture as shipped video/ethernet) |
| **Size class** | S–M (after the find_rom_resource fix lands) | **L–XL** (a DBDMA channel engine is QEMU's `mac_dbdma.c` ≈ the largest single device in the macio family) | **M** (PEF stub + node + NativeOp; the host backend already exists) |
| **Key risk** | 9.2.x System may sideline ROM DRVRs it doesn't recognize; depends on patch archaeology holding across 9.x ROM revisions | cost lands mostly before any signal; violates §2f's HLE-for-runtime-paths decision and the §9 stop-rule (building with no live consumer) | does 9.2 *Startup* accept a non-ATA ndrv boot volume? (USB/FW boot + the ROM's own USBUnitTableStorageDriver say yes, but unproven for our synthetic node) |

---

## §5. Recommendation + decision evidence

### Recommended default: **(c), with (a)'s repairs banked as shared infrastructure**

Rationale: (c) is the only option consistent with all three standing constraints at once —
§2f's "LLE for what the ROM probes, HLE for what the OS uses" (the OS *uses* the disk; the
ROM probe surface it actually needs — bus, VIA/Cuda, clock — is already modeled), the §9
stop-rule (no DBDMA engine before a frontier demands one), and the no-phantom-capabilities
device-tree rule. It also has the strongest in-tree precedent: the fidelity plan already
commits video to exactly this ndrv seam at M5 ("the future `.ndrv`/Metal seam", plan §2a/M5
row), so a disk ndrv rides the same M5 machinery rather than inventing its own.

Do **not** discard (a): the `find_rom_resource` 9.x-layout fix is required regardless (six
other patch sites silently depend on it), it is small, and it is statically verifiable
against `/tmp/rom901_decompressed.bin` (corrected walk ⇒ 157 entries). Fix it early, keep
the EMUL_OP DRVRs as the fallback if the ndrv route hits a Startup-gate wall.

(b) activates only on evidence: if a tripwire shows the boot **gating on real ATA-bus
presence** (ATA Manager refusing to enumerate any drive absent an `ata` node with real
registers), the first response is still not a DBDMA engine — it's a *minimal* ATA register
stub satisfying the probe (PIO-only, no DMA advertised), per the same probe-vs-use split.

### The first observable ("what will the boot ask through?") and the tripwires to plant

When a far-enough boot wants a disk, the ask will surface as one of four signatures, each
already observable or one cheap hook away:

| # | Tripwire | What it decides | Cost |
|---|---|---|---|
| T1 | `SS_ROM_PATCH_TRACE=1` on the next sanctioned 9.0.1 boot: capture the `find_rom_resource`/sony/`drvr_install` outcomes and the exact `patch_68k` abort point | Confirms/refutes §1.4's static analysis live (it is currently inference, not observation) | zero (exists) |
| T2 | `SS_EMULOP_COUNTS=1` (sheepshaver_glue.cpp, EMULOP-COUNTS line): do *any* 68k EMUL_OPs fire on a parcels boot? | Whether the 0x380000 dispatch table survives on 9.0.1 — gates option (a) and every other 68k HLE shim | zero (exists) |
| T3 | M1 bus per-region fault logging on MacIO **+0x08000** (DBDMA) and **+0x18000–0x21000** (both disputed IDE windows): first guest touch = the ROM probing for real ATA | Whether the ROM-side stack demands LLE (option b trigger) AND settles the §3/Q3 address dispute from the guest's own behavior | zero–S (M1 loud stubs + fault stats exist; ensure both IDE candidate windows are registered as named regions) |
| T4 | Watchpoints on `UTableBase` (0x11c) and `DrvQHdr` (0x308) via `SS_JIT_WATCH_ADDR` | When/whether the Start Manager reaches driver-install and drive-queue population — the stage where ANY disk option becomes consumable | zero (exists) |
| T5 | dev_cuda PRAM read logging for the DefaultStartup/boot-device bytes (`pram_rd` counters exist; add offset histogram if needed) | When boot-volume *selection* starts — the last pre-disk stage | S |
| T6 | Name-Registry lookup logging for `driver,AAPL,MacOS,PowerPC` / `ata` node searches (host-side hook in the registry HLE; on the fidelity path, in the M5 tree publisher) | Whether the NDRV framework comes looking for a disk node — the option-(c) consumer signal | S |

**The eventual milestone's Task 0** = run T1+T2 on one diagnostic boot (zero new code),
land T3's second IDE window + T6's hook, and write down which signature fires first at the
then-current frontier. That converts this recon's static map into a live decision in one
session.

### Honest residues

1. **No live verification.** Every §1.4 claim is static analysis of dumps + code; the
   "wild walk reads ~guest 0xB8800000 and benignly returns 0" step is *inferred* from the
   fact that 9.0.1 boots survive `PatchROM` (a host-side read of unmapped NATMEM would
   otherwise crash before emulation starts). T1 is the check.
2. **EMUL_OP viability on parcels is unproven** (the unconditional 0x36e600/0x380000 table
   writes; HANDOFF:587). If broken, option (a) grows by the table-relocation RE and even
   option (c)'s NativeOp path needs a second look (NativeOps ride a different opcode but
   the same glue).
3. **The boot is nowhere near asking for a disk.** Current frontier is the FE1F service
   surface (plan M6 row); Start-Manager driver install is several walls away. This recon
   de-risks the *plan*, not the *schedule*.
4. **9.2.x System-side behavior** (which ROM drivers it keeps, whether Startup accepts a
   non-ATA boot ndrv, the $76 residue from UPGRADE-CARD-PATH) was not re-derived here;
   SYSTEM-BOOT-GATES covers the gate anatomy but not driver selection policy.
5. **CD-ROM** (ATAPI, `.AppleCD` absent from ROM) and **floppy** were not costed; the
   recommended seam extends to them but each needs its own Driver Gestalt surface.
6. The parcels-TOC and resource-map probes were done with throwaway scripts against the
   /tmp dumps; offsets above are reproducible from the cited files but no tool was
   committed (deliberate — rom-inspect via `rom_decode.hpp` is the right home if one is
   wanted, and `elliotnunn/tbxi` already exists for the container, HANDOFF:866).
