# Review Recommendations — 2026-06-03 (diagnostics + build session)

Backlog of findings from an adversarial review of this session's changes (per-instance
diag log, terminal heartbeat + warning matrix, `build-ss` platform guard, `hb-test.cpp`).
Deferred deliberately: pick these up once the JIT correctness work stabilizes (the
extension-loading hang is still open — see `docs/HANDOFF-2026-06-02-SESSION7.md`).

**Status legend:** ☐ open · ☑ done · ⏸ blocked on data/other work

Nothing here is a known crash or memory-safety bug. The review explicitly *disproved* the
buffer-overflow and hot-path-cost concerns (hb_tick is out-of-line per `nm`, gated behind
the pre-existing 4096-block + 5s double-gate; the snprintf length guards hold). These are
correctness-of-diagnostics, maintainability, and upstream-hygiene items.

---

## P1 — Headline: the warning matrix is calibrated for the wrong failure mode

⏸ **Blocked on boot data.** The single most important finding.

The thresholds in `jit-heartbeat.hpp` were invented from memory, not measured. Consequence:

- The only **documented** hang (extension-loading, HANDOFF:46) is a *fast busy-spin*:
  `jNK frozen, jRAM ~85M/s forever`. The `rate < 0.5M/s` WARN never fires (85M ≫ 0.5M);
  the `cpu < 50%` rule (doc-claimed to catch "VBL timer death") also misses, because a
  spin-wait pegs the core near 100%.
- The one rule that *does* fire — `comp frozen 5+ HBs` — **also fires on a healthy idle
  desktop** (LEARNINGS.md:276: idle loop runs at JIT speed, no new blocks compile). A red
  WARN during normal operation trains the reader to ignore red.

**Why deferred:** the fix is not a threshold tweak. The discriminator must be *orthogonal
to raw block rate* (e.g. jNK re-entry rate, unique-PC churn in a recent window). Designing
it requires captured diag logs of (a) the real hang and (b) a real idle desktop, side by
side. We don't have those yet, and the hang itself is still under investigation.

**Action when ready:**
1. Capture a diag log of the reproduced hang and of a healthy idle desktop.
2. Diff the region/transition/compile signals between them to find what actually differs.
3. Replace the rate/CPU/comp rules with a discriminator validated against both logs.
4. Until then, treat all heartbeat warnings as provisional; consider demoting `comp frozen`
   from WARN to SUSPECT so it doesn't read as authoritative.

---

## P2 — Cheap, safe, do-now (my code; ready whenever)

### ☐ Double heartbeat line in JIT mode
`ppc-cpu.cpp` interp-site `hb_tick` is not gated on `!jit_enabled`. When the interpreter
fallback path crosses 4096 blocks during a normal JIT run, it emits a spurious near-empty
`[HB Nm] ... interp | iNK=0 iDR=0 ...` line alongside the real JIT line. Confirmed in a
real log (`[HB 9m] ...interp...` at line 33853). Frequent under `SS_JIT_NO_ROM=1` /
`SS_JIT_ROM_SIZE<0x500000` bisection configs where the interp loop runs heavily.
**Fix:** gate the interp-site `hb_tick` call on `!jit_enabled` (one line). The interp site
already updates its own throttle clock, so it won't re-fire every 4096 blocks.

### ☐ Single-source the warning thresholds
The matrix lives in three places — `jit-heartbeat.hpp` comment, `SheepShaver/docs/DIAGNOSTICS.md`,
and `docs/superpowers/specs/2026-06-03-heartbeat-warnings-design.md`. Nothing keeps them in
sync; DIAGNOSTICS.md's own "keep both copies in sync" note already undercounts the copies.
**Fix:** extract the six thresholds as named constants at the top of `jit-heartbeat.hpp`
(`HB_RATE_WARN_M = 0.5`, `HB_TRANS_WARN = 1e6`, …); have the docs reference the constants
instead of restating numbers. (Pairs naturally with the P1 recalibration.)

### ☐ `make clean` is incomplete on macOS
`SheepShaver/Makefile:172` `clean` removes only `obj/ppc-jit.o` + the binary. The macOS
autoconf build (`build-ss` Darwin branch) produces the full object set under `src/Unix`,
which `clean` leaves stale — so `make clean && make build` relinks old objects instead of
forcing a rebuild. **Fix:** Darwin `clean` should also `cd src/Unix && make clean`.

### ☐ `hb-test.cpp` is a demo mislabeled as a test
`SheepShaver/tools/hb-test.cpp` prints `(expect WARN ...)` but has **no assertions** and
**no build target** (manual `g++` only). It rots silently if `hb_tick`'s signature changes,
and its presence implies the warnings are tested when they aren't. **Fix:** either add
assertions + a `make test-heartbeat` target that fails non-zero on mismatch, or rename to
`hb-demo.cpp` and stop calling it a test. Should not ship to upstream as-is.

---

## P3 — Minor / latent (my code)

### ☐ Linux peak-RSS latch (dormant on macOS)
`hb_rss_mb()` non-`__APPLE__` fallback returns `ru_maxrss` (peak, monotonic), so the
"RSS 2× initial" WARN can never clear once tripped. macOS uses current `resident_size` and
is unaffected, but this bites any non-Apple build (matters for an upstream PR). **Fix:**
read `/proc/self/statm` for current RSS on Linux, or skip the 2×-initial rule when `!__APPLE__`.

### ☐ OTH-region rule is cumulative, not windowed
`jit-heartbeat.hpp` `rgn[3] * 100 > blocks` is a run-lifetime ratio, unlike the windowed
rate/transition rules — a single early transient latches the WARN for the whole run.
**Fix:** make it windowed (per-HB delta) like its peers.

### ☐ Transition thresholds not validated against the *booting* config
The `j2i > 100K/s` (SUSPECT) / `> 1M/s` (WARN) thresholds were reasoned against the clean
config. But the only path that currently boots is the skip-list workaround (HANDOFF:18),
which forces ~20 opcode types to the inline interpreter call — inflating `j2i` on every
block containing one. **Fix:** capture `j2i/s` under the skip-list workaround before
trusting these thresholds (folds into the P1 data-capture work).

### ☐ `/tmp` diag logs accumulate forever
Per-instance `jit_diag.<ts>.<pid>.log` files are never pruned (~35 from one morning).
Works now, fills `/tmp` over weeks. **Fix:** cap to N newest on open, or document a
cleanup step. (Trade-off: keeping old logs is useful for before/after comparisons — maybe
just document.)

### ☐ Always-on heartbeat/diag I/O can skew perf baselines
Not a runtime regression (proven out-of-line + double-gated), but periodic syscalls +
stderr lines complicate profiling comparisons and automation logs. **Fix (if/when
measuring perf):** an env-gated quiet mode (`SS_JIT_NO_HEARTBEAT=1`).

---

## P3.5 — New World ROM support in rom-harness (PARTIALLY ADDRESSED 2026-06-03)

☑ **Reusable decoder built.** The decode logic (LZSS, parcels, CHRP container) is now in
`src/include/rom_decode.hpp` — a self-contained, header-only decoder shared by the
emulator (`rom_patches.cpp` `DecodeROM`) and the new `rom-inspect` tool. `rom_patches.cpp`
was refactored to call it (behavior-preserving). See
`docs/superpowers/specs/2026-06-03-rom-inspector-design.md`.

☐ **Remaining: rom-harness *scanning* of New World ROMs.** `rom-harness` still loads raw
bytes and scans for PPC blocks. To exercise New World ROM code it would call
`decode_rom_image()` (now available) into a 4MB buffer first, then scan the decoded image.
The decoder dependency is done; only the scan-loop wiring remains. Revisit if New World
ROM JIT coverage becomes a focus.

☐ **Bonus finding worth a follow-up: model the full PatchROM gauntlet.** `rom-inspect`
models decode + type-detection only. The parcels `Mac OS ROM 9.0.1` *passes* both yet the
emulator rejects it downstream (a `patch_*` byte-pattern search or patch-space check
fails). The emulator's "Unsupported ROM type" alert (`main.cpp:162`) fires for ANY
`PatchROM()` failure, which is misleading. Two possible improvements: (1) extend
`rom-inspect` to run the patch-space checks + report which `patch_*` stage fails; (2) make
the emulator's error message distinguish type-detection failure from patch failure. Either
would have saved real debugging time here.

## P4 — Flag for the other agent (their files — NOT touched by this review)

These came up during review but live in the icbi/isync + opcode-correctness work another
agent owns and is actively editing. Listed here only so they aren't lost; **do not edit
these without coordinating** — concurrent edits to `ppc-cpu.cpp`/`ppc-jit.cpp` have already
caused churn this session.

### ☐ Duplicate `ppc_jit_aarch64_invalidate_range()` call — CONFIRMED
`ppc-cpu.cpp:1756` **and** `:1766` call it with identical args, both under the same
`__aarch64__ && USE_AARCH64_JIT` guard. The second (icbi/isync eviction block) duplicates
the first. Redundant W^X toggles + chain-patch walks on every self-modifying-code
invalidation — the "works for boot, flakes later" class, and chaining is on by default.
**Fix:** collapse to one call.

### ☐ XO63 FP/control semantics still partial
`ppc-jit.cpp`: `fcmpu`/`fcmpo` has an explicit unordered-behavior TODO (~3449); `mcrfs`
native path writes the CR field but doesn't mirror the interpreter's FPSCR exception-bit
clearing (compare ~3584 vs `ppc-execute.cpp` ~1016); several FPSCR update paths are
simplified vs the interpreter's `record_fpscr`/`record_cr1` flow. Latent correctness debt
in FP-control-heavy workloads even though Finder now boots.

---

## Verified non-issues (do NOT re-investigate)

The review attacked these and proved them safe — recorded so nobody burns time re-checking:

- **Buffer overflow in the findings/line snprintf accumulation** — guarded with
  `(size_t)len < sizeof line` at every accumulating call; truncation short-circuits safely.
- **`findings[6]` overflow** — the six rule groups are mutually exclusive `if/else-if`, so
  max exactly 6 simultaneous; cap is exact, severity still escalates at cap.
- **Hot-path cost of hb_tick / getrusage / task_info** — out-of-line (confirmed via `nm`
  on `ppc-cpu.o`), behind the pre-existing 4096-block + 5s gate; getrusage/task_info run
  at hb_tick's own 10s/60s cadence, never per-block. No new clock read added.
- **Wrongful interpreter fallthrough from these changes** — the diag/heartbeat diffs touch
  only diagnostic blocks; dispatch/compile/chaining untouched. (The lwarx/stwcx/mftb
  fallthroughs are the *other* agent's correctness fix and are intentional.)
- **Same-second double emulator start** — diag filename includes pid; logs stay distinct.
- **build-ss Makefile Darwin change** — dropping `| tail | || true` is a correctness
  improvement (failed builds now halt instead of silently relinking stale objects).
