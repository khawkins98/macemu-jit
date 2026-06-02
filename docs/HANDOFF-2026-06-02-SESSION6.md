# SheepShaver ARM64 JIT — Session 6 Handoff: Parallel Boot-Time Investigation

## Context — read these first

1. `LEARNINGS.md` "session 5 part 2 — RETRACTION" — the previous deadlock theory was wrong;
   read the retraction so you don't rebuild it.
2. `docs/HANDOFF-2026-06-02-SESSION5.md` — RETRACTED, kept for history. Do not implement
   anything from it.
3. This document — the current state and the parallel investigation plan.

## Where we are

**The question**: Why is SheepShaver's aarch64 JIT boot slow, and what is the highest-leverage fix?

**What changed this session**: We discovered the question itself was mis-framed. The premise
"interpreter boots in 10s, JIT takes 22 minutes" is wrong — live measurement shows the
interpreter ALSO takes 3+ minutes (still booting at 190s) with the current configuration
(Mac OS 8.6 ISO, CD boot). Neither mode's true boot time has been measured to completion.

**Diagnostic infrastructure now available** (all committed, branch macos-arm64):

| Tool | What it gives you |
|------|-------------------|
| Region counters in heartbeat (commit d0307ad6) | Per-region block counts (NK/DR/RAM/other) + j2i transitions, logged every 5s to /tmp/jit_diag.log, in BOTH modes |
| `SheepShaver/tools/jit-analyze.py` | diag/ring/hot analysis of the log (needs updating for new heartbeat format) |
| `SS_JIT_TRACE_RING=1` + auto ring dump | Block-level execution history |
| `SS_JIT_WATCH_ADDR=<hex>` | Guest-memory watchpoints |
| VNC server (prefs: vncserver true, vncport 5999) | Headless screenshots of emulator screen |
| HOT-PC detector | Flags recurring heartbeat PCs (sampling hint, NOT proof of hang) |

**Key data so far** (region counters, partial runs):

| Metric | Interpreter (105s in) | JIT (65s in) |
|--------|----------------------|--------------|
| Total blocks | 615M (~5.9M/s) | 6.7B (~103M/s) |
| Nanokernel share | 2% | 32% |
| DR emulator share | 39% | 35% (interpreted) |
| RAM share | 59% | 33% |
| j2i transitions/sec | n/a | 2.4M/s |

**The two real anomalies to explain:**

1. **NK:DR ratio**: 1:21 in interpreter mode vs 1:1 in JIT mode. The JIT-mode guest enters
   the nanokernel exception dispatcher ~20x more per unit of DR work. Why?
2. **Neither mode boots fast.** Is the JIT actually slower than the interpreter at all?
   By how much? Where does the wall-clock time go in each?

## Parallel investigation plan (one agent per worktree)

Use `git worktree add <path> HEAD` to create isolated checkouts. Each agent builds its own
binary (`cd <worktree>/SheepShaver/src/Unix && make -j8`) and uses its own log paths
(pass distinct output paths; /tmp/jit_diag.log is hardcoded — either run agents sequentially
for log access or patch the log path per worktree).

**IMPORTANT: only one SheepShaver instance can run at a time** (they share ~/.sheepshaver_prefs,
the disk images, and the SDL window). Worktrees parallelize the BUILD and ANALYSIS, but
emulator runs must be serialized. Coordinate via the team lead.

### Agent A — Ground truth: boot-time measurement (highest priority)

**Goal**: Measure real wall-clock boot-to-desktop time for both modes with the current config.

Method:
1. Patch the diag log path to /tmp/jit_diag_A.log in your worktree
2. Run interpreter mode (`SS_USE_JIT=0`) to desktop. Detect desktop via VNC screenshot
   (vncserver is enabled in prefs, port 5999) or by watching for the block-rate signature
   of an idle desktop (sustained low block rate + 60Hz interrupt deliveries only).
3. Run JIT mode (default) to desktop. Same detection.
4. Record: wall-clock time, total blocks, final region split for each mode.
5. Deliverable: a table in LEARNINGS.md with the honest numbers. This recalibrates everything.

### Agent B — The NK:DR ratio anomaly

**Goal**: Explain why JIT mode enters the nanokernel ~20x more per DR block.

Method:
1. Instrument nanokernel ENTRY (not per-block): count entries to the dispatcher
   (0x50312a00-0x50313e00 range entered from a non-NK PC) and classify by what preceded
   them (DR PC / RAM PC / interrupt delivery).
2. Also count EMUL_OP executions per second in both modes (SS_EMULOP_COUNTS=1 exists
   already — see HANDOFF.md from the dyngen session).
3. Compare rates between modes. The hypothesis to test: fast JIT-compiled PPC code (RAM,
   CFM/Mixed Mode) makes nanokernel round-trips at JIT speed, while in interpreter mode
   those round trips happen at interpreter speed — i.e., the ratio difference is just
   "PPC code runs faster", not a bug. Alternative hypothesis: a retry/polling loop.
4. Deliverable: the cause of the ratio difference, with rates, in LEARNINGS.md.

### Agent C — Transition overhead measurement

**Goal**: Quantify the cost of the 2.4M/s JIT↔interpreter transitions.

Method:
1. Add cycle counters (mach_absolute_time or cntvct_el0 reads) around the transition paths:
   - JIT dispatch fallthrough → my_block_cache.find → pdi_execute entry
   - Interpreter loop break → outer loop → JIT lookup → fn() entry
2. Accumulate total transition time; log in heartbeat.
3. Deliverable: "transitions cost X% of wall-clock time" in LEARNINGS.md. If >30%, the fix
   is JIT-compiling the DR region (the dyngen-parity work, blocked on Bug #2) or batching.
   If <10%, transitions are a red herring — close that line of investigation.

### Agent D — Documentation keeper (continuous)

**Goal**: Keep LEARNINGS.md, this handoff, and code comments in sync with findings.

Method:
1. Watch for findings from agents A-C (the team lead relays them).
2. Update LEARNINGS.md incrementally — every finding gets an entry with evidence.
3. Enforce the retraction discipline: if a finding contradicts an earlier entry, mark the
   earlier entry SUPERSEDED with a pointer, never delete.
4. Update jit-analyze.py to parse the new heartbeat format (region counters).
5. Deliverable: documentation that lets the NEXT session start without re-deriving anything.

## Known traps (do not fall in)

1. **"STUCK"/HOT-PC detections are sampling artifacts**, not hangs. See retraction.
2. **Block counts are not comparable across modes** — DR dispatch blocks are 2-4 instructions,
   toolbox blocks are larger. Use wall-clock + instruction counts, or compare like with like.
3. **lldb attach disrupts the VBL timer** (CLAUDE.md) — attach at most once per run, detach
   immediately. Prefer the built-in diagnostics.
4. **Only one emulator instance at a time** — shared prefs/disks/window.
5. **The `jit` pref is inert** — the aarch64 JIT is controlled by `SS_USE_JIT` env var only.
6. **prefs now have vncserver true** — if a run seems to hang at startup, check port 5999
   isn't already bound by a zombie instance (`pkill -9 -x SheepShaver`).

## Current configuration

- Branch: macos-arm64, HEAD = d0307ad6 (or later)
- ROM range: 0x460000 (toolbox only), chaining: 0
- Prefs: ~/.sheepshaver_prefs (documented inline, boots Mac OS 8.6 ISO via CD)
- Assets: /Users/Shared/macemu/
- Harness: must stay 233/233 (`cd SheepShaver && make test-opcodes`)
