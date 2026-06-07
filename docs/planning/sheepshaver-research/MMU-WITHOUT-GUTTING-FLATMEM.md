# Could SheepShaver emulate the PPC MMU on Apple Silicon WITHOUT gutting the flat memory model?

> **Status:** 📖 Research / decision-input · **Created:** 2026-06-07
> **Why this doc exists:** The standing verdict ([`MMU-NANOKERNEL-MP-PLAN.md`](../MMU-NANOKERNEL-MP-PLAN.md) sub-plan A) defers a real PPC MMU as "very high effort, low payoff" because naive per-access translation would destroy the DIRECT_ADDRESSING / NATMEM flat model. This doc enumerates every *lateral* path that could change that calculus, ranked by feasibility, Apple-Silicon-specific. It does **not** revise the verdict's bottom line for today's target — it sharpens *which* technique to reach for if/when a real target ever demands translation.
> _Companion to the standing verdict, lead-6 (fastmem), and the c4 W^X spike — builds on them, does not repeat them._

---

## Headline answer

**Qualified YES — and largely moot for the OS we can run today.**

There is a path to MMU support that does **not** gut the flat model. The single most promising
candidate is **shadow-arena translation, Dolphin "Dynamic BAT" style: rebuild the host NATMEM
arena at guest *map-change* time, and keep the access-time codegen (bare `LDR/STR [RMEMBASE,
UXTW(ea)]`) bit-for-bit identical.** Translation cost lands on the *rare* `mtspr` to BAT/SDR1/SR —
which our stub trace counts at **zero** on 9.0.4 — never on the hot load/store. That is exactly why
it preserves the fast path.

But the deeper finding is that **the flat model isn't actually threatened, because nothing on
today's target demands translation.** DIRECT_ADDRESSING is already a *degenerate identity MMU*;
fastmem already gives us Dolphin's fast path for free (lead-6 closed as "already satisfied"); and the
2026-06-07 stub-pressure trace shows a 9.0.4 boot+idle executes **zero** runtime supervisor ops and
runs **fully virtual=physical**. The "destroy the fast path" fear in sub-plan A is conditioned on the
guest *enabling* translation. It never does. So the cost-model question for fastmem ("how rare must
faults be?") has a degenerate answer here: **faults are never, because no non-identity mapping is ever
requested.**

The MMU question only goes live in two concrete scenarios:

- **(a)** you deliberately un-patch the nanokernel (sub-plan B) so the real virt→phys / page-table
  code runs; or
- **(b)** you boot an OS, or enable a guest feature, that *programs a non-identity mapping at
  runtime.* The one such feature that exists **on 9.0.4 itself** is the **Memory control panel's
  Virtual Memory** (paging to disk) — see the recommended probe.

Until (a) or (b), the entire area is mapped-but-not-scheduled, and the right move is to **name the
winning technique and stop** — which is what this doc does.

---

## The two discriminators that decide the whole ranking

Everything below hinges on two facts. Both are marked **inferred** (strongly, from the stub trace +
codegen) rather than verified by a translation-on run — and both have a cheap probe that would settle
them. **Recommend running the probes; do not assume.**

### Discriminator A — does Mac OS 9 map *coarse* (BAT/V=P, ≥256 MB blocks) or *fine* (4 KB pages) at runtime?

This single fact cuts both ways and is the linchpin for the **16 KB host page vs 4 KB guest page**
mismatch:

- **If coarse** (which the stub trace strongly implies — the nanokernel patch *skips* SR/BAT/SDR
  init entirely, and zero `mtspr SDR1/BAT` writes are seen): the 16 KB ≥ 4 KB mismatch **rarely
  bites**, because a shadow scheme mirrors large, 16 KB-aligned BAT blocks, not scattered 4 KB pages
  with per-page permissions. And real MMU is genuinely unnecessary.
- **If fine 4 KB paging at runtime**: the mismatch is **fatal** for any host-page-granularity shadow
  — one 16 KB host page would have to hold four guest 4 KB pages that may carry *different*
  permissions, which `mprotect` cannot express. That forces the QEMU softmmu route (inline TLB probe)
  → which *does* gut the fast path. This is the standing verdict's nightmare, and it would be correct.

**Verdict input:** the evidence (skipped BAT/SDR init, zero runtime supervisor ops) points to
**coarse / V=P**, which keeps the shadow approach 16 KB-safe and keeps MMU moot. *Inferred, not
verified.*

> **Recommended probe (don't run blind — recommend in the plan):** re-run `SS_STUB_TRACE=1` with the
> Mac OS **Memory control panel → Virtual Memory turned ON**. Guest VM is the one runtime trigger
> that exists on 9.0.4 and would actually program page-table mappings + (potentially) take DSI page
> faults. If the trace stays zero / coarse, the shadow approach is 16 KB-safe and MMU stays moot for
> 9.0.4. If it lights up with 4 KB page-table writes, Discriminator A flips to "fine" and the
> calculus changes. Hours, decisive.

### Discriminator B — does the NATMEM data arena actually need `PROT_EXEC`? (corrects the task's c4 framing)

The task frames the c4 spike's failures as "relevant to any remap-based scheme." **They are not, for
a *data* shadow arena.** c4's two failures were both about mirroring *executable* code:

- `MAP_SHARED`/shm dual mapping → **SIGBUS on execute**;
- `vm_remap` *from a `MAP_JIT` region* → **`KERN_PROTECTION_FAILURE`**.

Both are execute-path problems. A shadow NATMEM arena holds **guest RAM (data)**, not host code —
the host JITs guest PPC into the *separate* `MAP_JIT` code cache and **never branches into guest
RAM** (under `EMULATED_PPC`, the JIT only emits `LDR/STR` against `RMEMBASE`; `jump_to_rom` enters
*compiled* blocks, not guest memory). 

**Verified in-tree:** `main_unix.cpp:1340/1366` mark RAM/ROM `VM_PAGE_READ|WRITE|EXECUTE`, but the
`DR_CACHE` EXEC `vm_protect` is `#if !EMULATED_PPC` (`main_unix.cpp:1275`, native-PPC only). For our
aarch64 JIT the arena's EXEC bit is **vestigial for the JIT's access path** — data loads/stores need
only RW. Therefore **c4's execute-mirroring blockers do not disqualify a data shadow**, and
`mach_vm_remap` / shared-fd-backed RW mirroring of NATMEM becomes viable (the working c4 recipe —
plain RW `mmap` → `vm_remap` → `mprotect` — needs only RW, which we have).

**Net correction:** treat c4 as a constraint on *code-cache* dual-mapping, **not** as a blanket
disqualifier of remap-based *data* shadowing.

---

## Ranked shortlist (most → least feasible)

### #1 — Shadow-arena translation ("Dynamic BAT" model) — *the winner*

| | |
|---|---|
| **Mechanism** | Keep the flat NATMEM arena as the *fastmem arena*. On every guest `mtspr` to BAT/SDR1/SR (rare), recompute the host mapping: `mprotect`/remap the affected NATMEM sub-range so `host = NATMEM_OFFSET + guest_virtual` lands on the right physical backing. Access-time codegen is **unchanged** — still `LDR/STR [RMEMBASE, UXTW(ea)]`. Dolphin does exactly this: it reserves a 4 GiB fastmem arena and `UpdateDBATMappings()` unmaps/remaps arena ranges as the guest reprograms BATs, "valid memory able to move around" while fastmem stays live. |
| **Why it preserves the fast path** | Translation cost is paid at *map-change* time (`mtspr` — **zero/sec on 9.0.4**), not at *access* time. The hot load/store keeps its single-instruction form. This is the whole point: it inverts QEMU's tradeoff. |
| **Apple-Silicon catch** | 16 KB host page vs 4 KB guest page (**Discriminator A**). Safe *only if* the guest maps coarse, 16 KB-aligned regions (BAT-style). Fine 4 KB page-table mappings with mixed per-page perms cannot be expressed at 16 KB granularity → falls back to #5/#6. Use `mach_vm_remap` of a shared physical backing, **RW only** (Discriminator B), not the code-cache `MAP_JIT` path. Remap consumes ~2× VA per aliased range (fine — arena is sparse). |
| **Effort** | High (map-change hook + per-BAT-region remap + DSI delivery for the rare miss), but **far below** sub-plan A's "rewrite every load/store." Bounded by the number of *map-change events*, which is ~0 today. |
| **Unlocks** | Real translation *iff* a target ever programs one, with the fast path intact. Pairs with sub-plan B (the rare arena miss becomes a real DSI/ISI instead of an `enter_mon()` crash). |

### #2 — Lazy / hybrid: stay V=P until the guest actually remaps

| | |
|---|---|
| **Mechanism** | Identity-mapped by default (today). Watch the (currently zero) `mtspr SDR1/BAT/SR` writes; the *first* non-identity write flips a "translation active" flag and switches the affected range to the #1 shadow path. "Pay only when the guest uses it." |
| **Relationship** | This is **not a separate candidate from #1** — it is #1's *trigger policy*. Folded in: #1 *is* lazy by construction (a map-change hook that fires zero times = pure identity). Listed separately only to name the gate explicitly. |
| **Why it preserves the fast path** | Until a remap fires, behavior is byte-identical to today. Zero overhead on the common case. |
| **Apple-Silicon catch** | Same 16 KB granularity gate as #1. |
| **Effort** | The gate itself is cheap (reuse the `SS_STUB_TRACE` counter sites as live hooks); the *handler* it gates into is #1's work. |
| **Unlocks** | A safe incremental on-ramp: ship the gate (detect + log + currently-no-op), prove it stays cold, only build the #1 handler if a target lights it up. |

### #3 — Selective: emulate only the *one* thing a real target needs

| | |
|---|---|
| **Mechanism** | Don't build a general MMU. If 9.1/9.2 (or guest VM) needs translation for exactly one narrow thing — a single protected nanokernel page, a VM backing-store window — model *that*, as a special-cased shadow region, and leave everything else V=P. |
| **Why it preserves the fast path** | The flat model stays the default; only a named, bounded window is special. |
| **Apple-Silicon catch** | Whether the "one thing" is 16 KB-alignable. A single protected page at 4 KB granularity inside a 16 KB host page is the hard sub-page-protection case → may need a guard-page or a duplicated-mapping trick for just that window. |
| **Effort** | Lowest of the real-translation options — *if* the target's need is genuinely narrow (unknown until a post-9.0.4 boot is diagnosed, per the standing verdict's sequencing). |
| **Unlocks** | Exactly one compatibility unblock, at minimum cost. The pragmatic answer if the New World ROM work surfaces a single, specific translation dependency. |

### #4 — Fastmem + SIGSEGV/SIGBUS backpatching (Dolphin `JitArm64_BackPatch`)

| | |
|---|---|
| **Mechanism** | Bare fast `LDR/STR`; on a host fault, decode the faulting ARM64 access and patch the site to a `BL slow_path`. |
| **Status** | **Already closed** — see [`lead-6-fastmem-backpatch.md`](research/lead-6-fastmem-backpatch.md). The *fast half* is what our JIT already emits (DIRECT_ADDRESSING + UXTW). The *backpatch half* has **nothing to patch**: SheepShaver reaches hardware via EMUL_OP traps, not faulting MMIO; the only recurring faults are VOSF framebuffer (which *wants* to keep faulting) and ~6 known ROM probe PCs. |
| **Why it's ranked here, not higher** | It's the access-time fault model, complementary to #1's map-time model. It would matter only as the *miss handler* for #1 (turn the rare unmapped access into a guest DSI instead of an `enter_mon()` crash) — and lead-6 notes that needs only a plain helper-exit, not the full backpatch machinery. |
| **Apple-Silicon catch** | W^X patching live JIT memory from the Mach exception thread; pairs with the c4 dual-mapping win (drop the toggle). Reentrancy on `exc_thread`. |
| **Effort / payoff** | High effort, ~zero perf payoff (lead-6's verdict). Salvage value = the correctness fix (DSI delivery) only. |

### #5 — QEMU softmmu: inline TLB probe on every load/store — *the anti-pattern, ranked last on purpose*

| | |
|---|---|
| **Mechanism** | Emit an inline TLB lookup + compare + branch before every guest access (QEMU TCG `qemu_ld/st`: fast path = TLB hit stays in generated code; slow path = C helper at end of block). |
| **Why it SACRIFICES the fast path** | This is *exactly* the cost the standing verdict feared: it replaces our single `LDR/STR` with a probe+branch on **every** access, in **every** block, whether or not translation is active. It is the baseline that #1 beats, **not a peer candidate.** |
| **When you'd be forced into it** | Only if **Discriminator A flips to "fine 4 KB paging"** — then per-page granularity can't be expressed by host `mprotect` at 16 KB, and software TLB is the only general answer. Gate it to *only* blocks proven to touch translated ranges (QEMU can't; we might, since ours is ~0). |
| **Effort / payoff** | Very high (codegen rewrite of every memory op); payoff only in the forced case. |

### #6 — Hypervisor.framework / host-MMU via the hypervisor — *not applicable, killed*

`hv_vm_*` / `hv_vcpu_*` virtualize the **host ARM64 ISA**. SheepShaver's PPC CPU is *emulated in
software* and never runs on a hardware vCPU, so the hypervisor has no guest to give an address space
to. There is no creative angle that survives: the address-space primitives you'd actually use
(`vm_remap`, `mach_vm_remap`, shared-memory mirroring) are **Mach VM**, not HV, and they're already
covered by #1 / the c4 spike. **Do not invest in an HV angle.**

---

## What the other emulators actually do (portability to us)

| Emulator | PPC/host memory model | Portable here? |
|---|---|---|
| **Dolphin** (PPC→ARM64, Apple Silicon) | Reserves a 4 GiB **fastmem arena**; bare `LDR/STR`; **Dynamic BATs** remap the arena at guest map-change time (`UpdateDBATMappings`); SIGSEGV backpatch for MMIO. Clears fastmem for pages with memchecks. | **Directly** — this *is* candidate #1. The closest production analog; Apple-Silicon-proven. Its memcheck/BAT-clear logic is the model for our map-change hook. |
| **QEMU** (TCG softmmu) | Inline TLB probe per access; slow path as C helper at end of TB. | As an *anti-pattern* (#5) and a fallback only if Discriminator A flips. Not portable as a default — it discards the flat-model win we already have. |
| **MAME** (PPC DRC, `drcbearm64`) | Goes through `static_generate_memory_accessor` (TLB + address spaces + `read32align`). [c2 study](research/c2-mame-ppc-drc-study.md): "our DIRECT_ADDRESSING is simpler than MAME's flat mapping" — we'd reuse `drcbearm64`/`drc_cache`, not its memory path. | Not portable for memory; its emitter is a separate (already-studied) lead. |
| **DingusPPC** (interpreter, full real MMU) | Full software MMU + 4 KB pages, no fastmem — accuracy over speed. | Datapoint for "what full MMU costs": it's the slow, accurate end of the spectrum we are explicitly *not* targeting. Confirms #5's cost shape. |

---

## Decision-oriented summary

1. **Is there a path that doesn't gut the flat model?** **Qualified yes.** Named candidate:
   **#1 shadow-arena ("Dynamic BAT") translation** — translate at map-change time (≈0 events on
   9.0.4), keep the access-time codegen identical.
2. **Is it needed today?** **No.** 9.0.4 runs fully V=P, zero supervisor ops (stub trace, 2026-06-07).
   DIRECT_ADDRESSING is already a degenerate identity MMU; fastmem is already satisfied (lead-6). The
   flat model is not under threat because nothing requests translation.
3. **What would ever demand it?** Un-patching the nanokernel (sub-plan B), a post-9.0.4 OS that
   programs non-identity maps, or — the only trigger on 9.0.4 itself — **enabling guest Virtual
   Memory** in the Memory control panel.
4. **Two facts gate the ranking — probe, don't assume:**
   - **A (coarse vs fine mapping):** decides 16 KB-safety *and* whether MMU is ever needed. Probe:
     `SS_STUB_TRACE=1` with guest VM enabled. *Inferred coarse.*
   - **B (does the data arena need EXEC):** decides whether c4 blocks remap-based shadowing. **It
     doesn't** — the JIT never executes from guest RAM; RW shadow is viable. *Verified in-tree.*
5. **If a target ever forces fine 4 KB paging:** the 16 KB granularity mismatch forces #5 (softmmu),
   which *does* gut the fast path — and the standing verdict's "very high effort" stands. The
   shadow approach is safe *only* in the coarse-mapping world the evidence currently points to.

**Recommended next action (cheap, decisive, no implementation):** run the Discriminator-A probe
(`SS_STUB_TRACE` + guest VM on). It is the one experiment that, on the OS we can already boot,
distinguishes "MMU stays moot, #1 is 16 KB-safe" from "fine paging exists, #1 is impossible, softmmu
or bust." Everything else waits on the New World ROM sequencing in the standing verdict.

---

## Sources

**In-repo (build on these):**
- [`MMU-NANOKERNEL-MP-PLAN.md`](../MMU-NANOKERNEL-MP-PLAN.md) — standing verdict; stub-trace zero result (2026-06-07); dependency structure (MMU only needed for VM/protection + DSI/ISI, none under V=P).
- [`research/lead-6-fastmem-backpatch.md`](research/lead-6-fastmem-backpatch.md) — fastmem already satisfied; backpatch has nothing to patch; the DSI-crash correctness gap.
- [`research/c4-wx-dual-mapping-spike.md`](research/c4-wx-dual-mapping-spike.md) — `vm_remap` works (RW→RX), `MAP_SHARED`-execute and remap-from-`MAP_JIT` fail. **Re-scoped here:** those are *execute*-path blockers; a RW data shadow is unaffected.
- [`research/c2-mame-ppc-drc-study.md`](research/c2-mame-ppc-drc-study.md) — MAME's memory path vs our DIRECT_ADDRESSING.
- `SheepShaver/src/Unix/main_unix.cpp:1275/1340/1366` — arena `vm_protect`; `DR_CACHE` EXEC is `#if !EMULATED_PPC` (**verified**: EXEC vestigial for the aarch64 JIT data path).
- `docs/ARCHITECTURE.md` §DIRECT_ADDRESSING / MAP_JIT.

**External (verified facts):**
- PPC MMU architecture (4 KB page, 256 MB segment, 16 SRs, hashed page table, BAT ≤256 MB blocks):
  [NXP PowerPC Arch Primer](https://www.nxp.com/docs/en/white-paper/POWRPCARCPRMRM.pdf),
  [IBM 750GL User Manual MMU ch.](https://www.manualsdir.com/manuals/126142/ibm-powerpc-750gl-powerpc-750gx.html?page=179),
  [MMU — Wikipedia](https://en.wikipedia.org/wiki/Memory_management_unit).
- QEMU softmmu TCG fast/slow path (inline TLB probe; slow path at end of TB):
  [Airbus Seclab — TCG memory accesses](https://airbus-seclab.github.io/qemu_blog/tcg_p3.html),
  [QEMU multi-thread TCG docs](https://www.qemu.org/docs/master/devel/multi-thread-tcg.html).
- Apple Silicon 16 KB-only page size / `mprotect` granularity:
  [Ampere — Arm64 page sizes](https://amperecomputing.com/tuning-guides/understanding-memory-page-sizes-on-arm64),
  [HN — macOS Apple Silicon 16 KiB pages](https://news.ycombinator.com/item?id=34476480).
- Dolphin Dynamic BATs + fastmem arena coexistence:
  [Dolphin MMU.cpp](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/PowerPC/MMU.cpp),
  [Dolphin Memmap.cpp](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/HW/Memmap.cpp),
  [Dolphin — "always turn on MMU" WIP PR #1831](https://github.com/dolphin-emu/dolphin/pull/1831).
- DingusPPC (full real MMU, interpreter): [dingusppc](https://github.com/dingusdev/dingusppc).
</content>
</invoke>
