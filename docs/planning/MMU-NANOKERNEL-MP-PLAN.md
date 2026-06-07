# Plan: Supervisor-level fidelity — MMU, nanokernel, and preemptive (MP) tasks

> **Status:** ⏸ Not started — exploratory / pick-up-later · **Created:** 2026-06-06 · **Updated:** 2026-06-06
> **Why this doc exists:** Specs the three things SheepShaver deliberately *stubs* instead of emulating — the PowerPC MMU, the Mac OS nanokernel exception/interrupt model, and Multiprocessing-Services preemptive tasks — what real emulation of each would take, what it would unlock, and the dependency order between them. Written so the reasoning survives if/when someone chases post-9.0.4 compatibility or MP-app support.
> _Markers: ✅ done · 🟡 in progress · ⏸ blocked/deferred · ☐ todo. Finished an item? Flip its marker, bump **Updated**, and add a `CHANGELOG.md` entry (see [CONTRIBUTING](../../CONTRIBUTING.md) → "Documentation Lifecycle")._

---

## TL;DR / verdict

These three are the **supervisor / system-software fidelity** frontier: the privileged layer
SheepShaver replaces with stubs so it can run the guest as a single, flat-addressed, cooperatively-
multitasked CPU. They are **deeply interrelated**, **large and exploratory**, and **low priority**
against the project's actual goals (Track A correctness, Track C Silicon Sheep). This doc specs them
so the option is mapped — not because it's scheduled.

Three findings that should govern any pickup:

1. **It's not "ignore," it's "actively patch."** SheepShaver patches the ROM nanokernel to *make*
   the stubs work — faking virtual=physical, skipping segment/BAT/SDR init, bypassing the PPC
   exception table, replacing `rfi`. "Fuller emulation" = *reversing* those patches and standing up
   the real machinery. (`rom_patches.cpp` `patch_nanokernel*`.)
2. **The cheap path to MP probably skips the MMU.** Full MMU *translation* is only strictly needed
   for guest virtual memory and memory protection (both low value here) and for DSI/ISI page faults
   — which **don't occur under identity mapping**. Preemptive MP tasks ride on the *decrementer +
   external-interrupt + exception* model, not on translation. So the highest-value, lowest-cost
   slice is **"restore real exception/interrupt/DEC handling while keeping identity mapping"** — MP
   fidelity without rebuilding the memory model. See [Dependency structure](#dependency-structure).
3. **This is gated behind, and partly answered by, New World ROM.** You can't even *test* 9.2.2
   without the parcels-ROM work in [`NEW-WORLD-ROM-SUPPORT-PLAN.md`](NEW-WORLD-ROM-SUPPORT-PLAN.md).
   That plan is the "first wall"; this doc is the possible "second wall" behind it. Run that plan's
   Phase 0/3 before committing here — it may show the second wall doesn't exist (ROM was the only
   blocker) or is shallower than feared.

**Effort/payoff at a glance:**

| Sub-plan | Effort | Payoff | Risk | Strictly needs MMU? |
|---|---|---|---|---|
| A — MMU / address translation | **Very high** | Low (VM + protection only) | **Very high** (guts the fast memory model) | — |
| B — Nanokernel & exception/interrupt fidelity | High | Medium (timing fidelity; enables C) | High (but self-contained-ish) | No (identity mapping OK) |
| C — MP / preemptive tasks | Medium *on top of B* | Medium (a slice of pro/media apps, maybe 9.2.x) | Medium | No |

---

## Dependency structure

The intuitive order ("MMU → nanokernel → MP") is **wrong** for this codebase. The real structure:

```
                 ┌─────────────────────────────────────────────┐
   MMU (A) ──────┤ only required for: guest VM, memory protection,│  ← low-value leaves
                 │ and DSI/ISI page faults (absent under V=P)     │
                 └───────────────────┬─────────────────────────┘
                                     │ (page-fault exceptions only)
   Exception/interrupt model (B) ────┴──────────────► MP preemptive tasks (C)
        real DEC + ext-int + sc + rfi + SRR0/1            (the nanokernel schedules
        (works fine under identity mapping)                them itself, once B is real)
```

- **C is mostly a *consequence* of B, not separate work.** SheepShaver would not implement an MP
  scheduler — the *guest nanokernel already contains one*. MP tasks "just work" once the emulated
  CPU faithfully takes decrementer + external interrupts and runs the real nanokernel dispatch
  instead of our patched shortcuts. C = "stop preventing it," not "build it."
- **B does not require A.** Real exceptions/interrupts/DEC work under identity mapping; only page
  faults (DSI/ISI) need translation, and those don't fire when virtual=physical.
- **A is the expensive, low-value leaf**, valuable mainly as a *correctness/compatibility* bet if
  something in 9.2.x genuinely depends on real translation (unknown until tried).

Practical consequence: **if a pickup ever happens, start at B (scoped to non-page-fault
exceptions), treat C as its tail, and treat A as a separate, much later, much riskier bet.**

---

## Current reality (grounded — file:line)

All three are stubbed through the **same mechanism**: a flat memory model plus ROM nanokernel
patches that assume it.

**Memory model — flat, no translation.**
- `DIRECT_ADDRESSING` with `NATMEM_OFFSET` (or `REAL_ADDRESSING`); every guest access is a constant
  offset + deref. `kpx_cpu/src/cpu/vm.hpp:208–267` (`vm_do_get_real_address` → `VMBaseDiff + a`);
  JIT base in `ppc-jit.cpp:393–403` (`JIT_MEM_BASE = 0` or `NATMEM_OFFSET`). No TLB, no page walk.
- The CPU state struct holds **no MMU state** — no `sr[]`, BAT, `SDR1`, `DSISR`, `DAR`
  (`kpx_cpu/src/cpu/ppc/ppc-registers.hpp:203–250`).

**Privileged instructions — faked in the SIGILL handler.** `sigill_handler()`
(`main_unix.cpp:2227`, installed :1260) catches privileged ops the patched ROM still executes:
`mtsr`/`mtsrin`/`tlbie` → no-op; `mfmsr` → `0xf072`; `mfspr SDR1` → `0xdead001f`; BAT/PMC/MMCR
`mtspr` → no-op (`main_unix.cpp:2263–2336`).

**Exceptions — bypassed.** `sc` is treated as **illegal** (`execute_syscall` →
`execute_illegal`, `ppc-execute.cpp:994–1002`); DSI/ISI/program/alignment/decrementer are **not
emulated**. The guest's PPC exception vectors do not run.

**Nanokernel — patched to assume all of the above.** `rom_patches.cpp`:
- `patch_nanokernel_boot()` (:620–759): skips SR/BAT/SDR init (:635), fakes the PVR read.
- `patch_nanokernel()` (:1187–1400+): disables virt→phys (identity `mr r31,r27`, :1192), patches
  exception-table activation to set `MODE_NATIVE`/`MODE_68K` instead (:1200, :1253), NOPs DEC
  modification (:1234), replaces `rfi` with `bctr` (:1289), injects the `XLM_IRQ_NEST` decrement
  (:1302), and fakes the page-table lookups (`FE0A` opcodes) as V=P for RAM and ROM (:1308–1395).

**Interrupts — a host-signal shortcut.** A 60.15 Hz host timer thread (`tick_func`,
`main_unix.cpp:1603`) sets `INTFLAG_VIA` and `TriggerInterrupt()` → `pthread_kill(emul_thread,
SIGUSR2)` (:1800). There is no real decrementer-driven interrupt.

**MP — absent.** One emulated CPU on one host thread (`emul_func` → single `jump_to_rom`,
`main_unix.cpp:1457`); the guest cooperatively multitasks within it. Host helper threads (tick,
nvram, redraw, audio, ether) are **not** guest task contexts; there is no guest context-switch in
the emulator. (No `MPLibrary`/Multiprocessing code anywhere — confirmed by absence.)

---

## Sub-plan A — MMU / address translation

**Effort: Very high · Payoff: Low · Risk: Very high**

**What it is:** emulate real PPC 32-bit address translation — segment registers → BAT → hashed page
table → TLB — instead of the flat identity map.

**What it would take:**
1. Add MMU state to the CPU struct: `sr[16]`, IBAT/DBAT (4 pairs), `SDR1`, `DSISR`, `DAR`.
2. Implement the translation algorithm (BAT match → SR → PTE search) + a software TLB.
3. Rework the memory path — the hard part. Two options, both bad here:
   - **(a) Software TLB in the JIT:** a probe/translate sequence (or helper call) on *every* load/
     store. Destroys the single-instruction `LDR/STR [base, gaddr]` that makes the JIT fast and
     small — even "performance aside," it's a large codegen rewrite.
   - **(b) Host-MMU shadowing:** mirror guest mappings via `mmap`/`mprotect`, catch host SIGSEGV,
     walk the guest page table, patch the host mapping. Fights Apple Silicon's 16 KB host page vs
     4 KB guest page, and the JIT's existing `MAP_JIT`/W^X dance.
4. **Un-patch the nanokernel** — stop faking V=P (reverse `rom_patches.cpp:1192/1308–1395`) and let
   the real virt→phys / page-table code run against the emulated MMU. Feeds DSI/ISI into sub-plan B.

**What it unlocks:** guest Virtual Memory (paging to disk — near-worthless on a multi-GB host);
memory protection (limited — classic Mac OS apps share one address space, so no real crash
isolation); and *possibly* a 9.2.x dependency on real translation (unknown).

**Decision gate:** do **not** start A unless a concrete, wanted target is proven (by sub-plan B's
probe or a 9.2.2 boot attempt) to need real translation. It is the worst effort/payoff in this doc.

---

## Sub-plan B — Nanokernel & exception/interrupt fidelity

**Effort: High · Payoff: Medium · Risk: High (but more self-contained)**

**What it is:** let the real nanokernel run its own exception/interrupt/timing dispatch instead of
our patched shortcuts — *without* necessarily adding MMU translation (keep identity mapping).

**What it would take:**
1. Implement the PPC exception model: vectors (0x300 DSI, 0x400 ISI, 0x500 ext-int, 0x600 align,
   0x700 program, 0x900 dec, 0xC00 syscall, …), `SRR0`/`SRR1` save, MSR/`MSR[PR]` semantics, real
   `rfi`. (Under identity mapping, DSI/ISI simply won't fire — so they can be stubbed last.)
2. Make `sc` a real syscall exception instead of `execute_illegal` (`ppc-execute.cpp:997`).
3. Real **decrementer** exception for timing, replacing the `SIGUSR2`/`INTFLAG_VIA` host-signal
   shortcut (`main_unix.cpp:1603/1800`) — this is the keystone for preemption (sub-plan C).
4. Restore external-interrupt delivery through the real nanokernel path.
5. **Reverse the bypass patches** in `patch_nanokernel()` (exception-table activation :1200/:1253,
   `rfi`→`bctr` :1289, DEC NOP :1234, IRQ_NEST inject :1302) so the nanokernel's own dispatch runs.

**What it unlocks:** faithful interrupt/timing behavior; supervisor-mode correctness; and the
substrate MP tasks need (sub-plan C). Possibly clears a class of 9.1+/9.2.x boot issues.

**Why it's the right entry point:** self-contained relative to A (no memory-model rewrite), and it's
where the actual user-visible payoff (MP, timing fidelity) originates.

**Risk:** the nanokernel patches exist because the real paths were hard to run; reversing them is
boot-bringup-grade work with high chance of deep, hard-to-debug regressions. Gate every step behind
`SS_JIT_VERIFY` + the e2e boot harness.

---

## Sub-plan C — MP / preemptive (Multiprocessing Services) tasks

**Effort: Medium *on top of B* · Payoff: Medium · Risk: Medium**

**What it is:** preemptively-scheduled guest tasks (`MPLibrary` / Multiprocessing Services) actually
running preemptively, instead of being absent/serialized.

**Key insight — this is mostly *not* new emulator code.** The preemptive scheduler lives in the
*guest nanokernel*. Once sub-plan B gives the emulated CPU a real decrementer + external-interrupt +
exception model, the nanokernel schedules and context-switches its own MP tasks within the single
emulated CPU — SheepShaver doesn't add host threads or its own scheduler. **C ≈ "finish B, then stop
preventing the nanokernel from preempting."**

**What it would take beyond B:**
1. Verify the nanokernel's task-switch path (save/restore of task register context) executes
   correctly under the real exception model — it's guest code, but our patches currently short-
   circuit the paths it uses.
2. Make sure MP-task memory (task stacks, the MP work queues) behaves under identity mapping (it
   should — MP doesn't require protection).
3. Validate against a real MP-Services consumer (see probe below).

**What it unlocks:** the slice of pro/media apps and OS components that use preemptive MP tasks; a
more faithful 9.x. **Payoff is a long tail** — most software people run on 8.6–9.0.4 is cooperative
and works today.

> **Multi-core angle.** Once MP tasks exist, they're the *only* guest-sanctioned unit that could be
> placed on **separate host cores** (the cooperative Blue world cannot be split). That's the gated
> "true guest SMP" idea — Tier 3 of [`MULTICORE-OFFLOAD-PLAN.md`](MULTICORE-OFFLOAD-PLAN.md), which
> needs this sub-plan *plus* a cross-core memory-coherence project (real `lwarx`/`stwcx.` + PPC→ARM64
> barriers). The cheaper multi-core wins (background compilation, async devices) are in that doc's
> Tiers 0–2 and need none of this.

---

## Sequencing & relationship to New World ROM

```
NEW-WORLD-ROM-SUPPORT-PLAN  →  boot 9.2.2  →  diagnose what breaks  →  (maybe) this doc
  (parcels ROM = "first wall")    (Phase 3)     is it MMU? nanokernel?    B first, then C;
                                                 MP? or nothing extra?    A only if forced
```

The New World ROM plan is the **prerequisite probe**: until 9.2.2 boots, "does it need real MMU/
nanokernel?" is untestable. Its Phase 3 ("boot & downstream debug") is exactly where a "second wall"
would appear. Possible outcomes: (i) ROM was the only wall → this doc is moot; (ii) shallow
nanokernel drift → small slice of sub-plan B; (iii) genuine supervisor dependence → this doc in
earnest. Bet on (i)/(ii) until proven otherwise.

---

## Recommended first step (cheap, decisive — do before any of A/B/C)

Two probes, neither requiring the full work, both runnable on today's 9.0.4:

1. **Stub-pressure trace.** Env-gate counters at the stub sites (`sigill_handler` privileged-op
   cases; the `patch_nanokernel` bypass points; `sc`→illegal) and log how often a *normal* 9.0.4
   boot + app run actually hits them. If the faked paths are hit only at boot, the stubs are
   boot-time-only and real emulation is a smaller behavioral delta than feared; if hit constantly,
   it's a deep change. (Mirror the New World plan's env-gated Phase-0 tracer style — zero behavior
   change when unset.)
2. **MP demand probe.** Launch a known MP-Services consumer under current 9.0.4 and record the
   failure mode (cooperative fallback? hang? crash?). That tells you whether C has real demand and
   *what specifically* breaks — turning "MP support" from abstract into a concrete repro.

Both are hours, not weeks, and either can kill or justify the whole area before a line of A/B/C is
written.

> **⚠️ aarch64 implementation correction (2026-06-07).** The stub-pressure trace above describes the
> *x86/PPC-host* model where guest privileged ops fault to the host **`sigill_handler`**
> (`main_unix.cpp:2398`, which fakes mfmsr/mtsr/tlbie/mfspr/mtspr). **On our aarch64 JIT that handler is
> NOT the stub site** — a guest PPC privileged op is not a host-illegal instruction, so it never
> SIGILLs. The real fake sites on aarch64 are: (1) the **JIT** `mfspr`/`mtspr` cases
> (`ppc-jit.cpp` case 339 / case 467) + `mfmsr` (case 83 → `0xf072`); (2) the **interpreter**
> `execute_mfspr`/`execute_mtspr` (`ppc-execute.cpp:1185/1211`, fake `SDR1=0xdead001f` etc.) +
> `execute_illegal`; and `sc` falls back to interp (`ppc-jit.cpp:3693`). So the env-gated counters go
> THERE (keyed by SPR number / primop+xo), with a boot-vs-steady split via a `g_boot_done` flag set at
> the first `OP_IDLE_TIME` (reuse the `[BOOT]` idle marker). Pattern: mirror `g_compiled_mix`
> (`ppc-jit.cpp`) / the `SS_LOG_PPCF` env gate — zero cost when off, dump on clean exit.

## MMU feasibility — RECONCILED verdict (2026-06-07, generative + adversarial agents)

Two opposed agents revisited "can we *really* not emulate the MMU?" Full memos:
[`sheepshaver-research/MMU-WITHOUT-GUTTING-FLATMEM.md`](sheepshaver-research/MMU-WITHOUT-GUTTING-FLATMEM.md)
(generative) and [`MMU-DEFERRAL-REDTEAM.md`](MMU-DEFERRAL-REDTEAM.md) (adversarial). Reconciled:

- **CAN we emulate the MMU without gutting the flat model? Conditionally YES.** The viable design is
  **shadow-arena / "Dynamic BAT"** (Dolphin runs this on Apple Silicon): rebuild the host NATMEM arena
  mapping at the *rare* guest map-change (`mtspr` BAT/SDR1/SR), while the per-access codegen stays
  bit-identical (`LDR/STR [RMEMBASE, UXTW(ea)]`, zero translation cost on the hot path). This is NOT the
  per-access **softmmu** the original verdict feared (that remains the anti-pattern). The c4 `vm_remap`
  failure does **not** disqualify it — that was an *execute*-path blocker; a shadow arena is **data-only
  (RW)** and under EMULATED_PPC the JIT never branches into guest RAM.
- **The single hinge = host 16 KB vs PPC 4 KB page granularity** (verified `getconf PAGESIZE`=16384).
  Shadow-arena works only if Mac OS 9.x maps memory at COARSE (BAT/≥16 KB) granularity; if it does
  fine 4 KB runtime paging in a hot path, you're forced into per-access softmmu (catastrophic). Both
  agents land here. Current evidence (skipped BAT/SDR init, zero supervisor ops) *suggests* coarse, but
  that's **inferred**.
- **SHOULD we build it now? NO — "never, unless a specific trigger appears."** Neither agent found ANY
  primary evidence that wanted 9.1/9.2 software needs non-identity translation in a hot path; the
  "no MMU ⇒ no 9.1" line is folklore ("asserted everywhere, bisected nowhere"); QEMU/DingusPPC "proofs"
  are confounds (they also differ on the ROM-patch axis). Hypervisor.framework is a confirmed dead end
  (host-ISA only — can't host a software PPC guest). The trigger that would flip this: a post-ROM-
  acceptance **9.2.2 boot that bisects to a translation-dependent DSI/ISI in a hot path** — untestable
  until a NewWorld ROM loads, which is *why* ROM-first ordering is unconditionally correct.
- **⚠️ Correction to the result below:** do NOT cite the `SS_STUB_TRACE` zero as positive proof the MMU
  is unneeded — it is zero *partly by construction* (ROM-patching strips supervisor ops pre-runtime).
  It proves "no runtime supervisor hot path on 9.0.4 as-patched," not "9.x will never need translation."

**Cheap decisive next experiment (both agents converged on it; runs on TODAY's 9.0.4, no NewWorld ROM):**
run `SS_STUB_TRACE=1` with the guest's **Virtual Memory ENABLED** (Memory control panel → restart) — the
one runtime-translation trigger that exists on 9.0.4. **Zero** stub hits with VM on ⇒ coarse mapping ⇒
shadow-arena stays 16 KB-safe and viable if ever needed. **Non-zero** ⇒ fine paging exists ⇒ the 16 KB
hinge bites and softmmu risk is real. Either way it converts the central *inferred* assumption into data.

## Stub-pressure trace — RESULT (2026-06-07): zero runtime supervisor pressure on 9.0.4

Implemented `SS_STUB_TRACE` (env-gated runtime counters in `ppc-execute.cpp`
`execute_mfspr`/`execute_mtspr`/`execute_illegal`, boot-vs-steady split at first idle, dump on
clean shutdown). Ran a full **Mac OS 9.0.4 boot + 25 s idle**:

```
[STUB-TRACE] supervisor-stub pressure (boot | steady)
  mfspr faked reads:                       (none)
  mtspr dropped writes (BAT/SDR1/SPRG):    (none)
  illegal primop-31 (mtmsr/mtsr/tlbie/rfi):(none)
  TOTAL: boot=0  steady=0  => boot-time only (2nd wall SHALLOW)
```

**Reading:** the guest executes **zero** runtime supervisor ops we fake — neither at boot nor steady.
SheepShaver's nanokernel **ROM-patches** (skip SR/BAT/SDR init, replace `rfi`, EMUL_OP the supervisor
routines — `patch_nanokernel*`) handle the privileged layer **at patch time**, so nothing reaches the
runtime fake handlers. The runtime supervisor surface on 9.0.4 is **empty**, not merely small.

**Honest limitation (don't over-read):** the probe measures *runtime hits*, but the supervisor ops are
removed by ROM-patching *before* execution — so `0` is partly *by construction*. It proves there's no
runtime MMU/supervisor hot path to worry about on 9.0.4 (real translation/exception machinery would only
matter if something *executed* and faulted — nothing does). It does **not** measure how much ROM-patch
*coverage* a NewWorld 9.1/9.2 ROM would need — that's the `NEW-WORLD-ROM-SUPPORT-PLAN.md` domain, and is
unmeasurable until a parcels ROM loads.

**Net for the verdict:** reinforces "ROM-first, MMU-deferred." The hard part of broader-OS support is
**ROM-patch parity** (getting the supervisor layer patched out for the new ROM), not standing up a real
runtime MMU — there is no runtime supervisor pressure to replace. The MMU "second wall" has **no runtime
footprint on the OS we can run today**; re-confirm on 9.1/9.2 once a NewWorld ROM boots. (Probe:
`SS_STUB_TRACE=1`; commit `127d54d8`.)

## Decision (2026-06-07): "both — cheap probe + EV work" (user)

Per `COMPATIBILITY-PAYOFF-DINGUSPPC-REVISIT.md`, the chosen Phase-3 plan is **(a)** run the cheap probes
to keep the frontier mapped, while **(b)** making the CopyBits/idle/video EV levers the headline. Concrete
next-actions, both measurement-first (no blind implementation):
1. **Stub-pressure trace** (frontier map; runs on today's 9.0.4) — instrument the aarch64 stub sites above,
   boot 9.0.4 + run an app, read the boot-vs-steady counts. Decides if the MMU/nanokernel "second wall"
   is real *before* any NewWorld-ROM investment. ← do first (cheapest, decisive).
2. **CopyBits call/rect histogram** (EV go/no-go for the headline blitter) — needs a `_CopyBits` trap
   (0xA8EC) intercept via the NativeOp/EMUL_OP mechanism that counts calls + logs rect/byte sizes (the
   probe is the first increment of the eventual HLE). Run on boot + an app + a game. Gate the blitter
   build on the result (CROSS-EMULATOR-IDEATION #1).
3. (Frontier, opportunistic) the existing NewWorld 9.0.4 PatchROM work (D3 Phase 2) stays the lower-EV
   exploratory track; pair any further attempt with the stub-pressure trace.

---

## References

- [`NEW-WORLD-ROM-SUPPORT-PLAN.md`](NEW-WORLD-ROM-SUPPORT-PLAN.md) — the prerequisite "first wall".
- `SheepShaver/src/rom_patches.cpp` (`patch_nanokernel`, `patch_nanokernel_boot`) — the stub patches
  any real emulation must reverse.
- `SheepShaver/src/Unix/main_unix.cpp` (`sigill_handler` :2227; `tick_func`/`TriggerInterrupt`
  :1603/:1800; `emul_func` :1457) — privileged-op fakes + interrupt shortcut + single-CPU loop.
- `SheepShaver/src/kpx_cpu/src/cpu/ppc/ppc-execute.cpp:994` (`sc` → illegal),
  `ppc-registers.hpp:203` (no MMU state), `vm.hpp:208` (flat addressing).
- `SheepShaver/docs/DIAGNOSTICS.md` — the heartbeat/trace tooling to use for any bring-up.
- `CLAUDE.md` "lldb Workflow" — guest-address arithmetic for inspecting nanokernel code.
