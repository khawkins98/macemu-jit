# Cross-emulator ideation — techniques from Dolphin / RPCS3 / Cemu / QEMU / Rosetta

> **Status:** 🟡 Open — idea bank · **Created:** 2026-06-06 · **Updated:** 2026-06-06
> **Why this doc exists:** Captures a structured lateral-ideation pass (6 emulator-specialist personas → combine → rank → deep-dive) on what SheepShaver could borrow from other emulators, **reality-checked against the actual source** so we don't re-chase work that's already done or ruled out.
> _Markers: ✅ done · 🟡 in progress · ⏸ blocked/deferred · ☐ todo. Finished an item? Flip its marker, bump **Updated**, and add a `CHANGELOG.md` entry (see [CONTRIBUTING](../../../CONTRIBUTING.md) → "Documentation Lifecycle")._

## How this was produced (and why to trust the verdicts)

A multi-agent workflow: 6 specialist personas brainstormed in parallel (deliberately picking
PowerPC emulators — **Dolphin/Gekko, RPCS3/Cell-PPU, Cemu/Espresso are all PowerPC**, so their
dynarec/caching/HLE techniques transfer to SheepShaver's guest ISA directly; plus QEMU,
Rosetta-2/FEX on the same ARM64 host, and a wildcard) → a *combine* pass for second-order ideas →
a *rank* pass → per-pick *deep-dives that verified each claim against the code*. 88 ideas total.

**The deep-dives' biggest value was the reality-check.** They found the codebase is further along
than the brainstorm assumed (two "top picks" are already shipped), that several **stale code
comments mislead** (the same doc-rot we keep fixing), and one **latent bug**. Those byproduct
findings are at the bottom; the verdicts below already bake in the corrections.

**External corroboration (2026-06-06).** A separate external analysis (web-sourced) independently
converged on the host-side-parallelism-not-guest-SMP thesis and the same device candidates, which is
a useful validation. It added three items now folded into [`MULTICORE-OFFLOAD-PLAN.md`](../MULTICORE-OFFLOAD-PLAN.md):
**hardware cursor** (grid #11 above), the **"synthetic devices"** framing for the HLE/Graphics-Packs
layer, and **deterministic event queues** for interrupt/device-completion (which also serves
record/replay). A code audit also **debunked its "pin the JIT to one core" claim** for this fork — our
runtime is cleanly single-writer-guest; see the MULTICORE plan's "Concurrency baseline." (Its
"no-MMU ⇒ no 9.1+" line is the common-wisdom version; our [`NEW-WORLD-ROM-SUPPORT-PLAN.md`](../NEW-WORLD-ROM-SUPPORT-PLAN.md)
found the *first* blocker is parcels-ROM rejection, with the MMU only a possible second wall.)

---

## The grid — everything worth doing (or worth knowing is done)

Effort/Payoff: Low / Med / High. "Blocked by" = what must happen first. Verdict drives the order.

| # | Idea (origin) | Effort | Payoff | Blocked by | Connects | Verdict |
|---|---|---|---|---|---|---|
| 1 | **`CopyBits` HLE** — native/Metal blitter for the hottest QuickDraw raster op (Cemu HLE-the-OS) | Med | **High** (games/media) | cheap call-histogram probe | optimization → desktop (Metal) | ✅ **DO — nominated #1** |
| 2 | **Idle-skipping / busy-wait detection** — yield host core in cooperative idle/VBL spins (Dolphin idle-skip) | Med | **High** (battery/thermal/"good citizen") | none | desktop-integration | ✅ **DO — nominated #2** |
| 3 | **Dual-W^X mapping (R8/C4)** — RW/RX aliases, drop per-thread toggle (Rosetta/FEX) | ~1 wk | Med (+unblocks MT) | none (spike proved ~27%) | multicore / optimization | ✅ **DO anyway** (strict win + substrate) |
| 4 | **Fastmem SIGBUS backpatching** — fault handler over NATMEM for IO/framebuffer (Dolphin fastmem) | Med-High | Med (correctness; MMU prereq) | none | mmu-nanokernel-mp | 🟡 foundational — do if pursuing MMU/IO correctness |
| 5 | **Constant propagation / immediate folding** — fold lis+ori address chains (Dolphin regcache) | Med | Med | none | optimization | 🟡 backlog |
| 6 | **Carry via native NZCV (lazy XER[CA])** — ADCS/SBCS chains, materialize at boundary (Dolphin flags) | Med | Med (+kills a bug class) | pairs w/ lazy-CR0 (§0g) | optimization | 🟡 backlog |
| 7 | **Native `lwarx`/`stwcx` (inline reservation flags, NOT LDXR/STXR)** (FEX/QEMU LL-SC) | Low | Low-Med (conditional) | **frequency probe** | multicore | 🟡 probe-then-maybe |
| 8 | **Background compilation (R9/C5)** — E-core worker compiles while P-core runs (Cemu/Dolphin tiered) | Med | Low (boot/launch only) | **C1 residency gate** + R8 + measurement | multicore | ⏸ gated — not now |
| 9 | **Bucket the SMC-invalidation scans** — range-key the two O(pool) loops | ~1 day | Low | profiled icbi-storm | optimization | ⏸ only if measured |
| 10 | **Persistent JIT cache → VM snapshot/resume** (RPCS3 module cache / Rosetta .aot) | ~1 wk | Low as boot-speed / **High as snapshot** | re-scope; LR-pred bug (#L) | desktop-integration | 🔁 defer + re-file under Silicon Sheep |
| 11 | **Hardware cursor** — host-overlay cursor, decoupled from guest 60 Hz redraw (external review) | Low-Med | Med-High (responsiveness) | none | desktop-integration | 🟡 do — strong "feels native" win (see MULTICORE Tier 2) |
| — | **Block chaining + page-granular SMC invalidation** (Dolphin/QEMU link/unlink) | — | — | — | optimization | ✅ **ALREADY DONE** (default-on, boots; comment stale) |
| — | **AltiVec byte-multiply ev_mixed fix** | — | — | — | optimization | ✅ **ALREADY DONE**; leftover = `vpkuwum` + signed/halfword test vectors (A1/A2) |

### Nominated first candidates
*(Promoted to first-class tracked items — **ROADMAP B5** (`CopyBits` HLE), **B6** (idle-skipping).)*
1. **`CopyBits` HLE** — biggest net-new throughput win for the workloads that justify throughput at all (games/media), no SMC risk (unlike `BlockMove`), and the natural on-ramp to Metal-accelerated blits. **Probe first:** a half-day call-frequency + rect-size histogram decides go/no-go.
2. **Idle-skipping** — the single best "behaves like a real macOS app" win: a backgrounded Silicon Sheep VM must not peg a P-core at 100%. Unblocked, and it directly serves the desktop ambition.

(Third, do-regardless: **Dual-W^X mapping** — already spiked, strict win, and the substrate every future multithreading idea needs.)

---

## Near-term keepers — detail

### 1. `CopyBits` HLE *(nominated #1)*
Today `CopyBits`/blits run through the *worst layer in the stack* — the PPC-JIT emulating the ROM's
68K DR-emulator. HLE replaces that with one native ARM64/NEON blit via the **existing NativeOp
mechanism** (`main.cpp` `NATIVE_*`, same path the ethernet driver + video accel already use).
**Crucial split** (from the deep-dive): `CopyBits` (pixels) is safe; `BlockMove`-for-code is a
**trap** — a native memcpy bypasses the JIT's only SMC flush path (icbi/isync →
`ppc_jit_aarch64_invalidate_range`), so it would run stale translations over relocated CODE
resources unless the handler explicitly calls `invalidate_range(dst,len)`. `BlockMoveData`
(data-only) is safe but probably threshold-gated to near-nothing. **First step:** histogram
call-frequency + rect/byte sizes on boot + an app + a game; expect it to redirect the whole effort
to `CopyBits`. Effort Med (a faithful clipped/scaled blitter is the cost). Payoff High.

### 2. Idle-skipping / busy-wait detection *(nominated #2)*
The cooperative guest spends huge time in ROM spin-waits (the `0x5031040c` VBL spin,
`WaitNextEvent` null-event poll). Tag those idle-loop PCs; when a chained block re-enters one with
no pending spcflags, issue a short `WFE`/nanosleep until the next 60 Hz tick instead of spinning at
full ARM64 speed. Reuses the existing spcflags poll points. **First step:** identify the 2–3 hot
idle PCs from a heartbeat/HOT-PC sample, gate one behind an env flag, measure host-CPU drop while
the VM sits at the Finder. Effort Med, Payoff High (battery/thermal/citizenship).

### 3. Dual-W^X mapping (R8 / C4) — *do regardless*
Already spiked (`spikes/wx-dual-mapping/`): `vm_remap`-from-MAP_JIT fails (`KERN_PROTECTION_FAILURE`);
the working recipe is `mmap(RW,ANON)` → `vm_remap` to a 2nd VA → `mprotect(RX)`, **~27% faster
per write-batch**. Replaces the per-thread `pthread_jit_write_protect_np` toggle with process-wide
RW/RX aliases, deleting a class of W^X bugs and unblocking any future writer thread. **Background
compile (R9) sits on top but is C1-gated — ship Layer A alone.** (See [`MULTICORE-OFFLOAD-PLAN.md`](../MULTICORE-OFFLOAD-PLAN.md) Tier 1.)

### 4–7 (backlog/probe)
**Fastmem SIGBUS backpatching** — a host fault handler over the NATMEM window that decodes the
trapping ARM64 LDR/STR and routes to device/IO emulation; the prerequisite to ever un-stub the MMU
(→ [`MMU-NANOKERNEL-MP-PLAN.md`](../MMU-NANOKERNEL-MP-PLAN.md) sub-plan A). **Constant propagation**
and **carry-via-NZCV** are standard JIT levers (the latter also retires the carry-codegen bug class
that cost sessions 5–7); both fold into OPTIMIZATION-PLAN. **`lwarx`/`stwcx`** — use inline
reservation-flag bookkeeping (`reserve_valid`/`reserve_addr` already exist in the regs struct under
`KPX_MAX_CPUS==1`), **not** ARM64 LDXR/STXR (wrong tool for a single cooperative CPU; would clear
spuriously and desync interp/JIT). Frequency-probe first; payoff is small unless lock-heavy.

---

## Already done — do not re-investigate
- **Block chaining** is `#define JIT_BLOCK_CHAINING 1`, default-on, boots Mac OS 8.6→Finder. SMC
  invalidation is already range-scoped with chain link/unlink (`ppc_jit_aarch64_invalidate_range`).
  The only residual is bucketing two O(pool) scans (grid #9, measurement-gated). *The source comment
  at `ppc-jit.cpp:~113` still says "default OFF" — stale (see byproduct findings).*
- **AltiVec byte multiplies** (`vmulo/eub`, signed/even/odd) are wired (`emit_vmul_byte`, dispatch
  cases 8/520/264/776) and promoted. Real leftover: `vpkuwum` (case 78, ignores vA) + test vectors
  for the prospective signed/halfword ops — tracked in ROADMAP A1/A2. **Reject the "global REV32 at
  load/store" approach** — it's the per-op-vs-global decision already ruled out; +2 NEON ops on every
  AltiVec op to fix 2–4 ops.

## Defer / re-scope
- **Persistent ROM JIT cache** (grid #10): marginal as a boot-speed feature on a fast host, and
  verbatim-mmap is infeasible (codegen is cache-state- and boot-order-dependent). **Re-file under
  Silicon Sheep desktop-integration as the JIT-state layer of VM snapshot/resume** — the
  ROM-immutability property makes the ROM portion of a snapshot trivially shareable. Depends on
  fixing the LR-prediction latent bug (#L below).

## Strategic anchors / longshots (record, don't build)
- **Apple hardware TSO mode** — the Tier-3 guest-SMP ordering keystone (makes PPC's stronger memory
  model near-free); Apple-Silicon-unique; private entitlement; only matters once SMP is on the table
  (→ MMU-NANOKERNEL-MP Tier-3 / MULTICORE Tier-3).
- **Deterministic record/replay = whole-VM time-travel** — single cooperative CPU makes exact replay
  tractable; collapses *three* roadmap items into one: the verify oracle, suspend/resume, and rewind.
- **Static AOT recompilation of the ROM → native `.dylib`** — best-possible code for the fixed ROM,
  and the same front-end parses New World CHRP ROMs the runtime scanner can't (→ D3 / New World).
- **GGPO-style rollback on idle E-cores** — speculative look-ahead to hide input latency (the one
  thing a fast host doesn't auto-fix).
- **Coherence-lite: classic apps as borderless native NSWindows** — the Silicon Sheep north star.

## Cross-cutting themes (the real gold)
1. **Exploit that the guest is finite, old, and fully understood** — content-addressable ROM,
   documented/unchanging Toolbox (HLE leaf routines), whole state = one flat NATMEM region + reg
   struct (trivial snapshots).
2. **Same-host Apple-Silicon leverage** — dual-W^X aliasing, hardware TSO, P-core-guest/E-core-
   housekeeping QoS, Metal dirty-rect present.
3. **Single cooperative CPU + flat memory is a gift for tooling** — record/replay, snapshots, atomics
   without real concurrency, one quiesce-to-safe-point service for lock-free background work.
4. **One RE'd hook, many features** — a Process-Manager *app-identity* event (launch/creator-code) is
   independently wanted by per-app trust, per-app cache pre-warm, and per-app coherence. Build the
   event bus once; a **"Graphics-Packs"-style community manifest** then carries correctness patches
   *and* desktop integration **as data**.
5. **Attack the worst layer** — PPC-JIT emulating the ROM's 68K DR-emulator (the `CopyBits`/
   `BlockMove` HLE and any 68K→ARM64 work all target this).
6. **Correctness-first under a fast host** — a differential interp-vs-JIT fuzzer over fragile families
   (carry chains, AltiVec siblings, rlwinm masks) + replay-as-oracle attacks the documented
   "green harness, broken app" failure mode.

## Byproduct findings (surfaced by the deep-dives — see handoff)
- **`#C1` Stale comment — ✅ FIXED (2026-06-06, `72c5e525`).** The chaining comment said "default
  OFF / the define stays 0" — but it's `1`, on, and boots. Refreshed in the same code-review pass as #L.
- **`#C2` Stale comment — ✅ FIXED (2026-06-06, `72c5e525`).** The `KNOWN AltiVec BUG (PARKED)` header
  listed "STILL BROKEN: vmuloub/vmuleub" — they're wired/fixed (cases 8/520/264/776). Refreshed too.
- **`#C3` Stale plan note** OPTIMIZATION-PLAN **R3**: claimed "no W^X toggling exists" — wrong;
  `MAP_JIT` + `pthread_jit_write_protect_np` is live. **✅ Fixed 2026-06-06** (this pass).
- **`#L` Latent bug — ✅ FIXED (2026-06-06, `72c5e525`).** The LR-prediction path emitted a raw
  `B chain_code` with **no `record_chain_site`** — unlike the registered chain sites (956/965), so
  `ppc_jit_aarch64_invalidate_range` couldn't revert it → stale translation after SMC/icbi (the
  icbi-hang class). The naive "register it" fix is unsafe: the B sits mid-hit-path, and the revert
  writes a lone LDP epilogue word → double epilogue / stack corruption at that position. **Fix:**
  only direct-chain LR predictions to NEVER-INVALIDATED (ROM) targets; RAM returns fall back to the
  standard dispatcher (correct, just unoptimized). Verified: ISO boot to Finder clean, test-jit
  302/302. **Perf follow-up (tracked, OPTIMIZATION-PLAN):** restore RAM LR-prediction via a
  registered chain-site carrying a *revert-to-miss-path* word (so invalidation re-enters the
  dispatcher instead of an LDP) — that also re-unblocks the persistent-JIT-cache idea (#10).

## References
- Existing plans this feeds: [`OPTIMIZATION-PLAN.md`](../OPTIMIZATION-PLAN.md) (R8/R9 + HLE),
  [`MULTICORE-OFFLOAD-PLAN.md`](../MULTICORE-OFFLOAD-PLAN.md),
  [`MMU-NANOKERNEL-MP-PLAN.md`](../MMU-NANOKERNEL-MP-PLAN.md),
  [`NEW-WORLD-ROM-SUPPORT-PLAN.md`](../NEW-WORLD-ROM-SUPPORT-PLAN.md),
  [`DESKTOP_INTEGRATION_PLAN.md`](../DESKTOP_INTEGRATION_PLAN.md),
  [`BASILISKII-CROSS-POLLINATION.md`](BASILISKII-CROSS-POLLINATION.md) (the fast-hardware framing).
- C1/C4/C5 research: `sheepshaver-research/research/{c1-residency-root-cause,c4-wx-dual-mapping-spike,c5-background-compilation-feasibility}.md`.
