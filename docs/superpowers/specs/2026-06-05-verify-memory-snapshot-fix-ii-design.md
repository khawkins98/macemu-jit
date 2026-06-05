# SS_JIT_VERIFY fix (ii) — touched-memory snapshot/restore around the replay

**Status:** DESIGNED + REVIEW-VETTED, **NOT IMPLEMENTED** — awaiting user greenlight.
**Date:** 2026-06-05  ·  **Owner:** (unassigned)  ·  **Risk gate:** real-boot blast radius (see §6).

> One-line: the verify oracle is register-only and does **not** snapshot guest memory, so the
> interp replay double-applies any block that reads-modifies-writes the same slot. Fix (ii)
> snapshots the bytes the JIT touched and restores them before the replay. This is the **last**
> of the 6 oracle-confound classes (5/6 already fixed by fix (i) + dedup).

---

## 1. Motivation & evidence

After fix (i) (`5ac5e676`, replay mirrors the JIT single-block exit) and per-block dedup
(`7314bc9c`), a **broad boot-wide verify sweep** was run (2026-06-05,
`SS_JIT_VERIFY=1 SS_JIT_NO_CHAIN=1`, HD boot). Result:

| Class | Blocks | Verdict |
|-------|--------|---------|
| `ARTIFACT-PC` (structural, PC mismatch) | `1011e198 1011e31c 1018ac70 1018ac80` | handled by fix (i) — not reported as bugs |
| `SUSPECT` (PC matches) | `100fd0e0 10106b50 1011e734 1018b04c` | **all four are class-6 memory-RMW** |

Every remaining `SUSPECT` is a load→modify→store on a slot the block also loads from:

- `100fd0e0` — `lwz r10,0(r8); addi r10,r10,1; stw r10,0(r8)` — counter, GPR10 **steps by 2**.
- `10106b50` — `lhz r3,10(r31); addi r3,r3,1; sth r3,10(r31)` — counter, GPR3 off-by-1 per visit.
- `1011e734` — `lwz r12,4(r3); …; stw r6,4(r3)` — pointer-walk, slot `4(r3)` is RMW.
- `1018b04c` — `lwzx r0,r3,r4; …; stwx r6,r3,r4` — indexed RMW on `[r3+r4]`.

**Conclusion that gates priority: the now-working oracle finds ZERO real codegen bugs boot-wide.**
The four residual divergences are an oracle limitation (no memory restore), not a JIT defect.
Fix (ii) therefore **buys oracle completeness, not a bug fix** — which is exactly why it is *not*
urgent and *must not* be landed autonomously (§6).

## 2. Why the replay double-applies (root cause)

The oracle runs each block twice: once JIT, once interp-replay, then `memcmp`s the register
files. The JIT path executes real guest stores (the JIT inlines its own stores; it does **not**
go through `vm_write_memory_*`). When the replay then re-executes the same instructions, the
load reads the **already-mutated** slot, so the recomputed value differs by exactly the store's
effect — and the replay's own store mutates the slot a *second* time, corrupting guest memory
for real if verify were left on. The register divergence is the visible symptom; the silent
memory corruption is the reason verify boots are not trustworthy past the first such block.

## 3. Design — interp-first reorder + RAM-write journal

Gated **entirely** behind `SS_JIT_VERIFY` (zero effect on normal boots).

1. **Reorder to interp-first.** Run the interp replay on the *pristine* pre-state **before** the
   JIT executes the block. Snapshot `powerpc_registers` pre-state, run interp, capture its
   post-state, **restore registers**, then run the JIT, then `memcmp(jit_post, interp_post)`.
   This removes the read-after-JIT-write hazard for the *register* comparison without any memory
   bookkeeping at all — the interp now reads pristine memory.
2. **RAM-write journal (to undo the interp replay's own stores).** Because the interp replay now
   runs first and *does* write memory, its writes must be undone before the JIT runs (else the
   JIT reads contaminated memory — the mirror of today's bug). Wrap `vm_write_memory_{1,2,4,8}`
   (the interp store chokepoint, `vm.hpp`) with a journal-on-write under a
   `verify_journal_active` flag: record `(addr, size, old_bytes)` before each store; after the
   interp replay, replay the journal in reverse to restore the original bytes; then run the JIT.

This is purely tooling, but it touches the interpreter store path and the dispatch ordering — a
real-boot-critical region — hence the review gate in §6.

## 4. Must-fix items from review (do not skip)

These two were flagged as **landing-blockers** by sub-agent + advisor review:

- **(a) `ends_in_fallback` block-info bit.** A block that ends in the inline-interp fallback
  (`emit_inline_interp_call`, `ppc-jit.cpp:~4789`) leaves `complete == true`, so the replay
  accounting can over-run `n_insns` past the fallback boundary. The reorder must consult an
  explicit "this block ended in a host-side fallback" bit on `jit_bc_entry` (it does **not** end
  at a clean PPC terminator), and stop the interp replay at the fallback instruction rather than
  trusting `n_insns`. Without this, the interp-first replay and the JIT diverge on block length,
  not on codegen — a new false-positive class.
- **(b) Journal capacity.** Size the journal to **16384 entries** (not a small fixed buffer). A
  single block can issue many stores (`stmw` writes up to 32 words; string/multiple ops more). If
  the journal overflows mid-block and silently drops entries, the reverse-replay leaves guest RAM
  permanently corrupted — the exact failure fix (ii) exists to prevent. On overflow: **abort the
  verify for that block and log loudly**, never silently truncate.

## 5. Register-only-oracle caveat (scope boundary)

Even with fix (ii), the oracle compares **registers only**. It does not compare the *memory* the
JIT vs interp wrote — a JIT store-to-wrong-address bug with no register footprint is still
invisible. Fix (ii) removes the *register* false-positives that memory-RMW causes; it does **not**
turn the oracle into a memory-divergence detector. If memory-correctness coverage is wanted, that
is a separate, larger feature (diff the touched-byte sets post-block), explicitly out of scope
here. Document this in the `[VERIFY]` banner so nobody over-trusts a clean verify run.

## 6. Why this is NOT auto-landed (decision record)

Advisor + sub-agent review reached **NO-GO for autonomous landing** while the user is away:

- **Asymmetry.** Waiting for greenlight is nearly free; a memory-journal bug that corrupts a real
  boot (overflow, missed unwrap on an early-return path, reorder breaking the spcflags poll) is
  expensive and hard to notice. The change lives in the interpreter store path + dispatch order.
- **Marginal ROI.** The sweep (§1) proves no real bug hides behind the artifacts, so (ii) is
  polish on a tool that has already done its job (P1a validated, 5/6 classes fixed).
- **Oracle stays register-only regardless** (§5), so (ii) does not even make the oracle
  "complete" — it removes one false-positive class.

**Recommendation: LOW priority. Defer** unless/until new codegen work wants a memory-clean
oracle to bisect against. When picked up, implement §3 with both §4 must-fixes, validate by: (1)
harness `make test-jit` still 270/270; (2) a verify sweep where the four §1 blocks now verify
**clean**; (3) a normal (verify-off) boot to Finder unaffected.

## 7. Touch list (when implemented)

- `SheepShaver/src/kpx_cpu/src/cpu/ppc/ppc-cpu.cpp` — reorder replay to interp-first; journal
  restore; banner caveat.
- `SheepShaver/src/kpx_cpu/include/vm.hpp` (`vm_write_memory_*`) — journal-on-write under
  `verify_journal_active`.
- `SheepShaver/src/kpx_cpu/src/cpu/jit/aarch64/ppc-jit.cpp` (`jit_bc_entry`, ~164;
  `emit_inline_interp_call`, ~4789) — `ends_in_fallback` bit.
- Docs: flip OPTIMIZATION-PLAN §0b-extra4 class-6 row to ✅; ROADMAP A1.
