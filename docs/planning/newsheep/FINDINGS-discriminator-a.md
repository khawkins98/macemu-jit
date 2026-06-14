# Discriminator-A — NanoKernel MMU granularity (SS_M18 Stage 1)

**Date:** 2026-06-14  **Agent:** Discriminator-A probe (S1 gating-Task-0)
**Authority:** `docs/superpowers/plans/2026-06-14-ss-m18-trampoline-lle-program.md` — "Discriminator-A runbook" + Stage 1.
**Q-S1.2 (walker-vs-window):** Does NanoKernel-v02.27 map memory COARSE (segment/BAT, >=256 MB -> Dolphin shadow-arena, 16 KB-host-page-safe, bounded map-change hook) or FINE (dense 4 KB hashed-PTEs w/ mixed per-page perms -> softmmu, fast path lost, S1 balloons)?

## Verdict
**COARSE — mechanism conclusively confirmed by static RE of the md5-verified NK binary; live-QEMU HTAB-density measurement BLOCKED (OF-handoff stall).** Path **a (Dolphin shadow-arena)** indicated. The one residual FINE-falsifier (live HTAB PTE density / per-page perm diversity) could NOT be measured on the real mapping within the <=4-boot budget because the QEMU rig never executes the NK MMU-install cluster (stalls in OF/Trampoline bootloader). That residual is NOT retired by live observation; routed to S1's standalone MMU-oracle unit test (test-ladder item 2). Conservative fallback (UNKNOWN->fine/softmmu) applies only if the S1 oracle later contradicts the static mechanism evidence.

## Provenance (verified first)
- 9.0.1 ROM (rig SRC_ROM `/Users/Shared/macemu/2001-12-19 - Mac OS ROM 9.0.1.rom`): `66210b4f71df8a580eb175f52b9d0f88` == 66210b4f OK
- NK parcel `/tmp/newsheep/dump-9.0.1/Parcels.src/MacROM.src/NanoKernel-v02.27`: `61c176e90b6365e84e5c660d703e56af` OK
- RSP client `/tmp/newsheep/gdbcli.py`: `f468451974d179919ddc09b1d3d09d30` (P2 watchpoint version, unmodified) OK
- Q2 pinned addrs re-verified by capstone vs parcel (base 0x50310000): SDR1 @0x50310604, SR loop @0x503104b4, mtsrin loop @0x50315290, MMIO seg-swap @0x50325894 — all matched exactly.

## Evidence A — static RE of md5-verified NK binary (PRIMARY, conclusive on mechanism)
1. **SR translation, 256 MB granularity.** @0x503104b4 NK programs all 16 SRs (unrolled `mtsr 0..15`). Per-context loop @0x50315290: `lwzu r30,-8(r28); addis r31,r31,-0x1000; mtsrin r30,r31; bne` — `addis -0x1000` steps the EA top 4 bits = one 256 MB segment/iter -> context switch at segment granularity.
2. **BAT block translation.** @0x503152c4 all 4 IBATU/DBATU invalidated, then (0x503152e4+) upper/lower pairs loaded from a descriptor table (`lwz r30,0x280(r29)/r31,0x284(r29)`) into `mtibatl/mtibatu 0..3`. Large (>=256 MB-class) blocks.
3. **MMIO via SR-swap (not per-page).** @0x50325894 `mfsrin r21,r22; mtsrin r24,r22; <stb/lbz -0x6000(r22)>; restore` — MMIO reached by swapping a whole segment register (SR-granular).
4. **HTAB exists, RAM-proportional, but residual.** @0x503105a4 HTABMASK from memory size via `cntlzw + lis 0x1ff; srw; ori 0xffff; clrlwi 0xa`, then `mtspr SDR1` @0x50310604. PTEs present, but RAM/ROM (JIT-fast-path regions) are BAT/SR-mapped per (1)-(3); HTAB handles residual Mac OS logical/pageable space (already covered by SS flat-mem + DR model).
Runbook COARSE criterion (NK relies on SR/BAT for the mappings it uses — mtsrin@0x50315290, SR-swap MMIO@0x50325894) is satisfied; this is the live install path, not dead code.

## Evidence B — live QEMU (BEHAVIORAL, attempted, BLOCKED) [QEMU-BEHAVIORAL]
Rig: `qemu-rig.sh --gdbstub -S` (QEMU 11.0.1, mac99 -m 512, 9.2.1 CD + 9.0.1-ROM swap); gdbcli.py (bp) + qemu-mon.py (HMP info registers).
- `info registers` exposes SDR1/SPRG/SRR/PVR (PVR 000c0209 = 7400/G4) but NOT SR/BAT; `info tlb` is empty for this hash-MMU CPU (QEMU dumps only soft-TLB CPUs). [QEMU-BEHAVIORAL]
- **NK MMU-install cluster never executed.** Both boots: hw bps at 0x50310604/0x503104b4/0x50310000 never fired; SDR1 stayed at OpenBIOS's `0x1f80003f` (HTABORG 0x1f800000, HTABMASK 0x3f) throughout. NIP cycled in OF/Trampoline (0xf24030 <-> 0x463ae0/0x45fb60; RAM sample 0x6806e8c0); MSR toggled 0x49032 (bootloader) <-> 0x1000 (OF). The 9.0.1-swap boot stalls in OF->OS handoff before any NK-equivalent MMU install — no second `mtspr SDR1` at any base (rules out stop-rule-#6 "NK ran elsewhere": SDR1 unchanged = nothing installed a new MMU). [QEMU-BEHAVIORAL]
- Address-oracle caveat honored: QEMU MacIO=0x80000000 (ours 0xF3000000), no QEMU MMIO addr cited as reference; the one live value (SDR1 0x1f80003f) is OpenBIOS's, reported only to prove NK install was never reached.

## Boots used
- Boot 1 (/tmp/qemu-rig-disA, --timeout 120 -S): bp SDR1 + post-mtsrin; continue timed out; free-ran; capability survey (info registers/info tlb); 12x poll — SDR1 unchanged.
- Boot 2 (/tmp/qemu-rig-disA2, --timeout 200 -S): bp 0x50310604/0x503104b4/0x50310000, ~160s window; never fired; monitor confirmed SDR1 still 0x1f80003f.
- Boots 3 & 4: NOT used (reserved). Same OF-handoff stall would recur; no added signal. Budget <=4 respected.
gdbcli.py unmodified (md5 f468451...). No edits under SheepShaver/src/**.

## Implication for Stage 1
- Take **path a — Dolphin Dynamic-BAT shadow-arena** (16 KB-host-page-safe; bounded map-change hook on NATMEM/vm_alloc reservation + JIT memory emit sites; gated SS_M18_PAGED_MMU AND MachineProfileIsNewWorld()). NK working-set is BAT/SR (256 MB-class) -> JIT fast path preserved.
- Q-S1.3 minimal faithful translation = segment + context-table satisfying mtsrin@0x50315290 (+ tlbie/BAT program), NOT a full hashed-PTE walk.
- G1.b MMIO microtest mirrors SR-swap @0x50325894 (mfsrin/mtsrin around access).
- **Residual to retire in S1 (NOT retired here):** live HTAB PTE density / per-page perm diversity — settle via standalone MMU-oracle unit test (ladder item 2: shadow-arena vs reference-translator on identical (SR/BAT/SDR1, EA) -> equal PA, exercising JIT). If it shows NK depending on dense mixed-perm 4 KB PTEs for JIT-covered regions, fall back to UNKNOWN->fine/softmmu and re-band S1.

## Caveats
- PRIMARY evidence is static RE of the exact md5-verified NK binary (strong, unambiguous on mechanism: BAT+SR+SR-swap MMIO). QEMU is behavioral-only and here could not reach the NK, so it neither confirms nor refutes — it only establishes live-NK observation is unavailable via this rig for the 9.0.1-swap boot.
- The single FINE-falsifier not exercised on a real mapping is HTAB PTE density/perm diversity; verdict is COARSE on mechanism, FINE residual carried forward to the S1 oracle test rather than declared retired.
