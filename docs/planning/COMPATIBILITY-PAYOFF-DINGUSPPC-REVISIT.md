# Strategy revisit: biggest compatibility payoff — through the DingusPPC lens

> **Status:** 📋 Decision memo · **Created:** 2026-06-07
> **Why this doc exists:** Focused revisit of "what's the single biggest-payoff area to increase
> guest-software compatibility?" using DingusPPC as the lens, to CONFIRM or CHALLENGE the standing
> "NewWorld-ROM-first, MMU-deferred" verdict. Decision-oriented; no code changes.
> **Scope note:** the prompt bundles two distinct goals — (i) *boot newer OS* (9.1/9.2 frontier)
> and (ii) *run software that runs poorly today*. They have different biggest-payoff answers; this
> memo separates them deliberately.

---

## Bottom-line recommendation

**The single biggest compatibility payoff is NOT the OS-version frontier (NewWorld ROM → maybe MMU)
— it is the performance/HLE work on the OS range that already boots (8.6–9.0.4): `CopyBits` HLE +
idle-skipping + the `.ndrv` video path.** Reason: "compatibility" for an emulator is *"does the
software people actually want run, and run usably?"* — and the largest pool of such software (games,
media, graphics apps, QuickTime-era titles) already targets 8.6–9.0.4 but runs *poorly* because the
hottest path (`CopyBits`/blits) is executed through the worst layer in the stack: the PPC-JIT
emulating the ROM's 68K DR-emulator. Fixing that is **Medium effort, High payoff, low risk** (no SMC
hazard, uses the existing NativeOp mechanism), and it's unblocked today. By contrast, Mac OS 9.1 was
largely stability/Carbon plumbing; the software it *exclusively* unlocks (vs 9.0.4) is a thin slice,
and reaching it is an **open-ended exploratory bet** (our own `NEW-WORLD-ROM-SUPPORT-PLAN.md` Phase 2
= "days to weeks, no guarantee," followed by a possible second MMU wall). **Key risk of the
recommended path:** `CopyBits` payoff is workload-dependent — gate it behind the half-day
call-frequency/rect-size histogram already specified (CROSS-EMULATOR-IDEATION #1) before committing
the blitter work.

**On the OS-frontier sub-question the user actually asked:** the standing **"ROM-first, MMU-deferred"
ordering is CONFIRMED** — but it is a *lower-EV* track than the HLE/perf work above. Pursue it as the
cheap-probe path (load a parcels ROM, run the stub-pressure trace), not as the headline compatibility
investment.

---

## Ranked ladder (compatibility levers, effort vs software unlocked)

| Rank | Lever | Effort | Software unlocked | Depends on / gate |
|---|---|---|---|---|
| **1** | **`CopyBits` HLE** (native/NEON blitter via NativeOp) | Med | **High** — games/media/graphics apps on the *already-booting* 8.6–9.0.4 range run *usably* instead of poorly | Half-day call/rect histogram probe first (go/no-go). Split: `CopyBits`=safe, `BlockMove`-for-code=SMC trap, exclude. |
| **2** | **Idle-skipping / busy-wait yield** | Med | **High citizenship** — VM stops pegging a P-core in cooperative idle; enables backgrounded long-running guests (thermal/battery), unattended workloads | None. Identify 2–3 hot idle PCs from HOT-PC sample, gate behind env flag, measure. |
| **3** | **Extend `.ndrv` video path** (host HW cursor → dirty-rect → accel primitives) | Low→Med | Med-High — responsiveness + smoother UI across *all* software; the proven MoL/`qemu_vga.ndrv` design | HW cursor first (Low, zero guest change). Do NOT emulate real ATI silicon (the DingusPPC path). |
| **4** | **NewWorld parcels-ROM support (9.0.4 G4 → 9.1/9.2)** | High (open-ended) | Low-Med — thin slice *exclusive* to 9.1+; mostly "newer OS for its own sake" (AltiVec is already unlocked under 1.1 ROM, so no longer a reason) | Prereq for *any* 9.1+ test. Phase 2 in progress; per-patch RE of ~12 absent HLE patches. **Run stub-pressure trace alongside.** |
| **5** | **Real MMU translation** | Very High | Low — VM/protection (near-worthless on multi-GB host); only matters *if* a 9.2.x boot empirically proves a translation dependency | Gated behind #4 booting 9.2.2 first ("second wall"). Worst effort/payoff in the entire grid. |

**Dependency note:** #1–#3 are independent and unblocked *today*. #4→#5 is a serial, exploratory
chain where each step gates the next, and #5 is explicitly do-not-start-unless-proven-needed.

### #1 CopyBits histogram probe — executable plan (the go/no-go gate; ready to build)

The gate before any blitter work. Implement as a **two-phase, env-gated (`SS_COPYBITS_TRACE`) trap
head-patch** — the probe IS the first increment of the HLE (mechanism mirrors the existing EMUL_OP
trap patches in `rom_patches.cpp`/`emul_op.cpp`; scoped 2026-06-07):

- **Phase 1 (frequency, low-risk — do first):** count `_CopyBits` calls, boot vs steady.
  1. `emul_op.h`: add `OP_COPYBITS_PROBE` before `OP_MAX` → `M68K_EMUL_OP_COPYBITS_PROBE = M68K_EMUL_BREAK + OP_COPYBITS_PROBE` (lands in the 0xFExx EMUL range).
  2. `emul_op.cpp` `EmulOp()` switch: `case OP_COPYBITS_PROBE:` → `g_copybits[phase]++;` (reuse the
     SS_STUB_TRACE boot/steady `phase` flag). Read-only; must not disturb regs/stack (the stub's JMP
     chains to the real CopyBits right after).
  3. Install at first idle (the `e2e_emit_idle_signals` one-shot, where the System + traps are up), if
     `SS_COPYBITS_TRACE`: `orig = Execute68kTrap(_GetToolboxTrapAddress 0xA146, d0=0xA8EC) → a0`; build a
     stub via `NewPtrSysClear`: `[M68K_EMUL_OP_COPYBITS_PROBE][0x207C orig_hi orig_lo (move.l #orig,a0)][0x4ED0 (jmp (a0))]`;
     `Execute68kTrap(_SetToolboxTrapAddress 0xA047, d0=0xA8EC, a0=stub)`.
  4. Dump `g_copybits[boot|steady]` at exit (same teardown hook as SS_STUB_TRACE). Run on boot + an app +
     a game (workload disk). **Go/no-go:** high steady CopyBits frequency ⇒ build the blitter.
- **Phase 2 (rect/byte sizes — after Phase 1 proves it's hot):** the EMUL_OP reads the Pascal args off
  `r->a[7]`: entry stack is `4(a7)=maskRgn, 8(a7)=mode(2B), 10(a7)=dstRect*, 14(a7)=srcRect*, 18(a7)=dstBits*, 22(a7)=srcBits*`
  (Pascal pushes L→R; verify offsets empirically — log a few then check against a known blit). Deref the
  Rect ptrs (guarded by `guest_ptr_ok`) → w×h histogram buckets. This sizes the blitter (small rects =
  not worth native; large = high payoff) and confirms the **CopyBits-only split** (BlockMove-for-code is a
  separate SMC trap — out of scope).
- **Safety:** env-gated, default off → zero effect on normal boots even if the stub is mis-encoded (only
  `SS_COPYBITS_TRACE` runs would fail, caught immediately by a boot that doesn't reach Finder). Land
  Phase 1, validate one boot, then Phase 2.

#### STATUS (2026-06-07) — come-from stub abandoned; profile-correlation shipped (phase 1 partial)

- ❌ **The come-from heap-stub crashed** (EMUL_OP + `move.l #orig,a0` + `jmp (a0)` via
  `SetToolboxTrapAddress`). Crash report: first CopyBits call from the **ADB cursor-draw** path →
  `jmp (a0)` → garbage PC **0x55590000** (RAM top). The EMUL_OP mis-resumed inside *nested*
  `execute_68k`, so `move.l` was skipped and `a0` held a stale (stack-ish) value. Lesson: **don't
  do come-from heap stubs that EMUL_OP from within nested 68k execution.** Never committed.
- ✅ **Shipped instead — `SS_COPYBITS_TRACE` safe resolver** (commit `9fbfcff6`): resolves `_CopyBits`'s
  guest PC via `GetToolboxTrapAddress(0xA8EC)` at first idle and logs it (e.g. `102daa5c`); zero guest
  patching, clean shutdown verified. **Frequency = correlate that PC with `SS_JIT_PROFILE`'s per-block
  (PC-keyed) counts.**
- ⏭ **Remaining for the go/no-go number:** (1) the profiler prints only the **top-40** hot blocks — add a
  small `SS_JIT_PROFILE_PC=<pc>` hook to print an *arbitrary* block's exec count (CopyBits won't be top-40
  under light use); (2) run it under a **graphics-heavy workload** (an app/game on the apps disk), not
  just Finder menu nudges, to get a representative call rate + (Phase 2) rect sizes.
- **Phase 2 interception (rect sizes):** do it the **ROM-patch way** (EMUL_OP written into the routine at
  PatchROM time, like the existing `adbop`/nvram patches), NOT a runtime heap come-from.

#### ⚠️ FINDING (2026-06-07): trap address ≠ JIT block PC on PPC (Mixed Mode indirection)

The resolver logged `_CopyBits` (0xA8EC) → **0x102daa5c**, but `SS_JIT_PROFILE_PC=102daa5c` after a 9.0
boot + Finder activity reported **"(not compiled) 0"** — that block never JIT-executed. Reason: on a
PowerPC Mac, QuickDraw's CopyBits is **PowerPC (CFM/PEF) code**; the 68k trap `0xA8EC` dispatches through
the **Mixed Mode Manager** via a **routine descriptor**. `GetToolboxTrapAddress` returns the
descriptor/glue address (`0x102daa5c`), NOT the PPC code the JIT actually compiles+runs. So
"resolve-trap-addr → correlate with the JIT PC-keyed profile" **does not work** for PPC Toolbox routines.
**Fix attempt + result (2026-06-07):** added a RoutineDescriptor-follow to the resolver (RD magic 0xAAFE
→ procDescriptor @+0x18 → TVector[0]). **It reported `0x102daa5c` is NOT a RoutineDescriptor** (magic
mismatch) — so the trap address isn't a standard Mixed-Mode RD either. CopyBits's real PPC code path is
not trivially derivable from the trap address. **Remaining (needs live investigation, not autonomous):**
dump guest memory at `0x102daa5c` to see what it actually is (68k glue? a non-standard descriptor?),
and/or under a **graphics workload** identify CopyBits's hot PPC block from the top-40 by disassembly.
The **graphics workload is the real prerequisite** (Finder barely blits — confirmed: 0 exec). Tooling
(`SS_COPYBITS_TRACE` resolver + RD-follow, `SS_JIT_PROFILE_PC`) is in place.

#### ✅ Graphics workload ACHIEVED (2026-06-07) — 3D Ultra Pinball runs; profile-capture gap found

The user staged `3D_pinball_demo.hqx`. Pipeline that worked, fully autonomous: `unar` decodes the BinHex
→ "3D Ultra Pinball Demo ƒ" (a **PowerPC PEF app** + data; resource fork + type/creator `APPL`/`PBJ!`
preserved as native macOS xattrs). Mounted host-side into the guest via **`extfs /Users/Shared/macemu/pinball_ext`**
(SheepShaver sees it as the "Unix" volume). Launched via VNC type-select → the game runs and **renders its
pinball table** (`[APP] frontApp='3D Pinball Demo' title='Ultra Pinball'`, screenshot `/tmp/pinball_running.png`).
Non-fatal init dialogs handled: "could not create Sierra folder" (read-only extfs — OK'd, game continues
without saves) and "could not init sound" (we boot `nosound` — OK'd, expected). **So a real graphics
workload is reachable today on the booting 9.0 disk — no NewWorld ROM needed.** Decoded game kept at
`/Users/Shared/macemu/pinball_ext` for re-use.

**⚠️ Profile-capture gap (the remaining blocker for the CopyBits number):** the JIT profile only dumps on
**clean shutdown** (`ppc_jit_aarch64_exit` → `jit_profile_dump`), but the **fullscreen game resists
automated quit** (Esc / Cmd-Q / the SIGUSR1 Power-key shutdown all fail to return to Finder while it grabs
the screen), so a force-kill yields no profile. **Fix (small, generic, next step):** add a
*dump-profile-without-clean-shutdown* path — e.g. a host signal that sets a flag the JIT heartbeat checks
and calls `jit_profile_dump()` mid-run (the heartbeat runs even during the game). Then: run pinball with
`SS_JIT_PROFILE` + the new signal → read the top-40 (the graphics hot path: how much is load/store/blit vs
FP/int) → the CopyBits-HLE go/no-go. (Combine with the `0x102daa5c` identification, still open.)

---

## CONFIRM / CHALLENGE of the standing verdict

**Two separate verdicts, two separate calls:**

1. **The *ordering* "ROM-before-MMU" → CONFIRMED.** (verified)
   The decider is **our own empirical trace, not DingusPPC**: `PatchROM()` rejects the parcels ROM
   *before any guest OS code — let alone any MMU-dependent code — runs* (the
   `SS_ROM_PATCH_TRACE` result, `NEW-WORLD-ROM-SUPPORT-PLAN.md`: first miss is
   `patch_nanokernel_boot`'s `sr_init_dat`). So ROM acceptance is the prerequisite *regardless* of
   whether 9.1+ also needs a real MMU. This holds **even if the forum common-wisdom "no MMU ⇒ no 9.1"
   is correct**, because: (a) SheepShaver runs 9.0.4 *today* under pure identity mapping, so "no MMU"
   is not a wall one minor version below — any 9.1 MMU dependency is *new-in-9.1 and unproven*; (b) it
   is *untestable* until the ROM loads. The folk "MMU" line almost certainly conflates ROM-rejection
   with MMU because **nobody bisected it** — our PatchROM trace did. (verified our docs; the
   common-wisdom is widely repeated on Emaculation/forums — see Sources, marked *inferred-conflation*.)

2. **The *premise* that the OS-version frontier is the biggest compatibility payoff → CHALLENGED.** (position)
   The standing planning docs treat NewWorld-ROM as "the first wall" to break the 9.0.4 ceiling, which
   implicitly frames the ceiling as the main compatibility prize. That's the wrong prize for
   *biggest payoff*: the software that runs *poorly* (not "doesn't boot") is a far larger pool and
   lives on the range that **already boots**. The cheapest, lowest-risk compatibility-per-unit-effort
   wins are #1–#3 above. ROM-first stays *correct as an ordering* but should be **re-ranked as a
   lower-EV exploratory track**, not the headline.

### Why DingusPPC does NOT decide the ROM-vs-MMU question (and what it does tell us)

- **DingusPPC is a confound, not a tie-breaker.** (verified, by construction) It differs from
  SheepShaver on *both* axes simultaneously — it "aims to more accurately emulate actual hardware as
  opposed to patching the ROM and RAM, like SheepShaver" (Emaculation wiki, *verified*). So "DingusPPC
  boots 9.2.2" cannot isolate which of SheepShaver's two stubs (ROM-patching vs no-MMU) is binding.
  It's an **existence proof** that the full 7.x–9.2.2 (+ OS X 10.0–10.2) range is *reachable*, but
  only via the expensive real-hardware path.
- **And that path is not yet practically usable.** (verified, date-stamped) DingusPPC is a **pure
  interpreter, no JIT** — minutes from boot to Finder (persistent.info, Dec 2023). As of **2025** Mac
  OS 9 *hard-disk* boot still hits bus errors / WIP (GitHub issue #107, Emaculation wiki Jan 2025
  build); the README itself says "highly unfinished." Video **acceleration is disabled**
  (our `landscape-3` doc, *verified*). **OS X is out of realistic SheepShaver scope** and even
  DingusPPC's OS X install attempts failed — so it must not sneak in as a phantom MMU-path payoff.

**Net read:** DingusPPC *corroborates* that real-MMU + real-hardware is the only known route to
9.1/9.2/OS X — and simultaneously shows that route costs a from-scratch interpreter-grade emulator
that, after years, still isn't a usable daily driver. That makes SheepShaver chasing the same frontier
**a very high-cost, low-EV bet**, and reinforces playing to SheepShaver's strength: a *fast, booting*
8.6–9.0.4 where the remaining gap is "runs poorly," not "doesn't run."

---

## The higher-payoff lever we were underrating

Yes: **`CopyBits` HLE (#1) + idle-skipping (#2) + `.ndrv` video (#3)** — the "attack the worst layer"
theme from CROSS-EMULATOR-IDEATION. These were already nominated #1/#2 there and rated High payoff,
but they sit in OPTIMIZATION/ROADMAP framing ("performance"), so they were not being weighed as
**compatibility** levers. Reframed correctly, "make the software people run actually usable" *is*
compatibility, and these beat the OS-version frontier on EV. (Note: our docs already answer the device
question — `landscape-3` and IMPLEMENTATION-BACKLOG explicitly say *extend `.ndrv`, do NOT emulate ATI
silicon (the DingusPPC path)*; we don't need to redo that.)

Levers we can *de-prioritize* with confidence: real-ATI-silicon device modeling (ruled out, GPL-3 +
unproven everywhere incl. DingusPPC); MMU (gated, lowest payoff); FPU/AltiVec completeness (AltiVec
already unlocked + wired; only `vpkuwum` + test vectors remain).

---

## What's already answered in our docs (don't redo)

- **ROM is the first wall, MMU only a possible second** — `MMU-NANOKERNEL-MP-PLAN.md` (TL;DR + grid),
  `NEW-WORLD-ROM-SUPPORT-PLAN.md` (Phase 0 trace result). This memo confirms the ordering and adds the
  EV re-ranking.
- **DingusPPC = interpreter, real MMU, GPL-3, reference-only, video accel off** —
  `DINGUSPPC-EVALUATION-PLAN.md`, `landscape-3-classic-mac-video-accel.md`. This memo adds the
  "confound, not tie-breaker" framing + 2025 maturity update.
- **CopyBits HLE / idle-skipping nominated, .ndrv-not-silicon** — `CROSS-EMULATOR-IDEATION.md` (#1/#2),
  `IMPLEMENTATION-BACKLOG.md`. This memo elevates them from "perf" to "biggest compatibility lever."

---

## Sources

DingusPPC (verified):
- Repo / README — https://github.com/dingusdev/dingusppc (models: Power Mac 6100/7200/7500/G3 Beige,
  early New World, Pippin; "more accurately emulate actual hardware ... like SheepShaver"; "highly unfinished")
- Emaculation wiki — https://www.emaculation.com/doku.php/dingusppc (most functional: 6100 + G3 Beige; can run 9.x / OS X 10.2.8; WIP, Jan 2025 macOS build)
- persistent.info hands-on, **Dec 2023** — https://blog.persistent.info/2023/12/dingusppc.html (pure interpreter, no JIT; minutes to Finder; HD/SCSI boot broken; OS X install failed)
- GitHub issue #107 "How to install Mac OS 9" — https://github.com/dingusdev/dingusppc/issues/107 (2025: 9.x HD boot bus-error, still WIP)

SheepShaver 9.1+ limit (common-wisdom — *inferred conflation of ROM-rejection with MMU*):
- Emaculation / BetaArchive threads + Wikipedia — "SheepShaver caps at 9.0.4; 9.1 needs MMU it doesn't emulate"
  (e.g. https://en.wikipedia.org/wiki/SheepShaver , https://www.emaculation.com/doku.php/sheepshaver_mac_os_x_setup).
  Our PatchROM trace shows the *first* binding blocker is parcels-ROM rejection, not MMU.

Internal (verified): `docs/planning/MMU-NANOKERNEL-MP-PLAN.md`, `docs/planning/NEW-WORLD-ROM-SUPPORT-PLAN.md`,
`docs/planning/DINGUSPPC-EVALUATION-PLAN.md`, `docs/planning/sheepshaver-research/CROSS-EMULATOR-IDEATION.md`,
`docs/planning/sheepshaver-research/research/landscape-3-classic-mac-video-accel.md`, `JIT-STATUS.md`.
