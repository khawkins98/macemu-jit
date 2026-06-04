# Lead 7 — Block Dispatch and Linking (Dolphin cross-check)

> **Status:** 📖 Reference / archive · **Created:** 2026-06-02 · **Updated:** 2026-06-04
> **Why this doc exists:** Research lead: block dispatch & linking cross-check — verdict in backlog.
> _Markers: ✅ done · 🟡 in progress · ⏸ blocked/deferred · ☐ todo. Finished an item? Flip its marker, bump **Updated**, and add a `CHANGELOG.md` entry (see [CONTRIBUTING](../../../../CONTRIBUTING.md) → "Documentation Lifecycle")._


Deep dive on Dolphin's JitArm64 block linking and dispatcher versus our chain-site /
`jit_bc_heads[]` design. Sources verified against Dolphin `master`
(`JitArm64Cache.cpp`, `JitAsm.cpp`, `JitCommon/JitCache.h`) on 2026-06-02.

---

## 1. Verified Dolphin implementation

### 1.1 Block linking — `JitArm64BlockCache::WriteLinkBlock`
(`Source/Core/Core/PowerPC/JitArm64/JitArm64Cache.cpp`)

Every exit site is emitted as a **fixed-size slot** (`BLOCK_LINK_SIZE`) so it can later be
re-patched in place without disturbing surrounding code. The slot is filled differently
depending on what is known at patch time:

- **Unknown target (no destination):** emit `MOVI2R(DISPATCHER_PC, exitAddress)` then `B`
  (or `BL` for a call-style exit, with a NOP to align to `BLOCK_LINK_FAST_BL_OFFSET`) to the
  asm dispatcher. This is the "not linked yet" state.
- **Destination in range (±0x40000 instructions, i.e. ±256 KiB words):** emit a conditional
  `B(CC_GT, dest->normalEntry)` — taken only when downcount is still positive — followed by
  `B(source.exitFarcode)` for the timeslice-expired path. This is the *linked* fast path: a
  direct conditional branch straight into the next block's entry.
- **Destination out of range:** slow path — `B(CC_LE, …)` to far code then a plain
  `B(dest->normalEntry)`, because a single conditional branch can't reach.
- **Call case (LK):** `B(CC_GT)` + `B(source.exitFarcode)`, then `BL(dest->normalEntry)`
  emitted at the fixed BL offset so the return address is correct.

Critically, **all paths pad the slot to `BLOCK_LINK_SIZE` with `BRK(101)`**. The padding
guarantees the slot is always the same byte length, so a later relink overwrites cleanly.

### 1.2 Block destruction — `WriteDestroyBlock`

Writes a single `BRK(0x123)` over the block's `normalEntry` (4 bytes), bracketed by JIT-page
write-enable and followed by an icache flush. Any stale code that still holds a direct branch
into this entry will **trap immediately** at the BRK instead of executing a recycled/garbage
block. This is a hard tripwire, not a fallback — in a correct build it must never fire.

### 1.3 Dispatcher — `JitArm64::GenerateAsm` (`JitAsm.cpp`)

Downcount/timeslice gate first:
```
FixupBranch bail = B(CC_LE);          // downcount <= 0 -> leave to scheduler
dispatcher_no_timing_check = GetCodePtr();
```
Then an **inline, in-asm** block lookup with no C++ call on the hot path:
```
LDR(..., feature_flags, PPC_REG, PPCSTATE_OFF(feature_flags));
MOVP2R(cache_base, GetBlockCache()->GetEntryPoints());
LSL(pc_and_feature_flags, EncodeRegTo64(DISPATCHER_PC), 1);
BFI(pc_and_feature_flags, EncodeRegTo64(feature_flags), 33, 31);
LDR(block, cache_base, pc_and_feature_flags);   // direct-indexed array load
FixupBranch not_found = CBZ(block);
BR(block);                                       // jump straight into native code
```
Only on miss (`CBZ`) does it call C++:
```
MOVP2R(X8, &JitBase::Dispatch); MOVP2R(X0, this); BLR(X8);
FixupBranch no_block_available = CBZ(X0);
BR(X0);
```

### 1.4 Fast block map structure (`JitCommon/JitCache.h`)

- `u8** m_entry_points_ptr` backed by a `LazyMemoryRegion` arena.
- `FAST_BLOCK_MAP_SIZE = 0x10'0000'0000` (1 TiB of *index* space = 4 GiB guest ÷ 4 × feature
  flags). The arena is lazily/sparsely backed (shared memory), so the resident footprint is
  only the touched pages — the lookup is a **single direct-indexed load**, no hashing, no walk.
- Fallback when the sparse arena can't be mapped:
  `std::array<JitBlock*, 0x10000> m_fast_block_map_fallback` (64 K entries, ~512 KiB),
  masked by `FAST_BLOCK_MAP_FALLBACK_MASK`, with a PC compare to resolve aliasing.
- Authoritative slow structure: `std::multimap<u32, JitBlock> block_map` keyed by physical
  address (used for invalidation, not the hot path).

The design point: **the entry-points array is indexed, not searched**, so dispatch is O(1)
with no branch-heavy hash walk, and the array's value is the *entry pointer itself* (CBZ tests
presence), so a single load + BR completes a non-linked dispatch entirely in asm.

---

## 2. Our current dispatch + linking

All file:line refer to `SheepShaver/src/kpx_cpu/src/cpu/jit/aarch64/ppc-jit.cpp` unless noted.

### 2.1 Block cache structures
- `jit_bc_heads[JIT_BC_BUCKETS]` / `jit_bc_pool[JIT_BC_POOL]`, sentinel −1 (lines 52–89).
  **Actual sizes: 32768 buckets / 65536 pool entries** (`JIT_BC_BUCKETS`/`JIT_BC_POOL`,
  lines 52–54). Note: CLAUDE.md's "8192-bucket" figure is stale — the source says 32768.
- Hash: `bucket = (pc >> 2) & JIT_BC_MASK`; collisions chained via `.next` (lines 199–208).
- Chain-site pool: `chain_site_pool[JIT_CHAIN_SITE_POOL=16384]`, `chain_site_heads[]`
  (lines 68–76), with a per-site `bool patched` (line 72) for invalidation reversal.

### 2.2 How a block finishes — `emit_epilogue_with_pc` (lines 676–711)
1. `ra_flush_all()` — write back dirty GPRs.
2. `emit_load_imm32 + a64_str_w` — store `next_pc` to `regs->pc`.
3. **Compile-time chaining** (lines 686–698): `jit_bc_lookup(next_pc)`; if the target block
   exists with a `chain_code` within ±2^25 bytes, record the site (marked `patched=true`) and
   emit a single `B <offset>` straight into the target's chain entry. No frame teardown — the
   chained block reuses the current prologue's stack frame. **This is our equivalent of
   Dolphin's in-range linked path.**
4. **Standard epilogue** (lines 699–710) when the target isn't compiled yet: record a chain
   site at the LDP address, then emit the **7-instruction** teardown:
   `LDP x27,x28 / x25,x26 / x23,x24 / x21,x22 / x19,RSTATE / FP,LR` (post-index) + `RET`.

### 2.3 How the next block starts — the C++ loop (`ppc-cpu.cpp:712–795`)
- JIT gate at `ppc-cpu.cpp:712`, env-gated by `SS_USE_JIT` (line 730–732).
- After a block returns (RET), `pc()` was updated by the epilogue. The loop does an **inline
  fast re-dispatch** (ppc-cpu.cpp:786–790): `ppc_jit_aarch64_compile(pc())` (which is really a
  `jit_bc_lookup`, ppc-jit.cpp:3838) and, on hit, calls the next block directly via the C++
  function pointer (`fn((void*)regs_ptr())`) and `goto pdi_jit_post`.
- On miss it falls to `my_block_cache.find(pc())` → interpreter (`pdi_execute`).

### 2.4 Cost of one non-chained dispatch
When two adjacent blocks are *not* directly chained (target wasn't compiled when the source
was emitted, or got reverted), the round trip per transition is:

- **Block exit (emitted asm):** 7 instructions — 6× `LDP` post-index + `RET` (lines 704–710),
  plus the 2 store-PC instructions already counted as block work.
- **C++ round trip (ppc-cpu.cpp:786):** call `ppc_jit_aarch64_compile` → `jit_bc_lookup`
  (lines 199–208): mask + indexed load of bucket head, then a **pointer-chase loop** comparing
  `.pc` and testing `.code != NULL` for each entry in the collision chain (≥1 iteration,
  typically 1–3), populate the `ppc_jit_block out` struct (8 field stores, lines 3842–3848),
  return, re-load the function pointer, indirect `BLR`.

So a non-chained dispatch is roughly **7 emitted insns + a full C++ function call + a hash
walk + struct fill + indirect call** — on the order of 30–60 host instructions plus a
non-inlinable call boundary and the associated register save/restore. Dolphin's non-linked
dispatch by contrast is **~7 asm instructions total** (LDR feature flags, MOVP2R, LSL, BFI,
LDR, CBZ, BR) with zero C++ unless the entry pointer is null.

### 2.5 T2 counter findings (commit 7030a441, LEARNINGS.md ~L533–545)
- JIT compiled ~16,916 *complete* blocks; `jit_blocks_attempted` plateaued at **66,836** and
  then grew ~1/sec.
- After the first ~15 s burst, **profiling shows ALL ~1500 samples in the interpreter inner
  loop** (`ppc-cpu.cpp` `skip_jit:`), **zero in the JIT code cache.**

**This is the load-bearing fact for Lead 7:** at steady state we are *not* dispatching
JIT→JIT at all — we've fallen out of the JIT loop into the interpreter and stay there.
The dispatcher's per-transition cost is therefore **not currently the bottleneck**; the
bottleneck is that execution leaves the JIT loop and doesn't return. See §4.

---

## 3. BRK tripwire analysis

**Where we invalidate today:**
- `jit_bc_invalidate_pc` (lines 179–197): unlinks the pool entry and sets `code=NULL`.
  Does **not** touch emitted code or any inbound direct branches.
- `ppc_jit_aarch64_invalidate_range` (lines 3765–3807): Step 1 reverts live chain-patches
  whose `target_pc` is in range back to `JIT_EPILOGUE_FIRST_LDP` (0xA8C17BFB); Step 2 nulls
  `code` for in-range pool entries.

**Would a BRK-on-destroy tripwire help us?** Yes, as a *debugging* assertion, with a caveat:

- Our correctness model is "revert inbound chains, then null the entry." If Step 1 ever misses
  a chain site (e.g. a site recorded as `patched=true` at compile time, line 694, whose pool
  slot was recycled, or a site beyond `JIT_CHAIN_SITE_POOL=16384` that was silently dropped at
  line 147), a stale `B` would still jump into reused code cache bytes — **silently**, because
  we never overwrite the old entry.
- A BRK tripwire would convert that silent corruption into an immediate, localized trap.
  Concretely: when `jit_bc_invalidate_pc` / range-invalidate nulls an entry, also write
  `BRK #imm` over the **first word of that block's `code`** (bracketed by
  `jit_cache_begin_write`/`end_write` + icache flush, exactly as `patch_chain_sites` does at
  lines 168–171). Any surviving inbound `B` then traps at the BRK instead of running garbage.
- **Caveat specific to us:** unlike Dolphin, we *recycle the code-cache write pointer only on
  full flush* — individual blocks are never freed/reused mid-run, the bytes just go stale when
  `code=NULL`. So a BRK over a destroyed entry is safe (nothing legitimately re-enters a nulled
  PC without recompiling, since `jit_bc_lookup` skips `code==NULL`). The tripwire is therefore
  low-risk and high-signal: it specifically catches the "dropped/recycled chain site" class of
  bug that our `patched` bookkeeping is exposed to.

Recommendation: add it **gated behind a debug `#ifdef`** (or an env flag), not unconditionally —
in production the revert path is the correctness mechanism; the BRK is the assertion that the
revert path is complete.

---

## 4. Inline-asm dispatcher analysis

**What an emitted fast lookup would look like for us.** Our entries are pointers in
`jit_bc_pool[]`, indexed via a hashed bucket + collision walk — not a flat array. Two options:

- **(a) Mirror Dolphin's flat entry-points array.** Add a directly-indexed array
  `code_ptr entry_points[PC_SLOTS]` keyed by `(pc>>2) & mask`, storing the *chain_code pointer*
  (or NULL). The emitted block epilogue, instead of LDP+RET, would emit:
  `MOVP2R(xN, entry_points); UBFX(xM, pc, 2, log2(slots)); LDR(xT,[xN,xM,lsl#3]); CBZ skip; BR xT`.
  On miss, fall through to the existing LDP+RET → C++. This collapses the JIT→JIT hot path to
  ~5 asm instructions and removes the C++ call entirely for cached transitions. Aliasing
  (two PCs hashing to one slot) is handled by storing only blocks whose low bits match, or by
  a PC compare like Dolphin's fallback path.
- **(b) Keep the hash but emit the walk** — not worth it; a branchy emitted hash walk gives up
  most of the win.

**Expected win given the T2 numbers.** Here is the honest assessment: **the inline dispatcher
optimizes a path we are barely on.** At steady state we execute ~zero JIT blocks (profiling:
zero samples in the code cache). The win from an inline dispatcher is proportional to JIT→JIT
transition frequency, which is presently ~1/sec. Therefore:

- As a standalone perf change today: **near-zero payoff** — we'd be speeding up a loop the
  program has already exited.
- The real problem Lead 7 surfaces is upstream: **why does execution leave the JIT loop after
  ~15 s and never return?** (Each block exits to C++; the fast re-dispatch at ppc-cpu.cpp:786
  only re-enters JIT if the *next* PC is already a complete block — otherwise it drops to the
  interpreter via `my_block_cache.find`, and apparently never climbs back.) An inline
  dispatcher would only matter *after* we fix re-entry so the JIT loop sustains itself.

So the inline dispatcher is a **real but premature** optimization. Its precondition is fixing
JIT-loop residency; once residency is high, an inline entry-points array is the natural way to
make the high-frequency JIT→JIT path cheap, and *then* the T2 transition count becomes the
metric to watch.

---

## 5. Chain design comparison (ours vs Dolphin BRK-padded slots)

| Aspect | Ours (chain_site_pool) | Dolphin (BRK-padded slots) |
|---|---|---|
| Link representation | Single `B` overwritten over the first LDP of the epilogue (line 169, 695) | Fixed-size slot, all states padded to `BLOCK_LINK_SIZE` with `BRK(101)` |
| Relink in place | Yes — overwrite one word | Yes — rewrite the whole fixed slot |
| Inbound-link tracking | External side table `chain_site_pool[]` keyed by target PC | Recomputed from block metadata; slot is self-describing |
| Capacity limit | **`JIT_CHAIN_SITE_POOL = 16384`; sites silently dropped when full (line 147)** | No fixed side-table cap — links derived from blocks |
| Stale-link safety | Revert-on-invalidate (range step 1); **correctness depends on the side table being complete** | BRK-on-destroy hard-traps any missed inbound link |
| Out-of-range targets | Fall back to LDP+RET → C++ (offset > ±2^25, line 689) | Slow-path `B(CC_LE)`+`B` within the padded slot |

**Correctness/capacity issues ours has that theirs avoids:**

1. **Silent chain-site pool exhaustion.** `record_chain_site` returns silently when
   `chain_site_pool_next >= 16384` (line 147). A dropped site means a future block at that PC
   can't back-patch the waiting epilogue — *correct but slow* (stays unchained). More subtly,
   the pool **only ever grows** (`chain_site_pool_next++`), reset solely by `jit_bc_flush`
   (line 108). Over a long run with no full flush, 16384 sites is easily exceeded, after which
   **no new runtime chaining happens at all** until the next flush. Dolphin has no such cap.
2. **Side-table-completeness dependence for invalidation.** Range-invalidate (Step 1) reverts
   only sites it finds in `chain_site_pool[]`. If a site was dropped (issue 1) but the `B` was
   nonetheless emitted via the *compile-time* path (lines 690–695, which records a site right
   before incrementing — so this specific path can't drop, but a recycled pool slot could),
   the revert could miss it. Dolphin's BRK-on-destroy is the backstop ours lacks.
3. **No PC-collision disambiguation in the side table beyond `target_pc` compare.** This is
   actually fine (we compare `target_pc` exactly, line 165), but it means every patch is an
   O(chain length) walk of `chain_site_heads[bucket]`.

Theirs avoids 1–2 structurally (links derived from live blocks + BRK backstop). Ours is
simpler and, given we full-flush on pool exhaustion, *correct* — but it degrades silently
under chain-site pressure and has no tripwire for the bug class in issue 2.

---

## 6. Effort, risk, payoff

| Item | Effort | Risk | Payoff (today) | Payoff (after JIT residency fix) |
|---|---|---|---|---|
| BRK-on-destroy tripwire (debug-gated) | Low (~30 LOC, reuse `patch_chain_sites` write bracket) | Low | Catches stale-chain bugs; protects the in-flight `patched`-field work (commit, LEARNINGS L597) | Same |
| Chain-site pool exhaustion fix (counter + warn, or grow/recycle) | Low–Med | Low | Removes a silent "chaining stops" cliff on long runs | Higher — sustained JIT needs sustained chaining |
| Inline-asm entry-points dispatcher | Medium | Medium (W^X on the array writes; aliasing correctness) | ~Zero (we don't run JIT blocks at steady state) | High — collapses JIT→JIT to ~5 insns |
| Investigate JIT-loop residency (the actual ceiling) | Med (diagnostic) | Low | High — this is the real bottleneck the T2 data exposes | — |

---

## 7. Recommendation

1. **Do now (cheap, high-signal): the BRK-on-destroy tripwire**, debug-gated. It directly
   hardens the chain-patch correctness model we already depend on and complements the recent
   `patched`-field work. Write `BRK #imm` over a block's first code word whenever
   `jit_bc_invalidate_pc` / range-invalidate nulls it, using the existing
   `jit_cache_begin_write`/`end_write`+flush bracket.
2. **Do now (cheap): instrument chain-site pool exhaustion.** Add a counter + one-time warning
   when `chain_site_pool_next` approaches `JIT_CHAIN_SITE_POOL`, so the "runtime chaining
   silently stopped" failure mode is observable. Consider tying pool reset to a softer trigger
   than full flush.
3. **Defer the inline-asm dispatcher.** The T2 data is unambiguous: at steady state we run
   ~zero JIT blocks, so optimizing the JIT→JIT transition has near-zero payoff *now*. It is the
   right tool *after* we understand and fix why execution leaves the JIT loop after ~15 s.
4. **Reframe the lead's real value:** Lead 7's biggest contribution is diagnostic — it forces
   the question "why isn't the JIT loop self-sustaining?" Pursue JIT-loop residency
   (the `pdi_jit_post` re-dispatch at ppc-cpu.cpp:786 and why the next PC is so often not a
   complete cached block) before any dispatcher micro-optimization. Only when residency is high
   does the inline entry-points array become the natural, high-payoff follow-up.
