# ROM-Patch Audit — device-neutralizing patches vs the Machine Layer

> Purpose: MACHINE-LAYER-PLAN M0 deliverable (rev-3 finding #16). On the newworld
> fidelity profile, each device model is dead code until the patch that NOPs the
> guest's init of that device is retired. This table assigns every such patch a
> disposition and an owning milestone.
>
> Line numbers are from `SheepShaver/src/rom_patches.cpp` **as of branch
> `machine-layer-m0` (2026-06-10)** — a concurrent edit pass (getenv→profile
> consolidation) may drift them by a few lines; the `*_dat` pattern names are the
> stable keys. Every line number below was verified by reading the code, not
> copied from the rev-3 review.

## Disposition key

- **KEEP** — harmless/required on both profiles (e.g. config-identity fakes, EMUL_OP trap inserts for HLE we keep)
- **RETIRE@Mx** — must be skipped on the newworld profile when milestone Mx's device
  model lands (the milestone's DoD asserts the un-patched init completes)
- **REPLACE@Mx** — the patch is the paravirtual stand-in for a device model
- **PARAVIRTUAL-ONLY** — applies only on the paravirtual profile by nature (skip on newworld from M0)

Milestone owners (MACHINE-LAYER-PLAN §3): M1 = MMIO bus + SCC 8530 + VIA timer/IFR
surface (Cuda loud stub), M2 = virtual clock (TB/DEC), M3a = exception core + DEC delivery
(**complete 2026-06-10; no patches retired in M3a — all RETIRE@M3 rows below are M3b work**),
M3b = OpenPIC + Cuda/ADB + external-source wiring + patch retirements (the RETIRE@M3 cluster),
M4 = NVRAM + MacIO, M5 = supervisor environment + trampoline (publishes device tree),
M6 = PPC→68k handoff + shim triage.

## PatchROM / patch_nanokernel / patch_68k device-neutralizing patches

All rows are in `patch_68k()` unless noted. "Line" = `rom_patches.cpp` as of machine-layer-m0.

| Patch (`name_dat` / site) | Line | What it neutralizes (1-line effect) | Disposition | Owning milestone |
|---|---|---|---|---|
| `mdec_dat` (in `patch_nanokernel()`) | 1642–1655 | Stops the nanokernel from programming the real decrementer (`li r31,0` + NOPs the `mtdec` path) — DEC never ticks under guest control. | **✅ RETIRED@M2 (gate)** (commit `47a82c78`) | M2 ✅ — gate landed: profile check at the patch site skips it on the newworld profile (same idiom as M1's `scc_init` retirement). **Honest scope:** the `mdec_dat` pattern is present in the 1.1/OldWorld NK (ROM offset 0x312c24) but **absent from the 9.0.1 parcels ROM** (byte-verified — the patch was ALWAYS lenient-skipped on the 9.0.1 boot; every prior 9.0.1 run emits `[ROMPATCH] SKIP mdec`). The gate is therefore live for 1.1-ROM newworld only; the 9.0.1 fidelity boot exercises the M2 clock seams directly (not the retirement gate). `1.1-ROM newworld probe 2026-06-10: the [M2] retirement line fired (gate live-verified; the un-patched NK DEC code is now reachable); guest DEC-traffic counters unobserved (run alarm-killed before any dump) — 1.1 traffic verification carried forward` |
| `powermac_id_dat` | 1817–1831 | Replaces the read of the machine-ID register at `0x5FFFFFFC` with `move.l #0x3020,d0` (fake PowerMac 9500 ID). | KEEP | — (machine identity is profile *configuration*, not a device; if a fidelity milestone ever serves `0x5FFFxxxx` through the bus, revisit at M5) |
| `univ_info_dat` + AddrMap construct | 1834–1897 | Rewrites UniversalInfo to PowerMac 9500 values and builds the AddrMap table (`0xF3016000`→VIA, `0xF3012000`→SCC, …) the 68k side uses to locate devices. | KEEP | — (this *is* the device-address contract the M1 bus must serve — SPIKE-S3 §3 cross-checks it; retiring it would un-define where the device models live) |
| `via_init_dat` | 1900–1907 | Branches over the guest's VIA init (via Universal) so boot never programs VIA registers. | RETIRE@M3 | M3 (full VIA/Cuda model). **SPIKE-S3 dependency, see note below table.** |
| `via_init2_dat` | 1909–1916 | Turns the second VIA-init entry into `jmp (a6)` (immediate return). | RETIRE@M3 | M3 (same cluster) |
| `via_init3_dat` | 1918–1925 | Turns the third VIA-init entry into `jmp (a6)` (immediate return). | RETIRE@M3 | M3 (same cluster) |
| `run_diags_dat` (NW 1929–1948 / other 1950–1959) | 1928–1960 | Skips RunDiags (hardware self-test) and instead loads the BootGlobs pointer directly into a6 (`lea RAMBase+RAMSize-0x1c,a6`). | KEEP | — (diags probe physical memory/devices wholesale; no planned milestone models that, and the BootGlobs substitute is required for boot on any profile) |
| `nvram1_dat` | 1963–1971 | Replaces the NVRAM/XPRAM read routine with `M68K_EMUL_OP_XPRAM1` + RTS (host-file-backed XPRAM HLE). | REPLACE@M4 | M4 (full partitioned 8 KB NVRAM behind the bus) |
| `nvram2_dat`–`nvram7_dat` (NW branch) | 1973–2038 | NewWorld NVRAM cluster: read/write/multi-byte ops → `EMUL_OP_XPRAM2/3`, `EMUL_OP_NVRAM3`; NOPs a format check (`nvram5`); stubs NVRAM-clear (`nvram6`); RTS-out the exit path (`nvram7`). | REPLACE@M4 | M4 |
| `nvram2_dat`/`nvram3_dat` (OW branch) + `nvram4_loc`/`nvram5_loc` hardcoded offsets | 2040–2091 | OldWorld equivalents: NVRAM read/write → `EMUL_OP_XPRAM2/3`, `EMUL_OP_NVRAM1/2` at per-ROMType hardcoded offsets. | REPLACE@M4 | M4 (OldWorld ROMs only; not applied on NewWorld) |
| `scc_init_caller_dat` + `scc_init_dat` | 2106–2119 | Locates the SCC init routine via its caller at vector 0x1ac, then replaces it with `M68K_EMUL_OP_RESET` + RTS — the guest never initializes the SCC. | **✅ RETIRED@M1** (commit `0f2e83f2`) | M1 ✅ — SCC 8530 model landed; patch skipped on the newworld profile via profile check at the call site (`[M1] scc_init ROM patch retired` confirmed firing in the acceptance run); guest SCC init now reaches the model. The WR-write-sequence boot telemetry could not be observed live — the acceptance boot dies at the NK boot ceiling (pc=0x503123fc) before any SCC traffic; the write set is covered by the SPIKE-S3 §1.3 conformance vectors in the unit suite. Live confirmation carried forward to when the fidelity boot resumes. |
| Serial driver install (`gen_ain/aout/bin/bout_driver`, in PatchROM driver section) | 2617–2621 (generators 448–597) | Installs replacement `.AIn`/`.AOut`/`.BIn`/`.BOut` drivers whose entry points are `NATIVE_SERIAL_*` TVECTs (host serial HLE) instead of the ROM's SCC drivers. | REPLACE@M1 | M1 lands the SCC chip model; whether the guest's *own* ROM serial drivers replace this HLE is an M6 shim-triage call (drivers are guest software above the device, not the device itself) |
| `ext_cache_dat` (also disables DisableIntSources via 0x1fc) | 2122–2133 | RTS-out EnableExtCache **and** DisableIntSources — guest neither touches L2 cache control nor masks interrupt sources at boot. | KEEP (cache half) / RETIRE@M3 (DisableIntSources half) | L2 cache control is meaningless under emulation; but the same patch suppresses the interrupt-source masking call, which belongs to M3's PIC — split when retired (see ambiguity note) |
| `timek_dat` (SetupTimeK) | 2136–2155 | Replaces the VIA-timed DBRA calibration loops with fixed TimeDBRA/TimeSCCDBRA/TimeSCSIDBRA/TimeRAMDBRA constants. | KEEP | — (DBRA calibration against an emulated clock yields meaningless constants on any profile; the fake values are the right answer under JIT) |
| `gc_mask_dat` | 2269–2286 | NOPs four writes to the Grand Central interrupt-mask register (via 0x262). OldWorld-only (`ROMType != ROMTYPE_NEWWORLD` guard — not applied on NewWorld ROMs). | RETIRE@M3 | M3 (OpenPIC/interrupt-controller model; only relevant if an OldWorld ROM ever runs on the fidelity profile) |
| `gc_mask2_dat` | 2288–2316 | NOPs the longer GC interrupt-mask write sequences (5–11 sites depending on ROMType). OldWorld-only. | RETIRE@M3 | M3 (same) |
| `cuda_init_dat` | 2320–2333 | NOPs 7 words of the Cuda init (VIA shift-register / handshake setup, via 0x274) — guest never brings up Cuda. | RETIRE@M3 | M3 (full Cuda: timers via M2 scheduler, ADB, RTC). **Loud-stub tension:** in M1 Cuda is deliberately a *loud stub* — if this patch were retired at M1, the un-patched init's shift-register traffic hits the stub and aborts/log-spams every boot. The patch must stay until M3 even though the VIA it programs arrives (partially) at M1. |
| `cpu_speed_dat` (×2 occurrences) | 2336–2354 | Replaces GetCPUSpeed (via 0x27a) with `move.l #configured-MHz,d0` + RTS. | KEEP | — (reports configured `CPUClockSpeed`; identity/config, no device behind it) |
| `time_via_dat` | 2357–2366 | Early-returns the InitTimeMgr routine that pokes VIA timer registers — Time Manager never calibrates against VIA T1/T2. | RETIRE@M3 | M3 (timers need the M2 scheduler + full VIA). *Possible early retirement at M1* if the M1 VIA timer/IFR surface proves sufficient — same SPIKE-S3 note as the via_init cluster. |
| `open_firmware_dat` | 2370–2383 | Replaces a read of the OF/Name-Registry pointer at `0xFF800000` with `#0xdeadbeef` and NOPs the FE03 opcode that would jump through it. | RETIRE@M5 | M5 (trampoline handoff publishes the real device tree / OF properties; until then nothing answers at `0xFF800000`) |
| `ext_cache2_dat` | 2386–2393 | RTS-out the second EnableExtCache routine (via 0x2b2). | KEEP | — (same reasoning as `ext_cache_dat`'s cache half) |
| `tm_task_dat` (NW/Gossamer 2397–2409 / other 2410–2421) | 2396–2421 | NOPs the installation of the 60 Hz Time Manager interrupt task (Enable60HzInts, via 0x2b8) — paravirtual injects 60 Hz ticks from the host instead. | RETIRE@M3 | M3 (real delivery: VIA timer → PIC → CPU exception replaces host injection) |

### SPIKE-S3 dependency note (VIA cluster)

`docs/planning/spikes/SPIKE-S3-STALL-DEVICE-PROBE.md` (§4) pulls a **VIA 6522 timer/IFR
surface into M1 scope**: the 68k serial monitor behind the 9.2.1 post-splash stall uses VIA
T2 + IFR as its timeout escape on the bounded-probe path. This does **not** force early
retirement of `via_init`/`via_init2`/`via_init3`: the monitor programs T2/IER itself at use
time (S3 §1.5 — direct writes at 0xcc966), so M1's consumer does not depend on the guest's
*boot-time* VIA init running. The dispositions therefore stay RETIRE@M3 — **but M1's DoD
should explicitly confirm that the un-initialized boot-time VIA state (ACR/IER defaults)
doesn't matter to the T2-timeout path; if it does, the via_init cluster (and `time_via_dat`)
move to RETIRE@M1.** Record the outcome here when M1 lands.

**M1 outcome (2026-06-10):** the acceptance boot terminated before any VIA (or SCC) traffic — the 9.0.1 diagnostic boot crashes at the pre-existing NK boot ceiling (pc=0x503123fc, page-descriptor build loop) before the monitor/idle phase, with SCC/VIA region counters zero on both baseline and accept runs. The boot-time VIA init question is **UNRESOLVED at M1**; dispositions unchanged (RETIRE@M3). Revisit when a boot reaches the monitor/idle phase.

## patch_68k shims (referenced, not re-derived)

`docs/planning/PATCH-68K-SHIM-INVENTORY.md` remains the authoritative per-pattern inventory
of all 84 `find_rom_data` patterns (concept, EMUL_OP, search range, 9.0.1-parcels status);
**M6 owns the shim triage**. Listed here are only the inventory rows that touch
**devices** (Cuda/SCC/VIA/NVRAM) and are not already rows in the main table above:

| Shim (inventory row) | What it does (per inventory) | Device | Disposition | Owning milestone |
|---|---|---|---|---|
| `via_int_dat` | Patches the VIA Level-1 interrupt handler (inline `moveq #2,d0` + NOPs) | VIA | REPLACE@M3 | M3 (paravirtual stand-in for real VIA interrupt decode; retired when the PIC routes a real VIA IFR) |
| `via_int2_dat` | Patches the 60 Hz VIA handler to `M68K_EMUL_OP_IRQ` + tst/beq | VIA | REPLACE@M3 | M3 (the host-injection IRQ path is deleted on the fidelity profile per MACHINE-LAYER-PLAN M3) |
| `via_int3_dat` | Redirects the CHRP Level-1 handler (`M68K_JMP` to level1_int; NW only) | VIA | REPLACE@M3 | M3 |
| `adb_init_dat` | NOPs the wait in ADBInit (via 0x36c) — guest doesn't wait for ADB/Cuda to respond | Cuda/ADB | RETIRE@M3 | M3 (Cuda model answers ADB; until then the wait would spin on the M1 loud stub) |
| `nvram1`–`nvram7`, `nvram4_loc`/`nvram5_loc` | NVRAM/XPRAM HLE EMUL_OP replacements | NVRAM | REPLACE@M4 | (main table above) |
| `scc_init_caller_dat`/`scc_init_dat` | SCC init suppression | SCC | RETIRE@M1 | (main table above) |
| `via_init`/`via_init2`/`via_init3`, `time_via_dat`, `tm_task_dat`, `cuda_init_dat`, `gc_mask`/`gc_mask2` | Boot-time device-init suppression | VIA/Cuda/GC | RETIRE@M3 | (main table above) |

Non-device shims (drivers, Resource Manager, scrap, memory sizing, gestalt, …) are
deliberately out of scope here — see the inventory + M6.

## How dispositions get enforced

The enforcement mechanism is an **M1 design decision** — this document only assigns
dispositions. Candidate mechanisms, to be decided when M1 retires its first patch
(`scc_init`):

1. **Profile check at each site** — `if (MachineProfileIsNewWorld() && milestone_landed) skip;`
   using the M0 `machine_profile` module. Cheapest; matches how the M0 serial-skip/ignoresegv
   gating already works; risks scattering milestone conditions through `rom_patches.cpp`.
2. **Patch-table refactor** — convert `patch_68k()`'s inline sites into a table of
   `{pattern, patch_fn, disposition, owning_milestone}` entries with one gate loop. Cleaner
   audit trail (this table becomes executable), bigger diff; natural to combine with the
   M6 shim triage which needs per-pattern bookkeeping anyway.
3. **Hybrid** — profile checks now (M1, a handful of sites), table refactor when M6's triage
   forces per-pattern metadata regardless.

Whichever lands, each milestone's DoD must assert the **un-patched init actually executes
and reaches the device model** (observed register traffic, per MACHINE-LAYER-PLAN's
M1 false-pass risk row) — not merely that the patch was skipped.
