# Handoff: New World supervisor-stack fidelity (the PPC MMU / page-table work)

> **Status:** ⏸ **Parked** (2026-06-09) — superseded by the **[Machine Layer](MACHINE-LAYER-PLAN.md)**
> (M0+M1 complete as of 2026-06-10). The Machine Layer absorbed this doc's §2.7 hybrid
> insight and §3 rung-ladder as components (ML §2c); §2.8 obstacle map remains the
> per-wall reference for nanokernel boot issues.
>
> **History:** Path A (this doc) was parked when its obstacle map showed diminishing ROI.
> The Upgrade Card (Path B) was tried next and **closed as a dead end** — the 1.1 ROM is
> structurally incompatible with 9.2.1 (CFM fragment gap + A-line vector corruption; see
> [`UPGRADE-CARD-PATH.md`](UPGRADE-CARD-PATH.md) §2.7-2.8). The Machine Layer supersedes
> both: proper device models (MMIO bus, SCC 8530, VIA 6522) with the 9.0.1 ROM. The
> nanokernel now reaches the MMU/SR wall at 0x50326050 — further than Path A ever got.
> Path A scaffolding is cataloged in [`DEPRECATED-SCAFFOLDING-INVENTORY.md`](DEPRECATED-SCAFFOLDING-INVENTORY.md).
> · **Created:** 2026-06-08 · **Updated:** 2026-06-10
> _Markers: ✅ done · 🟡 in progress · ⏸ deferred · ☐ todo._

---

## 0. Read this first — the mission, and why it is NOT what it looks like

**You will be tempted to push back. Please read this section before you do.**

The visible task is "make the New World ROM / Mac OS 9.2 boot." The natural objections are:
- *"SheepShaver was never designed to emulate the real PPC MMU — it fakes flat virtual=physical."*
- *"If you just want to run Mac OS 9.2, use QEMU / DingusPPC / a real-hardware setup."*
- *"A real MMU is slow and complex (4 KB PPC pages vs 16 KB Apple-Silicon pages); not worth it."*

**Every one of those is true, and none of them is the point.** The actual goal of this project is to
**broaden and harden the correctness of this emulator's PowerPC stack — especially the JIT.** Booting
the New World ROM / 9.2 is a **forcing function**: a stress test that exercises supervisor-level PPC
behaviour (SPRGs, the decrementer, segment registers/BATs, the hashed page table, the nanokernel's
init handshakes) that **no Old-World workload ever touches**, so those paths in our JIT and emulator
have never been validated. The boot is the *oracle*, not the *deliverable*.

**The deliverable is the bugs we fix on the way.** This is already proven, not hypothetical. In the
sessions leading to this handoff, the New World boot attempt — used purely as a forcing function —
surfaced and fixed **real, general correctness bugs that were latent for *every* guest OS**:
- **SPRG0–3 were silently dropped** (`mtspr` ignored, `mfspr`→0). That's wrong for any OS; nothing
  else had exercised it. Now real registers. (`ppc-registers.hpp`, `ppc-execute.cpp`.)
- **`fctiw` ignored the dynamic FPSCR rounding mode** (hardcoded FCVTAS / nearest-ties-away). Now
  selects FCVT{AS,ZS,PS,MS} by FPSCR[RN], matching the interpreter exactly. This was a real FP
  correctness hole for *any* app using non-default rounding + float→int. Resolved a harness quarantine;
  `test-jit` went 349→350.

So when you feel the urge to argue "we shouldn't be doing this" — reframe it as: **we are using a hard
boot as a correctness fuzzer for our JIT, and it keeps finding real bugs.** That ROI stands whether or
not 9.2 ever reaches the Finder.

**The honest guard-rails (so this doesn't become a tar pit):**
- Keep advancing the boot **only while each wall yields a generally-useful emulator fix** (the common
  case so far). If you reach a point where the remaining work is pure ROM-specific byte-patching that
  unlocks no new *executed* code and surfaces no general bug — stop and say so.
- **Never regress the working path.** Mac OS 8.6–9.0.4 on the 1.1 ROM boots today and `make test-jit`
  is the gate (must stay `score=100`). All New-World scaffolding is **env-gated, default-off**, and the
  1.1 path must stay byte-identical. (See §4.)
- You are **not** committing to "9.2 must boot." You are committing to "harvest the correctness gaps
  this exposes, cheaply and safely."

This is the maintainer's explicit, standing framing. Treat it as the project's intent, not a rumor.

---

## 1. Where things stand (what's done, where the boot dies now)

The New World **parcels-format** ROM (`Mac OS ROM 9.0.1`, decoded) now boots its PPC **nanokernel**
under our JIT — far further than ever before. The journey + the proven path:

| Wall | Resolution | Status |
|---|---|---|
| PatchROM rejects the parcels ROM (`:715` CPU-detect) | The block is redundant on parcels (ROM self-handles the faked G4 PVR) → **skip-guard** | ✅ committed |
| `sr_load` (SR/BAT-load) patch absent | SR/BAT are JIT no-ops → **skip** | ✅ committed |
| `jump68k` (PPC→68k handoff) + 68k-side HLE absent | **Diagnostically skipped** (`SS_ROM_SKIP_JUMP68K`) so PatchROM completes and the PPC side can be exercised. NOT a real port. | 🟡 diagnostic |
| First spinlock deadlock (block 12, `0x312700`) | Root cause: nanokernel reads its per-CPU/KDP pointer from **SPRG0**, which we were dropping. Fixed SPRG registers + **`SS_NW_TRAMPOLINE`** probe seeds `SPRG0=KernelDataAddr`, backs+zeros the negative KDP scratch, sets `[SPRG0-4]=KDP`. Boot advances **27 → 128 distinct PCs**, clean, no derail. | ✅ committed (probe env-gated) |
| Sub-KDP pool region unmapped (`0x68FF7000` below KERNEL_AREA) | Root cause: `KERNEL_AREA_SIZE=0x2000` (8 KB) → shmem covers only `[0x68FFC000,0x69000000)`. Pool init's stores to `0x68FF5000–0x68FF7000` fault → `ignoresegv` silently skips them → pool data never written → allocator reads garbage → infinite zeroing loop at `0x50322990`. Fix: `vm_acquire_fixed` 32 KB below shmem base (env-gated). **VERIFIED** via SIGSEGV-handler instrumentation with probe present during both before/after runs (10 faults → 0). Boot advances 128 → 265 unique PCs; pool-init + zeroing-loop + allocator + HTAB/page-table init all complete. | ✅ committed + verified (env-gated) |
| SDR1/HTAB zeroing stall at `0x50311ff4` | **Three-part fix:** (a) real SDR1 register (`ppc-registers.hpp`+`ppc-execute.cpp`: `mfspr`/`mtspr SDR1` read/write); (b) trampoline allocates 64 KB HTAB at `0x68FE0000`, seeds `SDR1=0x68FE0000`; (c) ROM patcher skips `sdr1_read`+`pgtb_clear` patches for parcels (gated on `g_rom_904_lenient`) so the real `mfspr SDR1` + real zeroing loop execute against mapped memory. HTAB zeroing now completes in milliseconds. **General correctness fix** (SDR1 was silently wrong for all guests). | ✅ committed |
| Page descriptor free-list empty (`0x50312250`) | **Three-part fix:** (a) Seed `[KDP-0x20]` = IRP base (`KDP - 0xA000 = 0x68FF4000`), with bank entries at `IRP+0xDF0/DF4`. (b) Skip `desc_create` ROM patch for NW path (it NOP'd the `stwu r31,4(r29)` that stores page descriptors). (c) Lower `KernelMemoryBase` to `sub_kdp_base - pgdesc_size` (256KB for descriptors growing UPWARD). **General fix**: `desc_create` skip gated on `g_rom_904_lenient`. Free list now correctly populated: r22=0x3FFFC (65566 pages), 568 blocks, 153M blocks/s. | ✅ verified |
| **⚠️ RETRACTION — page-descriptor build loop ceiling at `0x503123fc` (ROM 0x3123a8–0x312424)** | **ROOT-CAUSED + FIXED (commit `d8932203`, 2026-06-10; exposed by Machine Layer M0 fidelity profile).** The build loop reads its trip-count cap from `KDP+0x6b4`; that field is un-seeded (=0), clamping the loop to ~16k iterations. The stride-8 pointer walk at `KDP+0x80` overran the ~64 valid entries into `0xFFFFFFFF` poison at `KDP+0x340` → faulting `stw r30,0(r8)` at `0x5031240c`. **Under the Path A paravirtual path, `ignoresegv` silently skipped these faults for the entire page-init stage — the "✅ verified" above was incomplete; the faults were being eaten, not fixed.** The fidelity profile's abort-loudly design (`ignoresegv` disabled on `machine newworld`) exposed the fault immediately, working as intended. Fix: ROM instruction patch in `rom_patches.cpp` gated `g_rom_904_lenient` + newworld profile: replaces `lwz r8,0x6b4(r1)` at ROM offset 0x3123ac with `lis r8,1` (cap=65536 pages for 256 MB RAM). Trampoline-time seeding is ineffective because the NK cold-init zeroing clobbers it; the patch fires after zeroing completes. Boot advances one full stage further after this fix. | ✅ ROOT-CAUSED+FIXED (d8932203) |
| CreateAreasFromPageMap wall (`0x5031f3b8`) | **Root cause: KDP+0x80 (SegMap pointers) corrupted during page-init loop** — correct value `0x68FFE920` written by NKInit SegMap copy, but overwritten to `0x0000FFFF` by an indirect store during page-init. **Spike fix**: PPC stub at ROM+0x30d600 writes minimal SegMap + PMDT data (one 256MB RAM area + sentinels for 16 segments) immediately before calling CreateAreasFromPageMap, bypassing the corruption by construction. Both `bl` call sites (0x3124e4 non-cr5, 0x312568 cr5 path) redirected. **Confirmed**: CreateAreasFromPageMap processes the data (PC 0x5031f530 = normal-area handler reached), boot advances 451→568 compiled blocks at 54M blocks/s. Env-gated on `SS_NW_TRAMPOLINE`. | ✅ spike verified |
| Nanokernel idle/yield primitive at `0x5032751C` | Nanokernel completed all init and entered the **idle path of the yield primitive** at `0x503272e0` (107 callers: 106 via `crset cr1eq` = idle, 1 via `crclr cr1eq` = char-processing). The idle loop at `0x5032751C` spins calling `check_work` (`0x50326880`) which reads **SCC RR0** (not VIA IFR) via `[KDP-0x900]`; if null, returns -1. The loop also serves as the serial debug console (Thud) — when a character arrives, it falls through to char processing at `0x50327540`→`0x5032756c`. **Fix (2026-06-09):** set `[KDP-0x900]=0` (no SCC hardware) so check_work returns -1 immediately. Prior `scc[2]=0x01` caused infinite phantom character processing. **Remaining problem:** with SCC base=0 the idle loop spins forever — the nanokernel needs interrupt injection (`ppc_cpu->interrupt()`) to break out and dispatch tasks, but MODE_NATIVE is dead for NewWorld (see §1.7.1 #2). Env-gated on `SS_NW_TRAMPOLINE`. | 🟡 partial |
| Interrupt injection gap (RESOLVED) | Tick-gated injection from HandleInterrupt MODE_68K path: after 50 ticks (~5s), calls `ppc_cpu->interrupt(ROMBase + 0x312b1c)`. Handler enters correctly, traverses interrupt prologue → scheduler (0x503242a8) → dispatch (0x503244cc → 0x50318000) → **idle task** at 0x50324f04. Idle task loops forever — **never reaches EXEC_RETURN trampoline** (0x5058f5c8). **Root cause (§1.7.2):** No boot task exists (jump68k was skipped), so scheduler dispatches idle task. The idle task's `sc` polling is dead (`sc` = `execute_illegal` no-op, PC += 8, skips `cmpwi r3,0`). With tasks registered, scheduler would dispatch them directly — idle loop never entered. Interrupt dispatch and scheduler work correctly. | ✅ verified |
| DR Emulator entry (0x5046e8c0) — BLOCKED on interrupt injection | Nanokernel dispatch at 0x503126b4 does `rfi` to DR Emulator entry at 0x5046f900 (ROM mirror). DR Emulator reads low-memory globals: ECB ptr from 0x2804, counter at 0x2818, context from KDP+0x65c. All uninitialized → crash at guest PC 0x00000000. Root cause: `patch_68k` / `jump68k` diagnostically skipped (`SS_ROM_SKIP_JUMP68K`). The NW ROM's jump68k signature differs from OldWorld (the 1.1 byte pattern is absent). **Character change**: no longer a supervisor-memory problem — this is the PPC→68k emulator boundary, requiring 68k HLE shim infrastructure. **New approach (2026-06-09):** synthetic ECB stub — see §2.7. | 🟡 blocked |

**SDR1/HTAB wall (RESOLVED).** With the sub-KDP fix verified, the nanokernel hit the SDR1/HTAB
wall at `0x50311ff4`: `mfspr SDR1` returned the `0xdead001f` sentinel → 524K faulting stores.
**Fixed** with a three-part approach:
1. **Real SDR1 register** (`ppc-registers.hpp`, `ppc-execute.cpp`): `mfspr`/`mtspr SPR 25` now read/write
   a real register instead of returning a sentinel. General correctness fix for all guests.
2. **Trampoline HTAB allocation** (`sheepshaver_glue.cpp`): `vm_acquire_fixed` 64 KB at `0x68FE0000`
   (below sub-KDP pool), memset zero, seed `SDR1=0x68FE0000`. All env-gated.
3. **ROM-patch skip** (`rom_patches.cpp`): the `sdr1_read` and `pgtb_clear` patches (which replaced
   `mfspr SDR1` with a `lis r8,0xdead` sentinel and NOP'd the zeroing `stwx`) are now **skipped for
   parcels** (gated on `g_rom_904_lenient`). The real instructions execute against mapped memory.
   HTAB zeroing completes in milliseconds.

**Page-table/free-list wall (RESOLVED).** After HTAB zeroing, the nanokernel's bank scan and
free-list builder (at `~0x503121A4` in the decompressed ROM) stalled with `r22=0xFFFFFFFC` (no pages
found). Root cause was **three problems stacked**:

1. **`[KDP-0x20]` (the IRP base pointer) was never set.** The skipped cold-init normally writes
   `[KDP-0x20] = KDP - 0xA000`. Without it, the bank scan reads from guest low memory (all zeros),
   finds no banks, and produces the "no pages" signature.
2. **The `desc_create` ROM patch NOPs the `stwu r31,4(r29)`.** This is the instruction that stores
   page descriptors into the free list. Correct for OldWorld (flat addressing, no MMU); kills the
   NW nanokernel's page management.
3. **Page descriptors overflow upward into KDP.** The free-list grows UPWARD from KernelMemoryBase
   (via `stwu`). With only 56KB between KernelMemoryBase and KDP, 256KB of descriptors overwrite
   the sub-KDP pool, IRP, KDP itself, and HTAB. Fixed by lowering KernelMemoryBase.

**⚠️ Key methodology finding: the raw .rom file is CHRP-compressed.** SheepShaver decompresses it
into the 5MB ROM area. The file bytes DO NOT match guest memory. Always dump the decompressed ROM
from the emulator (via `fwrite(Mac2HostAddr(rom_base), ...)`) and disassemble that. An agent that
analyzed the compressed file produced an entirely fabricated disassembly — plausible addresses and
register names, but wrong instructions. This wasted a full investigation cycle.

**Idle/yield + SCC fix (2026-06-09, corrected 2026-06-09).** After all init completes (568 unique
compiled blocks, 54M blocks/s), the nanokernel enters the **idle path of the yield/scheduler
primitive** at `0x503272e0` (107 callers: 106 via `crset cr1eq` = idle, 1 via `crclr cr1eq` =
char-processing). The idle loop at `0x5032751C` spins calling `check_work` (`0x50326880`), which
reads `[KDP-0x900]` — **an SCC (Zilog 8530) base address, NOT a VIA 6522** (the register access
pattern at offsets 2/6 with alternating reg#/data writes matches SCC, not VIA). If null, returns -1
(no serial hardware). The same loop doubles as the serial debug console (Thud) — when check_work
returns a character, control falls through to `0x50327540` → `0x5032756c` for echo, line-editing,
and command dispatch via `0x5032879c`.

**Key correction:** the original fix set `scc[2]=0x01` (SCC RR0 bit 0 = Rx char available), which
caused the nanokernel to enter the character-processing path at `0x50327540`, endlessly consuming
phantom 'B' characters (from `scc[6]=0x42`). The hot PCs during this spin were all in check_work's
BAT-setup/teardown code (`0x50426880`–`0x50426af8`, mirror addresses). **Fixed** by setting
`[KDP-0x900]=0` (no SCC hardware at all), so check_work returns -1 immediately with no risk of
self-modification (check_work's timeout loop at `0x50326548` writes to `scc[2]`, corrupting any
initial zero byte on a non-null base).

**Remaining problem:** with SCC base=0, the idle loop spins forever (check_work always returns -1).
The nanokernel needs **interrupt injection** (`ppc_cpu->interrupt(ROMBase + 0x312b1c)`) to break out
of the idle loop and dispatch tasks. The MODE_NATIVE code path in HandleInterrupt already has this
call, but MODE_NATIVE is dead for NewWorld ROMs (§1.7.1 #2). See §1.7.1 #6 for the interrupt
injection gap analysis.

**The current wall (DR Emulator entry at 0x5046e8c0 / 0x5046f900).** The nanokernel's dispatch
routine seeds SPRG0 (`KDP+0x5a0`), SRR0 (`KDP+0x5a4 + 0x26e8 = 0x5046f900`), and SRR1
(`KDP-0x964 = 0x0000D032`), then does `rfi`. The DR Emulator's cold-start at 0x5046f900 reads
low-memory globals: ECB ptr at guest address 0x2804, counter at 0x2818, context block from
`KDP+0x65c`. All of these are uninitialized → crash at guest PC 0x00000000. Root cause: `patch_68k`
/ `jump68k` is diagnostically skipped (`SS_ROM_SKIP_JUMP68K`) because the NW ROM's jump68k byte
signature differs from OldWorld (the 1.1 pattern is absent). **Character change**: no longer a
supervisor-memory problem — this is the **PPC→68k emulator boundary**, requiring 68k HLE shim
infrastructure to populate the low-memory globals and ECB the DR Emulator expects.

---

## 1.5 Which ROM to use — the cross-ROM finding (you do NOT patch each one differently)

A real worry was "do we have to patch every New World ROM separately?" **Measured answer: no, not within
the 9.x family.** Two different "Mac OS ROM" files — **9.0.1** (cksum `ec86128e`) and **9.1.1**
(`ecef6af1`) — were booted through the identical diagnostic path and behaved **identically**: same
`:715` CPU-detect at `0x310a1c`→`0x311350`, same `sr_load`/`jump68k`/`mdec`/`suspend` skips, same SPRG0/
KDP shim, same 128 PCs, and the **same page-table wedge at `0x50322990`**. A byte-diff of the decoded
nanokernel region (`0x300000–0x340000`) shows **only 93 differing bytes (0.04%)** — the nanokernel is
effectively the same across the family; the 34% whole-image difference is all in the *other* parcels
(device tree, drivers, hardware support), which don't affect the boot-critical supervisor path.

**Implication:** one supervisor-environment fix should cover the whole **9.x parcels family
(9.0.1 / 9.1.1 / 9.6.1 / 9.8.1 / 10.2.1)**. The **outlier is `9.0.4-G4`** (`MacOS-ROM-9.0.4-G4-extracted.rom`),
a *separately-extracted, different-lineage* ROM with a different patch profile (sizing 49/14/12 vs the
family's 27/31/17 — see NEW-WORLD plan "ROM target strategy"). Don't assume 9.0.4 behaves like the family.

**Pinned reference ROM for this work: `Mac OS ROM 9.0.1`** (it's the one all the diagnosis/offsets above
were calibrated on). Develop against 9.0.1; once it boots, re-run 9.1.1 (and the rest of the family) to
confirm — expect them to "just work" given the 0.04% nanokernel diff.

### Exact setup (assets + how to reproduce)

| Asset | Path | Notes |
|---|---|---|
| **Reference ROM (use this)** | `/Users/Shared/macemu/2001-12-19 - Mac OS ROM 9.0.1.rom` | also in `~/Downloads/New_World_Mac_Roms/New World ROM/`. CHRP/parcels, decodes to a 4 MB image. |
| Family ROMs (confirm-after) | `~/Downloads/New_World_Mac_Roms/New World ROM/` | 9.1.1, 9.6.1, 9.8.1, 10.2.1 — same nanokernel. |
| 9.0.4-G4 outlier | `/Users/Shared/macemu/MacOS-ROM-9.0.4-G4-extracted.rom` | different lineage; separate analysis if pursued (it's the AltiVec-era G4 ROM). |
| Working baseline ROM (must keep booting) | `/Users/Shared/macemu/1998-07-21 - Mac OS ROM 1.1.rom` | Old World LZSS; the `make e2e`/normal config. Never regress this. |
| OS media for an eventual *full* boot | `~/Downloads/Apple Mac OS 9.2.1/macos_921_ppc.iso` | only needed once the ROM boots past the nanokernel into the 68k OS; the diagnostic boot needs NO disk. |
| Decoded image (offline disasm) | make it: `SheepShaver/rom-inspect/rom-inspect "<rom>" --dump /tmp/rom901.bin` | capstone PPC big-endian. |

**Diagnostic prefs** (`/tmp/trace901.prefs` — recreate it; `/tmp` is ephemeral):
```
printf 'rom /Users/Shared/macemu/2001-12-19 - Mac OS ROM 9.0.1.rom\nramsize 268435456\nnogui true\n' > /tmp/trace901.prefs
```
(no `disk`/`cdrom` — the diagnostic boot wedges in the nanokernel long before it would need OS media).

## 1.6 Conceptual model — OldWorld vs NewWorld, and the source-verified correction that reshapes the fix

*(From a research agent that read elliotnunn's reverse-engineered nanokernel `Init.s` directly + Mac
history sources. Confidence: **[SRC]** = read from RE'd nanokernel source; **[FACT]** = cited;
**[INFER]** = reasoned.)*

**The boot chains.** *OldWorld* (what "Mac OS ROM 1.1" emulates): a ~4 MB Toolbox ROM on the logic board
does **everything** — hardware init, MMU/page-table setup, nanokernel, 68k emulator, Toolbox. *NewWorld*:
the on-board ROM is just ~1 MB **Open Firmware**; the "ROM" is a **disk file** ("Mac OS ROM", a
compressed **CHRP/parcels `tbxi` container**). Chain: **OF** (power-on HW init, builds the device tree)
→ loads the tbxi → the **Trampoline** (an ELF inside it) decompresses/relocates the ROM image into RAM,
finishes Northbridge/Southbridge init, wires interrupts, copies/modifies the device tree, and builds
**`NK*Info` descriptor structs** → jumps to the **nanokernel** (then OF's memory is overwritten — OF is
gone) → 68k emulator → Mac OS. The **nanokernel↔68k-emulator split is identical in both worlds.** [FACT]

**⭐ THE CORRECTION (most important thing in this doc for the fix).** On entry the NewWorld nanokernel
receives, in r3–r9, **only descriptor structs** — incl. `NKSystemInfo` with the physical-memory **bank
map** (`Bank0Start`/`PhysicalMemorySize`) — **NOT a finished page table.** It then, in `Init.s` cold-init,
**does the MMU setup ITSELF** [SRC]:
- zeros all 16 **segment registers** + clears **BATs** (it tears down translation, doesn't inherit it);
- **sizes & allocates its own HTAB** from RAM and **writes `SDR1` itself** (`mtspr sdr1`);
- places the **KDP at HTAB−0x2000** and **writes `SPRG0` itself** (`mtsprg 0`).

So the Trampoline does **not** hand over SDR1/HTAB/SPRG0 — the nanokernel builds them. **Why our boot
still wedged:** SheepShaver's flat-V=P **supervisor-MMU stub drops `mtspr SDR1`/SR/BAT and returns a
fake SDR1 (`0xdead0000`)** — it **silently defeats the nanokernel's own setup.** The nanokernel computes
real values; SheepShaver eats the writes; later reads return the fake. Our `SS_NW_TRAMPOLINE` SPRG0-seed
"worked" only by papering over that far enough to expose the next swallowed write (the HTAB/page-table
init). **[INFER, reconciles §1 ground truth with the source.]**

⇒ **The walls are two coupled problems:** (1) the Trampoline-built **environment is absent**
(`NKSystemInfo` bank map, decompressed image, interrupt wiring, device tree — SheepShaver jumps straight
in with none), and (2) the **MMU stub eats the nanokernel's own SDR1/SPRG0/segment writes.** This means
the likely correct fix is **"let the nanokernel's own init run" — stop dropping its supervisor writes
and give it a real, RAM-backed HTAB** (exactly the MMU §3 "rung 1 / honor-the-write" path), rather than
faking Trampoline post-conditions. Note: SheepShaver *also* `patch_nanokernel_boot`-intercepts parts of
cold-init to substitute flat fakes (that's how 1.1 works), so the real choice is **(a) honor the
NewWorld nanokernel's real MMU setup** vs **(b) intercept it more completely** like we do for 1.1.

**THE FIRST QUESTION TO RESOLVE (do this before building):** **does SheepShaver actually run the
NewWorld nanokernel's cold-init, or does our patching jump past it?** Trace the entry PC against the
cold-init signature (the `mtsr 0..15` block, then `mtspr sdr1` / `mtsprg 0`).
- **Runs cold-init** → fix = *let it run*: supply a valid `NKSystemInfo` bank map and **stop eating its
  SDR1/SPRG0/segment writes** (honor-the-write). Our SPRG0 seed becomes redundant with what it writes —
  consistent with "seeding it merely advanced to the next step."
- **Jumps past cold-init** → fix = *fake the post-conditions wholesale* (pre-built SDR1/HTAB, SPRG0/KDP,
  segments).

**Maintainer's "comprehensive vs left-to-the-System" framing — verdict:** right that NewWorld moved the
ROM to a disk file and offloaded init off the on-board ROM, **but** (1) it offloaded to **firmware (OF +
Trampoline)**, *not* "the System" (Mac OS software) — easy to conflate, and the distinction matters; and
(2) the **page table is NOT among the offloaded items** — the nanokernel still builds it. "For our needs
that's fine" is correct: **we never need to emulate Open Firmware** (OF overwrites itself before the OS
runs; its lasting value is the device tree, which SheepShaver already fakes via NameRegistry). The
minimal hand-off to fake is: a sane **`NKSystemInfo` bank map** + a **non-hostile supervisor MMU** (honor
or consistently fake SDR1/HTAB/SPRG0/segments) + the **device-tree** identity we already provide.

## 1.7 Decision log — CreateAreasFromPageMap wall (2026-06-09)

**The wall.** `CreateAreasFromPageMap` (`0x5031f3b8`) reads **SegMap pointers from KDP+0x80**. After `NKInit` copies the SegMap the correct value is `0x68FFE920`, but by the time CAFPM is called that pointer had been **overwritten to `0x0000FFFF`** by something inside the page-init loop (`0x503214fc`, visited 10–100 times). CAFPM then operates on garbage and cannot create the memory areas the nanokernel expects.

**Approach chosen — PPC stub bypass (option 2).** Rather than hunting the corruption source, a **34-instruction PPC stub at ROM+0x30d600** writes minimal, correct SegMap + PMDT data *immediately before* the `bl CreateAreasFromPageMap`, so nothing can clobber it. Both call sites (`0x3124e4` non-CR5 path, `0x312568` CR5 path) are redirected to the stub, which writes: **PMDT[0]** — a 256 MB RAM area `{page=0, count=0xFFFF, flags=0x2}`; **PMDT[1]** — sentinel `{page=0, count=0xFFFF, flags=0x200}`; empty sentinels for segments 1–15; and **SegMap[0] → PMDT[0]**, SegMap[1–15] → empty sentinel. The stub then falls through to the real CAFPM call. Corruption is bypassed by construction — zero gap between write and read.

**Paths not taken, and why.**

*Hunt the exact corrupt store.* All checked stores (the `stwu` at `0x503121d4`, `dcbst`/`sync` sequences) were innocent; the culprit is likely an **indirect store through a runtime-computed pointer** inside the page-init loop that would require deep RE to isolate. Abandoned because: the stub bypasses it by construction, understanding the cause wouldn't change the fix, and diminishing returns for a hypothesis test.

*Write SegMap/PMDT from C++ at patch time (`WriteMacInt32`).* Won't work — the corruption happens at **runtime** during page-init, so data written at patch time is overwritten before CAFPM reads it.

*NOP the entire `bl CreateAreasFromPageMap` (`SS_NW_NOP_AREAS`).* Tested earlier — boot did reach the idle loop, confirming it unblocks. But it doesn't exercise CAFPM and can't confirm the SegMap data theory. The stub is strictly better: it tests the hypothesis **and** creates the areas.

*Full SegMap/PMDT implementation up front.* Build a complete SegMap with proper IRP/KDP/EDP/ROM/I/O region entries for all 16 segments. Deferred — the spike is a cheaper hypothesis test; if CAFPM can't process minimal data, a full implementation fails for the same reason. Now that the spike confirms the theory, a full implementation can follow.

*Fix the corruption root cause in page-init.* The "proper" fix, but: (a) the corruption is likely intentional nanokernel behaviour (page-init builds a free list that temporarily reuses KDP fields as scratch space); (b) the real Trampoline would repopulate SegMap data **after** page-init anyway; (c) our stub emulates exactly what the Trampoline would do.

**Result.** The spike confirmed the hypothesis — CAFPM processed the stub's data (PC `0x5031f530`, the normal-area handler, was reached), boot advanced from **451 → 568 compiled blocks at 54M blocks/s**, and the nanokernel completed all init and entered the **idle/yield primitive at `0x5032751C`** (see §1.7.1 #4 — it's both the scheduler idle loop and the Thud serial debug console poll). The SCC fix (`[KDP-0x900]=0`) prevents serial phantom chars but the idle loop now spins forever — the **interrupt injection gap** (§1.7.1 #6) is the actual remaining wall. Once resolved, the nanokernel will dispatch to the **DR Emulator entry at 0x5046f900**, where the next wall awaits (low-memory globals uninitialized — see §1 table).

**Open question for a production implementation.** The spike uses a single minimal 256 MB RAM area. A full implementation should add **I/O region entries (type=`0xC00`)** for the VIA/CUDA address ranges and possibly separate entries for IRP/KDP/EDP/HTAB supervisor regions. The SegMap sentinel-writing loop at `0x503123f4` normally writes I/O sentinels; the spike's stub overwrites those. The stub is env-gated on `SS_NW_TRAMPOLINE` and does not affect the OldWorld path.

### §1.7.2 Interrupt handler trace — nanokernel alive, idle task confirmed (2026-06-09)

**Method.** Added a depth-2 PC trace to the `do_interpret` loop in `ppc-cpu.cpp` (the bare
interpreter path used by nested `execute()` calls at depth > 1). All existing JIT diagnostics
(probes, trace ring, heartbeat, block counters) are blind at depth > 1 — they only fire in
the depth-1 JIT/decode-cache path. The trace logged 5000 PCs to `/tmp/htrace.log`.

**Handler path (in order of execution):**
1. **Interrupt prologue** at `0x50312b1c` — saves r17–r21, r13, XER, CTR, r2–r4 into context
   block at `[r6+offset]`. Runs ~60 instructions (0x50312b1c–0x50312bd4).
2. **Exception dispatch** at `0x50313ecc–0x50313f68` — the nanokernel exception dispatcher.
3. **Handler body** at `0x50312bd8–0x50312dcc` — reads KDP fields, sets up scheduler context.
4. **Scheduler dispatch** at `0x503242a8` — walks the task queue, finds no runnable task.
5. **Check memory/context** at `0x503244cc → 0x50318000` — (the "no-task" path).
6. **Idle task entry** at `0x50324f04` — loads ASCII strings into registers:
   - r20/r21 = "idle"/"task" (0x69646c65/0x7461736b)
   - r22–r27 = "Ren\x8e", "Alan", "Jim ", "Alex", "Derr", "ick " (developer credits)
7. **Idle loop** at `0x50324f48–0x50325000` — register waterfall (shifts all regs down one
   position), then `sc` (r0=0x2e, r3=0xc, r4=1) = intended check-for-work syscall. But `sc`
   is a no-op in SheepShaver (see below), so r3 is unchanged (still 0 from init) and CR0
   is stale (EQ from `cmpwi r31,0`). Loops forever. **104 iterations observed in 5000 PCs.**

**Key observations:**
- The `sc` instruction at `0x50324fd8` is a **complete no-op** in SheepShaver. In the
  `#ifdef SHEEPSHAVER` path, `execute_syscall()` calls `execute_illegal()`, which — with the
  default `ignoreillegal=true` pref — does `increment_pc(4); return;`. Then `execute_syscall`
  itself does another `increment_pc(4)`. Total: **PC += 8**, skipping the `cmpwi r3, 0` at
  `0x50324fdc` entirely. No registers (r3, CR0) are modified.
- The `beq` at `0x50324fe0` therefore tests **stale CR0** from the earlier `cmpwi r31, 0`
  (r31 is always 0 → CR0=EQ), so the branch is always taken → infinite loop.
- The idle task never reaches `0x5058f5c8` (our EXEC_RETURN trampoline), confirming the
  `execute()` call in `interrupt()` never returns.
- The `twui r31, 5` at `0x50324fec` (task-dispatch trap) is never executed.

**Root cause: `sc` is dead in SheepShaver.** On real hardware, `sc` vectors to the nanokernel's
syscall handler, which checks for runnable tasks and returns a result in r3. SheepShaver uses
EMUL_OP/NativeOp for its own syscall mechanism and treats the PPC `sc` instruction as illegal.
The double `increment_pc(4)` is a **latent bug** in `execute_syscall` (it should either not call
`execute_illegal` or not do its own increment), but fixing it alone won't help — `sc` still
wouldn't execute the nanokernel's syscall handler.

**Conclusion:** The nanokernel's interrupt dispatch, exception handling, scheduler, and idle
task entry all work correctly — the nanokernel IS alive. The idle loop is a structural dead-end
because `sc` is a no-op, not (only) because no tasks are registered. Even if `jump68k` were
enabled and tasks registered, the `sc`-based polling mechanism cannot work in SheepShaver.
The next step is **not** more interrupt work — it's the synthetic ECB/task-injection approach
in §2.7, which bypasses the `sc` polling entirely.

### §1.7.1 Corrections from static analysis + diagnostic boot (2026-06-09)

Six findings from ROM disassembly, runtime probes, and code analysis that correct prior assumptions:

1. **KDP fields populated (confirmed).** KDP+0x658, 0x674, 0x67c are all set by nanokernel init
   at ROM 0x50310834/50/5c. These are the fields read by `HandleInterrupt` for MODE_68K interrupt
   delivery. No fix needed — they work.

2. **MODE_NATIVE permanently dead for NewWorld.** The `ppc_excp_tbl` and `m68k_excp_tbl` byte
   patterns (which toggle `XLM_RUN_MODE` between MODE_68K and MODE_NATIVE) do not exist in the
   9.0.1 ROM. With `SS_ROM_LENIENT=1`, both patches are skipped. `XLM_RUN_MODE` stays MODE_68K
   permanently. **Implication:** the `HandleInterrupt` MODE_NATIVE path (which calls
   `ppc_cpu->interrupt()`) is unreachable for NewWorld ROMs. All interrupt delivery goes through
   the MODE_68K path (KDP field writes + Ticks increment).

3. **KDP-0x900 is SCC, not VIA.** The register access pattern in `check_work` (alternating
   reg#/data writes to the same base address at offsets 2 and 6, with `eieio` barriers) matches a
   **Zilog SCC (8530)** serial controller, not a VIA 6522. Byte 2 = SCC RR0 (status register 0),
   bit 0 = "Rx character available". Byte 6 = SCC data register. Code comments in
   `sheepshaver_glue.cpp` corrected from "VIA" to "SCC".

4. **0x5032751c is the idle path of the nanokernel yield/scheduler primitive.** The function at
   `0x503272e0` has 107 callers: 106 via `crset cr1eq` (idle/yield — "nothing to do, poll serial"),
   1 via `crclr cr1eq` (explicit char-processing, from `0x5032360c`). At `0x50327518`, `bne cr1`
   branches to `0x50327540` (char path) when cr1eq is clear; the 106 idle callers fall through to
   `0x5032751c`. The idle loop: increments a counter at address 0, calls `check_work`, loops if -1.
   When a character arrives, it falls through to `0x50327540` → sets KDP+0xedc bit 1 → calls
   `0x5032756c` (echo, line-editing, command dispatch via `0x5032879c` — the Thud debug console).
   The prologue at `0x503272e0` performs a **full machine-state save** — all GPRs (via `stmw`), all
   32 FPRs (after enabling FP via `ori r0,r0,0x2000; mtmsr; isync`), all 16 segment registers
   (`mfsr 0..15`), XER, CTR, SPRGs, MSR, FPSCR — into KDP+0x700..0x8fc. A debug console does not
   save FP and segment registers before polling a keystroke; a **context-switch/yield primitive**
   does exactly this. Combined with the 106:1 caller split, idle/yield is the dominant role;
   console is the secondary branch. The idle loop polls **only** serial — no decrementer, no timer,
   no task queue check. The only way to break it in emulation (no SCC) is PPC exception injection.

5. **Diagnostic boot result (runtime probe).** With `SS_PROBE_PC` at check_work entry
   (0x50426880, mirror address), `[KDP-0x900]=0x68faf000` (SCC base populated). Hot PCs were
   scattered across check_work (0x504268d4), BAT-setup (0x50426ae8–af8), and BAT-teardown
   (0x50426580–5a4) — all mirror addresses. The nanokernel was spinning in check_work's SCC
   polling path, reading our fake `scc[2]=0x01` (Rx char available) endlessly, with the
   character-processing path at `0x50327540` calling check_work repeatedly. Setting
   `[KDP-0x900]=0` (no SCC hardware) is the correct fix — check_work returns -1 immediately
   with no risk of self-modification.

   - With SCC base=0, the idle loop at 0x5032751c spins forever (check_work always returns -1).
     Hot PC at 0x50427590 (mirror of 0x50327590, +116 bytes from 0x5032751c) confirms the spin
     is in the idle loop itself, not in check_work internals.

6. **Interrupt injection gap (RESOLVED — see §1.7.2).** Tick-gated injection added to
   HandleInterrupt MODE_68K path: after 50 ticks (~5s), calls `ppc_cpu->interrupt(ROMBase +
   0x312b1c)`. Handler enters correctly. Depth-2 interpreter trace (5000 PCs) confirmed the
   full path: interrupt prologue → exception dispatch → scheduler at 0x503242a8 → idle task
   at 0x50324f04. The idle task attempts to poll via `sc` (r0=0x2e), but **`sc` is a no-op
   in SheepShaver** (treated as `execute_illegal` → double `increment_pc(4)` = PC += 8, no
   register modification). The `cmpwi r3,0` after `sc` is skipped; the `beq` tests stale
   CR0=EQ and always loops. **Never reaches EXEC_RETURN trampoline.**

   **Resolved open questions from the original analysis:**
   - "Does the handler dispatch a task or return to the idle loop?" → **Neither.** It dispatches
     to the *idle task* (the scheduler's fallback when no real tasks exist). The idle task's
     `sc`-based polling is dead (no-op), so it loops forever regardless of task state.
   - "What event is the boot task waiting for?" → **No boot task exists** (jump68k was skipped).
     With jump68k enabled, the scheduler at `0x503242a8` would find the task on its queue and
     dispatch it directly — the idle loop would never be entered. The `sc` no-op is only relevant
     for the idle case (no tasks). The interrupt dispatch mechanism is correct; the path forward
     is getting tasks registered (§2.7 synthetic ECB + re-enabling jump68k).

   **Key methodology finding:** nested `execute()` at depth > 1 runs in the bare interpreter
   loop (`do_interpret` at ppc-cpu.cpp:2261) with **zero** instrumentation — no probes, no trace
   ring, no JIT diagnostics, no heartbeat. The only way to observe what happens inside is a
   temporary PC trace in the `do_interpret` loop itself.

---

## 2. Your task

Give the New World nanokernel a **consistent SDR1 ↔ HTAB ↔ KDP** environment so its page-table init
(and the HTAB/segment-register dependencies that follow) operate on **real, sanely-sized, RAM-backed**
state instead of the `0xdead0000` fake — **without** taxing the JIT's flat-memory (V=P) hot path. Then
keep running the forcing-function loop (§4): boot → find the next wall → diagnose → fix (preferring
general fixes) → repeat.

The critical design question — **does the nanokernel need a *functional* MMU, or just a
*self-consistent* page-table data structure that happens to resolve identity (V=P)?** — is explored in
§3 (seeded by a lateral brainstorm) and `MMU-NANOKERNEL-MP-PLAN.md`. Strong prior: classic Mac OS runs
effectively V=P; we likely need the nanokernel's *bookkeeping* to be consistent (HTAB in real RAM at a
sane SDR1; SR/BAT/SDR1 stored & readable — we already store SPRG the same way) while the JIT keeps
doing V=P with **zero** added hot-path cost. Validate that reframe cheaply before building anything big.

---

## 2.5 Strategic fork — two paths to "run 9.2", and why we're (mostly) on Path A

There are **two legitimate routes**, and the new agent should understand both before committing effort.

**Path A — port the NewWorld parcels ROM (the current path, §1–2).** Load the 9.x "Mac OS ROM", emulate
the Trampoline hand-off (SPRG0/KDP/SDR1/HTAB), boot its real nanokernel. This drags the *real NewWorld
supervisor boot* into the light → it is the **richer correctness fuzzer** (exercises low-level PPC code
nothing else does; already harvested the SPRG + fctiw bugs). Cost: the MMU/page-table wall (§3).

**Path B — "do we even need a ROM file?" Synthesize the environment on the working 1.1 ROM.** SheepShaver
already boots Mac OS up to **9.0.4 fully** on the OldWorld **1.1 ROM**, and the **9.2.1 OS already runs
on the 1.1 ROM** up to the *"won't work on this Macintosh model"* check. So instead of porting a NewWorld
ROM, boot the **9.2 OS itself** on our proven 1.1 environment and **fake whatever 9.2 additionally
demands.** Tempting because it reuses a working supervisor; the OS still stresses the JIT with newer
code.
- **Why it's not free (measured):** the `SS_NW_MODEL` probe (in `name_registry.cpp`) injected the
  NewWorld device-tree `model`/`compatible` to pass that check on the 1.1 ROM — and **9.2.1 still
  rejected the model.** So 9.2 wants *more* than identity-faking. And the 1.1 vs 9.x nanokernels differ
  **88.8%** in the boot core, so you can't graft 9.x's nanokernel onto 1.1 — Path B means "make the 9.2
  *OS* happy on the *1.1* nanokernel," not reusing 9.x's.
- **The unknown that decides Path B:** the **delta between what 9.0.4 accepts and what 9.2 demands** on
  the 1.1 ROM. We know the model check is one item (and a device-tree fake wasn't enough); there are
  likely more (a gestalt/version gate, a NewWorld-only API/structure the OS reads, etc.).
- **Cheap Path-B experiment (do this before committing to either path if Path B tempts you):** boot the
  **9.2.1 ISO on the 1.1 ROM** (a normal boot, needs the ISO + a scratch disk + a GUI session — ask the
  maintainer), get past/around the model check (extend `SS_NW_MODEL`, or find the actual check), and
  **characterize where 9.2 dies next.** That single data point tells you whether Path B is a short hop
  or its own rabbit hole. Compare against the known-good 9.0.4-on-1.1 boot to isolate the 9.2 delta.

**Recommendation / why Path A for now:** the project's stated goal is **correctness hardening**, for
which Path A is the better fuzzer (it runs *new* supervisor code; Path B re-runs the supervisor we've
already validated). Path A is also further along (27→128 PCs). **But Path B is a genuinely cheaper route
to the *capability* of running 9.2, and the two converge** (both end up running the 9.2 OS, which is
fresh JIT stimulus either way). Don't treat this as settled — if Path A's MMU wall proves unbounded
*and* the Path-B 9.2-delta experiment shows a short list, switching (or running both) is rational. Keep
the forcing-function discipline: pick the path that keeps surfacing fixable general bugs.

## 2.6 Path C — "a completely synthetic ROM?" (what's achievable, what isn't)

A natural question: skip ROM files entirely and have SheepShaver **synthesize** the ROM. Verdict: a
*fully* synthetic ROM is **not feasible**, but the reasoning clarifies the project. The ROM = (1)
nanokernel (supervisor), (2) 68k emulator, (3) **the Toolbox / Mac OS itself** (the bulk — QuickDraw,
Memory/File/Resource Managers, …), (4) boot structures. We already replace/patch 1, 2, 4. But **layer 3
*is* Mac OS** — Apple's copyrighted code, an entire OS; it cannot be synthesized from nothing (even
elliotnunn's `newworld-rom` only *assembles* a ROM from the real Power Mac ROM + real PEF binaries).

**What IS synthesizable is the supervisor/firmware layer — which is exactly where all our walls are.**
So Path C, in its achievable form, is "**synthesize the supervisor environment, keep a real ROM only for
the Toolbox**" — i.e. Path A taken to its end: instead of *patching* the real nanokernel's MMU/boot
setup, *replace* it with our own synthetic init while still loading the real Toolbox. A **synthetic
nanokernel is genuinely possible** (elliotnunn's `NanoKernel`/`powermac-rom` are buildable), but it's
**more work than patching the real one** (you're re-implementing a kernel) and **does not remove the
Toolbox dependency**. So "fully synthetic" collapses to "synthesize the environment, borrow the
Toolbox" — which is what the supervisor work in §1.6/§3 already is. **Conclusion: we always need a real
ROM for the Toolbox; we are effectively synthesizing the firmware/supervisor layer regardless.** Don't
chase a no-ROM-file design; do treat "synthesize the supervisor environment" as the legitimate core.

## 2.7 The hybrid approach — minimal hardware model for boot, HLE for runtime (2026-06-09)

> **Status:** ⏸ Absorbed into [Machine Layer](MACHINE-LAYER-PLAN.md) §2c (2026-06-10).
> **Decision:** The stop-rule that closed the NW forcing function (§1.7) was based on the premise
> that the remaining work = "84 HLE shim byte-pattern porting." The hybrid reframe changes that
> calculus: the DR Emulator wall may be solvable with a **synthetic ECB stub** (same proven technique
> as the SegMap/PMDT spike), not a full shim port. *This insight was adopted by the Machine Layer
> architecture, which implements proper device models (MMIO bus + SCC + VIA) for the boot phase
> and retains HLE for runtime — exactly the two-phase strategy proposed here.*

### The insight: two phases, two emulation strategies

| Phase | What the ROM talks to | Natural emulation style |
|---|---|---|
| **Boot** (nanokernel init → 68k OS startup) | Hardware: VIA, PMU, NVRAM, OF device tree, interrupt controllers | **LLE** — model the hardware responses |
| **Runtime** (Finder, apps) | Toolbox traps, framebuffer, file system | **HLE** — intercept traps → host calls (existing shims) |

SheepShaver already does HLE for runtime. The hybrid adds minimal LLE for boot — not a full
machine model, but enough device registers to satisfy the ROM's probing.

### The reframe: most hardware is already neutralized

The 84 `patch_68k` shims already NOP out Cuda init, SCC, GC interrupt mask, CPU-speed probes.
The fake SCC page (§1 wall table) is the only genuine "device model" needed before DR Emulator.
**The actual wall is the PPC→68k handoff, not more hardware.**

### Approach: synthetic ECB stub (same technique as SegMap/PMDT spike)

The DR Emulator entry at `0x5046f900` reads a small set of low-memory globals:
- ECB (Emulator Control Block) pointer at guest `0x2804`
- Counter at guest `0x2818`
- Context block from `KDP+0x65c`

Rather than RE'ing the full `jump68k` byte pattern in the 9.0.1 ROM (which differs from
OldWorld), write a **synthetic initializer stub** — a PPC stub at a spare ROM offset that
populates the words the DR Emulator cold-start reads, then jumps to entry. Construct the
post-jump68k world by ABI contract rather than pattern-matching.

**Precedent:** The SegMap/PMDT spike at ROM+0x30d600 used exactly this technique — 34 PPC
instructions that write minimal correct data immediately before the consumer reads it. The
synthetic ECB stub is the same pattern applied to the next wall.

### Implementation plan

1. **Disassemble DR Emulator entry (`0x5046f900` in decompressed ROM)** — enumerate every
   memory read in the cold-start sequence. This tells us whether the stub needs 4 words or 40.
   Use `SS_DUMP_ROM` + capstone. **(S — hours)**

2. **Write synthetic ECB stub** — PPC stub at a spare ROM offset, env-gated on
   `SS_NW_TRAMPOLINE`. Populate the enumerated globals, redirect the nanokernel→DR Emulator
   dispatch. Same pattern as the SegMap spike. **(S — hours, once reads are enumerated)**

3. **Trace first 68k Toolbox traps** — once DR Emulator executes, see which `patch_68k`
   shims fire and which fail. The shim inventory (PATCH-68K-SHIM-INVENTORY.md) shows 28
   in-range + 31 relocated + 25 absent for 9.0.1. **(M — days per wave)**

4. **MMIO dispatch for live devices (if needed)** — the existing SIGSEGV handler (`sigsegv.h`)
   can serve as an MMIO dispatch: leave a page unmapped, decode the faulting instruction on
   access, dispatch to a C++ handler. No new abstraction needed. Only build this if step 3
   reveals a device-register access the passive fake-page model can't satisfy. **(L — only if
   data demands it)**

### Scariest unknowns

1. **ECB structure depth** — could be 2 fields or 20; one missed init word = same crash.
2. **68k low-memory layout divergence** — OldWorld vs NewWorld may have different globals at
   the same addresses; the 25 absent shim patterns are this risk concretized.
3. **jump68k vs DR Emulator distinction** — NewWorld uses a different PPC→68k handoff path
   (nanokernel dispatch + `rfi` + SRR0) than OldWorld's `jump68k`. The OldWorld shim
   infrastructure may not apply without modification.

### Why this reopens the NW chapter

The stop-rule fired because "remaining work = ROM-specific byte-patching with no general fix
on the horizon." The synthetic-stub approach changes that: if the DR Emulator wall falls to a
4-word init (like SegMap fell to a 34-instruction stub), the cost is low and the forcing
function can keep running — potentially surfacing more general bugs in the 68k execution path.
The stop-rule should be re-evaluated after step 1 (ECB enumeration) gives us the real scope.

### §2.7.1 Result: DR Emulator entry cleared (2026-06-09)

The synthetic ECB stub approach worked on first attempt. The fix was even simpler than
expected: the KDP field writes already existed inside `SS_NW_SYNTH_ENTRY` (Path B diagnostic)
— they just needed promoting to the outer `SS_NW_TRAMPOLINE` block (Path A).

**What was added to `SS_NW_TRAMPOLINE`:**
- `KDP+0x65c` → ECB pointer (KDP+0x1000)
- `KDP+0x660` → 0
- `KDP+0x5f0`, `KDP+0x5f4` → EMUL_RETURN handler (ROMBase+0x366080)
- `KDP+0x648` → opcode dispatch table (ROMBase+0x480000)
- `KDP-0x964` → UserModeMSR (0x0000d032)
- Guest `[0]`=0 (68k SP), `[4]`=ROMBase+0x2a (68k reset PC), `[8..255]`=RTE stubs

**Test result (without `SS_ROM_SKIP_JUMP68K`):**
- `jump68k REDIRECTED` via Path A (`parcels_rfi_dat` pattern → `mtctr;bctr;nop`)
- DR Emulator region 50460000 compiled (pc=5046e8c0) — cold-start entered
- Crash at PC=0x72bf0000: nanokernel register-restore at 0x503244e8 loaded garbage from
  ECB, `rfi` to uninitialized SRR0

**Next wall:** The 68k dispatch table or cold-start init subroutine at ROM+0x36db94. The
crash pattern (sequential garbage in r0-r31, 0x80000 spacing) suggests the ECB fields
written by the cold-start init computed wrong handler addresses, or `patch_68k_emul()`
didn't fully populate the dispatch table for NewWorld.

**Stop-rule revision:** Original closure was "remaining work = 84-shim porting." This wall
fell to ~10 lines of existing code promotion — no byte-pattern work. New discipline: keep
the forcing function running while walls fall cheaply (<1 day each). Re-evaluate when a
wall requires multi-day ROM-specific RE with no general payoff.

## 2.8 Obstacle map — from here to a booting OS 9 screen (2026-06-09)

> **Purpose:** A clear-eyed assessment of every known wall between the current state (nanokernel alive,
> DR Emulator entry reached but crashing) and a working Mac OS 9.x boot on the NewWorld ROM. Organized
> by phase, with effort estimates and the forcing-function ROI for each.

### Phase 1: PPC→68k handoff (current wall)

**Status:** DR Emulator entry reached (§2.7.1), crashes on uninitialized ECB/dispatch table.

| Task | Effort | ROI (general bugs?) | Detail |
|---|---|---|---|
| Synthetic ECB stub completion | S (hours–1d) | Low (ROM-specific) | §2.7.1 got DR Emulator to execute. Crash at `0x72bf0000` = `rfi` to uninitialized SRR0 from garbage ECB fields. Need to enumerate every memory read in the DR Emulator cold-start sequence (`SS_DUMP_ROM` + capstone) and populate the missing fields. Same spike technique as SegMap/PMDT. |
| `patch_68k_emul()` dispatch table for NewWorld | S–M (1–3d) | Low-Med | The 68k trap dispatch table at ROM `0x36e600–0x36ea00` (twi→EMUL_OP branches) may live at different addresses in the parcels ROM. Without it, 68k traps crash instead of dispatching to host handlers. Need to find the parcels equivalent — may be at the same address (it's in a stable ROM region) or may need RE. |

### Phase 2: 68k HLE shim porting (`patch_68k`)

**Status:** `patch_68k` runs with `SS_ROM_LENIENT=1` (warnings, not aborts). 84 byte-pattern
searches, of which 28 in-range, 31 relocated, 25 absent in 9.0.1.

| Task | Effort | ROI | Detail |
|---|---|---|---|
| Verify 31 relocated patterns | M (3–7d) | Low (grind work) | Each pattern was found by whole-image fallback but the offset-relative patches (`found+N`) may land wrong. Example: `run_diags_dat` at offset 0xde (declared range 0x110–0x128), the `-6` patch write may hit the wrong instruction. Manual verification per pattern: disassemble at found offset, confirm the patch still makes semantic sense. |
| RE 7 hard-abort absent patterns | M–L (5–10d) | Low-Med | `powermac_id_dat`, `nvram2–6_dat`, `timek_dat` — byte sequences rewritten between 1998 and 2001. Each needs: find the equivalent routine in the 9.0.1 ROM, determine if a shim is still needed (parcels may self-handle), write a new search pattern or skip-guard. The NVRAM cluster (5 patterns) may share a single rewrite. |
| Triage remaining 18 soft-fail patterns | S (1–2d) | — | Already have lenient guards. Determine which are OldWorld-only (no action) vs genuinely needed for 9.0.1 boot (promote to the RE queue). |

**Key insight:** Not all 84 patterns are needed for boot. The boot path exercises a subset —
NVRAM read/write, VIA init, run-diagnostics skip, reset vector. Many patterns (Sony floppy,
SCSI, serial) are runtime shims that only matter after the OS is up. **Phase 2 is incremental:
fix patterns as boot hits them, not all 84 upfront.**

### Phase 3: MMU fidelity (§3 rung ladder)

**Status:** Rung 1 complete (real SDR1 + RAM-backed HTAB). Nanokernel builds page tables
successfully. Unknown: whether any non-identity PTEs exist.

> **2026-06-10 update (post-M1 spike):** The page-descriptor loop ceiling at `0x503123fc`
> (un-seeded `KDP+0x6b4`) that was previously silently masked by `ignoresegv` is now
> **ROOT-CAUSED + FIXED** (commit `d8932203` — ROM instruction patch in `rom_patches.cpp`,
> gated lenient+newworld; see §1 table retraction row above). Boot now advances one stage
> further. **Current frontier (the genuine SR/BAT/supervisor-environment wall):** SIGSEGV at
> `0x50326050–0x50326068` — `lwbrx` of hardcoded physical address `0x200a0` (below RAMBase)
> inside the NK's MMU/segment-fault handler: `mtdbatl/mtdbatu` (BAT register writes),
> `mtsrin` (segment register writes), `mtmsr` (translation toggling), byte-reversed PTE
> accesses. This is M3/M5 territory in the Machine Layer plan — crossing this wall requires
> real exception-delivery infrastructure and SR/BAT stored-state (rung 2+), not a one-liner.
> The stop-rule fired correctly; the wall is now documented and root-caused.

| Task | Effort | ROI | Detail |
|---|---|---|---|
| Rung 2: SR/BAT as stored state | S (hours) | Med (general) | Like SPRG — store what `mtspr` writes, return it on `mfspr`. General correctness fix (currently returning stale/fake values). **This is the minimum prerequisite to advance past the 0x50326050 frontier.** |
| Rung 3: Pre-seed identity HTAB | M (2–3d) | Low-Med | Only if nanokernel or OS reads back PTEs and expects non-zero entries. Measure first (watchpoint on HTAB region). |
| Rung 4: Shadow-arena / Dynamic-BAT | L (1–2w) | Med (if reached) | Only if data proves non-identity PTEs in a hot path. Fully designed in `MMU-WITHOUT-GUTTING-FLATMEM.md`. The 16 KB host-page vs 4 KB PPC-page tension is the hard part. |
| Rung 5: Software TLB | XL (weeks) | High (if reached) | QEMU-style softmmu. Last resort. ~6–8 extra instructions per memory op. Probably never needed (classic Mac OS is morally V=P). |

### Phase 4: OS 9.x compatibility (beyond the ROM)

These walls only appear after the 68k OS starts executing (Phases 1–2 complete).

| Task | Effort | ROI | Detail |
|---|---|---|---|
| Device-tree identity checks | S–M | Low | Mac OS 9.2 checks `model`/`compatible` in the Name Registry. `SS_NW_MODEL` probe was insufficient for 9.2.1. May need gestalt-based identity + nanokernel version fields. |
| NVRAM expansion (nvram4–7) | S–M | Low-Med | NewWorld ROM has 7 NVRAM routines vs 3 in OldWorld. Mac OS 9.x stores preferences in the extended space. Unshimmed routines hit nonexistent hardware. |
| AltiVec / processor feature detection | S | Med | 9.0.1 ROM targets G3/G4. `gestaltPowerPCFeatures` (`'ppcf'`) bit 0x10 = AltiVec. Already have `SS_FORCE_ALTIVEC=1` for the gestalt; may need deeper checks (PVR probe, vector instruction probe). |
| 68k trap table layout differences | M | Low-Med | NewWorld ROM may have different 68k trap dispatch table offsets. If `patch_68k_emul()` writes to wrong addresses, every 68k trap crashes. Need to verify the dispatch table location in the parcels ROM. |
| Open Firmware / Trampoline residue | ? | ? | The parcels ROM expects certain OF-initialized state (device tree, NVRAM partition map). SheepShaver fakes the Name Registry but may miss OF-specific structures. Unknown scope — may be zero (if the ROM re-initializes) or significant. |

### Phase 5: Runtime (if we get there)

| Task | Effort | ROI | Detail |
|---|---|---|---|
| NativeOp dispatch for 9.x managers | M–L | Med | The 60+ NativeOp selectors (video, ethernet, serial, resource mgmt) assume OldWorld calling conventions. NewWorld may pass arguments differently or expect different return conventions. Likely works mostly — test incrementally. |
| Toolbox manager differences (9.x vs 8.x) | ? | ? | Mac OS 9.x has updated Memory Manager, File Manager, etc. Most run in 68k emulation (our HLE handles them). Unknown whether any 9.x-specific manager hits an unhandled code path. |

### Effort summary

| Phase | Estimate | Blocks on | Forcing-function ROI |
|---|---|---|---|
| 1 — PPC→68k handoff | 1–3 days | Nothing (current) | Low (ROM-specific), but unlocks Phase 2 |
| 2 — patch_68k shims | 1–2 weeks (incremental) | Phase 1 | Low-Med per pattern, but high volume |
| 3 — MMU fidelity | Hours (rung 2) to weeks (rung 4+) | Independent | Med-High (general PPC correctness) |
| 4 — OS 9.x compat | Days–weeks | Phases 1+2 | Low (OS-specific) |
| 5 — Runtime | Weeks+ | Phases 1–4 | Med (tests fresh JIT code paths) |

**Total realistic estimate to reach a Mac OS 9 boot screen:** 3–6 weeks of focused work, assuming
no surprises beyond the cataloged walls. The forcing-function ROI is front-loaded (Phases 1+3
yield the most general bugs); Phase 2 is a grind with diminishing returns per pattern.

**The `sc` double-increment bug** (§1.7.2) is worth fixing independently as a general correctness
issue (`execute_syscall` calls `execute_illegal` which does `increment_pc(4)`, then does its own
`increment_pc(4)` = net +8). However, fixing it alone won't enable the idle loop's `sc` polling —
SheepShaver would still need a real PPC exception vector for `sc` to dispatch to the nanokernel's
syscall handler, which is a much larger change. The synthetic task-injection approach (§2.7)
bypasses this entirely.

---

## 3. MMU on Apple Silicon — approaches & ideas

### 3.1 The reframe that defuses "MMU is slow & complex": it's a *pointer* bug, not a *translation* bug

The current stall is **not** a translation problem. The nanokernel is zeroing/building a page table
whose base+size it derives from the **fake `SDR1`** (`0xdead001f` → HTAB at `0xdead0000`), which is
inside the NATMEM reservation but **not RAM-backed** → every store faults (slow `vm_fault` per host
page) and it stalls. **Nothing here needs the JIT to translate a single load/store.** It needs the HTAB
to **live in real RAM at a sane address.**

**The nanokernel needs self-consistent *bookkeeping*, not a functional MMU.** Classic Mac OS is morally
identity-mapped (one shared address space, no per-app isolation) — a "real" MMU here would mostly build
identity maps anyway. So if we let the guest build/zero/hash/walk its HTAB in **RAM-backed memory** and
**store** SR/BAT/SDR1 so reads are coherent (exactly what we just did for SPRG), then **every page-table
entry it makes maps V→V (identity)**, the JIT keeps doing `LDR [RMEMBASE, UXTW(ea)]` (V=P), and **never
reads the table** — because under V=P the answer the table would give *is* the address already used. The
guest keeps a coherent model of an MMU in a corner of RAM that we ignore. **Hot-path cost: exactly zero
added instructions. No `mprotect`. No 16 KB-page interaction.** That last sentence is the answer to
"slow and complex": the cheap rungs touch none of it.

**The one failure boundary:** the instant the nanokernel builds a **non-identity PTE that a hot-path
access depends on** (V≠P, and it expects translation to honor it), identity is a lie. The whole plan is
organized around *measuring whether that boundary is ever crossed* rather than assuming it.

### 3.2 The discriminator, and the lazy rung-ladder

> **D — Does the New World nanokernel build only identity (V→V) PTEs, or ever a non-identity mapping a
> hot-path access then depends on?** This is the only question that decides how far you must climb. The
> cheap rungs are **self-validating**: build rung 1; if it boots on, D was "identity-only" and you're
> done; if it stalls on a translation dependency, you've *measured* D = "non-identity" and earned the
> right to climb. Climb only as far as it stalls. **Do not promise a rung; do not pre-build rung 4/5.**

| Rung | Scheme | Effort | Hot-path cost | 16 KB issue | Build iff |
|---|---|---|---|---|---|
| 0 | V=P stub (today) | — | 0 | none | (Old World / 9.0.4) |
| **1** | **Real-RAM HTAB + sane SDR1** | **hours–1d** | **0** | **none** | New World builds a table but maps identity ← **START HERE** |
| 2 | Honor SR/BAT/SDR1 as stored state (like SPRG) | low-med | 0 | none | nanokernel reads back what it wrote |
| 3 | Selective: pre-seed identity HTAB / HLE the build routine / fake the post-condition | med (RE the routine) | 0 | none | the build/zero loop itself is the cost, or it expects an OF-pre-built table |
| 4 | **Shadow-arena / Dynamic-BAT** (already fully designed — `MMU-WITHOUT-GUTTING-FLATMEM.md`) | high | 0 (cost at rare map-change) | **safe only if maps ≥16 KB-aligned** | a *coarse* non-identity map appears |
| 5 | Software TLB / inline probe (QEMU softmmu) | high | **~6–8 insns + branch per mem op** | — | *fine* 4 KB non-identity paging in a hot path — the nightmare; last resort |

**Dead ends (don't chase):** **Hypervisor.framework** virtualizes the host ARM64 ISA only; our PPC CPU
is software-emulated and never runs on a hardware vCPU, so HV has no guest address space to give —
the address-space primitives you'd actually want (`mach_vm_remap`, shared-mem mirroring) are Mach VM,
which rung 4 already uses. **Lazy SIGSEGV per-4KB-page commit** is painful precisely because the host
page is 16 KB (one fault commits 16 KB → can't protect at PPC 4 KB granularity; false permission
sharing across 4 guest pages) + W^X/reentrancy hazards — only viable as rung 4's miss handler, not a
primary scheme.

### 3.3 Recommended path + the cheap, decisive first experiment

**Recommended:** climb lazily from rung 1; let each stall *measure* the next requirement; **stop** the
moment a boot stops depending on a missing rung. Rungs 1–3 add zero hot-path instructions, touch zero
host-MMU granularity, and reuse mechanisms already in the tree — "slow and complex" only starts at
rung 4 and only if **data** forces it.

**Lateral key — "honor-the-write beats fake-the-read":** the `0xdead` fake exists only because
`mtspr SDR1` is currently **dropped**. If the nanokernel *computes* its own HTAB base (likely, since
SheepShaver has no Open Firmware to pre-build one), then simply **storing `mtspr SDR1`** (stop dropping
one write) makes the table land in real RAM with no magic constants. Try that *first*.

**First experiment (hours, decisive):**
1. Either **stop dropping `mtspr SDR1`** (store it), or make `mfspr SDR1` return `HTABORG|HTABMASK`
   pointing at an HTABMASK-aligned, **RAM-backed** block reserved high in guest RAM (start small — a
   small HTABMASK shrinks the nanokernel's zero-loop, since it sizes the loop from the mask).
   Stub sites: `ppc-execute.cpp` `execute_mfspr`/`execute_mtspr` (SDR1 cases), and `ppc-jit.cpp`
   mfspr/mtspr (cases 339/467 fall back to interp for SDR1 — so the interp change may suffice).
2. Boot the New World 9.0.1 ROM (diagnostic config, §4); watch via heartbeat whether it advances past
   the ~128-block `0x322990` zero-loop stall.
3. **Measure D:** `SS_JIT_WATCH_ADDR` on the HTAB region — does it only *write* PTEs (build) or also
   *read/walk*? For each PTE written, check **RPN == VPN** (identity) vs not. **All identity → D =
   identity-only → rungs 1–3 finish the job.** Any non-identity PTE a later access depends on → D =
   non-identity → rung 4 is now justified *by data*, not folklore.
4. Outcomes: (a) clears + all-identity → ship rung 1/2, done; (b) clears + non-identity → schedule
   rung 4 with a concrete trigger; (c) doesn't clear → SDR1 wasn't the blocker; suspect an
   OF-pre-built-table expectation → try rung 3a (pre-seed an identity HTAB before boot).

**Belt-and-suspenders ideas:** pre-seed the reserved HTAB with identity PTEs so a walk always confirms
V=P; advertise the **smallest legal HTABMASK** to keep the table (and its zero-loop) tiny; park the
HTAB in a reserved high-RAM hole the OS heap won't reuse, so non-identity experiments stay clean. The
fake-SDR1 stall is itself a **precise sensor** of where New World first touches its page table —
instrument it (watchpoint) before fixing it; it's the cheapest probe of "what does New World actually
do with the MMU," which the prior docs flagged as unmeasurable without a booting ROM. You now have one.

**Heavier fallbacks (only if D forces them):** `MMU-WITHOUT-GUTTING-FLATMEM.md` (rung 4 shadow-arena,
fully designed), `MMU-DEFERRAL-REDTEAM.md` (the 16 KB-vs-4 KB hinge + softmmu cost case + HV dead-end),
`MMU-NANOKERNEL-MP-PLAN.md` (canonical verdict). Don't re-derive these — they stand.

---

### 3.4 Cross-emulator prior art — what to borrow, and the key caveat

Other PPC emulators solved adjacent problems, but mind *which* problem each actually solves:
- **Dolphin (GameCube/Wii, Gekko/Broadway PPC → ARM64/x86 JIT) — most transferable for our fast path +
  Apple-Silicon specifics.** Its **"fastmem"** is our model (direct V=P into one big host mapping; a
  SIGSEGV handler decodes + **backpatches** the faulting access to a slow path). It ships on Apple
  Silicon → battle-tested against our two hinges, **16 KB pages + W^X/MAP_JIT**. Also lift: block
  linking/dispatch, **idle-loop detection** (cf. our boot-stall watchdog), Gekko **FP rounding** craft
  (cf. fctiw). ⚠ Uses **BATs, not a hashed page table** — its "Dynamic BAT" remap-on-change IS our
  rung-4, but it never boots a kernel that builds an HTAB, so it doesn't crack the current wall.
- **RPCS3 (PS3 Cell PPU = 64-bit PowerPC → LLVM JIT) — the 64-bit-PPC reference** (full fctid/fcfid/64-bit
  semantics → relevant to the G5 gap; strong VMX/AltiVec cross-check) and the **HLE-the-OS** mindset
  (fakes the OS rather than booting the kernel → maps to our Path B / synthesize-the-environment / HLE
  options). ⚠ Because it HLEs the OS, it never builds a real page table — little for the HTAB wall.
- **⭐ Closest analogs for OUR exact problem (boot a real Mac OS kernel that builds its own hashed page
  table): PearPC and QEMU.** **PearPC** implemented a *real* PPC MMU (segments + BATs + hashed page
  table) and booted Mac OS X / Darwin — the reference for "honor SDR1/HTAB for real" (rung 1→4).
  **QEMU `qemu-system-ppc`** implements the full PPC MMU + OpenBIOS, and **elliotnunn already runs THIS
  nanokernel under QEMU** (see §5.1 e-maculation/QEMU thread) — so QEMU is both a reference and a live
  oracle: observe the nanokernel's real SDR1/HTAB behaviour there and diff against ours.

Through-line: borrow **Dolphin** for the fast-path + Apple-Silicon craft, **RPCS3** for the HLE mindset
(Path B), and **PearPC/QEMU** for the actual HTAB/SDR1 wall. Provenance: all GPL-compatible — read for
technique; cite any adapted approach (CONTRIBUTING / backport hygiene).

## 4. How to work (the loop, the gates, the footguns, repro commands)

**The forcing-function loop** (this is the proven method that got us 27→128 PCs):
1. Boot the parcels ROM in the diagnostic config (below). 2. Use the diagnostics to find *where* it
wedges/derails. 3. Disassemble that PC region (capstone) + cross-ref `elliotnunn/NanoKernel` source.
4. Form a root-cause hypothesis. 5. Make the **smallest, ideally general** fix, **env-gated**. 6. Boot
again; confirm it advances. 7. Commit, update `CHANGELOG.md` + this doc + `NEW-WORLD-ROM-SUPPORT-PLAN.md`
+ `LEARNINGS.md` + the `newworld_parcels_port` memory.

**Diagnostic boot (non-destructive; isolated prefs, no disk):**
```
# /tmp/trace901.prefs points at the decoded-capable 9.0.1 ROM with nogui + no disk.
cd SheepShaver
SS_ROM_LENIENT=1 SS_NW_TRAMPOLINE=1 SS_LOG_FIRST_BLOCKS=3000 \
  ./src/Unix/SheepShaver --config /tmp/trace901.prefs 2>/tmp/boot.log &
# let it run ~30s; it reaches DR Emulator entry then crashes (ECB stub incomplete).
# Add SS_ROM_SKIP_JUMP68K=1 to skip jump68k entirely (isolates nanokernel-only behavior).
```
Asset: `/Users/Shared/macemu/2001-12-19 - Mac OS ROM 9.0.1.rom`. Decode it to a flat image for offline
disasm with `rom-inspect --dump`: `SheepShaver/rom-inspect/rom-inspect "<rom>" --dump /tmp/rom901.bin`.

**Diagnostics (all env-gated, zero-cost off):**
- `SS_LOG_FIRST_BLOCKS=N` — dumps the first N block-entry PCs (the boot path) + a one-shot lock-state
  dump at the parcels spinlock. The primary tool for "where is it / how far did it get."
- `SS_JIT_TRACE_RING=1` + live `lldb -b -p $(pgrep -x SheepShaver) -o "expression -- (void)ppc_jit_dump_trace_ring()" -o detach -o quit` → `/tmp/ss_jit_ring.txt` (the recent block history at a wedge). Attach **at most once per run** (lldb defers the VBL timer — see CLAUDE.md/LEARNINGS).
- `SS_JIT_VERIFY=1` — re-runs every JIT block through the interpreter and flags register divergences =
  real codegen bugs. Slow but decisive (it cleanly ruled codegen *out* for the spinlock wall).
- `SS_SYNTH_DEC=1` — synthetic free-running decrementer (mfspr DEC was 0). Use to rule in/out timing.
- Heartbeat `[HB …]` (stderr) — block rate, `comp` (new-block count: advancing = progress, frozen =
  stuck), region split (`jNK`/`jDR`/`jRAM`), `iDR` (interp fallback). The `[ALARM]` boot-stall watchdog
  fires pre-idle on a wedge. Diag file: `/tmp/jit_diag.<ts>.<pid>.log` (HOT-PC, `pc=` samples).
- Analyzer: `python3 SheepShaver/tools/jit-analyze.py {diag|ring|hot} <log>`.
- Capstone disasm of the decoded ROM (PPC, big-endian): run scripts from a dir with **no** file named
  `dis.py`/`capstone.py` or you hit a circular-import error.

**The gate:** `cd SheepShaver && make test-jit` must stay `score=100` (currently **350/350**). Run it
after every codegen/struct change.

**Two footguns that WILL bite (memory `stale_build_struct_offsets`):**
1. Changing `ppc-registers.hpp` (the `powerpc_registers` struct) needs **all PPC TUs recompiled** — the
   Makefile doesn't track that header dep. A stale incremental build silently mismatches struct offsets
   across translation units and shows up as `test-jit=0` (looks catastrophic; it's not). After any
   struct change: `rm SheepShaver/src/Unix/obj/ppc-*.o SheepShaver/src/Unix/obj/sheepshaver_glue.o SheepShaver/src/Unix/obj/ppc-jit.o && make build-ss`.
2. The JIT **hardcodes byte offsets** into `powerpc_registers` (e.g. `PPCR_RESERVE_VALID 1060`,
   `PPCR_FPSCR 1040`). **Append new struct fields LAST**, after the reserve block, or you shift a
   hardcoded offset and corrupt `lwarx`/`stwcx`/FP.

**Hand-encoded ARM64:** the JIT emits raw instruction words (`emit32(0x…)`). **Always validate new
encodings with capstone** before trusting them (CS_ARCH_ARM64) — this caught two bad encodings during
the fctiw fix. Reuse known-good idioms only after verifying their exact bit layout.

**Process rules:** only one emulator instance at a time (`pkill -9 -x SheepShaver` first). The
diagnostic boot above is non-booting-to-OS and safe to run yourself; the maintainer otherwise prefers
to run interactive boots. Commit with `git commit -F-` (no backticks in `-m`). Don't commit the
co-worker's files (SiliconSheep/*) unless asked.

---

## 5. Key references (read these before touching code)

| File | Why |
|---|---|
| `docs/planning/NEW-WORLD-ROM-SUPPORT-PLAN.md` | The full parcels-ROM history, wall-map, the GUIDING POLICY (correctness-via-forcing-function), and the current MMU-wall characterization. **Start here.** |
| `docs/planning/sheepshaver-research/SPRG0-KDP-DESIGN.md` | RE design of the SPRG0/KDP/Trampoline environment (what SPRG0 points to, KDP layout, P0–P4 plan). |
| `docs/planning/MMU-NANOKERNEL-MP-PLAN.md` | The canonical MMU/nanokernel/MP options dossier (shadow-arena / Dynamic-BAT). |
| `LEARNINGS.md` (2026-06-07/08 entries) | The diagnostic methodology, the SPRG/stale-build/footgun findings, the "session 5 HOT-PC retraction" (don't infer hangs from PC samples). |
| `docs/ARCHITECTURE.md` | DIRECT_ADDRESSING / NATMEM, W^X, the JIT structure. |
| `elliotnunn/NanoKernel` (clone to /tmp) | The buildable, version-branched RE'd source of the exact nanokernel — the Rosetta stone for every wall. (`Init.s` = HTAB/KDP setup; `Exceptions.s`; `PPCInfoRecordsPriv.s` = KDP struct.) |
| Code: `SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp` `init_emul_ppc` (boot register/SPRG/KDP setup); `src/Unix/main_unix.cpp` (`KERNEL_DATA_BASE`=0x68ffe000, memory map); `src/kpx_cpu/src/cpu/ppc/ppc-execute.cpp` (interp mfspr/mtspr — ground truth); `ppc-registers.hpp` (the regs struct). |

### 5.1 Prior-art bibliography — adapt documented layouts, don't RE blind

*(From a sourced literature survey. Authority: **OFFICIAL** = Apple/IEEE primary; **RE** = buildable/
byte-accurate reverse engineering; **lore** = forum/wiki, orientation only.)* **Read these first:**

- **Apple TN1167 "The Mac ROM Enters a New World"** [OFFICIAL] — the canonical NewWorld map: the
  disk-based "Mac OS ROM" file, the `tbxi` bootinfo, the OF device tree in the Name Registry, the
  `model`/`compatible` machine-ID, Toolbox-loaded-to-RAM. *(Correction: the New World doc is **TN1167**,
  not "TN1061/Fundamentals of the New World"; TN1060/61/62 are the Open Firmware series — TN1062
  "Device Tree" is the authoritative device-tree reference.)*
- **`elliotnunn/tbxi`** [RE] (PyPI `tbxi`) — THE tool to dump/rebuild the parcels container; `tbxi dump`
  your actual 9.x ROM to see the nesting (`<CHRP-BOOT>` → Parcels → 4 MB MacROM → 3 MB 68k ROM). Also
  the de-facto parcels-format documentation. Use it on the real asset before anything else.
- **`elliotnunn/NanoKernel`** + **`elliotnunn/powermac-rom`** [RE, highest authority for the kernel] —
  buildable byte-accurate nanokernel source. `powermac-rom` (René Vega's "Multitasking" kernel) is the
  **closest RE to the 9.x kernel our NewWorld ROM actually runs** — prefer it for 9.x. This is the spec
  for KDP/SPRG/MMU/exception behaviour; **there is no published table** — the source IS the doc.
- **`elliotnunn/OldKern` → `KDP.h`** [RE] — the *only* explicit field-by-field **KDP layout** found
  (a 0x1000 page: GPR save area, segment maps, BAT table, vector tables, MMU pointers at 0x5c8–0x684,
  dispatcher table, panic save area, InfoRecords at 0xcc0+). ⚠ It's the **old-style** kernel — use it as
  the *shape/idiom*, verify 9.x deltas against `powermac-rom`/`NanoKernel` v2.x.
- **`elliotnunn/wedge`** (bootloader↔NanoKernel shim) [RE] — **the artifact most likely to encode the
  Trampoline→nanokernel hand-off contract.** Read it alongside the nanokernel's entry code to answer the
  §1.6 open question (what state the loader must leave: SPRG0/SDR1/KDP/`NKSystemInfo`).
- **`cebix/macemu` SheepShaver source** [RE, working impl] — how an emulator *fakes* KernelData/
  EmulatorData and *which* fields actually matter at runtime (grep `XLM_KERNEL_DATA`, `KernelData`,
  the interrupt routines). Pair with elliotnunn source to recover the contract.
- Background prose: **Amit Singh, *Mac OS X Internals* Ch.4** (OF/device-tree/boot mental model);
  **Inside Macintosh: *PowerPC System Software*** (nanokernel role + Mixed Mode Manager — the
  `[MIXEDMODE]` reference); **68kMLA "Picking apart the NewWorld ROM"** + **e-maculation "QEMU + the
  Nanokernel"** (Trampoline *behaviour*: device-tree copy, interrupt setup, bridge init).

**Documented GAPs (RE is unavoidable — expect to read source, not docs):** the **exact 9.x KDP byte
layout**, **9.x SPRG conventions**, the **CPU-detect/per-CPU/L2-L3 tables inside the ROM** (vendor G3/G4
upgrade material is marketing-grade only — no technote documents the patch surface; the real RE leads
are the MacOS9Lives ROM-patching threads + tbxi disassembly), and the **Trampoline's SPRG0/SDR1/KDP
post-conditions** (no public source states them — recover via `wedge` + nanokernel entry + reversing the
Trampoline PEF extracted by `tbxi`). These gaps are *why* we lean on elliotnunn + our own boot probes.

**One-line orientation for your first hour:** read §0 and §1 here → read the NEW-WORLD plan's "PROGRESS"
+ "next target" sections → run the diagnostic boot and reproduce the `0x322990` page-table-init wall →
read §3's MMU options → propose your smallest first experiment to make SDR1/HTAB consistent, and run it
past the maintainer before building anything large.
