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
| Sub-KDP pool region unmapped (`0x68FF7000` below KERNEL_AREA) | Root cause: `KERNEL_AREA_SIZE=0x2000` (8 KB) → shmem covers only `[0x68FFC000,0x69000000)`. Pool init's stores to `0x68FF5000–0x68FF7000` fault → `ignoresegv` silently skips them → pool data never written → allocator reads garbage → infinite zeroing loop at `0x50322990`. Fix: `vm_acquire_fixed` 32 KB below shmem base (env-gated). **VERIFIED** via SIGSEGV-handler instrumentation with probe present during both before/after runs (10 faults → 0). Boot advances 128 → 265 unique PCs; pool-init + zeroing-loop + allocator + HTAB/page-table init all complete. | ✅ committed + verified (env-gated) |
| SDR1/HTAB zeroing stall at `0x50311ff4` | **Three-part fix:** (a) real SDR1 register (`ppc-registers.hpp`+`ppc-execute.cpp`: `mfspr`/`mtspr SDR1` read/write); (b) trampoline allocates 64 KB HTAB at `0x68FE0000`, seeds `SDR1=0x68FE0000`; (c) ROM patcher skips `sdr1_read`+`pgtb_clear` patches for parcels (gated on `g_rom_904_lenient`) so the real `mfspr SDR1` + real zeroing loop execute against mapped memory. HTAB zeroing now completes in milliseconds. **General correctness fix** (SDR1 was silently wrong for all guests). | ✅ committed |
| **CURRENT WALL: stuck at `0x50312250`** | After HTAB zeroing, nanokernel advances through a small data-structure zeroing loop at `0x503109bc` (r9=0x1ff8, 2047 iterations, completes cleanly), then settles at `0x50312250` — a new tight loop. comp=420, HOT-PC=`0x50312250`, r9=`9700244f`, r10=`5046e8c0`. Not yet disassembled. Likely the next stage of nanokernel initialization hitting an unsatisfied dependency (descriptor table, memory mapping, or decrementer). | ☐ todo |

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

**The current wall (`0x50312250`).** After HTAB zeroing, the nanokernel passes through a small
data-structure init loop at `0x503109bc` (`addic. r9,r9,-4; stwx r0,r10,r9; bne` — zeroes guest
addresses `0x0–0x1ff8`, 2047 iterations, completes cleanly). Boot then reaches **420 unique compiled
blocks** and settles at `0x50312250` — a new tight loop that runs indefinitely. Heartbeat:
`r9=9700244f, r10=5046e8c0, cr=24e00088`. Not yet disassembled; likely the next nanokernel init
stage hitting an unsatisfied dependency (descriptor table creation, decrementer, or another
memory-mapping requirement).

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
