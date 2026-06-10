# Core99 Machine Description (Machine Layer §2a contract)

> **Status:** 🟢 v1 — M0 deliverable · **Created:** 2026-06-10
> · **Purpose:** the single versioned description of the machine the `newworld` fidelity
> profile emulates. Milestones M1–M5 of `docs/planning/MACHINE-LAYER-PLAN.md` implement
> against THIS file; change it by PR, not drift.
>
> **Sourcing rule (binding):** every address/IRQ row cites its source (file:line or URL).
> Anything that could not be sourced is in §5 Open questions — never guessed. This project
> has flip-flopped on device identities three times (see `SPIKE-S3-STALL-DEVICE-PROBE.md`
> header); an honest gap beats a confident wrong address.
>
> **QEMU citations:** fetched 2026-06-10 from `master`
> (https://raw.githubusercontent.com/qemu/qemu/master/hw/ppc/mac_newworld.c,
> https://raw.githubusercontent.com/qemu/qemu/master/hw/misc/macio/macio.c,
> https://raw.githubusercontent.com/qemu/qemu/master/include/hw/misc/macio/macio.h).
> Line numbers refer to master as of that date; `mac_newworld.c` last touched by commit
> `af2f0774cc43`. SheepShaver citations are this repo (`macos-arm64` / `machine-layer-m0`).

---

## 1. Physical address map

The contract fixes the MacIO container at **0xF3000000** — the value SheepShaver's own
AddrMap patch already tells the guest (`SheepShaver/src/rom_patches.cpp:1877–1896`), which
S3 §3 cross-checked against the canonical Gossamer/Heathrow map (Heathrow datasheet / QEMU
`macio` / DingusPPC `heathrow.cpp` agree on the intra-MacIO offsets). Note an honest
asterisk: QEMU `mac99` does **not** hard-place MacIO at 0xF3000000 — there it is a PCI BAR
(`pci_register_bar`, macio.c:114) assigned by firmware. 0xF3000000 is *our* fixed placement,
consistent with what our guest already believes (see §5 Q1).

| Region | Base | Size | Kind | Source |
|---|---|---|---|---|
| Guest RAM | `0x10000000` (`RAM_BASE`) | `RAMSize` (pref) | RAM | `SheepShaver/src/Unix/main_unix.cpp:187` (const), `:1469–1474` (fixed acquire). Alternate non-fixed path sets `RAMBase = Host2MacAddr(...)` (`:1456–1463`); the macOS arm64 build uses the fixed path. |
| Guest ROM | `0x50000000` (`ROM_BASE`) | `0x500000` (`ROM_AREA_SIZE`) | ROM | `SheepShaver/src/Unix/main_unix.cpp:189`, `:1495–1502`; `SheepShaver/src/include/cpu_emulation.h:31` |
| **MacIO (KeyLargo-class) container** | `0xF3000000` | `0x80000` | trapped-MMIO container (children below) | base: AddrMap `lp[43]` `rom_patches.cpp:1895` + S3 §3 row "+0x00000"; size: QEMU `macio.c:367` (`memory_region_init(&s->bar, …, 0x80000)`) |
| MacIO interrupt regs (Heathrow-style) | `0xF3000000` (+0x00000) | — | trapped-MMIO; Heathrow layout only — NewWorld KeyLargo uses the OpenPIC below instead | AddrMap `lp[43]` `rom_patches.cpp:1895`; S3 §3 |
| DBDMA channel block | `0xF3008000` (+0x08000) | — | trapped-MMIO, **abort-loudly stubs only until M4** | QEMU `macio.c:102` (`memory_region_add_subregion(&s->bar, 0x08000, …dbdma…)`) |
| MESH SCSI | `0xF3010000` (+0x10000) | — | trapped-MMIO (stub) | AddrMap `lp[25]` `rom_patches.cpp:1891`; S3 §3 |
| BMAC ethernet | `0xF3011000` (+0x11000) | — | trapped-MMIO (stub; real net stays HLE, §3) | AddrMap `lp[34]` `rom_patches.cpp:1892`; S3 §3 |
| **ESCC legacy/compat port** | `0xF3012000` (+0x12000) | 256 B | trapped-MMIO — **M1 SCC model**. Register layout: ch B ctrl +0, ch A ctrl +2, ch B data +4, ch A data +6 | AddrMap `lp[3]`/`lp[4]` (SCCRd = SCCWr) `rom_patches.cpp:1888–1889`; QEMU `macio.c:81` (subregion at 0x12000) + alias map `macio.c:59–69` (legacy 0x00→Cmd B, 0x02→Cmd A, 0x04→Data B, 0x06→Data A); S3 §1.4/§2.1/§3 |
| ESCC MacRISC-native port | `0xF3013000` (+0x13000) | — | trapped-MMIO; 0x10 register stride (`it_shift=4`). **Not used by either S3 consumer** — decode + loud-stub only | QEMU `macio.c:89` (subregion at 0x13000), `macio.c:107` (`it_shift, 4`); S3 §3 ("not used by these consumers") |
| AWACS sound | `0xF3014000` (+0x14000) | — | trapped-MMIO (stub; audio stays HLE, §3) | AddrMap `lp[39]` `rom_patches.cpp:1894`; S3 §3 |
| +0x15000 region | `0xF3015000` | — | trapped-MMIO — **identity disputed**: SWIM3 floppy (Heathrow, our AddrMap) vs "timer" (QEMU NewWorld macio). See §5 Q2 | AddrMap `lp[38]` `rom_patches.cpp:1893` + S3 §3 (SWIM3); QEMU `macio.c:304` (timer at 0x15000 in `macio_newworld_realize`) |
| **VIA-Cuda** | `0xF3016000` (+0x16000) | `0x2000` (16 regs × 0x200 stride) | trapped-MMIO — **M1 timer/IFR surface; Cuda SR protocol = loud stub** | AddrMap `lp[2]` `rom_patches.cpp:1887`; QEMU `macio.c:344` (cuda at 0x16000) / `macio.c:329` (pmu same slot); S3 §1.5 (0x200 stride observed) |
| IDE | `0xF3018000` (+0x18000) | — | trapped-MMIO (stub) — QEMU places IDE at +0x20000/+0x21000 instead; see §5 Q3 | AddrMap `lp[24]` `rom_patches.cpp:1890`; S3 §3; conflict: QEMU `macio.c:199` (`0x1f000 + ((index + 1) * 0x1000)`) |
| **OpenPIC** | `0xF3040000` (+0x40000) | not pinned (see §5 Q6) | trapped-MMIO — M3 interrupt architecture; KeyLargo OpenPIC model | QEMU `macio.c:275` (`OPENPIC_MODEL_KEYLARGO`), `macio.c:278–279` (subregion at 0x40000) |
| NVRAM | **unresolved** — QEMU mac99 puts it at `0xFFF04000` *outside* MacIO; OldWorld macio has it at +0x60000 | 8 KB partitioned (M4) | trapped-MMIO (M4) | QEMU `mac_newworld.c:157` (`nvram_addr = 0xFFF04000`), `:449` ("The NewWorld NVRAM is not located in the MacIO device"), `:460`; OldWorld: `macio.c:176` (0x60000). Decision deferred to M4 — §5 Q4 |
| Uni-North host-bridge ctrl regs | `0xF8000000` | — | trapped-MMIO (stub; installer "VM settings" code pokes it) | QEMU `mac_newworld.c:291` (subregion at 0xf8000000); AddrMap `lp[48]`/`lp[-1]` `rom_patches.cpp:1896,1885`; legacy skip hacks match `gpr==0xf8000000` at `sheepshaver_glue.cpp:930–936` |
| `lp[0]` slot | `0xFFC00000` | — | **identity unconfirmed** — §5 Q5 | AddrMap `lp[0]` `rom_patches.cpp:1886` |
| Boot framebuffer aperture | **placeholder — base NOT final** | — | mapped-aperture (fast path, no per-access trap) | finalized in M5 per `MACHINE-LAYER-PLAN.md` §2a/§2f; deliberately unpinned here — §5 Q9 |

Constraint carried from `MACHINE-LAYER-PLAN.md` §2b (via S3 §3): all polled offsets in the
0xF3000000–0xF3020000 block are 16 KB-alignable for the MMIO bus — the whole block is device
space, no RAM interleaved.

## 2. Interrupt tree

PIC = the KeyLargo-model OpenPIC at MacIO+0x40000 (§1). Input numbers below are QEMU's
NewWorld wiring; they are the conformance reference until cross-checked against
DingusPPC/Apple sources (§5 Q8).

| Source device | PIC input # | Source |
|---|---|---|
| ESCC channel B | `0x24` (36) | `include/hw/misc/macio/macio.h:53` (`NEWWORLD_ESCCB_IRQ`); wired `macio.c:282` |
| ESCC channel A | `0x25` (37) | `macio.h:54` (`NEWWORLD_ESCCA_IRQ`); wired `macio.c:283` |
| VIA-Cuda | `0x19` (25) | `macio.h:51` (`NEWWORLD_CUDA_IRQ`); wired `macio.c:343` (PMU variant uses the same input: `macio.h:52`, `macio.c:328` — we model the Cuda config) |
| IDE bus 0 | `0xd` / DMA `0x2` | `macio.h:55–56`; wired `macio.c:286–289` |
| IDE bus 1 | `0xe` / DMA `0x3` | `macio.h:57–58`; wired `macio.c:293–296` |
| GPIO1 / GPIO9 (PMU configs only — not ours) | `0x2f` / `0x37` | `macio.h:59–60`; wired `macio.c:313–317` |
| PCI bridge IRQ lines A–D (uni-north, AGP, internal) | `0x1b`–`0x1e` | `mac_newworld.c:364–366` (and repeated for the AGP/internal bridges, `:371–381`) |
| DBDMA channel completion | via the DBDMA controller; **stubs until M4**, per-channel inputs recorded when M4 lands | `MACHINE-LAYER-PLAN.md` M4 row; QEMU wires DMA IRQs through `macio_realize_ide` (`macio.c:121–134`) |

**PIC output → CPU:** OpenPIC `OPENPIC_OUTPUT_INT` drives the CPU's external-interrupt input
(`PPC6xx_INPUT_INT`) — i.e. the PPC **external interrupt exception, vector 0x500**
(`mac_newworld.c:253–266`).

**DEC is explicitly NOT a PIC input.** The decrementer is a CPU-internal exception
(vector 0x900) raised by the virtual clock (M2) and delivered by the exception architecture
(M3) — it never passes through the OpenPIC. Source: `MACHINE-LAYER-PLAN.md` §2c ("DEC is a
CPU-internal exception (vector 0x900)…", line ~209) and S3 §2.4 (check_work's timeout loop
reads SPR 22 directly).

## 3. Device-tree skeleton

The node set the M5 Trampoline synthesis publishes. Property *names* cross-checked against
what SheepShaver already fakes in `DoPatchNameRegistry()`
(`SheepShaver/src/name_registry.cpp:85–388`); `reg` values come from §1, `interrupts` from §2.

| Node | `device_type` | `reg` (base, size) | `interrupts` | Source for names/values |
|---|---|---|---|---|
| `/` (device-tree root) | — | — | — | props `model`, `compatible`, `clock-frequency` already faked: `name_registry.cpp:92–121` (incl. the `SS_NW_MODEL` PowerMac3,1 `compatible` list, `:109–118`) |
| `/PowerPC,G4` (cpu) | `cpu` | `(0, 0)` | — (DEC/external are CPU exceptions, §2) | `name_registry.cpp:131–185` (`clock-frequency`, `bus-frequency`, `timebase-frequency`, `cpu-version`, cache/tlb props `:298–328`, `reg` `:352–353`) |
| `/memory` | `memory` | `(RAMBase, RAMSize)` = `(0x10000000, pref)` | — | `name_registry.cpp:356–362`; base §1 |
| `/AAPL,ROM` | `rom` | `(0x50000000, 0x500000)` | — | `name_registry.cpp:123–129`; values §1. ⚠️ SheepShaver currently publishes `reg=(ROMBase, ROM_SIZE=0x400000)` (`cpu_emulation.h:29`), not 0x500000 (ROM_AREA_SIZE) — divergence to reconcile when M5 synthesizes this node |
| `/mac-io` | `mac-io` | `(0xF3000000, 0x80000)` | — (it *contains* the PIC) | base/size §1; node name per real NewWorld trees & QEMU macio ("mac-io" PCI device, `macio.c` header comment `:44–53`) |
| `/mac-io/interrupt-controller` (OpenPIC) | `open-pic` | `(0xF3040000, see §5 Q6)` | — | §1 OpenPIC row |
| `/mac-io/escc` (+ `ch-a`/`ch-b` children on real trees) | `escc` | legacy `(0xF3012000, 0x100)`, native `(0xF3013000, …)` | ch A `0x25`, ch B `0x24` | §1 ESCC rows; §2 |
| `/mac-io/via-cuda` | `via-cuda` | `(0xF3016000, 0x2000)` | `0x19` | §1 VIA row; §2 |
| `/mac-io/nvram` | `nvram` | **unresolved — §5 Q4** | — | §1 NVRAM row |
| `/video` (display) | `display` | aperture placeholder — §5 Q9 | — | `name_registry.cpp:364–373` (`AAPL,connector`, `driver,AAPL,MacOS,PowerPC` ndrv blob, `model`) |
| `/ethernet` | `network` | — (HLE) | — | `name_registry.cpp:375–385` |

**Display-node constraint (binding):** the display node is HLE-backed video and **MUST NOT
advertise DBDMA channels** or any other LLE capability we don't model — no phantom
`dma-channels`/DBDMA `reg` entries. Source: `MACHINE-LAYER-PLAN.md` §2a/§2f ("HLE-backed
devices must NOT advertise capabilities (e.g. DBDMA channels) that we don't model",
line ~130, and §2f line ~275). The same rule applies to `/ethernet`.

**Parcels/override handshake (from S1):** the 9.x boot gate audits CFM boot fragments
registered from **ROM parcels** via the override-resource family
`'sfvr'/'fovr'/'nlib'/'ntrb'/'ncod'` (`SPIKE-S1-QEMU-GATE-CHECK.md` §2.1 step 3, §4 "Bonus
for M0/M5"). The M5 Trampoline-handoff component must preserve that handshake alongside this
node list; `gestaltMachineType=406` selects the *short* 3-entry checklist (S1 §2.1 step 1).

## 4. What the 9.0.1 ROM actually probes (evidence-driven scope fence)

This is the M1 "implement exactly this" fence: registers **not** in this table get
abort-loudly stubs. It covers **both** S3 consumers — (a) the 68k serial test monitor
(the 9.2.1-on-1.1-ROM post-splash stall; M1's bus/device *testbed*, not a route to 9.2)
and (b) the 9.0.1 nanokernel `check_work` poll + idle loop.

| Probe site | Device / register touched | Evidence |
|---|---|---|
| **(a) STM hw-init, 1.1 ROM `0xcc81a–0xcc838`** | none (reads the AddrMap config record: +0x08 VIA1, +0x0C SCCRd, +0x10 SCCWr) | S3 §1.2 disassembly; matches AddrMap slots `rom_patches.cpp:1887–1889` and runtime probe r18/r19 |
| (a) STM SCC init, `0xcc83c–0xcc856` | SCC ch A **control +2**: WR-pointer writes — WR9=0xC0, WR15=0, WR4=0x4C, WR11=0x50, WR14=0/1, WR12=0x04, WR13=0, WR10=0, WR3=0xC1, WR5=0xEA, WR1=0; then **data +6** read (Rx flush) | S3 §1.3 (init table at 0xcc86e) — these 12 pairs are the M1 conformance write-vector |
| (a) STM poll loop, `0xcc992–0xcc9c0` (the observed HOT-PC stall) | SCC ch A: **RR0 bit 0** (Rx Char Available) read at +2; on success WR0←1, **RR1 bits 4–6** (error bits) read at +2, WR0←0x30 (Error Reset) on error, **data +6** read | S3 §1.4; unbounded caller at 0xcc9e0/0xcc9f2 loops on it forever |
| (a) STM timeout helper + Cuda transfer (sibling paths, same module) | VIA at 0xF3016000, 0x200 stride: **ACR +0x1600** (←0), **IER +0x1c00** (←0x20), **T2C-L/H +0x1000/+0x1200** (←0xFF), **IFR bit 5 +0x1a00** (T2 poll), **IFR bit 2** (SR complete), **SR +0x1400** (Cuda byte), **ORB bits 3/4 +0x0000**, **DDRA +0x0600 / ORA-no-handshake +0x1e00** | S3 §1.5 table. M1 scope: timer/IFR surface real; Cuda SR protocol = loud stub but register *decode* must exist (S3 §4.2) |
| **(b) `check_work` Rx poll, 9.0.1 ROM `0x503268d4–0x503268e4`** | SCC (base from `[KDP-0x900]`): `lbz +2` → **RR0 bit 0**; `lbz +6` → data | S3 §2.1 disassembly; identity SCC confirmed (HANDOFF-NEWWORLD-SUPERVISOR-MMU §1.7.1 #3) |
| (b) `check_work` Tx, `0x5032695c–0x5032696c` | SCC: **RR0 bit 2** (Tx Buffer Empty) spin at +2; `stb +6` data write | S3 §2.2 |
| (b) `check_work` SCC init, `0x50326980` | SCC ctrl +2, WR-pointer pairs (each prefixed by an `lbz +2` pointer-reset, `eieio` barriers): WR9=0x80, WR4=0x48, WR3=0xC0, WR5=0x60, WR9=0, WR10=0, WR11=0x50, WR12=0x0C, WR13=0, WR14=0x01, WR3=0xC1, WR5=0xEA | S3 §2.3 (= HANDOFF §1.7.1 #3's sequence, fully decoded); second M1 conformance write-vector |
| (b) `check_work` drain/timeout, `0x50326520–0x5032656c` | **DEC (SPR 22)** read twice (deadline math) — NOT a device register; plus SCC WR0←1, **RR1 bit 0** (All Sent) poll at +2 | S3 §2.4. ⇒ M1 needs a minimal **ticking DEC** (M2 dependency pulled forward — `MACHINE-LAYER-PLAN.md` M1 row) |
| (b) idle loop, `0x5032751c` (yield primitive `0x503272e0`) | **only** the SCC via `check_work` — no VIA, no timer, no task-queue MMIO | S3 §2.5; HANDOFF §1.7.1 #4 |

Notes for M1 implementers:
- Both consumers use the **legacy/compat ESCC port layout** (ch A ctrl +2 / data +6) at one
  base (SCCRd = SCCWr = 0xF3012000), never the 0x10-stride MacRISC port (S3 §3).
- Required SCC behavior set, distilled (S3 §4.1): WR register-pointer state machine; WR0
  commands 0x30 (Error Reset) / 0x80 (ch-A reset) / 0xC0 (hw reset); RR0 bit 0 (Rx avail) +
  bit 2 (Tx empty = 1); RR1 bit 0 (All Sent = 1) + bits 4–6 (errors = 0); WR1/3/4/5/9/10/
  11/12/13/14/15 as stored state. Channel B: the monitor's init prologue does READ ch-B
  control (+0) once (`tst.b (a3,d3.l)` before the init table — S3 §1.3) but never writes
  it — same decoder, stub state must tolerate the +0 read.
- The legacy gpr-pattern serial-skip hacks (`sheepshaver_glue.cpp:938–952`,
  `main_unix.cpp:2319–2338`) never matched consumer (a)'s pointer register (it lives in
  gpr(19); the hacks test gpr 8/16/20 — S3 §1.6) and are retired on the fidelity profile
  (M0 Tasks 3–4).

## 5. Open questions

One line each, with the experiment that settles it:

1. **MacIO base on a real/QEMU Core99:** our contract fixes 0xF3000000 (AddrMap + Heathrow
   convention), but QEMU mac99's MacIO is a firmware-assigned PCI BAR (`macio.c:114`), not a
   fixed address. *Experiment:* boot QEMU mac99 to OF and read `/pci/mac-io`'s `assigned-addresses`
   (or instrument the 9.0.1 ROM's own MacIO discovery) before M4 hard-codes anything beyond
   what the AddrMap already tells the guest.
2. **+0x15000 identity:** SWIM3 floppy (our AddrMap `lp[38]` + S3 §3 Heathrow row) vs a
   "timer" region (QEMU NewWorld macio, `macio.c:304`) — Heathrow vs KeyLargo genuinely differ
   here. *Experiment:* check DingusPPC's `keylargo` device map + whether the 9.0.1 ROM ever
   touches 0xF3015000 (bus fault-rate telemetry once M1's bus exists).
3. **IDE offset:** AddrMap says +0x18000 (`lp[24]`, `rom_patches.cpp:1890`); QEMU macio puts
   IDE at +0x20000/+0x21000 (`macio.c:199`). *Experiment:* same as Q2 (DingusPPC cross-check +
   observed ROM traffic); moot until a milestone models IDE.
4. **NewWorld NVRAM address:** QEMU mac99 = 0xFFF04000 outside MacIO (`mac_newworld.c:157,449`);
   OldWorld macio = +0x60000 (`macio.c:176`). *Experiment:* trace the 9.0.1 ROM's XPRAM/NVRAM
   access path (currently HLE'd by the nvram patches, `rom_patches.cpp:1962ff`) when M4 lands.
5. **AddrMap slot `lp[0] = 0xFFC00000`** (`rom_patches.cpp:1886`) — device identity unknown
   (plausibly OldWorld ROM alias, unsourced). *Experiment:* find the UniversalInfo field name
   for struct offset +0x00 of the AddrMap record in classic-Mac headers, or watch for guest
   reads of that slot.
6. **OpenPIC region size** for the KeyLargo model at +0x40000 — not pinned in `macio.c`.
   *Experiment:* read QEMU `hw/intc/openpic.c` (`OPENPIC_MODEL_KEYLARGO` register window) before
   M3 sizes the trap region.
7. **Why 9.2.1 enters the STM monitor at all** (bounded probe vs deliberate blocking 2-byte
   read — determines whether SCC+VIA-T2 honestly un-stalls it). *Experiment:* S3 §5's
   `SS_PROBE_PC` on 0xcc636/0xcc686/0xcc2d8 (+0x480000 mirrors) during one 9.2.1 boot.
8. **ESCC/VIA PIC input numbers (0x24/0x25/0x19) are QEMU values only** — not yet cross-checked
   against DingusPPC or Apple KeyLargo documentation. *Experiment:* compare DingusPPC's
   KeyLargo interrupt wiring before M3 freezes the tree.
9. **Boot framebuffer aperture base** — deliberately a placeholder; M5 finalizes it together
   with the display node `reg`. *Experiment:* none yet; M5 design decision (must keep the
   no-DBDMA constraint of §3).
10. **AddrMap negative slots** (`lp[-10]…lp[-2]` = 0x0300001c, 0x000108c4, 0x00300000,
    0x11010000 — `rom_patches.cpp:1881–1884`, non-contiguous slots lp[-10]/lp[-9]/lp[-4]/lp[-2]) — semantics unidentified (UniversalInfo header
    fields, not device bases as far as we know). *Experiment:* same UniversalInfo-headers dig
    as Q5; record identities here when found.
