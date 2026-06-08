# Handoff: New World supervisor-stack fidelity (the PPC MMU / page-table work)

> **Status:** 🟡 Active handoff · **Created:** 2026-06-08 · **Updated:** 2026-06-08
> **Why this doc exists:** Hand a fresh agent the next phase of work — giving SheepShaver's PPC JIT a
> consistent supervisor/MMU environment so the New World nanokernel boots further — AND, just as
> importantly, the *rationale* so the agent doesn't bounce off the predictable "SheepShaver wasn't
> built for this / run 9.2 some other way" objection.
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
| **CURRENT WALL: page-table / MMU init** (`0x322990` zero-loop, `0x3251e4` scan) | **Open — this handoff.** | ☐ todo |

**The current wall, precisely.** With the above shims, the nanokernel reaches its **page-table
initialization**: a memory-zeroing loop at ROM `0x50322990` (`stwu r14(=0),4(r15); cmpw r15,r16;
ble`) clearing the page-table / page-descriptor region, whose base+size it derives from **SDR1 / the
HTAB**. But SheepShaver **fakes SDR1** (`mfspr SDR1`→`0xdead001f`, so HTAB at `0xdead0000`) and our
`sr_init` patch feeds `lis r13,0xdead` + a `0x100000` page-table size. So the nanokernel zeroes a
**fake, non-RAM region** (`0xdead0000…`), which maps into the NATMEM reservation and is slow / wrong,
and it stalls (comp frozen at 146, ~136 blk/s, cpu busy, `iDR=0` so not interpreter fallback;
`SS_SYNTH_DEC` makes no difference → not a timing wait). **This is the genuine supervisor/MMU
page-table fidelity gap** — the "second wall" predicted in `MMU-NANOKERNEL-MP-PLAN.md`, now reached
concretely.

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

## 3. MMU on Apple Silicon — approaches & ideas

> **NOTE TO SELF (the agent writing this doc):** fold in the lateral/ADHD brainstorm here when it
> returns — concrete schemes ranked by effort/payoff, the 16 KB-page interaction, the perf cost model,
> and a recommended first experiment. Until then, the canonical options live in
> `MMU-NANOKERNEL-MP-PLAN.md` (shadow-arena / Dynamic-BAT verdict).

**[PLACEHOLDER — to be filled from the lateral brainstorm.]**

Key constraints the brainstorm must respect (repeat for the implementer):
- **Hot-path perf:** the JIT today does one `add` per guest memory op (V=P). Any scheme must keep that
  near-free for the common case; a per-access hash-walk/software-TLB is unacceptable on the hot path.
- **Page size:** PPC 4 KB vs Apple Silicon native **16 KB** — anything relying on host `mprotect`/`mmap`
  granularity must cope (host page = 16 KB).
- **W^X / MAP_JIT** for any code remapping.
- **Hypervisor.framework is almost certainly NOT applicable** (it virtualizes ARM guests, not PPC) —
  don't chase it.

---

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
SS_ROM_LENIENT=1 SS_ROM_SKIP_JUMP68K=1 SS_NW_TRAMPOLINE=1 SS_LOG_FIRST_BLOCKS=3000 \
  ./src/Unix/SheepShaver --config /tmp/trace901.prefs 2>/tmp/boot.log &
# let it run ~30s; it wedges (does not reach the 68k OS — jump68k is diagnostically skipped).
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

**One-line orientation for your first hour:** read §0 and §1 here → read the NEW-WORLD plan's "PROGRESS"
+ "next target" sections → run the diagnostic boot and reproduce the `0x322990` page-table-init wall →
read §3's MMU options → propose your smallest first experiment to make SDR1/HTAB consistent, and run it
past the maintainer before building anything large.
