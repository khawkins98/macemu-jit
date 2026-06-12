# Lead 4 — Punt OE-form overflow to the interpreter?

> **Status:** 📖 Reference / archive · **Created:** 2026-06-02 · **Updated:** 2026-06-04
> **Why this doc exists:** Research lead: punt OE-form overflow to the interpreter? (rejected) — verdict in backlog.
> _Markers: ✅ done · 🟡 in progress · ⏸ blocked/deferred · ☐ todo. Finished an item? Flip its marker, bump **Updated**, and add a `CHANGELOG.md` entry (see [CONTRIBUTING](../../../../CONTRIBUTING.md) → "Documentation Lifecycle")._


**Verdict: DO NOT delete our inline OE codegen.** Lead 4's premise — "OE forms are
rare; just fall back like Dolphin-ARM64" — is correct *for Dolphin's workload
(native PPC games)* and **inverted for ours (Mac OS, whose ROM-resident 68K
emulator derives 68K condition codes via `addco`/`subfco`)**. Same PPC750-class
ISA, opposite OE frequency. Deleting the inline OE add/sub/neg paths would regress
the exact instructions that got the JIT to boot to the desktop (commit `8f2acc9b`).

What we actually run today is a **hybrid that is strictly better than both naive
options**: inline the hot OE add/sub/neg forms, punt the rare OE mul/div to the
interpreter. The useful action item from this lead is not deletion — it is a
latent-correctness fix for `mullwo` and a measurement plan to confirm OE
mul/div really are cold.

---

## Verified Dolphin behavior

Source fetched from Dolphin `master`:

### JitArm64 (ARM64 backend) — `Source/Core/Core/PowerPC/JitArm64/JitArm64_Integer.cpp`
`FALLBACK_IF(inst.OE)` appears in these handlers, punting the OE form to the
interpreter:

- `addx`, `negx`
- `subfx` (subf, subfc), `subfex` (subfe, subfme), `subfzex` (subfze, subfme)
- `addcx` (addc)
- `divwx` (divw), `divwux` (divwu)

A helper `GenerateConstantOverflow()` *can* set XER OV/SO inline, but it is **not
called from any of the OE arithmetic handlers** — every OE arithmetic form falls
back. So Dolphin-ARM64 punts OE essentially wholesale.

### Jit64 (x86-64 backend) — `Source/Core/Core/PowerPC/Jit64/Jit_Integer.cpp`
The mature x86-64 backend handles OE **inline**: no `FALLBACK_IF(inst.OE)` in
addx/subfx/mullwx/divwx/negx. Instead it emits
`if (inst.OE) GenerateOverflow(...)` (conditional OV/SO from CPU flags) and
`if (inst.OE) GenerateConstantOverflow(true/false)` (compile-time-known overflow,
e.g. div edge cases).

**Conclusion from the cousin:** the *less mature* backend (ARM64) punts; the
*more mature* backend (x86-64) emits OE inline. Dolphin-ARM64 punting is a
maturity/priority artifact, not a considered "OE is never worth inlining"
decision. Their workload (GameCube/Wii games) just rarely hits OE, so the ARM64
backend never prioritized it.

---

## Our current OE codegen inventory (file:line)

All in `SheepShaver/src/kpx_cpu/src/cpu/jit/aarch64/ppc-jit.cpp`.

### Shared OV/SO emitter — 8 instructions of emit
- `emit_write_xer_ov_so_from_overflow()` — **lines 600–608**. CSET Wd,VS →
  STRB to XER.OV (offset 1029); LDRB XER.SO, ORR, STRB back (sticky SO).
  Clobbers RTMP1/RTMP2, preserves RTMP0 and NZCV. ~7 emitted ARM64 instructions.
- The comment at **lines 593–599** states these OE ops are "extremely hot"
  because the in-ROM 68K emulator uses them for 68K condition codes.

### Inline OE handlers (the hot five) — lines 1349–1400
- **522 `addco`** (1355–1363): ADDS, store, CA-from-carry, OV/SO, optional CR0.
- **520 `subfco`** (1365–1373): SUBS, store, CA, OV/SO, optional CR0.
- **778 `addo`** (1375–1382): ADDS, store, OV/SO (CA untouched), optional CR0.
- **552 `subfo`** (1384–1391): SUBS, store, OV/SO, optional CR0.
- **616 `nego`** (1393–1400): SUBS 0−rA, store, OV/SO, optional CR0.

Each adds ~3–6 emitted instructions over the non-OE form (mostly the shared
emitter call). **Total emit code that exists purely for OE forms: ~60–68 source
lines** (the emitter helper + the five case bodies' OE-specific portions).

### OE mul/div — NOT inlined (the cold forms)
- **715 / 235 `mullwo` / `mullw`** share one handler (**lines 1068–1075**). It
  emits `MUL` and stores the product but **ignores the OE bit entirely** — no
  `emit_write_xer_ov_so_from_overflow()` call. So **`mullwo` currently does not
  set XER OV/SO** (latent correctness gap; see Analysis).
- **`divwo` (XO=1003), `divwuo` (XO=971)** have **no case**. Only plain
  `divw` (491, lines 1076+) and `divwu` (459) are handled. The OE forms fall
  through to the miss path → interpreter.

### Fallback mechanism and cost
`compile_one()` returns `false` for any unhandled opcode (e.g. lines 1141, 1179,
1896, 2056, 2060; primary-miss bump at 3710, XO-miss bump at 2059/4014). A
`false` return means the **whole block** is not compiled — it stays interpreted
(see `ppc-cpu.cpp` `compile_block` / `execute` loop ~line 565–700). So the cost
of a punt is **not** a per-instruction trampoline; it is "this entire basic block
runs in the interpreter forever." That is the key asymmetry: punting a *hot* OE
op forfeits JIT coverage of every block containing it.

---

## Frequency evidence / measurement plan

**Direct measurement is not possible in this checkout:** the `make test-rom`
target points `ROM := /workspace/tmp/.../PowerMac-9500-OldWorld.rom`, which does
not exist on this machine, and no PowerMac/OldWorld `.rom` was found anywhere on
the filesystem. So the numbers below are from code comments, not a fresh run.

- **In-tree claim (comment, lines 1352–1354):** "blocks containing [addco/subfco]
  (76%+19% of all compile failures) stay interpreted." This is an inline source
  comment, **not** a recorded measurement in LEARNINGS.md/JIT-STATUS.md, and
  `git log -S "76%"` / `git log --grep=76` did not surface a commit that recorded
  it. **Treat the 76%+19% figure as an annotation of unknown precision**, though
  it is consistent with the boot history (commit `8f2acc9b` "boot to desktop with
  JIT" landed alongside the OE work; commits note OE add/sub were the dominant
  remaining compile-failure cause).

- **How to measure properly (reproducible):** the JIT already has the
  instrumentation. `jit_report_misses()` (lines 803–832) dumps `jit_miss_count`
  (primary opcode histogram) and `jit_xo_miss[1024]` (XO histogram for opcode 31).
  With a real OldWorld ROM present:
  1. `make test-rom` (rom-harness, headless) — prints the miss histograms;
     watch XO buckets 1003 (`divwo`) and 971 (`divwuo`); 715 (`mullwo`) will
     *not* appear as a miss because we accept-but-ignore it.
  2. A boot run (`make run-jit`) surfaces the same `PPC-JIT-A64:` lines on stderr.
  To quantify how often the *inline* OE forms execute (not just compile), add an
  execution counter in the 522/520/778/552/616 cases (cheap, behind an env gate)
  — but the existing miss histogram already answers the only decision-relevant
  question: are `divwo`/`divwuo` hot enough to be worth inlining? (Expected: no.)

---

## Analysis

**Risk of deletion (falling back like Dolphin-ARM64).** High and proven. Because a
punt drops the *entire enclosing block* to the interpreter, and because the ROM's
68K emulator uses `addco`/`subfco` in its hottest loops, deleting inline OE
add/sub/neg would re-interpret a large fraction of the hottest blocks — the
opposite of the boot-enabling work in `8f2acc9b`. Dolphin can afford the punt
because its games seldom emit OE; we cannot, because Mac OS's interpreter-inside-
the-emulator does.

**Simplification win of deletion.** Small: ~60–68 source lines and one emit
helper. Not worth a measurable performance regression on the hottest path.

**The real, defensible position is the hybrid we already have:**
- Keep inline: `addco` (522), `subfco` (520), `addo` (778), `subfo` (552),
  `nego` (616) — hot, cheap (~7 extra ARM64 insns via the shared emitter),
  correct, and test-covered.
- Keep punting: `divwo` (1003), `divwuo` (971) — rare, and the div path is
  already complex (edge-case guards); inlining OV there buys little.

**Latent correctness gap to fix (independent of Lead 4):** `mullwo` (715) is
*accepted* by the JIT but silently ignores OE — it never sets XER OV/SO. This is
worse than punting: a punt would at least be correct via the interpreter. Two
clean options:
  (a) **Punt 715** by removing it from the shared `case 715:` and letting it miss
      (simplest, correct), or
  (b) emit OV/SO inline for the OE form (signed 32×32 overflow = high word not the
      sign-extension of the low word; needs SMULL + compare, more work).
Given OE mul is cold, **(a) is the right call** — it makes behavior match the
"punt the rare OE mul/div" rule the code already follows for divwo/divwuo.

**Test coverage proving inline OE is safe.** `jit-test/run.sh` has dedicated OE
vectors (lines ~339–377): `addco_basic`, `addco_overflow`, `addco_rc_overflow`,
`subfco_basic`, `subfco_overflow`, `addo_overflow`, `subfo_basic`, `nego_basic`,
`nego_overflow`. These cover OV-set, OV-clear, sticky-SO, and Rc=1 CR0
interaction for the inline forms. Note the harness currently diffs interpreter
vs interpreter unless `SS_TEST_JIT=1` is set; to actually exercise the JIT OE
paths, run those vectors with `SS_TEST_JIT=1`. There are **no** vectors for
`mullwo`/`divwo`/`divwuo`, consistent with them being punted/ignored.

---

## Recommendation

1. **Reject Lead 4's deletion.** Inline OE add/sub/neg is load-bearing for Mac OS
   boot; the workload inversion vs Dolphin (ROM 68K emulator uses `addco`/`subfco`
   for condition codes) is the spine of the argument. Annotate Lead 4 in
   `EMULATOR-RESEARCH-LEADS.md` as "investigated — does not apply; our workload
   inverts the frequency assumption."
2. **Fix the `mullwo` (715) latent bug** by punting it (remove the `case 715`
   alias so it misses → interpreter), matching how `divwo`/`divwuo` are already
   handled. Add a one-line note; optionally add a `mullwo` interpreter-diff vector.
3. **Measure when a ROM is available:** run `make test-rom` and read the
   `PPC-JIT-A64:` miss histograms (XO 1003/971) to confirm OE mul/div stay cold.
   Record the result in LEARNINGS.md and replace the unsourced "76%+19%" comment
   with the measured figure (or relabel it as an estimate).
