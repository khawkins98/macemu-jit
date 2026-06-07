# Red-team: is the MMU deferral right, and is "MMU-via-fastmem" naive?

> **Status:** 📋 Decision memo (adversarial review) · **Created:** 2026-06-07
> **Why this doc exists:** Stress-test BOTH the standing "defer the MMU — high cost, low payoff"
> verdict AND the optimistic "MMU is almost-free via fastmem/lazy translation" idea. The brief was
> to be the skeptic on both sides and find the truth, not defend either. No code changes.
> **Inputs:** `MMU-NANOKERNEL-MP-PLAN.md`, `NEW-WORLD-ROM-SUPPORT-PLAN.md`,
> `COMPATIBILITY-PAYOFF-DINGUSPPC-REVISIT.md`, `lead-6-fastmem-backpatch.md`,
> `c4-wx-dual-mapping-spike.md`, `docs/ARCHITECTURE.md`; primary-source web research (forums,
> Wikipedia, Apple docs); two machine verifications.

---

## TL;DR — three blunt verdicts

1. **(A) Deferral is CORRECT — but the docs lean on the wrong reason.** It is right because the MMU
   is high-cost work with **unproven and currently-untestable demand** and **strictly worse EV** than
   the CopyBits/idle/video levers. It is *not* right because "we proved 9.x doesn't need the MMU" —
   the zero-stub-trace is zero **partly by construction** (ROM-patching removes the ops before
   runtime). The honest standing is *never-unless-proven*, not *later*.

2. **(B) "MMU is almost-free via fastmem" is NAIVE — it conflates two different fastmem claims.**
   Fastmem under the current flat V=P model is genuinely free and already shipping (✓). Fastmem as a
   way to *emulate a real guest MMU via the host MMU* is **broken on Apple Silicon by a verified
   16 KB-vs-4 KB page-granularity mismatch**, plus a no-slow-path / no-MMIO-fault-stream structural
   gap, plus signal-handler-over-W^X/NATMEM hazards. Lazy translation does not rescue it.

3. **Reconciled bottom line: spend effort on the MMU NEVER — unless one specific piece of evidence
   appears** (below). Until then, the boring deferral verdict survives every attack, and the EV is in
   the already-booting 8.6–9.0.4 range, not the OS-version frontier.

---

## Attack (A) — is deferral wrong? Is it a rationalization costing us compatibility?

### The demand crux: is there ANY hard evidence 9.1/9.2 *requires* real address translation?

**Finding: the "no MMU ⇒ no 9.1" line is universal folklore, asserted everywhere, bisected
nowhere.** (verified)

- Wikipedia, the cebix project lore, and every Emaculation/VCFed thread repeat it verbatim:
  *"SheepShaver does not emulate the MMU … Mac OS 9.1–9.2.2 are not supported."* But the posters are
  **support volunteers** (Ronald P. Regensburg et al.), not the developers, and **none demonstrate
  the MMU is the binding blocker.** WebFetch of the two most technical threads returned, explicitly:
  *"The claim about the MMU is asserted without proof … does not demonstrate or bisect whether the
  MMU is the actual blocking factor."* (verified — VCFed thread 72194, Emaculation t=6647)

- The *one* technical thread that goes deeper (Macintosh Garden "Mac OS 9.1-9.2 - How?") lists the
  real obstacles as **(i) the parcels NewWorld ROM format, (ii) nanokernel VM internals, (iii) the
  68k-emulator↔nanokernel HW/VM opcode hooks** — i.e. ROM/patch parity, *not* a runtime translation
  hot path. (verified — surfaced in search summary; consistent with our own evidence.)

- **What actually changed 9.0.4→9.1** (verified): CD burning in Finder, a Window menu, AppleScript
  Keychain, idle-loop CPU-sleep for battery, stability/Carbon plumbing. **No documented new virtual-
  memory or address-translation dependency.** 9.1/9.2 existed to be the Classic-environment on-ramp
  to OS X — exactly the "thin slice" the deferral assumes.

- **The primary source under our hands is silent too.** Grepping the SheepShaver source
  (`9.0.4|memory management unit|MMU|9.1` across `SheepShaver/src`) turns up **no upstream developer
  statement naming a specific 9.1 translation dependency** — the only hits are *our own* red-team
  artifacts (the `rom_patches.cpp` 9.0.4-lenient experiment and the `SS_STUB_TRACE` "MMU 2nd-wall
  probe" comments), which are themselves consistent with this memo. So the "MMU demand" claim is
  unsupported not just by the secondary forum sources but by the **primary code source**: nobody, at
  any layer, ever bisected it. (verified — grep, 2026-06-07.)

- **Our own bisection beats the folklore.** The `SS_ROM_PATCH_TRACE` result
  (`NEW-WORLD-ROM-SUPPORT-PLAN.md`) shows the *first* binding blocker is **parcels-ROM rejection in
  `PatchROM()`** — it fails before a single line of guest OS code (let alone MMU-dependent code)
  runs. So whatever 9.1 needs, **ROM acceptance is strictly upstream of it.** The folklore almost
  certainly **conflates ROM-rejection with MMU** because nobody bisected it — we did. (verified, our
  doc.)

**The QEMU "proof" is a confound, not evidence.** Forums cite "QEMU has an MMU and runs 9.2.2, ergo
the MMU is the blocker." QEMU differs from SheepShaver on **both** axes at once — it emulates the PPC
MMU **and** loads the ROM through a completely different (full-machine) path with no PatchROM
gauntlet. So "QEMU runs 9.2.2" cannot isolate which of SheepShaver's two stubs is binding. It is the
**same confound as DingusPPC** (`COMPATIBILITY-PAYOFF-DINGUSPPC-REVISIT.md`): an existence proof that
the frontier is *reachable via the expensive real-hardware path*, silent on whether SheepShaver's
no-MMU is the wall. (verified by construction.)

> **The honest epistemic position.** Does 9.1/9.2 need real translation in a *runtime hot path*?
> **Unknown, and untestable until a parcels ROM loads.** The `SS_STUB_TRACE=0` result on 9.0.4 is
> real but **partly true by construction** — ROM-patching strips the supervisor ops before they
> execute, so "zero runtime hits" is guaranteed, not discovered. It proves there is no runtime MMU
> hot path *on the OS we can run today*; it does **not** prove 9.1 lacks one. **Do not cite the zero-
> trace as positive evidence the MMU is unneeded — that is circular.** (This is the one place the
> existing docs overreach, and an adversarial reviewer should catch it.)

### Opportunity cost — is deferral *obviously* right vs the EV levers?

Yes. Even granting (generously) that some 9.1-exclusive software might one day want translation:

- The MMU is **"Very high effort, Low payoff, Very high risk"** by the project's own grid
  (`MMU-NANOKERNEL-MP-PLAN.md`), and it is **gated behind** an open-ended parcels-ROM RE effort
  ("days to weeks, no guarantee") that must succeed *first*.
- Its payoff even on success is thin: guest Virtual Memory (paging to disk — worthless on a multi-GB
  host) and memory protection. **Crash isolation is doubly worthless**: Mac OS 9 runs every app in
  one shared address space *on real hardware too*, so a perfect MMU buys no app sandboxing.
- The CopyBits HLE / idle-skipping / `.ndrv` levers are **Medium effort, *expected*-High payoff, low
  risk, unblocked today**, and serve the *largest* pool of wanted software (games/media/graphics on
  the already-booting range) which currently runs *poorly*, not "not at all." (Honesty hedge,
  symmetric with the zero-trace caveat: "High payoff" is *expected*, gated behind an unrun half-day
  CopyBits call/rect histogram — `COMPATIBILITY-PAYOFF-DINGUSPPC-REVISIT.md` — not yet *proven*-high.)

**Crucially, the deferral does NOT depend on this EV-elsewhere leg.** It stands on **cost + unproven
demand + unfinished ROM prerequisite + risk alone** — even if the CopyBits payoff probe came back
disappointing, the MMU would still be the worst bet in the grid. The EV comparison strengthens the
case; it is not load-bearing for it.

**Verdict (A): deferral is correct.** Not because demand was disproven (it wasn't — it's untestable),
but because the work is expensive, the demand is unproven, the prerequisite (ROM) is itself
unfinished, and the EV is decisively elsewhere. The deferral is **not** a rationalization costing us
compatibility: the compatibility it would buy is a thin, unproven, gated slice, and the real
compatibility wins sit on the range we already boot.

---

## Attack (B) — is "MMU almost-free via fastmem / lazy translation" naive?

**The conflation that the whole optimistic case rides on (name it first):**

| Claim | What it means | Status |
|---|---|---|
| **Fastmem-1** | `LDR/STR [base, UXTW(ea)]` under flat **V=P** — host page tables enforce validity, no per-access check | ✅ **Already shipping, genuinely free** (`lead-6`) |
| **Fastmem-2** | Use the **host MMU to emulate a real *guest* MMU** — shadow guest mappings, fault, walk guest page table, fix up | ❌ **The naive claim.** Several Apple-Silicon blockers below |

The fast path is free **precisely because translation is OFF.** `lead-6`'s "fastmem already
satisfied" is about Fastmem-1; it must not leak into "so the MMU is cheap." In fact `lead-6` contains
the *kill* for Fastmem-2: there is **no slow-path infrastructure** in the JIT, and **no hot MMIO
fault stream** to backpatch (hardware is reached via EMUL_OP traps, not faulting memory). A real
guest MMU forces a per-access cost that today does not exist.

Now each easy-win claim, with its specific Apple-Silicon blocker:

### Claim 1 — "mprotect/host-MMU fastmem works at PPC page granularity." NAIVE.

**Blocker: verified 16 KB host page vs 4 KB PPC page.** `getconf PAGESIZE` and
`sysctl hw.pagesize` on this machine both return **16384** (verified, 2026-06-07; arm64). The PPC
page is 4 KB. You **cannot** `mprotect`/`PROT_NONE` a single 4 KB guest page — the smallest host unit
covers **four** guest pages. If those four guest pages have different guest permissions/mappings
(the normal case once translation is non-trivial), you get **false sharing**: protecting one forces
the other three, so every access to the innocent neighbors faults too. The escape is **sub-page
emulation** (software-check each access) — which **is** the per-access cost the whole fastmem idea
exists to avoid. The host-MMU-shadow path is granularity-broken on Apple Silicon out of the gate.
(verified page size; granularity consequence is standard, inferred.)

### Claim 2 — "faults are ~never, so fastmem stays fast." CONDITIONALLY TRUE → the trap.

True **only while translation is identity (V=P)**. The current zero-fault reality holds because
9.0.4 is effectively V=P even with the guest "MMU" notionally on. The moment a guest workload uses a
**non-identity mapping in a hot path**, every translated access to it either (a) needs the inline
software TLB probe (kills the single-instruction access for *all* memory ops, everywhere — you can't
know statically which accesses are hot-non-identity), or (b) faults into a signal handler **per
access** = catastrophic (microseconds per load on what should be ~1 ns). So "fast-path preserved" is
**conditional on translation being cold**, and that condition is exactly the untestable unknown from
(A). (inferred from the architecture; the conditionality is the key honest point.)

### Claim 3 — "lazy translation makes it cheap." NAIVE about *what* it defers.

Lazy translation defers **when you populate the TLB/shadow**, not **whether every translated access
is checked**. It removes eager page-table-walk cost; it does **nothing** about the per-access
probe-or-fault that is the actual fastmem-killer in Claim 2. Naming this kills the "lazy = cheap"
hand-wave. (inferred — standard dynarec semantics.)

### Claim 4 — "decode the faulting load/store + fix up in a SIGBUS/SIGSEGV handler." HAZARD-LADEN on ARM64/macOS.

The pieces SheepShaver would need exist (`lead-6` §4 maps them), but each is a known sharp edge:
- **W^X on the code cache**: patching JIT memory from a fault handler needs the
  `pthread_jit_write_protect_np` toggle (or dual-map) + `sys_icache_invalidate`. The
  `c4-wx-dual-mapping-spike` shows `vm_remap` **fails with `KERN_PROTECTION_FAILURE` when the source
  is `MAP_JIT`** — so MAP_JIT regions are genuinely constrained on Apple Silicon. *(Caveat: this
  finding is about the **code cache**, not data fastmem — use it only as evidence MAP_JIT is
  constrained, not as a direct MMU killer.)*
- **PROT_NONE NATMEM holes**: the flat `NATMEM_OFFSET` window has unmapped regions; a guest MMU would
  add more, and the handler must distinguish a *guest* DSI/ISI fault from a *real* host bug or the
  JIT's own fault — today an unmapped access **crashes the emulator** (`enter_mon`/`QuitEmulator`),
  it does not raise a guest exception (`lead-6` §"correctness gap").
- **Async-signal-safety**: walking a guest page table + patching code inside a signal/Mach-exception
  handler, while the faulting thread is suspended, is "the single subtlest area in any dynarec"
  (`lead-6` §5, verbatim risk rating: High).
- **VOSF interaction**: the framebuffer fault path *wants* to keep faulting (dirty tracking);
  blanket fault-driven MMU handling would collide with it.
(verified file evidence in `lead-6`/`c4`; severity ratings are the docs' own.)

### Claim 5 — "use the host MMU / Hypervisor.framework." RED HERRING — confirmed dead.

**Apple's Hypervisor.framework virtualizes the host ISA only** — "an ARM hypervisor on ARM can only
run ARM guests." It **cannot** run a PowerPC guest on Apple Silicon. (verified — Apple docs +
The Register/eclecticlight coverage.) The thing that *does* run PPC Mac OS on Apple Silicon is **QEMU
TCG — pure software emulation**, the same class as SheepShaver, **not** the host MMU. So "let the host
MMU do the translation" is a dead end; there is no hardware-assisted PPC path on this platform.

### Claim 6 — "doing it properly (real MMU) is tractable." DingusPPC says otherwise.

DingusPPC **has a real MMU and real hardware modeling** and is **interpreter-only** (no JIT, minutes
to Finder). After years, as of 2025 it **still bus-errors on a Mac OS 9 hard-disk boot** (GitHub
#107), README says "highly unfinished," video acceleration disabled. (verified —
`COMPATIBILITY-PAYOFF-DINGUSPPC-REVISIT.md` sources.) This is the strongest empirical statement on
effort: even the project that did the MMU "properly" is years in and not a daily driver. It tells us
the *real* cost of the frontier is a from-scratch hardware-accurate emulator, not a fastmem bolt-on.

**Verdict (B): the "MMU is cheap" claims that survive scrutiny = exactly ONE (Fastmem-1, the flat
V=P path we already have).** Every claim that touches *real guest translation* is naive on Apple
Silicon: granularity-broken (16 KB pages, verified), per-access-cost-bound (Claims 2–3), hazard-laden
in the handler (Claim 4), and with no hardware-assist escape hatch (Claim 5). The "easy win" is an
illusion created by mistaking the V=P fast path for a translation mechanism.

---

## Reconciliation — where (A) and (B) meet

Both attacks bottom out at the **same single unknown**: *does any wanted 9.x workload use a
non-identity guest translation in a hot path?*
- If **no** (the evidence-free default, and what 9.0.4 demonstrably is): the MMU is unneeded *and*
  Fastmem-1 already covers everything → deferral correct, MMU work is pure waste.
- If **yes**: the MMU becomes needed *and* the fastmem shortcut simultaneously collapses (Claim 2) →
  you'd face the expensive software-TLB-in-JIT rewrite, on a granularity-hostile host, behind an
  unfinished ROM port. Still a bad bet, just a *needed*-bad-bet.

Either branch says **don't build it now.**

### Should we spend effort on the MMU — now, later, or never?

**Never — unless proven.** Stronger and more honest than "later." The MMU is gated behind a
ROM-acceptance effort that hasn't landed, its demand is folklore not fact, its fastmem shortcut is
naive on this hardware, and its payoff (VM + non-isolating protection) is near-worthless. Effort goes
to CopyBits HLE / idle-skipping / `.ndrv` (the actual compatibility levers) and, opportunistically
and cheaply, to the parcels-ROM probe.

### The single piece of evidence that flips the answer

**A Mac OS 9.2.2 boot (after parcels-ROM acceptance) that bisects to a translation-dependent failure
— a DSI/ISI in a hot path that identity mapping cannot satisfy.** Concretely: 9.2.2's ROM is
accepted, it boots far enough to run guest OS code, and a reproducible failure is traced to the guest
enabling a non-identity mapping the flat model can't honor (not a ROM-patch miss, not a missing
EMUL_OP, not a nanokernel-drift bug). *That* — and only that — converts "never" into "now scoped to
sub-plan B first, A only if the fault is genuinely translation (not exception-model) in origin." Note
this evidence is **untestable until the ROM loads**, which is exactly why ROM-first ordering is
unconditionally correct regardless of the MMU question.

Absence of this evidence to date is itself a finding: **nobody — not the forums, not us — has ever
produced it.** The deferral is not under-examined; it is correctly waiting for a trigger that has not
fired.

---

## Verified vs inferred — ledger

- **Verified (machine):** host page size = 16384 (`getconf PAGESIZE`, `sysctl hw.pagesize`), arm64.
- **Verified (primary web):** the "no MMU ⇒ no 9.1" claim is asserted-not-bisected across forums
  (VCFed 72194, Emaculation t=6647, Wikipedia); 9.1 changes are stability/Carbon/Finder, no VM
  dependency documented; Hypervisor.framework is host-ISA-only (Apple docs); QEMU runs PPC via TCG
  software emulation, not the host MMU.
- **Verified (our docs/code):** PatchROM rejects parcels ROM before any OS code runs
  (`SS_ROM_PATCH_TRACE`); `SS_STUB_TRACE=0` on 9.0.4 (and the "by construction" caveat);
  Fastmem-1 already emitted via UXTW (`lead-6`); no JIT slow path / no hot MMIO fault stream
  (`lead-6`); `vm_remap` fails on `MAP_JIT` source (`c4-spike`); unmapped guest access crashes
  rather than raising guest DSI (`lead-6`); DingusPPC real-MMU + interpreter, 2025 HD-boot bus-error
  (`COMPATIBILITY-PAYOFF` sources).
- **Inferred (standard, not independently re-derived):** the 16 KB→false-sharing→sub-page-emulation
  consequence; the per-access probe-or-fault cost of any real software TLB; lazy translation deferring
  population not per-access checking. These are textbook dynarec/VM mechanics, marked inferred for
  honesty.

## Sources

Primary web (verified):
- SheepShaver — https://en.wikipedia.org/wiki/SheepShaver (caps at 9.0.4, "no MMU" — *folklore*)
- "Why does SheepShaver not work with Mac OS 9.1 and 9.2?" — https://forum.vcfed.org/index.php?threads/why-does-sheepshaver-not-work-with-mac-os-9-1-and-9-2.72194/ (MMU claim **asserted, not bisected**)
- "What about using Sheepshaver to emulate Mac OS 9.2.2" — https://www.emaculation.com/forum/viewtopic.php?t=6647 (MMU asserted; no developer bisection)
- "Mac OS 9.1-9.2 - How?" — http://macintoshgarden.org/forum/mac-os-91-92-how (real obstacles = parcels ROM + nanokernel VM internals + 68k↔nanokernel opcode hooks)
- Mac OS 9.1 changes — https://apple.fandom.com/wiki/Mac_OS_9.1 , https://www.macworld.com/article/666257/mac-os-9-1-review.html (stability/Carbon/Finder; no VM dependency)
- Hypervisor.framework host-ISA-only — https://developer.apple.com/documentation/hypervisor , https://www.theregister.com/2023/09/29/utm_apple_hypervisor_foss_fest/ ; PPC-on-Apple-Silicon is QEMU TCG software emulation

Internal (verified): `docs/planning/MMU-NANOKERNEL-MP-PLAN.md`, `docs/planning/NEW-WORLD-ROM-SUPPORT-PLAN.md`,
`docs/planning/COMPATIBILITY-PAYOFF-DINGUSPPC-REVISIT.md`,
`docs/planning/sheepshaver-research/research/lead-6-fastmem-backpatch.md`,
`docs/planning/sheepshaver-research/research/c4-wx-dual-mapping-spike.md`, `docs/ARCHITECTURE.md`.
