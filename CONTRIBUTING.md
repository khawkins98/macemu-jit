# Contributing to macemu-jit (macOS arm64 fork)

Guide for humans and AI agents working on the SheepShaver/BasiliskII AArch64 JIT.

## Start here (orientation)

This is the macOS Apple Silicon (arm64) port of [rcarmo/macemu-jit](https://github.com/rcarmo/macemu-jit),
adding an AArch64 JIT that translates PowerPC/68K → native ARM64. Two emulators live here, but only one
works on macOS arm64 today:

- **SheepShaver** (PowerPC) — **builds and boots Mac OS 8.6/9 to the Finder desktop** with the full native
  JIT. This is where the work is.
- **BasiliskII** (68K) — **does not currently build on macOS arm64** (JIT backend unported). See
  `docs/planning/BasiliskII-MACOS-AARCH64-JIT-PORT.md`.

Current pass/fail and boot status lives in `JIT-STATUS.md`; what's next lives in `docs/planning/ROADMAP.md`.
To build and run SheepShaver, follow the **Build** section in `README.md` (autogen → configure → `make build`).
The rest of this guide is *how to work here* — gates, conventions, and where the deep docs are.

## Development Process (default for substantive work)

Larger streams of work follow **`docs/MILESTONE-WORKFLOW.md`** — the multi-agent
milestone machine (plan → pre-implementation red-team → binding Task-0 recon with
blocking-answer tables → env-gated implementation with falsifiable gates →
flip-last/revert-on-red acceptance → docs close-out) and its parallel workstream
layer. Established 2026-06-11; every rule in it cites the incident that earned it.
Skip it only for trivial mechanical changes. The canonical plan exemplar is
`docs/superpowers/plans/2026-06-11-nk-syscall-surface.md`.

## Before You Start

1. Read `CLAUDE.md` — build commands, test commands, asset locations, architecture overview
2. Read `LEARNINGS.md` — non-obvious findings that will save you from repeating mistakes
3. Check `docs/planning/OPTIMIZATION-PLAN.md` — what's done, what's open, what was tried and deferred
4. Check `docs/planning/sheepshaver-research/research/IMPLEMENTATION-BACKLOG.md` — ready-to-implement items

## Working in a git worktree (parallel agents / isolated branches)

When two efforts run at once (e.g. an E2E branch and a JIT branch), do the riskier/code-heavy one
in a **separate `git worktree`** so the builds and the emulator don't collide:

```bash
git worktree add ../macemu-jit-<topic> -b <branch>     # new isolated working copy + branch
```

A fresh worktree contains only **tracked** files — the autoconf build products (`configure`,
`src/Unix/Makefile`, `obj/`, the binary) are generated and gitignored, so they are **not** copied.
Run the one-time configure in the new worktree before the first build:

```bash
cd <worktree>/SheepShaver/src/Unix
NO_CONFIGURE=1 ./autogen.sh
./configure --enable-sdl-video --enable-sdl-audio --enable-jit --without-gtk --without-x \
            --without-esd --with-vdeplug CPPFLAGS=-I/opt/homebrew/include LDFLAGS=-L/opt/homebrew/lib
# Optional ccache speedup (~12x warm rebuilds): re-run configure with CC="ccache gcc" CXX="ccache g++"
# before building. Per-worktree config state — each new worktree needs its own configure run.
cd ../../ && make build-ss          # then build as usual
```

Each worktree builds its **own** binary/objects (good — no collision). Shared assets in
`/Users/Shared/macemu/` (ROMs, ISOs) are read-only and safe to use from any worktree. **Only one
emulator instance can run at a time** (shared SDL window / prefs), so if another agent is launching
the emulator, restrict yourself to the **harnesses** — `make test-jit` and the standalone
`rom-harness` exercise the JIT *without* opening the SDL window or booting, so they never collide.
To pull a parallel branch's commits in and avoid late surprises: `git merge <other-branch>`.

## Fast iteration loop

Three tools cut the build→test→investigate cycle from minutes to seconds. Reach for them
before resorting to a full rebuild or a boot. Each has a canonical deep reference — this
section is the discovery index, not a second copy of the details.

| Tool | What it buys you | Canonical reference |
|------|------------------|---------------------|
| **ccache** | Warm SheepShaver rebuilds ~12× faster (clean-tree ~5.88s cold → ~0.48s warm). Opt in by re-running `configure` with `CC="ccache gcc" CXX="ccache g++"`. Per-checkout config state — each worktree/clone needs its own configure run. | The worktree `configure` recipe above; `CLAUDE.md` SheepShaver build section. |
| **Batch harness** (`SS_HARNESS_BATCH=1 make test-jit`) | All vectors in one process per mode instead of one process per vector — **~3s vs ~32s**. Use for the inner loop; run plain `make test-jit` as the authoritative gate at least once per task/commit (process-per-vector isolation is the stronger contract). | `SheepShaver/jit-test/README.md` "Batch mode" — mechanics, per-vector reset, the content-vs-score equivalence proof. |
| **SS_SEED_MEM** | No-recompile guest-memory poke — write a 32-bit word at a fixed address (immediate, at NW-trampoline-end) or at the first JIT visit of a PC (`0xPC:0xADDR=0xVAL`). Probe a fix hypothesis without rebuilding. | `SheepShaver/docs/DIAGNOSTICS.md` env-var table (forms, limits, `[SEED]` output); `CLAUDE.md` "Guest Memory Seeding". |

**The equivalence lesson** (why batch mode's proof compares REGDUMP *content*, not scores):
a batch run that omitted the FP/VR reset between vectors still passed `score=100` because
both modes shared the same stale-state bug — score-only equivalence is vacuous when both
sides share a defect. See `LEARNINGS.md` (2026-06-10) and the jit-test README.

## Gate matrix (which test to run when)

These are the gates a change must pass. Run them from the directory shown. The per-change
checklists below say *which* gates a given change touches; this table is the menu.

| Gate | Command (from) | Gates / proves | When | Reference |
|------|----------------|----------------|------|-----------|
| **JIT codegen** | `make test-jit` (`SheepShaver/`) | Interp-vs-JIT REGDUMP diff, score=100 — the real codegen gate, catches encoding bugs | After **any** `ppc-jit.cpp` change | `SheepShaver/jit-test/README.md`; count via `make harness-count` |
| └ inner loop | `SS_HARNESS_BATCH=1 make test-jit` | Same contract, ~3s vs ~32s (one process per mode) | Development loop; run plain `make test-jit` once per task/commit as authoritative | See **Fast iteration loop** above |
| **Interp determinism** | `make test-opcodes` (`SheepShaver/`) | Interpreter is deterministic only — does **NOT** exercise the JIT | Rarely on its own; `test-jit` is the codegen gate | `SheepShaver/jit-test/README.md` |
| **Machine-layer unit tests** | `make -C SheepShaver/src/machine test` | Standalone machine layer (profile, MMIO bus, devices, clock, sched) — no emulator build needed | After any `src/machine/` change | `docs/planning/MACHINE-LAYER-PLAN.md` |
| **E2E offline tests** | `make e2e-test` (`SheepShaver/`) | ~79 harness unit tests, no emulator / GUI / assets — safe anywhere, ~2s | After E2E toolkit (`SheepShaver/e2e/`) changes | `SheepShaver/e2e/README.md` |
| **E2E smoke (live)** | `make e2e` (`SheepShaver/`) | System-level: boot ISO → shutdown → clean exit. Needs a GUI session + assets (isolated config) | System-level changes (boot, shutdown, prefs) | `SheepShaver/e2e/README.md` |
| **Microbench** | `make bench` (`SheepShaver/rom-harness/`) | ns/insn for codegen kernels — boot-free A/B (`--save-baseline` / `--compare`) | Codegen perf changes | See **Benchmarking** below; `SheepShaver/rom-harness/README.md` |
| **ROM harness** | `make test-rom` (`SheepShaver/`) | Standalone headless JIT exerciser against a real OldWorld ROM | Broad JIT coverage check | `SheepShaver/rom-harness/README.md` |

From the repo root, `make test` / `make test-jit` delegate to the SheepShaver harness.
The live gates (`make e2e`, boots) open the shared SDL window — **only one emulator instance can
run at a time** (see **Key Invariants** below); the harnesses (`test-jit`, `rom-harness`) do **not**
boot, so they never collide with a running instance or a parallel worktree.

## Commit Style

```
type(scope): short description — context/metric

Longer explanation if needed.
```

**Types:** `feat`, `fix`, `perf`, `docs`, `test`, `bench`, `refactor`, `chore`, `fix+perf` (combined)

**Scopes:** `jit`, `ss` (SheepShaver), `b2` (BasiliskII), `prefs`, `machine` (Machine Layer — profile/bus/devices, `src/machine/`), `planning`/`plans` (docs), omit for cross-cutting

**Examples from this project:**
```
perf: atomic spcflags — replace spinlock with std::atomic (P0d)
fix+perf: adde/subfe carry-out via ADCS (P0b) — fixes backlog A1/A2
feat: clean guest shutdown, session timing, disk debug cleanup, docs
jit: re-enable register allocator + TBZ optimization for bclr
docs: reprioritize P2 (bcctr) from miss data, update changelog + handbook
test: wire JIT-equivalence harness as the codegen gate (make test-jit)
```

**Include in the commit message body:**
- Harness result (`make test-jit` score=100; count via `make harness-count`)
- Benchmark delta if measurable (`Mix 638, +15.6%`)
- `SS_JIT_VERIFY=1` status for RA/CR/flag changes
- What was tried and reverted, if applicable

**CHANGELOG entries cite their commit.** When you add a `CHANGELOG.md` entry, append the
implementing commit short-SHA to the entry so it's traceable — e.g.
`### [SheepShaver] … (\`c2c43fd9\`)`, or inline `(commit \`5ac5e676\`)` for sub-bullets. Multi-commit
work cites the primary + key follow-ups (`\`fe378c5d\`, triage \`980cf4df\``). Add it after the commit
exists (amend the entry, or add the SHA in a follow-up doc commit).

## Crediting Borrowed Techniques

This JIT borrows codegen and optimization ideas from other emulators (Dolphin, RPCS3,
MAME), from upstream macemu, and from CPU-architecture references. **When you contribute
code that applies a non-obvious technique, leave a comment that says where it came from,
what the technique is, and why it works** — not just *what* the code does. The next person
should be able to evaluate the idea without reverse-engineering it from the emitted
instructions.

A good attribution comment answers three questions:
1. **Source** — where the idea came from (project + a specific anchor: a file, a function,
   an upstream commit SHA, or a doc like `PERFORMANCE_AUDIT`). For straight backports, cite
   the upstream SHA (see the backport-hygiene rule below).
2. **Technique** — the name of the trick, so it's searchable (e.g. "ARM64 bitmask-immediate
   encoding", "software link-stack return prediction", "two-step ADDS carry capture").
3. **Why it works / why it's safe** — the property that makes it correct or faster, and any
   precondition (e.g. "992/1024 PPC masks are encodable; falls back to MOV+AND otherwise").

Example (from the LogicalImm encoder):
```cpp
// Technique: ARM64 bitmask-immediate encoding (Dolphin's Arm64Emitter::TryEncodeLogicalImm,
// also RPCS3). Many PPC rlwinm/andi. masks are expressible as a single AND immediate, so we
// skip the MOV-imm + AND pair. Why safe: encoder returns false for the 32/1024 non-encodable
// masks and we fall back to the materialized path. See ppc-logical-imm.hpp.
```

For pure upstream backports, also follow the existing **backport-hygiene** rule: cite the
upstream SHA in the code comment, and mark prospective/untestable code as such in both the
comment and the CHANGELOG. Keep attributions honest — if a technique was tried and only
*partially* works (e.g. the compile-time link stack), say so in the comment and the plan,
don't imply a clean win.

## Documentation Lifecycle

Docs rot when work lands but the docs don't move. Two rules keep them honest.

### 1. When you finish (or change) something

- **Log it in `CHANGELOG.md`** — component-tagged (`[SheepShaver]` / `[BasiliskII]` / `[shared]`
  / `[build]` / `[docs]`), newest first. User-visible behaviour and notable internal changes
  both belong here.
- **Update the tracking doc's item**, don't just delete it. Flip the status marker (below) on
  the relevant line in the roadmap / plan / backlog so the *record of what was decided* survives
  — a folded item with a ✅ and a one-line outcome is worth more than a vanished one.
- **An item often lives in more than one tracker.** The cross-track map (`ROADMAP.md`) and the
  detailed plan (`OPTIMIZATION-PLAN.md` / `IMPLEMENTATION-BACKLOG.md`) frequently record the same
  work from different angles. When you finish something, **grep its name across `docs/`** and
  update *every* hit — updating one and leaving the other stale is the most common way these docs
  rot. (Real example: the AltiVec merge fix was logged in ROADMAP A2 but P1b/P5c in
  OPTIMIZATION-PLAN.md were left saying "still broken.")
- **Bump the doc's `Updated:` date** in its header (below).
- For a planning/spec doc, prefer **fold-and-retire over silent deletion**: move the still-open
  items into the live tracker (`docs/planning/ROADMAP.md` or
  `docs/planning/sheepshaver-research/research/IMPLEMENTATION-BACKLOG.md`), preserve any
  "verified non-issue / do-not-re-investigate" notes in `LEARNINGS.md`, *then* remove the dated
  doc. Git history keeps the original.

### 2. Every planning / spec / research doc carries a header

So a reader knows at a glance what the doc is, how current it is, and why it exists. Put this
block directly under the `#` title:

```markdown
> **Status:** 🟡 Active · **Created:** 2026-06-04 · **Updated:** 2026-06-04
> **Why this doc exists:** <one line — the decision or work it tracks, and what prompted it>
> _Markers: ✅ done · 🟡 in progress · ⏸ blocked/deferred · ☐ todo. Finished an item? Flip its
> marker, bump **Updated**, and add a `CHANGELOG.md` entry._
```

- **Status** (doc-level): ✅ done/decided · 🟡 active/open · ⏸ blocked/deferred · 📖 reference/archive.
- **Item markers** (per line in a list): ✅ done · 🟡 in progress · ⏸ blocked/deferred · ☐ todo.
  (`☑`/`☐` checkboxes are equivalent for checklist-style docs — pick one per doc and be consistent.)
- **Created** = when the doc was first written; **Updated** = last substantive edit. Don't let
  `Updated` drift — bumping it is part of "finishing" a change.

This is the standing convention for everything under `docs/planning/`. New plans/specs start
with the header; when you touch an old doc, add it if missing.

## Change Checklists

### JIT codegen change (ppc-jit.cpp)

- [ ] `make test-jit` passes (score=100) — NOT `make test-opcodes` (that only tests the interpreter). Use `SS_HARNESS_BATCH=1 make test-jit` (~3s) for the inner loop; run plain `make test-jit` as the authoritative gate at least once per task/commit.
- [ ] **Verify the actual machine encoding — don't hand-decode or trust inline comments.** Disassemble
  the exact `emit32()` words (`python3 -c 'import capstone…'`, AArch64 little-endian) and confirm the
  mnemonic + operands. The AltiVec shift codegen shipped *scrambled* comments and the wrong NEON ops
  (rounding `SRSHL` where AltiVec truncates, signed where it needs unsigned, `vsrb`↔`vsrab` swapped)
  precisely because it was eyeballed, not disassembled (2026-06-06; `c2c43fd9`).
- [ ] **Run `python3 SheepShaver/tools/altivec-xo-audit.py` if you touch the AltiVec or FP dispatch.**
  It cross-checks every `case N:` XO against the authoritative `{mnemonic→XO}` in `ppc-decode.cpp`
  (exit 1 on mismatch). It found THREE scrambled AltiVec families + a live `mtfsf`/`mtfsfi` body swap
  on 2026-06-07. CAVEAT: it catches XO/label mismatches only — **right-XO-wrong-codegen is invisible**
  (the sum-across saturation bug and `vrfin`→`FRINTN` both passed it); `make test-jit` is the codegen gate.
- [ ] **Referee any divergence against the REAL interpreter, not a secondary oracle.** The trusted
  ground truth is `SS_TEST_HEX="<words>" SS_TEST_DUMP=1 SS_TEST_JIT={0,1} ./src/Unix/SheepShaver`
  (REGDUMP includes all 32 VR/FPR, so a single op's result is directly observable; seed inputs with
  `SS_TEST_INIT=<32 hex words>`). The rom-harness uses its *own subset* interpreter — a divergence
  there can be a harness bug, not a JIT bug. Confirm with the real emulator before claiming either.
- [ ] **Mind the ev_mixed byte order for halfword/word vector ops.** VRs are stored bytes-reversed
  within each 32-bit word; per-byte ops are order-safe, but halfword/word ops (and their operand/mask
  vectors) are NOT — validate them with lvx-built byte-*asymmetric* operands, not `vspltisb`.
- [ ] `SS_JIT_VERIFY=1 ./SheepShaver` boot for 10-20s with no divergence (for RA/flag/branch changes)
- [ ] Normal boot to Finder desktop
- [ ] `make bench` before/after if the change affects codegen performance. **If no bench kernel
  covers the affected op** (e.g. AltiVec has no kernel today), say so explicitly in the commit
  body rather than silently skipping — "perf delta unmeasured, no kernel for X; cost bounded at
  N extra insns/op" — and consider adding a kernel (`rom-harness/README.md`).
- [ ] Add harness test vector for the affected instruction if one doesn't exist. **Position/
  byte-order-dependent ops (vector permutes, merges, packs) need DISTINCT operands** — a
  self-operand or palindrome vector can rubber-stamp a wrong fix (see LEARNINGS "masking trap").
- [ ] Add/update inline ARM64 mnemonic comments on emit32() calls
- [ ] Credit any borrowed technique in an inline comment — see **Crediting Borrowed Techniques** below
- [ ] **Update EVERY tracker that mentions the item, not just one.** A plan item is often recorded
  in more than one doc — typically `docs/planning/ROADMAP.md` (the cross-track map) AND
  `docs/planning/OPTIMIZATION-PLAN.md` or `…/IMPLEMENTATION-BACKLOG.md` (the detailed plan).
  Before marking done, **grep the op/feature name across `docs/`** (e.g.
  `grep -rni vpkuhum docs/`) and flip the marker + outcome line in each hit. Leaving one stale is
  the most common doc gap.
- [ ] Update `CHANGELOG.md` for user-visible changes
- [ ] Update `LEARNINGS.md` if the change reveals a non-obvious finding

### New instruction handler

- [ ] Call `ra_load()` for ALL source operands BEFORE `ra_store()` for the destination
- [ ] Call `lazy_flush_cr0()` before reading CR (branches, mfcr, block exits)
- [ ] Call `ra_flush_all()` before `emit_bare_epilogue()` on every exit path
- [ ] Add ARM64 mnemonic comments on emit32() lines
- [ ] Add harness test vector in `SheepShaver/jit-test/run.sh`
- [ ] If the instruction sets XER CA/OV, use `emit_write_xer_ca_from_carry()` /
  `emit_write_xer_ov_so_from_overflow()` BEFORE any instruction that clobbers NZCV

### Emulator feature change (shutdown, prefs, networking, etc.)

- [ ] Update `CHANGELOG.md`
- [ ] Update `SheepShaver/docs/USER-HANDBOOK.md` if it affects user-facing behavior
- [ ] Update prefs table in USER-HANDBOOK.md if adding/changing a pref
- [ ] Update env var table in USER-HANDBOOK.md if adding an env var
- [ ] Boot test to Finder desktop

### Documentation change

- [ ] One source of truth per number — don't duplicate harness counts or benchmark scores
  in multiple files without a clear "current as of" date
- [ ] State the harness MODE when citing a score (`test-jit` vs `test-opcodes`); for the
  count use `make harness-count`, do not hardcode a number that will drift
- [ ] Update `CLAUDE.md` Key Documentation table if adding a new doc file
- [ ] Planning/spec/research doc: carries the status header (Status/Created/Updated/Why) and
  its `Updated:` date is bumped — see **Documentation Lifecycle** above

### New test vector (jit-test/run.sh)

- [ ] No duplicate names in TEST_ORDER
- [ ] Strict 4-hex-word token format for bytecodes
- [ ] Unique sentinel (no reuse of existing vector hex)
- [ ] Comment explaining what the vector tests
- [ ] Passes in both `make test-opcodes` and `make test-jit`
- [ ] **Operands actually exercise the op (not vacuous).** The recurring failure mode here is a
  vector that "passes" because both interp and JIT produce the same *trivial* result (all-zero,
  all-same-lane). Check the interp result is **lane-asymmetric** (multiple distinct bytes). For
  shifts/rotates specifically: use **amounts that exceed the element width** (catches a missing
  mod-width mask) and **sign-boundary / high-bit data** (catches logical-vs-arithmetic confusion).
  The result reaching a GPR (`grab()`) AND the VR dump both matter.

## Key Invariants

**RA ordering:** `ra_load()` for sources before `ra_store()` for destination. Violating
this when `rD == rA` returns uninitialized data (caused a black-screen boot hang).

**RA flush:** Every code path that exits a JIT block must call `ra_flush_all()`. This
includes: `emit_epilogue_with_pc()` (already calls it), `emit_bare_epilogue()` (callers
must flush first), interpreter fallbacks (caller flushes before `emit_inline_interp_call`).

**lazy_cr0_reg vs RA eviction:** If lazy CR0 is ever re-enabled, `ra_evict()` must
materialize CR0 before evicting `lazy_cr0_reg` (infrastructure already in place).

**NZCV is shared:** Any ARM64 instruction that sets flags (ADDS, SUBS, CMP, TST, ANDS)
clobbers NZCV. Flag-reading helpers (`emit_write_xer_ca_from_carry`,
`emit_write_xer_ov_so_from_overflow`) use CSET which reads but does NOT clobber NZCV.

**emit_load_gpr64 coherence hole:** The 64-bit GPR accessors bypass the RA cache for
the low word. Safe today (PPC64 ops unreachable from 32-bit guests). Must fix before
enabling G5/PPC64 paths.

**One emulator instance at a time:** Instances share prefs, disk images, and SDL window.
Kill strays with `pkill -9 -x SheepShaver`. Agents must not launch an emulator instance
without asking the user first (the E2E harness — ROADMAP A5 — is the sanctioned, isolated
exception: it uses its own prefs + a pristine per-run disk, never the user's config).

## Benchmarking

**Quick A/B (seconds, no boot):**
```bash
cd SheepShaver/rom-harness
make bench BARGS=--save-baseline=/tmp/before.txt
# ... make change, rebuild ...
make bench BARGS=--compare=/tmp/before.txt
```

**Full benchmark (minutes, requires boot):**
Boot SheepShaver, run Speedometer 4.02. Compare Benchmark Mix (integer) and
Dhrystones (stable metrics). PR composite is volatile — don't use it for JIT
codegen comparisons.

## Files That Often Need Coordinated Updates

| If you change... | Also update... |
|------------------|----------------|
| `ppc-jit.cpp` (instruction handler) | `jit-test/run.sh` (test vector), **`ROADMAP.md` + `OPTIMIZATION-PLAN.md` (both, if the item is in both — grep the op name)** |
| `ppc-jit.cpp` (RA/flush/block structure) | `LEARNINGS.md`, **`ROADMAP.md` + `OPTIMIZATION-PLAN.md` (grep the item)** |
| Prefs parsing (`prefs.cpp`) | `USER-HANDBOOK.md` prefs table |
| Emul ops (`emul_op.cpp`, `emul_op.h`) | `CHANGELOG.md`, `USER-HANDBOOK.md` |
| Harness (`jit-test/run.sh`) | nothing — the count is derived (`make harness-count`); docs are de-hardcoded |
| Build system (`Makefile`, `configure.ac`) | `CLAUDE.md` build commands |
| Env vars (any `getenv()` call) | `USER-HANDBOOK.md` env var table, `CLAUDE.md` |
| Research findings | `docs/planning/OPTIMIZATION-PLAN.md`, `docs/planning/sheepshaver-research/research/IMPLEMENTATION-BACKLOG.md` |
| Benchmark results | `docs/BENCHMARKS.md` |
| SiliconSheep features (`SiliconSheep/`) | `docs/planning/DESKTOP_INTEGRATION_PLAN.md`, `ROADMAP.md` Track C, `CHANGELOG.md` |
| Host-guest interaction channels | `docs/planning/HOST-GUEST-CHANNELS.md` |

## Project Structure (key files)

```
CLAUDE.md                          # build/test/debug reference (gitignored, local)
CONTRIBUTING.md                    # this file
LEARNINGS.md                       # session-by-session non-obvious findings
CHANGELOG.md                       # user-visible changes
docs/planning/ROADMAP.md           # outstanding work, arranged + tracked
docs/planning/OPTIMIZATION-PLAN.md # perf roadmap: done, open, deferred
docs/BENCHMARKS.md                 # Speedometer/boot timing data
docs/TESTING.md                    # test strategy, maintenance contract
SheepShaver/docs/USER-HANDBOOK.md  # user guide
docs/planning/                     # all forward-looking plans
docs/planning/sheepshaver-research/  # Dolphin/RPCS3/MAME research + backlog
SheepShaver/src/kpx_cpu/.../ppc-jit.cpp  # THE JIT (4500+ lines)
SheepShaver/jit-test/run.sh        # opcode test harness (count: make harness-count)
SheepShaver/rom-harness/           # standalone JIT exerciser + microbench
SheepShaver/e2e/                   # end-to-end VNC test harness
SiliconSheep/                      # Tauri v2 launcher / VM manager (Track C)
docs/planning/DESKTOP_INTEGRATION_PLAN.md  # SiliconSheep feature plan
docs/planning/HOST-GUEST-CHANNELS.md       # host↔guest interaction reference
```
