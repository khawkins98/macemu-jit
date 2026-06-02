# How much of Mac OS 8.6 is 68k-emulated vs native PPC

**Date:** 2026-06-02
**Method:** READ-ONLY analysis of SheepShaver JIT-mode boot traces in `/tmp` + documented Mac OS 8.x architecture.
**Scope:** SheepShaver emulating a PowerPC Mac booting Mac OS 8.6.

---

## TL;DR

- The 68k-vs-native split is **strongly phase-dependent and bimodal** in the trace data.
- In **all-ROM boot segments** (traces `regs`, `bug`, `crash`, `flush`), **94–97% of block dispatches are inside the in-ROM 68k emulator** — i.e. the machine is spending almost all its time emulating 68k code.
- In the segment that progresses into **native PPC RAM code** (trace `ref`), the 68k emulator falls to **~1% of dispatches**, and native PPC RAM code rises to **85% and climbing** (91% in the last 20% of that trace).
- **Steady-state desktop idle is NOT present in this dataset.** The longest trace ends in active late-boot work (mixed RAM-PPC + toolbox + emulator), not a tight idle cycle. Steady-state claims below rest on the architecture/research section, not measurement.
- The 68k emulator burns **~2.4–2.6 emulator block-dispatches per dynamic 68k instruction retired** (consistent with the documented "3–6 PPC instructions per 68k instruction" once you account for block-level vs instruction-level counting).
- **Amdahl implication:** in emulator-dominated phases the 68k emulator is the floor — no amount of optimizing the *already-JIT-compiled* native PPC code can beat **~1.03–1.06×** there. Breaking that ceiling requires a **68k→ARM64 fast path that bypasses the PPC-interpreting-68k layer**.

---

## 1. Method and caveats

### What the traces contain

All traces start at guest PC `0x50310000` (early ROM boot). Two physical line formats:

- **`J` lines** (block-level): `J <from_pc> <to_pc> r24=… r27=… …`. The `<to_pc>` (2nd hex field, `$3`) is the guest PC *after* the executed block. This is the classification axis.
- **`I` lines** (dispatcher entries): `I <pc>`. These duplicate the adjacent `to_pc`, so to avoid double-counting, J-format traces are classified on `J` lines only; bare/`I`-only traces are classified on every line.

`<from_pc>` (`$2`) is the dispatch/loop origin and is often constant across consecutive J lines (it is **not** the previous instruction), so it is not used.

`r24` is the 68k PC inside the in-ROM emulator. It is stale (constant) while native PPC runs and only advances inside the emulator. r24 was extracted **only** from traces that label it (`r24=…`): `ref`, `regs`, `bug`, `flush`. The positional-column traces (`v2`, `crash`) have an unverified layout, so r24 was not extracted from them; only their `to_pc` is classified.

### Classification buckets (by guest PC)

| Bucket | Range | Meaning |
|---|---|---|
| **(a) 68k emulator** | `0x50460000–0x504FFFFF` | DR 68k emulator in ROM (emulating 68k code) |
| **(b) native PPC toolbox/nanokernel** | `0x50000000–0x5045FFFF` | native PPC ROM toolbox + nanokernel |
| **(c) native PPC RAM code** | `0x10000000–0x1FFFFFFF` | native PPC code in RAM |
| **(d) other** | SheepMem `0x5050–0x505F`, kernel data `0x68xxxxxx`, zero-PC fault, etc. | |

### Caveats (read before trusting any number)

1. **Block dispatches ≠ time.** These are block-execution counts, a *proxy* for time. A block of native PPC may do more real work per dispatch than a single emulator micro-step, so the emulator's *time* share is likely **somewhat lower** than its *dispatch* share — but the bimodal split is too large to be an artifact.
2. **No single clean timeline.** `ref`/`bug` are different runs (reference vs bug-repro); `regs`/`crash`/`flush` are separate captures. *Within-trace* progression (in `ref`) is measured. *Cross-trace* "boot is emulator-heavy, later RAM-native takes over" is **inferred**, not measured on one continuous run.
3. **No desktop idle.** The dataset captures boot, not steady-state idle (confirmed by tail inspection — see §4).
4. **`bug`/`crash`/`flush` end in fault signatures** (PCs walking `0x00000124→130→134`, jumps to `0x00000000`). Their bulk is valid emulator execution; only their tails are crash artifacts and are a negligible fraction.

---

## 2. Per-trace distribution

### J-format traces (classified on `to_pc`)

| Trace | Size | J lines | (a) 68k emu | (b) toolbox | (c) RAM PPC | (d) other | Character |
|---|---|---|---|---|---|---|---|
| **ref** | 806 MB | 6.55 M | **1.08%** | 13.40% | **85.50%** | 0.01% | reaches native RAM code; *progresses through boot* |
| **regs** | 961 MB | 11.57 M | **97.36%** | 2.64% | 0.0001% (14 lines) | ~0 | all-ROM, 68k-emulator-dominated |
| **bug** | 1060 MB | 8.62 M | **96.45%** | 3.55% | 0.0002% (14) | ~0 | all-ROM (ends in fault) |
| **crash** | 444 MB | 11.67 M | **97.38%** | 2.62% | 0.0001% (14) | ~0 | all-ROM (ends in fault) |
| **flush** | 359 MB | 4.33 M | **94.47%** | 5.53% | 0 | 0 | all-ROM (ends in fault) |
| **v2** | 13 MB | 0.31 M | 11.56% | 7.15% | **81.25%** | 0.03% | RAM-PPC-dominated segment |

### Bare / `I`-format traces (every line classified)

| Trace | Size | lines | (a) 68k emu | (b) toolbox | (c) RAM PPC | (d) other |
|---|---|---|---|---|---|---|
| **interp** | 4.7 MB | 0.52 M | 34.69% | 4.61% | 60.59% | 0.11% |
| **interp2** | 5.7 MB | 0.52 M | 34.60% | 4.62% | 60.67% | 0.11% |
| **jit** | 0.65 MB | 0.072 M | 39.81% | 12.93% | 47.04% | 0.21% |
| **jit2** | 0.065 MB | 0.006 M | 68.35% | 31.64% | 0 | 0.02% |

**Key observation — the data is bimodal:**

- **Emulator-dominated regime** (`regs`/`bug`/`crash`/`flush`): ~95–97% in the 68k emulator, essentially **zero** RAM dispatches. `regs` reaches RAM code on only **14 of 11.5 M** dispatches — it is effectively an *all-ROM* boot segment that never gets to native RAM application/extension code.
- **Native-RAM regime** (`ref`, `v2`): 81–85% in native PPC RAM code, 68k emulator down at 1–12%.
- The mixed bare traces (`interp`, `jit`) sit in between (35–60% RAM, 35–40% emulator), capturing a transitional slice.

The most trustworthy single number for "what does the machine do during 68k-heavy boot work" is the emulator regime's **~96%** (use `regs`/`bug`/`crash`/`flush`; they agree within 3 points). The most trustworthy number for "what does it do once native RAM code is running" is `ref`'s **85%+ RAM, ~1% emulator**.

---

## 3. 68k instruction throughput (r24 progression)

`r24` = 68k PC. Consecutive-line r24 *changes* = dynamic 68k instructions retired. Distinct r24 values = static 68k working set touched.

| Trace | emu dispatches | r24 transitions (dyn. 68k insns) | distinct r24 (static set) | **emu dispatches / 68k insn** |
|---|---|---|---|---|
| **regs** | 11,268,610 | 4,663,615 | 38,394 | **2.42** |
| **bug** | 8,312,899 | 3,285,483 | 38,433 | **2.53** |
| **flush** | 4,090,333 | 1,559,364 | 22,843 | **2.62** |
| ref | 71,068 | 46,249 | 15,902 | 1.54 *(low-sample, noisy)* |

- **Headline: ~2.4–2.6 emulator block-dispatches per dynamic 68k instruction** (from the three emulator-heavy traces, which agree tightly).
- This is **block dispatches**, not PPC instructions. The documented "~3–6" figure refers to *PPC instructions* per 68k instruction; one JIT-compiled ARM64 block covers several of those PPC instructions, so **2.4–2.6 blocks/insn is consistent with 3–6 PPC-insns/68k-insn, not a contradiction.**
- `ref`'s 1.54 is from only 71k emulator dispatches (the emulator is barely active in that trace) and is noisy — disregard in favor of the three larger samples.
- **Static 68k working set is small:** ~38 k distinct 68k PCs in the all-ROM traces. The boot-time 68k code is a compact body executed many times over (4.66 M dynamic insns over 38 k static = ~120× average reuse).

---

## 4. Boot-phase vs late-phase (within a single trace)

### `ref` — the only trace that visibly progresses through boot

Split into first 20% and last 20% of its 6.55 M J lines:

| Window | (a) 68k emu | (b) toolbox | (c) RAM PPC |
|---|---|---|---|
| **First 20%** | 2.94% | 32.31% | 64.75% |
| **Last 20%** | **0.85%** | 7.80% | **91.33%** |

**As boot progresses within `ref`, the 68k-emulator share falls (2.94% → 0.85%) and native PPC RAM share rises (64.75% → 91.33%).** Toolbox/nanokernel activity also drops (early init in ROM gives way to RAM code). This is *measured* boot progression.

### `regs` — an all-ROM segment, for contrast

| Window | (a) 68k emu | (b) toolbox | (c) RAM PPC |
|---|---|---|---|
| **First 20%** | 91.49% | 8.51% | 0 |
| **Last 20%** | **99.12%** | 0.88% | 0 |

`regs` stays in the emulator throughout and *never reaches RAM code* — it is the emulator-heavy boot phase that `ref` has mostly already moved past. Taken together (with the cross-run caveat from §1), the two traces are consistent with a timeline of: **early all-ROM boot is 68k-emulator-dominated → control migrates to native PPC RAM code, after which the emulator is a single-digit-percent background tax.**

### Steady-state desktop idle: NOT in the data

The tail of the longest trace (`ref`) does **not** settle into a short repeating cycle of `to_pc` values (the idle-loop signature). Its last thousands of blocks are a mix of native RAM PPC (`0x10xxxxxx`), in-ROM emulator (`0x5046xxxx`), and toolbox (`0x504a/50486`) — active late-boot work. So **desktop-idle steady-state cannot be measured here**; the steady-state characterization below is grounded in the documented architecture (§5), anchored by Apple's nanokernel design.

---

## 5. The architectural reality: what is 68k in Mac OS 8.6, and why

Mac OS 8.6 (released 1999-05-10) is a **hybrid OS** that was *progressively* ported from 68k to PowerPC over the 8.x line. It was never fully native.

**The dual-mode foundation.** Once Mac OS loads, native PowerPC and emulated 68k code execute transparently. Critical OS routines were *progressively* replaced with native PowerPC code, with the **Mixed Mode Manager** switching between the two instruction-set architectures. The Toolbox trap dispatch table historically linked to 68k code; native PowerPC Toolbox functions must first disable the emulator via the Mixed Mode Manager, which bridges the two calling conventions (PowerPC passes parameters in registers; 68k on the stack) and manages return addresses. ([Macintosh Toolbox, Wikipedia](https://en.wikipedia.org/wiki/Macintosh_Toolbox); [Apple, *Introduction to PowerPC System Software*](https://developer.apple.com/library/archive/documentation/mac/pdf/PPC_System_Software/Intro_to_PowerPC.pdf))

**The nanokernel + 68k emulator.** The Mac OS nanokernel provides a 68LC040 emulator giving transparent 68k compatibility on PowerPC. Crucially: *"the normal state of the Power Macintosh nanokernel is to be monitoring the on-board 68K emulator running 68K code, even in New World Macs, and even in later versions of the operating system where most of the Mac OS was finally PowerPC-native."* ([Mac OS nanokernel, Wikipedia](https://en.wikipedia.org/wiki/Mac_OS_nanokernel)) **This is the steady-state anchor:** even in 8.6, the default resting state of the system is running 68k code under emulation — matching the emulator-heavy dispatch shares measured in the all-ROM traces.

**What became native by 8.6.** Time-critical pieces were rewritten first; the OS got faster over the 8.x line as more went native:
- 8.0: native PowerPC multithreaded **Finder**, Platinum appearance.
- 8.5: native PowerPC **QuickDraw**, **AppleScript** (rewritten PowerPC-only, large speedup), **Sherlock**.
- 8.6: rewritten **nanokernel** (René A. Vega) adding Multiprocessing Services 2.x preemptive-task support.
([Mac OS 8, Wikipedia](https://en.wikipedia.org/wiki/Mac_OS_8))

**What remained 68k in 8.x.** Substantial portions still required the emulator:
- The **Appearance** extension was originally almost entirely 68k.
- **Apple Guide** (the standard help system, including its menu bar) was written entirely in 68k.
- A "primary and necessary part of the system software under 8.x requires the 68k emulator … for the myriad QuickDraw functions that it patches."
- **Memory Manager** and **Resource Manager** remained *partially* emulated in early 8.x, converted to native incrementally.
([Low End Mac, "680x0 vs. PPC Native Code"](https://lowendmac.com/1998/680x0-vs-ppc-native-code/); [Architecture of Classic Mac OS, E-Maculation](https://www.emaculation.com/forum/viewtopic.php?p=46817))

**Why boot is so 68k-heavy.** Early boot runs the ROM's 68k startup/patch-installation code, the trap-table setup, and a large body of still-68k toolbox patches before native RAM-resident code (Finder, extensions, native toolbox) takes over — exactly the measured `regs`(~97% emu, all-ROM) → `ref`(85%+ RAM late) progression.

---

## 6. Implication for the JIT (Amdahl, numerical)

### Framing (read first — this is load-bearing)

In SheepShaver's design, the in-ROM 68k emulator is itself **PowerPC code**. The current PPC→ARM64 JIT *already compiles it* — when the trace shows 96% of dispatches "in the 68k emulator," that is 96% of dispatches spent executing **already-JIT-compiled ARM64 that implements the PPC 68LC040 emulator**, which in turn interprets the guest 68k stream. So:

- **Baseline** = current PPC→ARM64 JIT running on *everything, including the emulator's own PPC code*. The emulator is not running as uncompiled PPC; it is compiled.
- **S (the speedup factor on the non-emulator fraction)** = further optimizing the *already-native* PPC paths (better register allocation, block chaining, peephole) — work that does not touch how 68k is emulated.
- **Breaking the 1/X ceiling** requires a *qualitatively different* move: a **68k→ARM64 fast path that bypasses the PPC-interpreting-68k layer entirely** (recognize guest 68k semantics and emit ARM64 directly, skipping the emulator's per-opcode PPC fetch/decode/dispatch). That is "compiling the 68k emulator [away]."

### Amdahl bound

With X = fraction of dispatches inside the 68k emulator (the fixed floor unless you attack the emulator itself), and the remaining (1−X) sped up by factor S:

> **total speedup ≤ 1 / ( X + (1−X)/S )**, ceiling **1/X** as S→∞.

| Regime (measured X) | S=2× | S=4× | S=8× | Ceiling (S→∞) |
|---|---|---|---|---|
| **Emulator-heavy boot** (X=0.97, `regs`/`crash`) | 1.02× | 1.02× | 1.03× | **1.03×** |
| **Emulator-heavy** (X=0.945, `flush`) | 1.03× | 1.04× | 1.05× | **1.06×** |
| **Mixed/transitional** (X≈0.68, `jit2`) | 1.19× | 1.31× | 1.38× | **1.46×** |
| **Native-RAM phase** (X=0.0108, `ref` overall) | 1.98× | 3.87× | 7.44× | **92.6×** |
| **Native-RAM late** (X=0.0085, `ref` last-20%) | 1.98× | 3.90× | 7.55× | **117.6×** |

### What this means concretely

1. **In the 68k-emulator-dominated phases (early/all-ROM boot, and per Apple, idle steady-state), you cannot do better than ~1.03–1.06× by optimizing native PPC code alone.** Every gain there is capped at 1/X ≈ 1.03–1.06×, *no matter how fast you make the non-emulator code* — because 94–97% of the work is the emulator. This phase is **emulator-bound**.

2. **The high-leverage target is the 68k emulator path.** Of every 68k instruction, ~2.4–2.6 ARM64 *blocks* (≈3–6 PPC ops) are spent in the emulator's fetch/decode/dispatch. A 68k→ARM64 fast path that collapses those to a handful of ARM64 instructions per 68k op would attack the 96% directly. With only ~38 k static 68k PCs and ~120× reuse, a 68k-block cache would be highly effective.

3. **For phases already in native PPC RAM code (Finder, native toolbox, applications), optimizing the PPC JIT is worth it:** X≈1%, so an 8× faster native path yields ~7.4× overall and the ceiling is ~90–120×. This is where conventional JIT improvements (S) pay off — but it is *not* where boot or idle time is spent.

4. **Net engineering read:** to speed up *boot* and (per the nanokernel design) *idle*, you must compile the 68k emulator's inner loop — i.e. emit ARM64 for guest 68k semantics directly, not just JIT the PPC that implements the emulator. Optimizing the native-PPC JIT alone helps only the application/RAM-code phase, which the trace data shows is a *minority* of boot dispatch volume and (architecturally) not the resting state of the OS.

---

## Sources

- [Mac OS nanokernel — Wikipedia](https://en.wikipedia.org/wiki/Mac_OS_nanokernel) (steady-state: nanokernel's normal state is monitoring the on-board 68K emulator running 68K code)
- [Macintosh Toolbox — Wikipedia](https://en.wikipedia.org/wiki/Macintosh_Toolbox) (dual-mode dispatch, Mixed Mode Manager)
- [Mac OS 8 — Wikipedia](https://en.wikipedia.org/wiki/Mac_OS_8) (8.0/8.5/8.6 native-ization milestones)
- [Apple, *Introduction to PowerPC System Software*](https://developer.apple.com/library/archive/documentation/mac/pdf/PPC_System_Software/Intro_to_PowerPC.pdf) (Mixed Mode Manager, calling conventions)
- [Low End Mac — "680x0 vs. PPC Native Code"](https://lowendmac.com/1998/680x0-vs-ppc-native-code/) (Appearance, Apple Guide, QuickDraw patches remained 68k)
- [Architecture of Classic Mac OS — E-Maculation](https://www.emaculation.com/forum/viewtopic.php?p=46817) (Memory/Resource Manager partial emulation)
- Trace data: `/tmp/trace_{ref,regs,bug,crash,flush,v2,interp,interp2,jit,jit2}.txt` (SheepShaver JIT-mode boot captures). Analysis scripts: `/tmp/classify.awk`, `/tmp/classify_I.awk`, `/tmp/r24.awk`.
