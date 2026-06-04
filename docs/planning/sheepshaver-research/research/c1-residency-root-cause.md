# C1 — JIT Residency Root-Cause Analysis

Read-only analysis of *why* SheepShaver's PPC→ARM64 JIT abandons the JIT loop after ~15 s of
boot and stays in the interpreter inner loop (re-entering the JIT gate only ~1/sec). This is the
mandatory "instrument *why* before building AOT machinery" first step of backlog item **C1**
(`IMPLEMENTATION-BACKLOG.md` §C1).

**Sources read:** `IMPLEMENTATION-BACKLOG.md` §C1, `EMULATOR-RESEARCH-LEADS.md` (§Rosetta/residency,
item 9), `research/lead-7-dispatch-linking.md` (§2.5 T2 data, §4), `research/landscape-2-…md`
(§7 Rosetta 2), `LEARNINGS.md` (L460–619), and `ppc-cpu.cpp:560–887` (the execute loop).

**Bottom line up front:** the root cause is a **dual-cache trap**, structurally provable from the
code (not merely sampled). There are two independent block caches — `my_block_cache`
(interpreter) and `jit_bc_heads[]/jit_bc_pool[]` (JIT). The interpreter inner loop
(`skip_jit:`, ppc-cpu.cpp:822–850) knows **only its own** cache. Once the hot working set is
interpreter-cached, the inner loop's exit test at line 848 keeps hitting → the loop never exits →
the JIT gate at `pdi_execute:` (line 712) is never consulted, *even for PCs that have a complete
JIT block.* AOT-compiling the ROM (C1's own headline) does **not** fix this trap — it changes JIT
*stickiness once entered*, not the *re-entry* into the JIT loop. The matching fix is a
**gate restructure** (Dolphin-shaped: prefer a compiled block at every transition).

---

## 1. Execute loop control flow (file:line + pseudocode)

All line numbers: `SheepShaver/src/kpx_cpu/src/cpu/ppc/ppc-cpu.cpp`, current working tree
(`macos-arm64`, post-`8f2acc9b`). The active path is `PPC_DECODE_CACHE` (the JIT is bolted onto
the decode-cache interpreter, NOT the legacy `PPC_ENABLE_JIT` dyngen path at 596–628, which is
dead on this build).

```
powerpc_cpu::execute(entry):                       # line 583
    pc() = entry
    execute_depth++
    if (execute_depth == 1):                        # line 595
        bi = my_block_cache.find(pc())              # line 631  INTERPRETER cache
        if (bi != NULL) goto pdi_execute            # line 633  (skips predecode)
        for (;;):                                    # line 634  OUTER block loop
            # ---- predecode a fresh interpreter block into decode_cache ----
            bi = new_blockinfo(); bi->init(pc())     # 640-641
            do { decode opcode; append to di; }      # 649-684
              while (!(cflow & CFLOW_END_BLOCK))
            my_block_cache.add_to_*_list(bi)         # 689-690

          pdi_execute:                               # line 697  <== JIT GATE LIVES HERE
            [SS_JIT_TRACE log of "I <pc>"]           # 702-710
            if JIT enabled (SS_USE_JIT != "0"):      # 728-732  GATE 1
                one-time: init cache + set ROM range # 733-762
                # GATE 2: only run a *complete* native block
                if ppc_jit_aarch64_compile(pc(),..,&jblk) && jblk.complete:  # 767
                    fn = jblk.code;  fn(regs)         # 769-770  RUN NATIVE BLOCK
                  pdi_jit_post:                       # 771
                    [SS_JIT_TRACE log of "J ..."]     # 775-781
                    [GATE 3: out-of-range PC diag]    # 792-802
                    if !spcflags().empty():           # 803
                        if !check_spcflags() goto return_site
                    # FAST JIT->JIT re-dispatch (the only in-JIT-loop path):
                    if ppc_jit_aarch64_compile(pc(),..,&jblk) && jblk.complete:  # 810
                        fn = jblk.code; fn(regs); goto pdi_jit_post   # 811-813
                    bi = my_block_cache.find(pc())    # 815
                    if (bi) goto pdi_execute          # 816  (next interp block -> re-try JIT gate)
                    continue                          # 817  (no interp block -> predecode new one)
                # GATE 2 false (no complete JIT block for this PC): fall through

          skip_jit:                                  # line 821  INTERPRETER INNER LOOP
            for (;;):                                  # 822
                run all di[] handlers for this block   # 826-834  (Duff's device)
                if !spcflags().empty():                # 836
                    if !check_spcflags() goto return_site          # 838  EXIT execute()
                    if SPCFLAG_JIT_EXEC_RETURN:        # 841  (cache invalidated)
                        clear it; invalidated_cache=true; break     # 842-844  EXIT to outer loop
                if (bi->pc != pc()) and                # 848  <== THE TRAP CONDITION
                   (bi = my_block_cache.find(pc())) == NULL:
                    break                              # 849  EXIT to outer loop (-> predecode)
                # else: bi now points at the next interp-cached block; LOOP (stay interpreting)
  return_site:                                         # 882
    if invalidated_cache: spcflags().set(SPCFLAG_JIT_EXEC_RETURN)
    execute_depth--
```

### When is the JIT gate checked?

**Only at `pdi_execute:` (line 712)**, which is reached from exactly three places:
1. Line 633 — on entry to `execute()`, when the start PC is already interpreter-cached.
2. Line 816 — from the JIT fast-dispatch fallback, when the next PC *is* interpreter-cached.
3. The top of the outer `for(;;)` (line 634→697) — after predecoding a brand-new interpreter block.

The JIT gate is **NOT** checked anywhere inside the `skip_jit:` inner loop (822–850). That is the
crux.

### When does the interpreter inner loop exit back toward the JIT gate?

The `skip_jit:` loop (822–850) has exactly **three** exits:
- **(E1) `goto return_site` (line 838):** `check_spcflags()` returned false — a pending interrupt /
  special-purpose flag forces `execute()` to *return entirely*. The caller
  (`sheepshaver_cpu::execute`/the outer dispatch) later re-enters `execute()`, which at line
  633 routes through `pdi_execute` → JIT gate. **This is the ~1/sec re-entry** — it is
  interrupt-driven (timer/IRQ via `HandleInterrupt`), not a cache miss.
- **(E2) `break` on `SPCFLAG_JIT_EXEC_RETURN` (line 844):** the decode cache was invalidated;
  break to the outer loop, which predecodes and falls into the JIT gate. Rare at steady state
  (LEARNINGS L525: "no more flushes" after startup).
- **(E3) `break` at line 848:** the only *hot* exit. Taken **only when** `bi->pc != pc()` **AND**
  `my_block_cache.find(pc()) == NULL` — i.e. the next PC is **not** in the interpreter cache.
  When the next PC *is* interpreter-cached, `find()` succeeds, `bi` is reassigned, and the loop
  **continues interpreting** — never touching the JIT.

**Therefore:** once the boot working set is fully resident in `my_block_cache`, every block
transition satisfies line 848's `find() != NULL`, the loop never breaks, and the JIT gate is
unreachable until an interrupt fires (E1). This is the residency collapse, derived from the
control flow alone.

---

## 2. Hypothesis evaluation

| # | Hypothesis | Verdict | Code evidence |
|---|---|---|---|
| **(a)** | Interpreter inner loop stays interpreting once entered, even when complete JIT blocks exist for the PCs it runs | **CONFIRMED (structural)** | Line 848: inner loop only exits on interp-cache *miss* (E3) or interrupt (E1). It performs **no JIT lookup**. So a PC with a complete `jit_bc` block but a present `my_block_cache` entry is run by the interpreter, forever. This is the trap. |
| **(b)** | The blocks the interpreter runs were never JIT-compiled (compile failures) | **TRIGGER, not the trap** | Compile failures cause the *first* fall into `skip_jit` (GATE 2 `jblk.complete` false at line 767). LEARNINGS L537: 25% complete rate (16,916/66,823) — so ~75% of *attempts* are incomplete/punted. That seeds the interpreter cache. But (b) only explains entry; (a) explains why execution never climbs back even for PCs that *do* have complete blocks. |
| **(c)** | The gate requires conditions (spcflags clear) rarely true at steady state | **REFUTED** | The gate (728–767) never tests spcflags before entering. `check_spcflags()` is consulted only *after* a JIT block runs (line 803) and inside the interp loop (836). Entry into the JIT path is unconditional on spcflags. |
| **(d)** | Code-cache flushes (4 MB fills → flush all) destroy the working set repeatedly | **REFUTED at steady state** | LEARNINGS L525: "Code cache fills twice at startup… then stable (no more flushes)." Cache is 64 MB (65536-block init, ppc-cpu.cpp:737). After the W^X fix (8f2acc9b) the working set is stable; recurring flushes are not occurring. (Chain-site pool exhaustion, lead-7 §5, is a separate silent-degradation risk but not the residency cause.) |

**Synthesis:** (b) is the *trigger* (incomplete/punted blocks drop execution into the interpreter);
(a) is the *trap* (the interpreter loop has no path back to the JIT for cached PCs). Residency
collapse = trigger + trap. Fixing only (b) (e.g. AOT, raising the complete rate) reduces how often
the trap is *sprung* but does not remove the trap: any single interp-cache hit at a transition
keeps the loop interpreting.

### Why the T2 numbers must be re-measured (staleness caveat)

The T2 data (LEARNINGS L532–542, commit `7030a441`) **predates** the W^X fix (`8f2acc9b`) that
brought boot to idle in ~2:40. `8f2acc9b` did **not** change the gate structure (it added an
early range-check inside `ppc_jit_aarch64_compile`), so the dual-cache trap should persist — but
the specific counts (66,836 attempted, ~1/sec) were taken on a 37×-slower build. **Do not present
them as current.** Re-measuring on the current build is instrumentation step 0 below.

---

## 3. Recommended instrumentation (precise, ready to implement)

Designed but **not implemented** — the JIT agent owns these files. All env-gated, zero cost when
off. The discriminating probe is **R1**.

### R0 — Re-confirm residency on the current build (sampling, no code change)
Run `make run-jit`, wait ~30 s past the first cache-fill messages, then attach lldb and sample:
```
sample <pid> 5
```
Confirm: samples concentrate in `powerpc_cpu::execute` at the `skip_jit:` inner loop
(ppc-cpu.cpp ~828–833), ~zero in the code-cache address range. Also read the counters
`jit_blocks_attempted` / `jit_blocks_complete` (ppc-jit.cpp) twice 15 s apart; confirm the
attempted counter is near-flat. This reproduces the T2 finding on post-`8f2acc9b` code before any
new code is written.

### R1 — DISCRIMINATING PROBE: "JIT-complete-but-unused at interp transitions"
This is the one counter that separates hypothesis (a) from everything else. Place it on the
**continue path** of the interpreter inner loop — i.e. when line 848's `find()` *succeeds* and the
loop is about to keep interpreting.

- **Where:** `ppc-cpu.cpp`, inside `skip_jit:`'s `for(;;)`, at the bottom, in the branch where
  `bi->pc != pc()` and `my_block_cache.find(pc())` returned non-NULL (the implicit "continue
  interpreting" case after line 848). Add an `else` to the `if` at 848 (or instrument right after
  the `bi` reassignment when `find()` is non-NULL).
- **What to call:** the **read-only** lookup `jit_bc_lookup(pc())` (ppc-jit.cpp:199) — **NOT**
  `ppc_jit_aarch64_compile()` (line 767). `compile` *mutates* cache state and would itself alter
  behaviour in the hot loop; `jit_bc_lookup` is a pure hash-walk that returns the pool entry (or
  sentinel) without compiling. The probe must expose a thin C wrapper, e.g.
  `bool ppc_jit_aarch64_has_complete_block(uint32 pc)` that does `jit_bc_lookup` and returns
  `entry && entry->code != NULL && entry->complete`.
- **What to count (two `static uint64_t` under `SS_JIT_RESIDENCY_PROBE`):**
  - `interp_transitions_total++` every time the inner loop continues (line 848 false → keep
    interpreting).
  - `interp_transitions_jit_available++` when, at that same point,
    `ppc_jit_aarch64_has_complete_block(pc())` is true.
- **What to print:** every N transitions (e.g. 1<<20) to stderr:
  `RESIDENCY: %llu/%llu interp transitions had a COMPLETE JIT block available but unused (%.1f%%)`.
- **Interpretation:** a high ratio (say >30%) **proves (a)** — execution is demonstrably running
  the interpreter on PCs for which a complete native block exists. A near-zero ratio would instead
  point at (b) (the interp loop only runs PCs that were never compilable), changing the fix shape.

### R2 — Companion: blocks-executed-per-gate-entry (cheap, no JIT lookup)
- **Where:** ppc-cpu.cpp. Increment `interp_blocks_run++` each inner-loop iteration (828) and
  `gate_entries++` each time `pdi_execute` is reached (line 697). Print the ratio periodically.
- **Why:** a large ratio (interp blocks ≫ gate entries) independently confirms "we enter the JIT
  gate rarely and then interpret a long run of blocks" — the residency collapse — without needing
  a JIT lookup. R1 says *whether those blocks were compilable*; R2 says *how long the interpreter
  runs between gate visits*. Together: high R2 ratio + high R1 fraction = (a) confirmed and
  quantified.

### R3 — Trigger breakdown (optional, attributes (b))
- **Where:** the GATE 2 `false` fall-through (after line 767/818, just before `skip_jit:`).
- **Count:** `gate2_miss_total++`, and split by reason via the existing `jblk` fields
  (`!jblk.complete` vs lookup-returned-false / out-of-RAM). Surface alongside the existing
  `jit_report_misses` output. Tells you *what* drops execution into the interpreter (which opcodes
  punt), informing whether raising the complete rate (A1–A3, MAME crib C2) is worthwhile
  independently of the gate fix.

### R4 — lldb-only spot check (no build)
Breakpoint on the inner-loop continue and inspect:
```
b ppc-cpu.cpp:848
# when bi->pc != pc() and find() succeeded, evaluate in lldb:
p (uint32)pc()
# then call the read-only lookup if exported, or read jit_bc_pool head for (pc>>2)&MASK
```
Confirms R1's finding manually without instrumentation, useful as a sanity cross-check.

---

## 4. Fix-shape recommendation (gated on what instrumentation reveals)

### The headline correction: AOT-the-ROM (C1's title) is the wrong shape for the *trap*
C1 is titled "AOT-compile the Mac ROM," and `EMULATOR-RESEARCH-LEADS.md` §2 / landscape-2 §7
reframe the Rosetta 2 lesson as "AOT-compile the ROM **as the fix**." **The code analysis
contradicts that framing for the residency *trap*:** even if every reachable ROM block is compiled
ahead of time, the moment a ROM PC is present in `my_block_cache`, line 848's `find()` succeeds and
the interpreter inner loop runs the *interpreter* block, ignoring the AOT translation. AOT raises
the JIT *complete rate* (attacks trigger (b)) and improves stickiness *once the JIT loop is
entered*, but it does **not** add a path from the interpreter inner loop back into the JIT. It is
necessary-but-not-sufficient, and on its own would burn significant engineering for little
residency gain.

### The matching fix: Dolphin-shaped gate restructure (attacks trap (a))
Dolphin never falls into a C++ interpreter between blocks: its in-asm dispatcher does a direct
indexed lookup and `BR`s straight into the next native block, consulting C++ only on a true miss
(`lead-7` §1.3). The structural lesson is "**prefer a compiled block at every transition.**"
Ported minimally to our hybrid loop — and crucially *cheaply*, since the inner loop already
re-checks `pc()` at line 848:

> **Add a read-only JIT-complete check at the inner-loop transition.** At line 848, when
> `bi->pc != pc()` (a new PC), *before* (or instead of falling through to) `my_block_cache.find()`,
> call `ppc_jit_aarch64_has_complete_block(pc())`. If true, `break` out of `skip_jit:` back to
> `pdi_execute` (the JIT gate) so the native block runs. Only if no complete JIT block exists do we
> consult the interpreter cache as today.

This converts the interpreter inner loop from "stay until interp-cache miss" into "stay until a
JIT block is available **or** interp-cache miss," which is the residency-sustaining behaviour. It
reuses the existing read-only lookup, adds no W^X traffic, and is small and local. It is the
direct structural analogue of Dolphin's "dispatcher prefers the compiled entry."

### Decision tree (gated on R1)
- **R1 ratio high (>~30%) — (a) confirmed [expected]:** implement the **gate-restructure** above as
  the primary fix. This alone should restore sustained residency without any AOT machinery.
  Re-run R0/R2 to confirm samples move into the code cache and the gate-entry ratio collapses
  toward 1 block/entry. *Then* consider the inline-asm entry-points dispatcher (lead-7 §4) to make
  the now-hot JIT→JIT path cheap, and AOT (C1) to push trigger (b)'s complete rate up.
- **R1 ratio near zero — (b) dominates instead:** the interpreter is only ever running PCs that
  *can't* be compiled (e.g. the deliberately-excluded ROM 68k emulator above `ROMBase+0x460000`,
  ppc-cpu.cpp:744–759). Then the gate restructure buys little; the lever is raising the complete
  rate — finish A1–A3 (carry/`mullwo` correctness), study MAME's `ppcdrc.cpp` (C2) for the top
  punted opcodes (drive the list from R3 / `jit_report_misses`), and reconsider compiling the 68k
  emulator range behind a designed interrupt strategy. AOT becomes relevant here, but as a
  complete-rate play, not a residency-trap fix.
- **Either way:** AOT-the-ROM (C1 full) is sequenced *after* the gate restructure, and re-scoped
  from "the residency fix" to "the trigger/complete-rate and warm-start improvement." Do not build
  AOT machinery before R1 has run.

---

## Appendix — key file:line index

| Thing | Location |
|---|---|
| `execute()` entry, decode-cache path | ppc-cpu.cpp:583, 595, 631–633 |
| Outer block loop (predecode) | ppc-cpu.cpp:634–691 |
| `pdi_execute:` + JIT gate (GATE 1/2/3) | ppc-cpu.cpp:697, 712, 728–767, 792–802 |
| Run native block / fast JIT→JIT re-dispatch | ppc-cpu.cpp:769–770, 810–817 |
| `skip_jit:` interpreter inner loop | ppc-cpu.cpp:822–850 |
| **The trap condition (interp-cache transition)** | **ppc-cpu.cpp:848** |
| Inner-loop exits: return_site / invalidate / miss | ppc-cpu.cpp:838, 842–844, 849 |
| `return_site` / re-entry | ppc-cpu.cpp:882; re-entry at 633 |
| Read-only block lookup `jit_bc_lookup` | ppc-jit.cpp:199 |
| `ppc_jit_aarch64_compile` (mutates) | ppc-jit.cpp:3838 (gate call 767/810) |
| Block cache sizes (32768/65536) | ppc-jit.cpp:52–54 |
| W^X residency fix (early range-check) | LEARNINGS L572–593, commit 8f2acc9b |
| T2 residency data (PRE-fix, stale) | LEARNINGS L532–542, commit 7030a441 |
| Rosetta 2 AOT lesson | landscape-2 §7; EMULATOR-RESEARCH-LEADS §2 |
| Dolphin in-asm dispatcher | lead-7-dispatch-linking.md §1.3, §4 |
