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
**[RETRACTED 2026-06-11 — see §7: the walker is correct and enumerates all 157 resources;
the layout delta below does not exist. (ii) and (iii) stand.]**
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
**[SUPERSEDED 2026-06-11 — §7: no layout fix exists to make; the surviving residues are
the sony-anchor redesign and the miss-guards, folded into the disk milestone's Task A.]**

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

---

## §6. EMUL_OP-on-parcels verdict (2026-06-11) — **ALIVE**

> Tripwire **T2 fired and is now retired**, and with it residue 2 above and the
> HANDOFF-NEWWORLD-SUPERVISOR-MMU:587 table-layout risk. One slot-protocol boot
> (`ss-slot-boot.sh --label emulop-verdict --timeout 50`, run
> `slot0/20260611-192832.64083`, env `SS_EMULOP_COUNTS=1 SS_ROM_PATCH_TRACE=1` on the
> standard newworld 9.0.1 diagnostic config).

**Verdict: ALIVE.** The full 68k→host EMUL_OP dispatch chain executed end-to-end on the
9.0.1 parcels ROM. Exactly **one** EMUL_OP fired in the 50 s boot: **op 1 = `OP_XPRAM1`**,
once, at ~0.01 s (`EMULOP-COUNTS: 1=1`, the only counts line of the session — i.e. no
further EMUL_OP traffic for the remaining ~49 s).

### Evidence chain (each link observed, not inferred)

1. **[PATCH] Table writes are layout-compatible on 9.0.1.** In `/tmp/rom901.bin`
   (md5 `e432df64…`), slots at `0x380000 + (op<<3)` for op `0xfe40…0xfe4b` read
   `1800000N 4bf66exx` — `POWERPC_EMUL_OP|N` + `b 0x366084` (emul_ret glue), exactly what
   `patch_68k_emul()` (rom_patches.cpp:2040–2050) writes. The RAW ROM (`7b1378be…`) has
   real, fully-populated table content at the same slots (`80bf0808 4bf6e55c` at 0xfe40's
   slot; 131072/131072 nonzero words in `0x380000–0x400000`) — i.e. the 9.0.1 image
   natively keeps the opcode table at the same base with the same `op<<3` geometry; the
   unconditional overwrite lands where it must.
2. **[STATIC] An emitter site exists.** `SS_ROM_PATCH_TRACE` shows the `nvram1` patch HIT
   at ROM offset `0x75d0` (`pat=48e7010e -> HIT @0075d0`, rom_patches.cpp:2714–2722) —
   `M68K_EMUL_OP_XPRAM1` (`0xfe44`) + RTS planted at guest 68k `0x500075d0`. (nvram2–6
   SKIP; most other 68k EMUL_OP emitters are never planted because `patch_68k` still
   aborts at the sony block, §1.4 — unchanged.)
3. **[PROBE✓] The mirror table slot was executed.** The boot log shows
   `first compile in 64KB region 504f0000 (pc=504ff220)` immediately before the counts
   line — `0x50480000 + (0xfe44<<3) = 0x504ff220`, the **mirror** dispatch-table slot for
   `0xfe44`. So the DR emulator fetched the 68k opcode, indexed the live (mirror) table,
   and the JIT compiled+ran the patched slot — confirming the `patch_68k_emul` writes at
   image `0x380000` survive the NK's `+0x100000` mirror copy (matching
   M6A-DR-HANDOFF-ANALYSIS's static finding).
4. **[PROBE✓] The host service ran.** `execute_emul_op` (sheepshaver_glue.cpp:381, the
   `SS_EMULOP_COUNTS` counter at :399–421) counted op 1 → `EmulOp()`/`OP_XPRAM1`
   (emul_op.cpp:573). Neither `execute_sheep`/`execute_emul_op` nor `EmulOp()` carries any
   `MachineProfileIsNewWorld` fence — the service is profile-agnostic (verified by grep).

### What each consumer takes from this

- **Disk milestone (option a / §4):** the "confirmation the EMUL_OP table works on
  parcels" line item in option (a)'s *What's missing* column is **done**. The remaining
  blockers for EMUL_OP DRVRs are purely the §1.4 install chain (`find_rom_resource`
  next-link fix + a new anchor + reaching the Start-Manager install stage) — the dispatch
  seam underneath is proven. Option (c)'s NativeOp concern (residue 2's "same glue") is
  likewise eased: the glue demonstrably executes sheep opcodes from mirror-table slots.
- **Framebuffer milestone (T-F3):** dispatch is alive, and the `name_reg` patch DID hit
  this boot (`pat=70ffabeb -> RELOCATED @0002fa`, exactly as T-F3 predicted) — but
  `OP_NAME_REGISTRY` (op 38) has **not yet fired** (only op 1 ever ran). So the
  stub-injection chain's dispatch half is retired; whether the boot *reaches* the
  name-registry call is a frontier question (NOT-YET-REACHED for op 38 specifically),
  not a machinery question.
- **General:** any future 68k HLE shim on newworld can assume EMUL_OP delivery works;
  zero-EMUL_OP boots mean *no emitter installed/reached*, never a broken table.

### Caveats

- The counts dump is 5 s-throttled and printed inside `execute_emul_op` only; ops landing
  in a trailing <5 s window with no successor wouldn't print. Irrelevant to the verdict
  (one line ⇒ ALIVE) but don't read "1=1" as a hard upper bound without a term-dump
  counter.
- One op, one execution, at the very start of 68k execution — coverage of the other 52
  ops is untested empirically; their slots carry the same verified write pattern.

Boots used: 1 of 3 budgeted (50 s).

---

## §7. find_rom_resource combined-map "fix" spec (2026-06-11) — **§1.4(i) FALSIFIED: there is nothing to fix**

> Stream-C follow-up, tasked to spec the corrected-walk fix that §1.4(i)/§5 banked as
> shared infrastructure ("six other patch sites silently depend on it"). The spec work
> falsified its own premise on the first evidence pass: **`find_rom_resource()`
> (rom_patches.cpp:154–180, re-verified) walks the 9.0.1 combined map correctly today —
> all 157 resources** — statically on both dumps AND live (proof below). Per the
> one-iteration rule this section is the dated re-pin; §1.4(i) is retracted in place.
> What §1.4 keeps: (ii) the anchors really are absent and (iii) `patch_68k()` really does
> abort at the sony block — the abort is an *inventory* problem, not a *walker* problem.

### 7.1 The layout, pinned — 1.1 and 9.0.1 share ONE format; no discriminator exists or is needed

The recon's claimed layout delta ("9.x moved the next-link from entry+0 to entry+8")
does not exist. Both ROMs use the identical combined-map entry record; the walker never
reads entry+0 at all.

Map header (`map` = R32(image+0x1a); 9.0.1: 0xaa2d0, 1.1: 0xd2f90):

```
map+0:  link to FIRST entry (entry start)
map+4:  0x04
map+5:  header_size byte = 0x08 (both ROMs)
```

Entry record (identical bytes-for-fields on both ROMs; [RAW-ROM] 9.0.1 entry #1 @0x257370,
[STATIC] 1.1 entry #1 @0x2115a0 — same `78 00 00 00 | 00 00 00 00` first 8 bytes):

```
entry+0:   0x78000000   header word (combo/attr field — NEVER read by the walker)
entry+4:   0x00000000   header word 2 (never read)
entry+8:   next-link    -> next entry START (0 terminates)
entry+12:  data offset
entry+16:  type (FOURCC)
entry+20:  id (int16)
entry+22:  attr byte (0x58 throughout)
entry+23:  pname (pascal string)
```

What the code actually does (rom_patches.cpp:154–180): position `rsrc_ptr` at
`entry + header_size` (= entry+8, the "body"), then read next-link at body+0 (= entry+8),
data at body+4 (= entry+12), type at body+8 (= entry+16), id at body+12 (= entry+20).
Every field lands exactly on the layout above. The §1.4(i) mis-trace assumed the link
read happens at entry+0; it happens at body+0. The recon's throwaway probe script was
wrong, not the ROM and not the code.

### 7.2 Validation — the EXISTING algorithm, three ways

1. **[RAW-ROM]** exact-arithmetic simulation of rom_patches.cpp:154 against
   `/Users/Shared/macemu/dumps/rom901_inventory.bin` (md5 7b1378be…, manifest --check OK):
   walks **157/157** resources, 39 types, clean 0-link termination at 0xaa338. No wild
   reads, no early exit.
2. **[PATCH]** same walk on `rom901.bin` (md5 e432df64…): 157/157 — no patch disturbs the map.
3. **[STATIC]** same walk on the 1.1 image (decoded offline from
   `/Users/Shared/macemu/1998-07-21 - Mac OS ROM 1.1.rom` via the rom_decode.hpp LZSS
   algorithm; NK id "NewWorld v1.0"): **123/123** resources — proving the classic ROM the
   code has always worked on uses the same record format.
4. **LIVE proof the walker works at runtime on 9.0.1** (this is decisive): the hpchk/macpgm
   patch (rom_patches.cpp:3267) derives its search window from
   `find_rom_resource('nlib', 10)`. The section-6 boot's trace
   (`/tmp/ss-slots/slot0/runs/20260611-192832.64083/boot.log`) shows
   `find_rom_data [16a290,16d290) … pat=80800316 -> HIT @16cb64` — window base 0x16a290
   IS StdCLib's data offset, reachable only by walking 48 entries deep. And
   `rom901.bin` @0x16cb64 reads `80 80 28 50` (lwz r4,XLM_ZERO_PAGE) vs raw `80 80 03 16`
   — the patch **applied**. The recon's "every find_rom_resource-relative patch has always
   no-op'd on 9.0.1" was wrong: macpgm works today; dsl is merely ROMType-gated off on
   NEWWORLD (:3221); SERD is simply never reached (sony abort comes first).

### 7.3 The 157-resource inventory (raw 9.0.1 image; id 'name' @data-offset)

Disk recon answers up front: **.ATALoad (DRVR -20175), .ATADisk (DRVR 53), .EDisk
(DRVR 48) all present** as §2.2 said; **no DRVR 4 and no ndrv -20196** (so §1.4-ii stands
and the sony anchor abort is real). Display-shaped, for the framebuffer milestone:
**ndrv -16515 '.BCScreen'** (a ROM-resident video ndrv, data 0xc97e0, extent ≤0x3810),
nlib VideoServicesLib (-16403) + VideoServicesGlobals (-16405), 'gama' StdGamma, 7 cluts.
Also notable: nlib -20186 'ATAManager', nlib -16402 'DriverLoaderLib', nsrd 1 'SerialDMA',
ndrv 'USBUnitTableStorageDriver'. Absent vs 1.1: SERD, sl05, thng (all three), DRVR 4 x2.

| type | n | resources |
|---|---|---|
| `ndrv` | 8 | -20994 'sbp609e,104d8' @0x248120 · -21143 'fw609e,10483' @0x243770 · -20777 'USBUnitTableStorageDriver' @0x227ce0 · -20776 'pciclass,0c0310' @0x1fb0a0 · -20164 'media-bay' @0x1cd6e0 · -20181 'pccard-ata' @0x1ca970 · -20166 'DefaultPCCardEnabler' @0x1c9ec0 · -16515 '.BCScreen' @0x0c97e0 |
| `gpch` | 1 | 1207 'Main' @0x248060 |
| `frag` | 3 | -21142 'sbp609e,104d8' @0x245f20 · -21141 'FWExpertRegistration' @0x245b60 · -21140 'FWPCIScanner' @0x2459a0 |
| `fexp` | 2 | -21141 'GenericDriverFamilyExpert' @0x2456e0 · -21140 'ComponentDriverExpert' @0x2447e0 |
| `usbd` | 7 | -20782 'USBMassStorageVSDriver' @0x23f980 · -20781 'USBMassStorageClassDriver' @0x23bad0 · -20780 'USBCompositeDriver' @0x222e10 · -20779 'USBHIDMouseModule' @0x220520 · -20778 'USBHIDKeyboardModule' @0x21d640 · -20777 'USBHubDriver1' @0x214220 · -20776 'USBHubDriver0' @0x20ade0 |
| `usbs` | 3 | -20776 'USBMassStorageLoader' @0x238600 · -20782 'USBShimMouse' @0x226860 · -20781 'USBShimKeyboard' @0x224e20 |
| `usbf` | 1 | -20776 @0x224dc0 |
| `nlib` | 21 | -20778 'USBManagerLib' @0x1fa6b0 · -20777 'USBFamilyExpertLib' @0x1e89a0 · -20776 'USBServicesLib' @0x1d7ec0 · -20186 'ATAManager' @0x1d2a20 · -16411 'PowerMgrLib' @0x197000 · -16405 'VideoServicesGlobals' @0x196ef0 · -16403 'VideoServicesLib' @0x195710 · -16404 'PCILib' @0x191570 · -16402 'DriverLoaderLib' @0x18ae50 · -16401 'DriverServicesLib' @0x182fc0 · -16407 'DSLGlobalsLib' @0x182950 · -16400 'NameRegistryLib' @0x17ed40 · -20264 'CursorDevicesLib' @0x17dd10 · -16420 'Math64Lib' @0x17d070 · 10 'StdCLib' @0x16a290 · 9 'MathLib' @0x14ea50 · 8 'MathLibGlobals' @0x148290 · 7 'BootStdCLib' @0x142c90 · 6 'PrivateInterfaceLib' @0x134990 · 5 'InterfaceLib' @0x0f17d0 · 3 'MPSharedGlobals' @0x0dad00 |
| `code` | 1 | -20164 'Main' @0x1d25a0 |
| `gcko` | 1 | 43 'Main' @0x1c8f50 |
| `nitt` | 1 | 43 'Native 4.3' @0x1b9730 |
| `ncod` | 5 | 50 'NativeNub' @0x19de70 · 8 'ProcessMgrSupport' @0x0f0c40 · 1 'MixedMode' @0x0eb880 · 0 'CodeFragmentMgr' @0x0db240 · 2 'MPLibrary' @0x0ce7b0 |
| `DRVR` | 3 | -20175 '.ATALoad' @0x19c6e0 · 53 '.ATADisk' @0x198a60 · 48 '.EDisk' @0x0b60d0 |
| `scod` | 2 | -20984 @0x1988b0 · -20961 @0x1987e0 |
| `ntrb` | 1 | -16400 'NameRegistryTraps' @0x17e4f0 |
| `cfrf` | 1 | 0 @0x0db1f0 |
| `GARY` | 1 | 1 'Main' @0x0ccff0 |
| `dfrg` | 1 | -20722 '.LANDisk' @0x0c1ec0 |
| `nsrd` | 1 | 1 'SerialDMA' @0x0bac70 |
| `PACK` | 3 | 7 'Main' @0x0ba6f0 · 5 'Main' @0x0b94b0 · 4 'Main' @0x0b7400 |
| `mitq` | 1 | 0 @0x0b73c0 |
| `gama` | 1 | 0 'StdGamma' @0x0b7280 |
| `clut` | 7 | 127 @0x0b7200 · 8 @0x0b69c0 · 4 @0x0b6900 · 2 @0x0b68a0 · 1 @0x0b6850 · 9 @0x0b39f0 · 5 @0x0b3930 |
| `PICT` | 12 | 106 'DiskMode 6' @0x0b5fc0 · 105 'DiskMode 5' @0x0b5eb0 · 104 'DiskMode 4' @0x0b5da0 · 103 'DiskMode 3' @0x0b5c90 · 102 'DiskMode 2' @0x0b5b80 · 101 'DiskMode 1' @0x0b5a70 · 100 'DiskMode 0' @0x0b5960 · 99 'DiskMode Battery' @0x0b5850 · 98 'DiskMode Arrow3' @0x0b56e0 · 97 'DiskMode Arrow2' @0x0b5570 · 96 'DiskMode Arrow1' @0x0b5400 · 95 'DiskMode SCSI' @0x0b4c80 |
| `pixs` | 12 | -10208 @0x0b4bc0 · -10207 @0x0b4b10 · -10206 @0x0b4a50 · -10205 @0x0b4990 · -10204 @0x0b48d0 · -10203 @0x0b4810 · -10202 @0x0b4750 · -10201 @0x0b4690 · -10200 @0x0b45d0 · -10199 @0x0b4510 · -14334 @0x0b4490 · -14335 @0x0b4410 |
| `ppat` | 2 | 18 @0x0b4320 · 16 @0x0b4230 |
| `cicn` | 4 | -20020 @0x0b3540 · -20021 @0x0b3170 · -20022 @0x0b2da0 · -20023 @0x0b29d0 |
| `ics8` | 1 | -16386 @0x0b28a0 |
| `ics4` | 1 | -16386 @0x0b27f0 |
| `ics#` | 1 | -16386 @0x0b2780 |
| `accl` | 9 | 9 @0x0b2230 · 8 @0x0b2050 · 7 @0x0b1fb0 · 6 @0x0b1f10 · 5 @0x0b1eb0 · 4 @0x0b1e50 · 2 @0x0b1db0 · 1 @0x0b1d10 · 0 @0x0b1cb0 |
| `KCAP` | 13 | 206 @0x0b1940 · 205 @0x0b1600 · 204 @0x0b12c0 · 200 @0x0b0f90 · 199 @0x0b0c80 · 198 @0x0b0990 · 17 @0x0b0780 · 16 @0x0b0580 · 14 @0x0b0420 · 5 @0x0b0100 · 4 @0x0afe90 · 2 @0x0afb80 · 1 @0x0af910 |
| `KMAP` | 9 | 206 @0x0af850 · 205 @0x0af790 · 204 @0x0af6d0 · 200 @0x0af610 · 199 @0x0af550 · 198 @0x0af490 · 27 @0x0af3d0 · 2 @0x0af310 · 0 @0x0af250 |
| `vadb` | 6 | 5 'ANSI Andy' @0x0af200 · 4 'JIS Andy' @0x0af1c0 · 3 'ISO Andy' @0x0af180 · 2 'ANSI Cosmo' @0x0af130 · 1 'JIS Cosmo' @0x0af0e0 · 0 'ISO Cosmo' @0x0af090 |
| `KCHR` | 1 | 0 'U.S.' @0x0aead0 |
| `snd ` | 1 | 1 'Simple Beep' @0x0ad490 |
| `FONT` | 4 | 521 @0x0acab0 · 396 @0x0abe10 · 393 @0x0ab3b0 · 12 @0x0aa560 |
| `CURS` | 4 | 4 @0x0aa4e0 · 3 @0x0aa460 · 2 @0x0aa3e0 · 1 @0x0aa360 |
| `rovm` | 1 | 0 @0x0aa2f0 |
(Reproducible from the manifest dumps with the §7.1 walk: start = R32(0x1a); repeat
{next = R32(p); stop if 0; entry = next; p = entry+8; fields at entry+12/16/20/23}.)

### 7.4 Blast radius — all find_rom_resource callers, re-dispositioned

All call sites in rom_patches.cpp (grep-verified, 2026-06-11). "Reached" = on a 9.0.1
NEWWORLD lenient boot today, where patch_68k aborts at the sony block (:3413).

| # | Site (line) | Lookup | 9.0.1 result | Reached? | Disposition |
|---|---|---|---|---|---|
| 1 | dsl_pvr/dsl_bus (:3222) | nlib -16401 | FOUND (DriverServicesLib @0x182fc0) | no — ROMType gate excludes NEWWORLD/GOSSAMER | none; correct as-is |
| 2 | InterruptTreeTNT (:3261) | nlib -16408 | ABSENT | no — ZANZIBAR-only | latent unguarded write (`+0x16c` from offset 0) on a hypothetical Zanzibar map miss; out of scope |
| 3 | hpchk/macpgm (:3267) | nlib 10 | FOUND (StdCLib @0x16a290) | **yes — patch APPLIES today** (7.2 item 4) | none; working |
| 4 | sony anchor (:3407–3417) | DRVR 4 (x2, cont), then ndrv -20196 | ALL ABSENT | yes — **the abort point** | the real disk-milestone work: anchor redesign (7.5) |
| 5 | SERD (:3455) | SERD 0 | **ABSENT** (present in 1.1 @0xe43e0) | no (post-abort) | **HAZARD**: result used unguarded — a miss writes `M68K_RTS` at `ROMBaseHost+0` (68k reset-vector area). Becomes LIVE the moment the sony abort is fixed. Needs a guard + profile decision |
| 6 | sl05 (:3458, :3464) | sl05 2 | ABSENT | no — other ROMType branch | unguarded too (writes at +0xc4/+0x8ee from offset 0) but branch never taken on NEWWORLD |
| 7 | nsrd rename (:3467) | nsrd 1 | FOUND (SerialDMA @0xbac70) | no (post-abort) | will fire once sony is fixed: renames type to 'xsrd' via `rsrc_ptr+8` (= entry+16, correct on this layout). Same intent as classic (suppress native SerialDMA so HLE serial wins) — wanted on newworld only if serial stays HLE; flag for the M-profile review |
| 8 | thng sound sifters (:3617, :3628, :3634) | thng any | ABSENT (no thng at all) | no (post-abort) | benign: loop body never entered, num_sifters stays 0 — ROM carries no sound components; audio comes from the System file |

Net blast radius of "lookups start succeeding": **zero today** — they already succeed.
The behavior change everyone should plan for is the **sony-abort fix** (whatever form it
takes): it un-dams sites 5–8, of which site 5 is a guaranteed ROM-offset-0 corruption on
9.0.1 unless guarded first.

### 7.5 What the disk milestone's Task A actually inherits

The §4 option-(a) "What's missing" column loses its first item and gains precision:

1. ~~find_rom_resource 9.x-layout fix~~ — **does not exist; retracted.**
2. **Anchor redesign** (unchanged, now with measured candidates). The sony block needs
   ~0xf02 contiguous bytes (drivers at +0x000/+0x100/+0x200, serial +0x300–0x700, icons
   +0x800–+0xe00+258). Candidate carve-outs on 9.0.1, by data-offset extent:
   `.ATADisk` ≤0x3c80 (but destroys the LLE/ndrv-path ATA driver), `.BCScreen` ≤0x3810
   (destroys the ROM video ndrv — collides with the framebuffer milestone),
   `.ATALoad` ≤0x1790 (destroys the ATA probe INIT); `.EDisk` ≤0x780 is TOO SMALL.
   None is free; synthesizing a new map entry over unused ROM space (the map format is
   now fully pinned, §7.1 — append an entry record + relink) is the clean option and
   needs a free-space census, which is Task A work.
3. **Miss-guards** for sites 5/6/2 (S-class, ~10 lines): treat lookup==0 as
   skip-with-warning under lenient mode, mirroring the find_rom_data pattern. Must land
   BEFORE or WITH any sony-abort fix (7.4 site 5).

### 7.6 Test contract (for the residual S-class work above)

- **Walker regression pin** (pure-function, no boot): `rom-inspect`
  (SheepShaver/rom-inspect/, shares rom_decode.hpp) grows a `--resources` enumeration
  subcommand using the §7.1 walk; offline asserts: 9.0.1 raw dump => 157 resources /
  39 types / terminates at a 0-link (no out-of-image read); 1.1 => 123. Guards against
  anyone "fixing" the walker per the retracted §1.4(i).
- **Miss-guard gate**: lenient 9.0.1 A/B — patched ROM dump byte at offset 0 unchanged
  (`4e` never written) once sites 5/6 are guarded and the sony abort is lifted; plus the
  standard per-commit tier (build-ss, test-jit 353/353, machine tests).
- **Paravirtual-unchanged gate**: 1.1 path must stay byte-identical — `SS_DUMP_ROM` A/B
  md5 on the 1.1 config before/after the guards (guards are miss-paths; 1.1 lookups all
  hit, so the dump must not move).

### 7.7 Recommendation

**Do not open a standalone "combined-map fix" task — it has no content.** Fold the two
real residues into the disk milestone's Task A: (i) the miss-guards (7.5 item 3, gated by
the 7.6 contract) as Task A's first commit, since the sony-anchor work that Task A exists
to do is exactly what arms the site-5 hazard; (ii) the anchor redesign with the 7.5
candidate table as its starting point. The walker regression pin in rom-inspect is
optional-but-cheap (S); bank it with Task A, not before. Update reads of this doc: §1.4(i)
and the §5 "fix it early" paragraph are superseded by this section; §1.4(ii)/(iii)
conclusions stand and the §5 option-(c) recommendation is unaffected (it never depended
on the walker).

Falsification bookkeeping (one-iteration rule): contract falsified once (the §1.4(i)
premise), re-pinned here with three independent evidence classes (7.2); no second
iteration needed. Boots used: 0 (static; live evidence reused from the §6 run's log).
