# EE / interrupt-delivery chain recon — the honest map, the first EE-rise, and the three-level test spec

> **Status:** 📋 Recon + test spec (READ-ONLY session — no emulator builds, no boots) ·
> **Created:** 2026-06-11 · **Branch:** `macos-arm64`
> **Mandate:** Re-score #2's honest negative (1) (`MACHINE-LAYER-PLAN.md` §9): *"the boot
> has still never risen past EE=0 — the entire interrupt-delivery chain (PIC wiring, tick
> restoration, via_int retirement) is unexercised and is the likeliest place for the next
> M3-class surprise."* This memo maps what exists vs what's missing (A), pins where EE will
> first rise on the boot path from static evidence (B), and specs three test levels so the
> chain is exercised BEFORE the boot demands it (C).
> **Inputs:** `M3A-ENTRY-TABLE.md`, `M6A-ONGOING-ENTRY-DESIGN.md` (rung-2 results + residue
> register), `M6A-WAVE2-SHIM-RECON.md` (night-run results), `M3-INTERRUPT-RECON.md`,
> `M3-PIC-CUDA-DONOR-STUDY.md`, `MACHINE-LAYER-PLAN.md` §2d + M3b row;
> code: `sheepshaver_glue.cpp`, `ppc-execute.cpp`, `ppc-cpu.cpp`, `main_unix.cpp`,
> `rom_patches.cpp`, `src/machine/{exc_core,virt_clock}.cpp` + tests;
> static RE: `/tmp/rom901.bin` (PATCHED post-PatchROM image, md5
> `e432df64122a5a89ec1c08d0bdf01609`, capstone PPC BE, base 0x50000000).
> Tags: **[STATIC]** = fresh disassembly this session, **[PROBE✓]/[CRASH]** = carried from
> the cited memos, **[CODE]** = file:line in the repo, **[GAP]** = missing/untested.
> Line numbers may drift slightly; symbols are stable.

---

## 0. TL;DR

1. **The chain is real up to and including delivery, and fictional after it.** DEC latch →
   gating → ExcEnter/KDP-shim delivery → NK handler entry is implemented and has *single-datum*
   live proof in BOTH EE regimes (delivery #1 cold-EE, and one EE=1 delivery + full NK
   round-trip under the quarantined `SS_M6A_USER_MSR` diagnostic — srr1=0xd032). Everything
   downstream of the NK handler — the from-emulator handler arm, the 68k tick dispatch, and
   the entire EXC_EXTERNAL/PIC side — is either unexercised guest code or simply absent.

2. **EE rises by `rfi`, not by guest `mtmsr` — and the NK *forces* EE=1 for the 68k world.**
   [STATIC] The DR emulator region (0x360000–0x370000) contains **zero** mtmsr/rfi
   instructions: the 68k world cannot raise EE itself. The live `[KDP+0x5f0]` slot-exit
   selector **0x50313bf8 opens with `ori r11, r11, 0x8000`** — the NK unconditionally sets
   EE in the MSR image destined for the (re-)dispatched emulator world. That image today
   parks in ctx GPR slots (the bctr-based scheduler restore never touches MSR), so the live
   MSR stays 0x7072 forever; **EE first reaches the real MSR at the NK's first *rfi* whose
   SRR1 image carries 0x8000** (§B). The pending DEC latch is set and never cleared
   (`exc=0/1/0/0`): **the first EE rise is an instant delivery** at the next block boundary.

3. **The chain is *probably* ready for that instant delivery — but two specific links are
   unexercised and now ARMED differently than when delivery was last proven:** (i) the
   `[KDP+0x660]` flags word now carries the V-seed bit 0x00200000, so the next delivery
   takes the NK handler's **from-emulator arm** (R-9 — never executed; delivery #1 ran the
   flags=0 arm); (ii) the depth-deferral branch and the post-rfi re-deliver storm path have
   never run. §C's three levels exercise exactly these before the boot does.

4. **Housekeeping fact:** `make -C SheepShaver/src/machine test` is currently **broken** —
   commit `884586a5` pre-wired the `test_dev_openpic` target but `dev_openpic.cpp` /
   `test_dev_openpic.cpp` do not exist yet (Wave-2 deferred model). Individual targets
   build and pass (test_exc_core 25 checks, test_virt_clock 26 checks, verified this
   session). The owner should drop the target from `TESTS` until the model lands, or land
   the model. *(Update 2026-06-11: RESOLVED the same day — the OpenPIC model + 206-check
   suite landed (`b86449c9`); the suite is 12/12 ALL PASS.)*

---

## A. The chain map

Verdict key: **EXISTS-TESTED** (unit test or live falsifiable evidence cited),
**EXISTS-UNTESTED** (implemented, never exercised), **EXISTS-PARTIAL** (one arm proven,
the other not), **MISSING**.

| # | Link | Verdict | Evidence / gap |
|---|------|---------|----------------|
| 1 | Device IRQ output → PIC (EXC_EXTERNAL source) | **MISSING** | No PIC model in tree (`dev_openpic.cpp` absent; Makefile target pre-wired only, `884586a5`). VIA/SCC/Cuda raise nothing — the 68k boot world *polls* VIA IFR via the ROM trampolines 0x6d58/0x6e90 (+ SCC poller 0x6ea0) (`M6A-WAVE2-SHIM-RECON.md` night-run §3 [PROBE✓]). OpenPIC surface spec'd in donor study §1/Q6 (MacIO+0x40000, 0x40000 span); EXC_EXTERNAL wiring deferred by the M3b stop-rule disposition. `exc_core` *already* models the class (EXC_EXTERNAL shares `interrupt_entry`, vector 0x500) — tested at the math level only (test_exc_core test 5). |
| 2 | DEC latch arming (virt_clock) | **EXISTS-TESTED** | `virt_clock.cpp` pure module; gen-fused CAS arm word (rev-2 C1); lazy + scheduler expiry. Unit: `test_virt_clock` 26 checks (run green this session). Live: armed by `vclk_dec_arm` (`main_unix.cpp:1319`, wired :1817); latch observed set on every newworld boot (`deferred_ee=1` telemetry). **The latch is currently pending FOREVER on the live boot** — set at cold-init expiry, never cleared since `delivered=0`. |
| 3 | Deliverability gating | **EXISTS-PARTIAL** | `deliver_pending_dec_exception` (`sheepshaver_glue.cpp:784`) gates in order: latch → `execute_depth != 1` → `ExcDeliverable(msr)` (EE bit, `exc_core.h:113`) → `[XLM_RUN_MODE]!=0` W2 native-excursion fence. Poll site: `ppc-cpu.cpp:1649` (newworld-only, before the legacy HandleInterrupt arm). **EE gate**: unit-tested (test_exc_core test 1) + live (deferred_ee=1 on every boot). **Depth gate**: live-UNTESTED — `deferred_depth=0` on every boot ever (M3a carry-forward #1; the M3b "deliverability harness vector" is its named first exercise). **Native fence**: live-INERT so far (`deferred_native=0`; only verified not-firing in the [0x2810]=0 regime). No unit test covers the gating *composition* — the decision logic lives in glue, not the pure module ([GAP] — §C level 1). |
| 4 | Delivery transition (ExcEnter math) | **EXISTS-TESTED** | `exc_core.cpp` pure module: PEM masks as LAW, SRR0/SRR1/MSR/PC composition, per-class targets. Unit: test_exc_core 25 checks incl. the boot-real 0xf072→0x1040 transform and EXC_PC_UNRESOLVED guard. Live: delivery #1 `[EXC] DEC delivered #1: restart=50429b40 srr1=0000f072 msr=00001040 -> entry=50412b1c` (M3A-ENTRY-TABLE Task 7). |
| 5 | KDP register-save shim → NK entry 0x50412b1c | **EXISTS-TESTED (single-datum, stale conditions)** | Shim transcribed from `interrupt()` with the three honest upgrades (`sheepshaver_glue.cpp:~850ff`); zero-ECB abort guard; `SS_EXC_BARE=1` bare-transition experiment knob exists. Live proof ×2: delivery #1 (EE via cold fiction 0xf072, flags=0) and the rung-1 night run — **one EE-enabled delivery with srr1=0xd032 delivered AND the NK round-trip RESUMED the guest** (`M6A-WAVE2-SHIM-RECON.md` "Wave 2 night-run results"). **Caveat:** both data points predate the current armed state (MM switch default-ON, W2 [KDP+0x65c] world-flip, V-seeded flags word) — the shim's r6=[KDP+0x65c] save target now flips ECB↔MMCB per world, and R-14 records the open mid-switch window where [0x2810]=0 while the backward save is in flight (benign-by-same-values, never observed). |
| 6 | NK handler r7-flag tree ([KDP+0x660] consumption) | **EXISTS-PARTIAL (guest code; the armed arm is the UNTESTED one)** | Delivery #1 exercised the **flags=0** arm: handler ran, reprogrammed DEC (`mtspr_dec 3->4`), exited via the r7-flag `blr` path. Since Task V, `[KDP+0x660]` carries bit **0x00200000** ("from emulator", cr2eq) — pinned as the switch-service classifier ([PROBE-O4]), but **the *interrupt handler's* flag tests on a nonzero flags word have never executed** (residue R-9, M3A finding-4 watch item). [STATIC this session]: the bounce at 0x50312cb0 tests `andi. r8,r7,0x30` and branches into a CR-mutation leg (`rlwinm r7...`, the post-68k-interrupt bookkeeping family) — the paravirtual analogue posts the pending 68k level via `[KDP+0x67c]` + CR-bit into the saved ctx (design doc §3). **The next delivery runs this arm for the first time.** |
| 7 | NK → guest tick/event dispatch (the 68k consumption side) | **MISSING-in-practice** | Three suppressors stand: (a) **tm_task patch** (`rom_patches.cpp:3139ff`) NOPs Enable60HzInts — deliberately NOT retired (M6a Wave-2 #4: retirement needs the real delivery chain first); (b) **via_int/via_int2/via_int3 patches** (`rom_patches.cpp:3474–3510`) rewrite the VIA level-1/60Hz handlers to the paravirtual `moveq #2` + `M68K_EMUL_OP_IRQ` path — i.e. even a correctly NK-posted 68k interrupt dispatches into *paravirtual* handler code; (c) the 68k boot world's own device service is **polled**, not interrupt-driven (link 1). Meanwhile `HandleInterrupt`'s newworld MODE_68K arm is fenced to Ticks-only (`sheepshaver_glue.cpp:2374ff`) and MODE_NATIVE is fully fenced. **Nothing today converts a delivered DEC into guest tick/event processing.** |
| 8 | XLM_IRQ_NEST lifecycle | **EXISTS-UNOWNED** | Parked at 0xFFFFFFFF on the newworld boot (M3A finding 3): the slot-0 stub increments (`rom_patches.cpp:2026ff`), the 0x318000 restore-tail stub decrements — historically 1 restore / 0 entries. **Since the MM switch went default-ON, warm switch-backs traverse the slot-0 stub (nest++) and scheduler restores decrement — the live balance is now unknown** ([GAP]: one WATCH probe, addr 10264 = 0x2818). Consumers: `HandleInterrupt` early-out `>0` (`glue:2360` — treats 0xFFFFFFFF-as-negative as "enabled"); the tick-thread trigger gate `==0` (`main_unix.cpp:2446` — permanently false ⇒ **no 60 Hz re-trigger safety net exists on newworld**; the EE-edge re-raises are load-bearing). The delivery hook itself deliberately IGNORES it (real MSR[EE] gate instead, `glue:773`). Nobody resets it; drift to a positive value would silently starve `HandleInterrupt`'s Ticks++ keep-set. |
| 9 | Guest EE arming (who sets EE=1) | **EXISTS(guest)-NEVER-EXECUTED** | §B in full. [STATIC]: DR region has zero mtmsr/rfi; the NK's ~40 mtmsr sites inspected are FP-bit (0x2000) and critical-section restore toggles, not EE-setters; the NK's 17 rfi sites restore SRR1 images. The single architectural EE-*forcer* found: `ori r11,r11,0x8000` at **0x50313bf8** ([KDP+0x5f0] selector — on the live warm switch-back path, [PROBE✓] traversed). The live MSR has been 0x7072 at every observation since the cold fix (sc wall SRR1=0x00007072). |
| 10 | EE-edge re-raise plumbing (host side) | **EXISTS-UNTESTED** | Three re-raise sites: `execute_mtmsr` 0→1 edge (`ppc-execute.cpp:1336–1362`), `execute_rfi` 0→1 edge via ExcRfi (`ppc-execute.cpp:1639–1660`), nested-execute-return recheck (`glue:744–748`). All newworld-gated, all check `VirtClockDECPending`. **None has ever fired with effect on a default boot** (no EE edge has occurred); the rung-1 night run is the lone (env-gated) proof the mtmsr edge → delivery path composes. No unit or harness test exists for the edge *predicate* ([GAP] — §C levels 1–2). |
| 11 | EXC_EXTERNAL delivery (PIC → hook) | **MISSING** | `deliver_pending_dec_exception` consumes ONLY the VirtClock DEC latch by contract (`glue` contract note: "InterruptFlags/VIA stays with HandleInterrupt until M3b's PIC"). No external-source latch, no PIC→spcflags assertion path (the §2g rule 3 design exists on paper only). The NK's *published* external-interrupt handler is `[KDP+0x374]=0x50314880` (≠ the DEC delivery target 0x50412b1c; reconciliation explicitly out of scope since the syscall recon). Entirely Wave-2 backlog. |

**One-line summary per link:** 1 MISSING (no PIC) · 2 TESTED (vclk latch) · 3 PARTIAL (EE
gate proven; depth/native gates never fired; no composition test) · 4 TESTED (exc_core math)
· 5 TESTED-once-each-regime, stale vs current armed state · 6 PARTIAL (flags=0 arm proven;
the now-armed from-emulator arm never run = R-9) · 7 MISSING-in-practice (tm_task/via_int
patches + polled dispatch swallow the guest side) · 8 UNOWNED (nest counter balance unknown
since MM-switch flip) · 9 NEVER-EXECUTED (EE has never risen; the NK forces it at 0x313bf8)
· 10 UNTESTED (re-raise edges) · 11 MISSING (EXC_EXTERNAL).

---

## B. Where EE first rises — static evidence and verdict

### B.1 The instruction inventory [STATIC, /tmp/rom901.bin]

- **DR emulator region 0x360000–0x370000: zero `mtmsr`, zero `rfi`.** The 68k world cannot
  change MSR. Its "MSR" is the fiction the slot stubs compose into r11 (`0x0002f072` —
  note **EE=1 in the fiction**) and the `SS_M6A_USER_MSR` UserModeMSR `0xd032` (also EE=1,
  + PR=1) — the paravirtual design *always claimed* the emulator world runs interrupts-on.
  The 68k SR lives in r25/cr2 and is unrelated to MSR; **a 68k RTE cannot raise EE** —
  rule that candidate out.
- **NK mtmsr sites (~40, region 0x310000–0x325000):** every site inspected is either an
  FP-availability toggle (`mfmsr; ori rX,rX,0x2000; mtmsr` around the FP save/restore
  helpers — e.g. 0x323994, 0x324838, 0x313dd4) or a critical-section save/restore pair
  (mtmsr r14/r15 families, 0x3133xx–0x3174xx). None composes 0x8000 into the value.
  (Inspection sampled the clusters, not all 40 — recorded honestly; none of the uninspected
  sites sits on a path the boot currently reaches.)
- **NK rfi sites (17):** all restore an SRR1 image (`mtspr SRR1, rX; ... rfi`); e.g. the
  interrupt-exit tails 0x313744 and 0x3141f8, the vector-stub fast exits 0x314ae8–0x314bd8
  (syscall negative selectors), the template page 0x301060/0x301158/0x301264. An rfi raises
  EE iff its SRR1 image has bit 0x8000.
- **The one architectural EE-forcer:** the `[KDP+0x5f0]` slot-exit selector
  **0x50313bf8 begins `ori r11, r11, 0x8000`** before its `mtcrf 0x3f, r7` classifier
  [STATIC, confirmed this session]. r11 at that point is the stub-composed MSR fiction for
  the world being (re-)entered. **The NK's design: the emulator world runs EE=1.** This
  instruction executes on the LIVE boot today (the warm switch-back traverses slot-0 stub →
  0x50313bf8, [PROBE✓] M6a rung 2 Q-C) — but on the legs the boot currently takes, r11
  flows into ctx GPR slots (r11→ctx+0x15c on the 0x313cf8 save path; observed parked as
  `[ECB+0xE4]=0x0002f072`, Task W) and the resume is the **bctr-based scheduler-restore
  tail (0x3244cc: `mtlr r12; mtctr r10; mtcrf; bctr`) which never writes MSR.** That is
  why the live MSR is sticky at 0x7072 and `delivered=0` — fully consistent telemetry.

### B.2 Who first executes an EE=1 rfi (the candidates, ranked)

1. **The NK's rfi-based context resume with an EE=1 SRR1 image — the designed path, gated
   on residue R-7.** The MixedMode/task ctx SRR1 slot contents (what the eventual NK rfi
   consumes) were explicitly NOT pinned in rung 2 (R-7: "the ctx SRR1 slot/value the NK rfi
   uses... remains unresolved"). The 0x313bf8 EE-force is the obvious *producer* of EE=1
   images for emulator-world contexts. First plausible execution window: **when the NK
   first resumes a context via an rfi tail instead of the bctr tail** — the interrupt-exit
   rfis (0x313744/0x3141f8) require a delivery to have happened (chicken-and-egg vs EE for
   DEC... but NOT for `sc`), and the vector-stub rfis run on syscall fast-negative
   selectors.
2. **The syscall surface (just armed, commit `bcce26c2`) is the likeliest *trigger* even
   though it does not itself raise EE.** `sc` delivery is EE-independent. MPLibrary's
   kernel-service phase (selector family — Q-S3 found siblings 6,7,8,9,0xa,0xc,0xd,0xe,0x63
   queued behind 0x3f) is exactly where classic NK kernels create/dispatch tasks and enable
   interrupts: the first service that **builds or dispatches a context through an rfi-based
   resume, or whose service body writes an EE=1 MSR image**, raises EE. Static bound: the
   0x3f body touches no MSR; the other selectors are next-milestone recon (recorded residue
   in the syscall memo). **Prediction: EE first rises during the post-sc-wall MPLibrary/NK
   service phase, via an NK rfi restoring an SRR1/ctx image whose EE bit traces back to the
   0x313bf8 force or to a service-supplied MSR.**
3. **The quarantined diagnostic:** `SS_M6A_USER_MSR=1` (trampoline `mtmsr 0xd032` at
   table[0] entry) — *empirically proven* to raise EE and trigger an instant, successful
   delivery + NK round-trip (the rung-1 night-run datum). Known-broken side effect:
   the un-root-caused zero-page slide (R-2). It remains the only on-demand EE-rise lever
   we own.
4. **Ruled out:** 68k RTE (no MSR access, B.1); the DR mtmsr toggles (EE-preserving);
   the scheduler bctr restore (no MSR write); `ExcRfi` on the sc fast path (restores the
   caller's own EE=0).

### B.3 What happens at the first EE rise — and is the chain ready?

The DEC latch is pending **forever** (link 2). The instant EE reaches the real MSR:

- If by guest `rfi` → `execute_rfi`'s edge check fires `trigger_interrupt()` → next
  block-boundary poll → `deliver_pending_dec_exception` passes all four gates → **instant
  delivery**, SRR0 = the just-rfi'd-to PC, handler entry 0x50412b1c with the KDP shim.
- If by guest `mtmsr` → same via `execute_mtmsr`'s edge (mtmsr is interpreter-only and
  block-ending, so the poll is immediate).

Readiness assessment, honestly:

| Sub-question | State |
|---|---|
| Transition math + entry resolution | Ready (link 4/5, twice-proven) |
| Shim save target under the W2 world-flip | *Architecturally* right ([KDP+0x65c] = interrupted-world ctx by design) but **delivery has never run since the flip landed**; R-14's mid-switch window is open-by-design and unobserved |
| NK handler arm | **Changed under our feet:** flags word now nonzero (V seed) → the unexercised from-emulator arm runs (R-9). Plausible behavior: posts 68k level via [KDP+0x67c] + CR-bit — into a world whose via_int handlers are paravirtual-patched (link 7). Outcome unknown — THE candidate M3-class surprise. |
| Post-handler resume | bctr restore tail with shim r10=r12=restart — proven by construction + night-run |
| Repeat deliveries | DEC reprogram by the handler proven once (delivery #1: `mtspr_dec 3->4`); the steady 60 Hz-class cadence (deliver→rfi→EE=1→next expiry) never sustained |
| Guest tick consumption | Not ready (link 7 MISSING-in-practice) — deliveries will succeed *mechanically* while the guest's timer/event service stays starved until tm_task/via_int retirement + (eventually) EXC_EXTERNAL |

**Verdict for (B):** the earliest plausible EE-rise point is **an NK rfi during the
post-syscall-wall kernel-service phase** (candidate 1 triggered by candidate 2), with the
EE bit originating at the 0x50313bf8 `ori r11,r11,0x8000` force or a service-built ctx
image. Delivery mechanics will likely survive it; **the unexercised from-emulator handler
arm (R-9) and the starved guest tick path (link 7) are where it can go wrong silently** —
which is exactly why the chain must be exercised first (§C).

---

## C. The test spec — three levels

### Level 1 — unit (machine suite, no emulator): the deliverable-decision chain as a pure state machine

**Blocker to remove first:** the gating *composition* (latch × depth × EE × run-mode fence,
plus the EE-edge re-raise predicate) lives inline in `sheepshaver_glue.cpp` /
`ppc-execute.cpp` and cannot be unit-tested. **Spec: extract a pure decision helper into
exc_core** (no guest-memory reads — the caller samples `[XLM_RUN_MODE]` and passes it in):

```c
/* exc_core.h */
enum ExcDecision { EXC_DECIDE_NONE, EXC_DECIDE_DEFER_DEPTH, EXC_DECIDE_DEFER_EE,
                   EXC_DECIDE_DEFER_NATIVE, EXC_DECIDE_DELIVER };
ExcDecision ExcDeliveryDecision(int pending, int execute_depth,
                                uint32_t msr, uint32_t run_mode_word);
/* EE-edge re-raise predicate (mtmsr/rfi/nested-return all share it): */
int ExcEdgeReRaise(uint32_t old_msr, uint32_t new_msr, int pending);
```

Glue then calls these (behavior-identical refactor — the gate ORDER is part of the contract
because the `exc=` telemetry tuple counts per-gate; pin it: pending → depth → EE → native).
This touches `sheepshaver_glue.cpp`/`ppc-execute.cpp` → **owned by the emulator agent; land
post-syscall-milestone.** The test file (`test_exc_chain.cpp`, linking exc_core + virt_clock)
is standalone.

**Case table (each one CHECK-counted, test_exc_core style):**

| ID | Case | Assert |
|---|---|---|
| U1 | pending=0, everything else permissive | NONE; no state consumed |
| U2 | pending=1, depth=2, EE=1, mode=0 | DEFER_DEPTH (depth outranks EE — telemetry contract) |
| U3 | pending=1, depth=1, EE=0, mode=0 | DEFER_EE |
| U4 | pending=1, depth=1, EE=1, mode≠0 | DEFER_NATIVE (the W2 fence; EE outranks native) |
| U5 | pending=1, depth=1, EE=1, mode=0 | DELIVER |
| U6 | deferral retains the latch | after U2/U3/U4 with a real VirtClock: `VirtClockDECPending` still true; after U5 + `VirtClockClearDECPending`: false (delivery consumes exactly once) |
| U7 | edge predicate truth table | re-raise iff (old EE=0 ∧ new EE=1 ∧ pending): test 0→1/pending=1 (fire), 0→1/pending=0, 1→1, 1→0, 0→0 (no fire); non-EE bit churn with EE stable (no fire) |
| U8 | mtmsr re-raise contract | ExcEdgeReRaise(0x7072, 0xf072, 1)=1; (0x7072, 0xd032, 1)=1 (the night-run pair); (0xf072, 0x7072, 1)=0 |
| U9 | rfi composition | ExcRfi from srr1=0xf072 at handler msr 0x1040 → new_msr EE=1; feed old/new into ExcEdgeReRaise with pending=1 → fire (the §B.3 instant-delivery scenario as math) |
| U10 | full state-machine round trip ("the storm") | VirtClock armed → expire → pending; EE=0 → DEFER_EE; mtmsr-edge fires; DELIVER → ExcEnter(0x...,0x...f072,DEC) → handler msr EE=0 → second expire → DEFER_EE at handler → ExcRfi back → edge fires → DELIVER again. Asserts the deliver→rfi→deliver cadence terminates each cycle and never double-consumes the latch |
| U11 | [0x2810]-fence release | mode=1 → DEFER_NATIVE; mode→0 (the NK's `stw r8,0x2810(0)` at 0x50312b04 switch-back clear, modeled as input) → DELIVER on next poll |
| U12 | EXC_EXTERNAL parity (future PIC) | decision logic identical for an EXT-pending input; ExcEnter(...,EXC_EXTERNAL) hits interrupt_entry with same masks (extends test_exc_core test 5 into the chain test) |

Sizing: **S** (half-day incl. the extraction refactor + suite run). Also fold in: fix or
drop the broken `test_dev_openpic` Makefile target (§0.4).

### Level 2 — harness vector (SS_TEST_HEX-level, no boot): the M3b-backlog "deliverability harness vector," made concrete

**Goal:** run real `mtmsr`/`rfi` instructions through the real interpreter (and the JIT's
block-ending fallback path), with a real pending latch, and assert the full delivery
transition (SRR0/SRR1/MSR/PC per exc_core) from the REGDUMP — interp vs JIT diffed as usual.

**What exists already (verified this session):**
- Multi-word vectors: `ss_run_one_vector` (`glue:1455ff`) injects N words at guest
  0x10004000, appends `blr`, EXEC_RETURN sentinel at 0x10008000; REGDUMP emits
  GPR0-31/CR/LR/CTR/XER/FPR/VR (`glue:1713ff`).
- Profile forcing: **`SS_MACHINE=newworld` wins over everything**
  (`machine_profile.cpp:35`) — the harness CPU can run the newworld delivery hook
  (`ppc-cpu.cpp:1649`).
- Entry-table override: `SS_EXC_ENTRY=0xINT,0xSC` env (glue init) — point
  `interrupt_entry` at a **test-RAM handler stub**.
- Shim bypass: **`SS_EXC_BARE=1`** applies only the architectural transition — essential
  here (test RAM has no KDP/ECB; the shim would abort on `[KDP+0x65c]=0`).
- mtmsr/rfi are interpreter-executed and block-ending in JIT mode → the poll site is
  reached in both modes; `execute_mtmsr`/`execute_rfi` carry the edge re-raises.

**What's missing (the infra to add — small, all in the harness path):**
1. **Latch injection.** The harness never runs `VirtClockInit` (`main_unix.cpp:1817` is on
   the boot path; the SS_TEST gate at `main_unix.cpp:1440` exits first) — `ss_vclk_active()`
   and `VirtClockDECPending` see a dead clock. Knob: `SS_TEST_DEC_PENDING=1` → in
   `ss_run_one_vector` setup, `VirtClockInit(&g_virt_clock, 25000000, stub_now_ns, 0)` +
   set `dec_pending=1` (and reset per vector in batch mode).
2. **Initial-MSR control.** `reset_supervisor_for_test` pins MSR=0xf072 (**EE=1**) — the
   pending latch would deliver at the FIRST poll, before the vector's edge. Knob:
   `SS_TEST_MSR=0x7072` (hex, default 0xf072 for byte-compatible legacy vectors).
3. **REGDUMP has no MSR/SRR0/SRR1/PC** — *no change needed*: capture via the handler stub
   (below) into GPRs.
4. **Harness lane.** These vectors are env-dependent (SS_MACHINE/SS_EXC_*/SS_TEST_*) — they
   must NOT enter the 353-vector table (whose contract is env-free determinism). New make
   target `test-exc-vectors` (a dedicated run.sh stanza or sibling script) running each
   vector interp-vs-JIT and diffing REGDUMPs, METRIC-formatted.

**The vectors (env: `SS_MACHINE=newworld SS_EXC_BARE=1 SS_TEST_DEC_PENDING=1
SS_TEST_MSR=0x00007072 SS_EXC_ENTRY=0x1000C000,0`):**

Handler stub planted by the vector itself at guest 0x1000C000 (the harness writes only the
code words; so the stub words are part of the vector, stored via the vector's own stores —
OR simpler: extend the knob set with `SS_TEST_EXC_STUB=1` to plant
`mfmsr r20; mfspr r21,srr0; mfspr r22,srr1; blr` at 0x1000C000 host-side):

| ID | Vector body (at 0x10004000) | Expected REGDUMP assertions |
|---|---|---|
| H1 mtmsr-edge | `lis r3,0; ori r3,r3,0xf072; mtmsr r3` | delivery fires at the post-mtmsr boundary: r20 = 0x1040 (0xf072 masked — the boot-real transform), r21 = 0x1000400C (the not-yet-executed next insn = restart PC), r22 = 0xf072; LR intact → stub blr → sentinel exit |
| H2 no-edge control | same but `ori r3,r3,0x7072` | NO delivery: r20/r21/r22 = 0 (untouched); proves the edge predicate, anti-vacuous vs H1 |
| H3 rfi-edge | `mtspr srr0, rA(=0x10004010); mtspr srr1, rB(=0xf072); rfi` (with start MSR 0x7072) | rfi restores EE=1 → edge → delivery at the rfi target: r21 = 0x10004010, r22 = 0xf072, r20 = 0x1040 |
| H4 deferral telemetry | H2 followed by reading the exc counters | optional: assert `SheepExcGetStats` deferred_ee incremented (needs a dump hook; nice-to-have, not gating) |
| H5 sc-class regression | `sc` with `SS_EXC_ENTRY=0,0` unresolved vs resolved-to-stub | guards the EXC_PC_UNRESOLVED abort path + SRR0=sc+4 ownership end-to-end (complements the syscall milestone's live gates) |

Depth-deferral (`execute_depth>1`) is **not harness-reachable** (needs a nested
EMUL_OP execute) — covered at level 1 (U2) and level 3 (telemetry), recorded honestly.

Sizing: **S–M** (≈1 day: 3 small knobs in the harness path + stub planting + the make
lane). Files touched: `sheepshaver_glue.cpp` (harness block only), `Makefile`,
new script — owner-scheduled.

### Level 3 — live probe plan (boots — DEFERRED, spec only)

One sanctioned session when the emulator frees up; diagnostic prefs
(`/tmp/m2accept.prefs`-class), bounded ≤4 boots.

**Boot P1 — baseline census (no behavior change, pure probes):**
```
SS_PROBE_PC='0x50313bf8:r3,r7,r11;0x50412b1c:r6,r7,[0x68ffe660],[0x68ffe65c]' \
SS_JIT_WATCH_ADDR=10264 SS_JIT_WATCH_DUMPS=0   # 10264 = XLM_IRQ_NEST 0x2818
```
Pins: (a) the 0x313bf8 EE-force live (r11 before/after class, selector r3 per visit);
(b) `[KDP+0x660]` live value at delivery-entry (the R-9 arm input); (c) **the nest-counter
balance post-MM-switch-flip** (link 8 gap) — watch hits should alternate ±1 around a stable
value; monotonic drift = finding.

**Boot P2 — the forced-EE experiment (the §B verdict, falsifiable now):**
`SS_M6A_USER_MSR=1` (the existing, quarantined lever — re-run deliberately as the EE-storm
experiment now that the MM switch + sc surface have landed; its rung-1 run predates both)
plus the P1 probe set and `SS_PROBE_PC=0x50429b40` (trampoline mtmsr site).
- **READY signature:** heartbeat `exc=` delivered_dec climbing (≥1 per DEC reprogram
  cycle); ring shows continued 68k execution (NO `100266f2 → 0 → 1` reset signature);
  comp/jDR climbing; `[KDP+0x67c]`-target writes appear (the from-emulator arm posting);
  boot frontier ≥ the sc wall.
- **BROKEN signatures (each maps to a chain link):** SIGSEGV with ea ≈ small-negative
  guest (the R-2 zero-page slide recurring — trampoline, not chain); 68k reset ring (the
  handler arm corrupted the world — link 6/R-9); deferred_native climbing with
  delivered flat (the [0x2810] fence stuck — NK never clearing on this path — link 3);
  nest drifting positive then Ticks frozen (link 8); MMCB/ECB save-slot clobber probed at
  [ECB+0xfc]/[MMCB+0xfc] across a delivery landing inside a switch window (R-14 made real —
  link 5).
- Budget rule: ONE falsification → stop, record, re-scope (the established one-iteration
  discipline).

**Boot P3 (alternative/cheaper lever, if P2's R-2 slide blocks):** re-propose the dropped
`SS_EXC_FORCE` knob (M3a "planned debug knob dropped") in a sharper form:
`SS_EXC_FORCE_EE_AT=0xPC` — at the first block-entry visit of PC, OR 0x8000 into the live
MSR (host-side, one line in the probe hook) — i.e. an EE-rise at a *chosen, well-understood*
site (e.g. the post-switch-back 68k resume) instead of every table[0] entry. Owner lands;
S.

**Boot P4 — depth-deferral live closure:** any P2/P3 boot doubles as this: a delivery
request landing during an EMUL_OP nested execute increments deferred_depth; first nonzero
value closes M3a carry-forward #1. No extra instrumentation needed (heartbeat tuple).

---

## Milestone recommendation

**Fold the EE chain into M3b Wave 2 (the OpenPIC milestone) as its FIRST two rungs — do
not spin a standalone plan.** Rationale: every missing link is already a named Wave-2
backlog item (OpenPIC model + EXC_EXTERNAL wiring, tm_task/via_int retirements,
SDL_PumpEvents relocation, the deliverability harness vector, nested-execute completion);
a separate EE plan would duplicate that scope. But re-order Wave 2 so the *verification*
rungs precede the *construction* rungs — the chain must be proven under the current armed
state before the PIC adds a second source:

| Rung | Work | Links closed | Size |
|---|---|---|---|
| W2-0 | Level-1 decision extraction + test_exc_chain + fix the broken `make test` (openpic target) | 3 (composition), 10 (edge predicate), housekeeping | **S** |
| W2-1 | Level-2 harness lane (3 knobs + 5 vectors) | 4/5/10 end-to-end without boots; the M3b-backlog vector item | **S–M** |
| W2-2 | Level-3 probe session (P1 census + P2 forced-EE; P3/P4 contingent) | 6 (R-9 arm), 8 (nest balance), 5 (post-flip delivery), §B verdict falsified-or-confirmed | **S** (one session) |
| W2-3 | OpenPIC model + EXC_EXTERNAL source latch + wiring to the (now-tested) decision chain | 1, 11 | **M** (2–3 d; donor-study surface is spec-ready) |
| W2-4 | tm_task/via_int retirement A/B + tick restoration through real delivery; SDL_PumpEvents relocation | 7, 8 retirement | **M**, gated on W2-2/W2-3 evidence |

Scheduling note: W2-0/W2-1 are independent of the syscall milestone's files only at the
test-file level — the extraction touches `sheepshaver_glue.cpp`/`ppc-execute.cpp`, so they
queue behind the current syscall work for the same owner. W2-2's P2 may fire the next
M3-class surprise *on purpose, in a bounded session* — which is precisely the de-risking
Re-score #2 asks for: better there than mid-boot during M4/M5.

---

## W2-2 static pre-recon (Q-W1 / Q-W2 / Q-W3 + publication-table bonus) — 2026-06-11

> **Status:** static halves of the Wave-2 plan's Task W2-2 recon questions, front-loaded
> while W2-0/W2-1 run (the questions don't depend on them). **READ-ONLY session — zero
> boots used, zero source edits.** The P1/P2 boot legs of W2-2 remain owed; every claim
> below that needs live confirmation carries an explicit residue.
> **Provenance:** `/Users/Shared/macemu/dumps/` manifest check PASSED this session
> (`rom901_inventory.bin` md5 7b1378be… [RAW-ROM]; `rom901.bin` md5 e432df64… [PATCH]).
> All windows disassembled from the PATCHED image (what the guest executes), capstone PPC
> BE, base 0x50000000; **every window was raw-vs-patched diffed** — divergent words are
> called out inline as [PATCH-DIVERGENT] with the owning `rom_patches.cpp` patch named.
> Staged-copy note: the delivery target 0x50412b1c is the staged (+0x100000) copy of
> static 0x312b1c; M3A pinned 16 live words byte-identical at 0x50412b0c–48 [PROBE✓].
> Branches inside the NK are relative ⇒ the staged copy self-references (+0x100000
> throughout). Per-target byte re-confirmation beyond those 16 words = P1 residue (T-M1).

### W2S-0. The load-bearing frame: the paravirtual patch seams ARE the chain's constants

Diffing every window raw-vs-patched surfaced something the chain map only knew as live
behavior — **three of the chain's standing mysteries are our own `rom_patches.cpp` edits
to the NK, not NK design**:

| Site (static) | RAW instruction(s) | PATCHED to | Patch (rom_patches.cpp) | Consequence |
|---|---|---|---|---|
| 0x3244d8–e0 (interrupt/trap exit tail) | `mtspr SRR0,r10; mtspr SRR1,r11; rlwinm…` (→ rfi exit) | `mtctr r10; mtcrf 0xff,r13; b 0x318000` | **`trap_return`** (:2417–2437; also rewrites the fast `lwz r6,0x18(r1); lwz r1,4(r1); rfi` at 0x324524 → `bctr`) | **THE EE-parker.** The raw NK resumes interrupted contexts via `rfi` with SRR1=r11 (the 0x313bf8 `ori r11,r11,0x8000`-forced image) — i.e. the kernel's DESIGNED exit raises EE. Our patch reroutes through the 0x318000 stub (`XLM_IRQ_NEST--; b 0x3244e4`) and a `bctr` (CTR=r10=resume PC), never writing MSR. §B's "EE first rises at the NK's first rfi" stands, but the reason no such rfi happens on the interrupt path is THIS patch. [STATIC] |
| 0x312b00–04 (central-dispatcher save entry) | `addi r8,r1,0x360; mtspr SPRG3,r8` (swap SPRG3 → KDP+0x360 native vector table) | `li r8,0; stw r8,0x2810(0)` | **`m68k_excp_tbl`** (:2370–2381) | The famous `[0x2810]`-clear "switch-back fence write" (U11's modeled input) is a PATCH laid over the NK's **vector-regime swap**. Raw semantics: entering this path re-arms the NATIVE-regime vector table. |
| 0x31154c (publication sequence) + 0x3113b8–c8 (init) | `mtspr SPRG3,r9` / `mtspr SPRG3,r8` + MQ probe | NOPs | **`sprg3` / `sprg3_mq`** (:1641–1666) | SPRG3 is NOT seeded at NK init on the live image. The M3A line "NK init sets SPRG3=KDP+0x360 ([STATIC] 0x3113b4)" cites a patched-OUT instruction — the +0x360 publication TABLE is real and probed, but live SPRG3 content is unpinned (other mtspr SPRG3 sites survive un-patched, e.g. 0x312db4, 0x312984). Residue W2S-R6. |
| 0x312bd4 / 0x312c24 (`bnel`→`bl` 0x313ecc / 0x313e20), 0x313ecc/ed4/edc + 0x313e28–38 (mfmsr/mtmsr/isync→NOP) | conditional FP save/restore + MSR[FP] enable | unconditional, MSR untouched | **`save_fpu_caller` / `restore_fpu_caller` / `save_fpu` / `restore_fpu`** (:2304–2404) | FP state always saved/restored across the save/switch path; no MSR writes. Benign for W2; listed for window honesty. |

Everything else cited below is raw==patched unless tagged.

### W2S-1. Q-W1 — the delivery-target decode and the r7-flag tree

**Identity of 0x50412b1c [STATIC]:** it is NOT a published vector handler. Static
0x312b1c is four instructions into the NK's **context-SAVE-and-SWITCH routine**
(0x312b0c–0x312bfc): save the interrupted world into the ctx in **r6** (r17–r31,
r2–r5, XER/CTR via the slots M3A documented; the four pre-entry words 0x312b0c–18 store
r7/[KDP-0xc] through r6 — the host shim's KDP writes substitute for them), FP save
(`bl 0x313ecc`, [PATCH-DIVERGENT] unconditional), `stw r11,0xa4(r6)` (the SRR1/MSR
image parks in **ctx+0xa4** — R-7's static half: THIS is the slot the raw rfi tail
would consume via r11), then **switch**: `lwz r8,0(r9); stw r9,-0x14(r1)`
([KDP-0x14] := r9 = the NEW current ctx), `xoris r7,r7,0x80` (toggle flags bit
0x00800000 — the world-parity bit flips on every switch), `rlwimi r11,r8,0,0x14,0x17`
+ `rlwimi r7,r8,0,0x11,0x1f` (new ctx word[0] supplies MSR-image bits 0x0F00 and the
**low 15 flag bits**), `mr r6,r9`, then a full register RESTORE from the new ctx
(0x312c00–0x312cac) falling into the bounce at **0x312cb0**.

**The canonical (non-shim) entry** is the central event dispatcher at **0x312ab4**:
`mtcrf 0x3f,r7` (CR fields 2–7 := flag bits), `rlwimi r7,r8,0x18,0,7` (**event code
r8 → r7 top byte**), per-code counter `[KDP+0xdc0+code*4]++`, then:
`blt cr4 → 0x314a38` (flags bit 0x8000: alternate-space fast return);
`bne cr2 → 0x312f70` (**bit 0x00200000 CLEAR** ⇒ "not from emulator" leg — task-level
accounting + lock path); code<0xc → 0x312a28 (extended-state save tail); code==0xc →
0x312f70; else fall to 0x312af8: `r9 := [KDP+0x658]`, the [PATCH-DIVERGENT] `[0x2810]:=0`
(raw: SPRG3 swap), `bltl cr2 → 0x312dd4` (**bit 0x00800000 set** ⇒ kernel-notify call),
then the SAVE-and-SWITCH above. **So on the canonical path, r9 = [KDP+0x658] (the
scheduled-context slot) and the CR is flags-composed before entry.**

**The r7-flag tree at the bounce (0x312cb0/0x312ccc) — the table:**

| Condition on r7 (post-switch flags) | Arm | What it does |
|---|---|---|
| `r7 & 0x30 == 0` | fast resume | `[KDP-0x10]:=r7; [KDP-0x114]:=0; b 0x3242a8` → reschedule gate: if `r7&0x8000` or `[KDP-0x118]==0` → restore tail 0x3244cc (`mtlr r12; mtctr r10; mtcrf 0xff,r13; b 0x318000` [PATCH-DIVERGENT: raw = SRR0/SRR1+rfi] → `XLM_IRQ_NEST--` → reload r0/r6–r13 from ctx → `bctr` to r10); else (`[KDP-0x118]` byte set, set by the EXT 0x8da1 return) → **scheduler**: lock 0x312700, `[KDP+0xee4]++`, `bl 0x324a98` |
| bit 0x10 set, bit 0x01 clear | post-flags consume | clear 0x10 (+0x20), `blr` back into the fast-resume store |
| bit 0x10 set, bit 0x01 set (heavy leg 0x312cfc), bit **0x00800000 set** | **68k-interrupt post & dispatch** (0x312d04) | copy ctx volatiles into the KDP frame, r25 := `[KDP+0x650]` composed with r17 (68k SR) / r19 bits, `lhz r26,0xd20(r25)` (pending-level halfword) + `mtcrf 0x10,r26`, `lha r22,0xc00(r25)`+`add r22,r25` (dispatch-offset table), **`mtspr SPRG3, KDP+0x4e0`** (swap to the 68k-regime vector table; old SPRG3 → r24), set MSR[DR] (`ori 0x10` + isync), `bnelr` → the 68k emulator's interrupt dispatch; fall-through → 0x31591c = **`li r0,-3; sc`** (NK self-call, fast-negative selector −3) |
| heavy leg, bit 0x00800000 CLEAR | kernel-task notify (0x312dd4) | `bl 0x3238ac` (nonvol save), ID-directory lookup `bl 0x325380` on `[r31+0xf4]`, lock 0x312700 — the kernel-object notification family (same lock/directory machinery as the sc services) |
| bits 0x10 AND 0x20 both set at 0x312ce4's recheck | double-event error | `li r8,8; b 0x312ab4` — re-dispatch as event code 8 |

**The R-9 arm (bit 0x00200000 set) — static behavior:** the bounce tree itself **never
tests 0x00200000**. The bit is consumed at: (a) 0x3143a0's `bnel cr2` (pinned, rung 2);
(b) the dispatcher's `bne cr2 → 0x312f70` ("not-from-emulator" leg) and `bltl cr2`
(that one is cr2.lt = bit 0x00800000); (c) **the NK's own 68k-post service at 0x3254e0**:
`r23:=[KDP+0x67c]`, compose `r28 := level|0x8000`, **test `rlwinm. r8,r7,0,0xa,0xa`
(bit 0x00200000) — beq SKIPS the post** — else `sth r28,0(r23)` (store pending level
through the [KDP+0x67c] pointer — byte-identical in effect to paravirtual
HandleInterrupt's `WriteMacInt16([[KDP+0x67c]],1)`) and OR `[KDP+0x674]` (CR mask) into
r13 (the saved CR) / AND `[KDP+0x678]`. Callers: 9 tail-branches from the 0x3258e0–0x326380
service bodies + init-time `bl 0x3254a0` ×2. **[KDP+0x67c]-target-class answer: the NK
posts 68k interrupts exactly the paravirtual way — pending-level halfword via the
[KDP+0x67c] pointer + [KDP+0x674] CR-mask OR into the saved CR — and the V-seed bit
0x00200000 is a PRECONDITION for that post.** So the armed bit does not change the
delivery-time arm; it ENABLES the NK's posting of 68k interrupts from its service legs.

**Three shim-vs-canonical mismatches found at the 0x412b1c entry (P1/P2 must watch):**
1. **r9 is NOT seeded by the M3a shim** (glue:920–957 re-read this session: r1/r6/r7/r8/
   r10/r11/r12/r13 only). The entry path consumes r9 as the new ctx (`[KDP-0x14]:=r9`,
   full register restore from it). On the canonical path r9=[KDP+0x658]. A delivery with
   live-garbage r9 corrupts [KDP-0x14] and restores garbage registers. Delivery #1
   survived — r9's live value at the trampoline was never probed. **P1 probe upgrade:
   add `r9` (and `[0x68ffe658]`) to the 0x50412b1c probe fields.**
2. **cr6/cr7 are caller-CR garbage at shim entry** (the CR splice covers fields 1–3
   only). The save's `bns cr6` arm (68k extra-register save) and the bounce's first
   `mtcrf 0x3f,r7` re-derivation mostly mask this, but the pre-bounce conditional saves
   key off live cr6 — flags-vs-CR consistency is NOT established by the shim.
3. **Every delivery through the patched restore tail decrements XLM_IRQ_NEST** (the
   0x318000 stub — staged copy 0x418000) **with no matching increment on the shim path**
   ⇒ each delivered DEC drifts the nest counter −1 (0xFFFFFFFF → 0xFFFFFFFE → …).
   Harmless to `HandleInterrupt`'s `>0` early-out (stays negative) but it is the
   pre-named link-8 drift signature: P1's `SS_JIT_WATCH_ADDR=2818` (hex, F3) should see
   exactly one −1 step per delivery. [STATIC]

### W2S-2. Q-W2 — IACK/EOI and source discrimination: the verdict

**The EXT body (0x314880, primary; raw==patched) [STATIC]:**
```
bl    0x313d40            ; the SHARED save prologue (same as sc/program):
                          ;   [KDP+4]:=SPRG1, [KDP+0x18]:=r6, r6:=[KDP-0x14] ctx,
                          ;   r0,r7..r13 -> ctx+0x104/0x13c..0x16c, r10:=SRR0, r11:=SRR1,
                          ;   r13:=CR, r12:=SPRG2, r7:=[KDP-0x10]  <-- FLAGS SELF-LOADED
rlwinm. r9,r11,0,0x10,0x10 ; SRR1 bit 0x8000 (EE at interrupt time)
beq   0x313ab0            ; EE was 0 => PANIC "*** CPU MALFUNCTION - Masked interrupt
                          ;   punched through. SRR1/0" (ASCII at 0x313ab4) [STATIC]
lwz   r9,-0x338(r8); lwz r9,0x20(r9); cmpwi r9,2
blt   0x314660            ; [[KDP-0x338]+0x20] < 2 => fallback: mtlr [KDP+0x5b0]; blr
bl    0x3238ac            ; save nonvolatiles
li    r9,9; stw r9,-0x238(r8)   ; *** SOURCE CODE IS A CONSTANT: 9 -> [KDP-0x238] ***
li    r8,1; bl 0x3148e0   ; dispatch through the REGISTERED-HANDLER table:
                          ;   index = [KDP-0x238]<<2 (because r8!=0); table base
                          ;   [[KDP-0x338]+0x38], bound [[KDP-0x338]+0x44];
                          ;   no table -> r8=0xffff8d9a; index OOB -> 0xffff8d99;
                          ;   else save r10-r13/XER/CTR/LR/r6/r7 to KDP-0x2d0..-0x2b0,
                          ;   address-space check [r22+0x4c] vs [KDP-0x1c]
                          ;   (bl 0x323f78 = space switch), call the handler
bl    0x32391c            ; restore nonvolatiles
cmpwi r8 vs 0x8da2/0x8da3/0x8da1:
  0x8da2 -> 0x314660 (fallback [KDP+0x5b0])      0x8da3 -> b 0x312cb0 (bounce)
  0x8da1 -> stb 1,[KDP-0x118]; b 0x312cb0 (sets the RESCHEDULE byte -> scheduler)
  else (incl. 0x8d9a/0x8d99 errors) -> 0x314660 (fallback)
```

**Verdict (Q-W2):**
1. **NO IACK read, NO EOI write, anywhere in the NK.** The EXT body and everything
   reachable ≤2 levels (0x313d40, 0x3238ac, 0x3148e0, 0x32391c, 0x323f78, 0x314660,
   0x312cb0 family) contain zero loads/stores in the 0xF3040000+0x40000 window. A
   whole-NK scan (0x310000–0x330000) for `lis` imm 0xF300–0xF3FF found exactly 3 hits
   (0x3259fc/0x325b3c/0x325c44) — all **segment-register setup** (`mfsrin/mtsrin`) for
   the 0xF3 space, not device access. **The NK expects the REGISTERED handler (OS-side
   code, installed via the [[KDP-0x338]+0x38] table) to read the PIC** — on real
   hardware that is Mac OS's native interrupt dispatcher. The NK's source
   discrimination is (a) by ENTRY POINT (per-vector published handlers — F6's expected
   verdict CONFIRMED) and (b) within EXT, a **hardcoded source code 9** indexing the
   registered-handler table — no hardware query at all.
2. **DEC reconciliation:** the published DEC handler 0x313200 = same prologue + the same
   EE-punch-through guard, then: `[KDP+0x5a0]` hook installed? → `bl 0x324a98`
   (scheduler-class service) + conditional DEC reprogram from `[KDP-0x9d4]`
   (`mtspr DEC` at 0x313234) → `b 0x312cb0`; no hook → kernel timer service
   (lock 0x312700, `[KDP+0xe8c]++`, `bl 0x322eac`, unlock 0x3272e0) → `b 0x312cb0`.
   DEC differs from EXT ONLY in body (timer service vs handler-table dispatch); both
   share prologue, EE guard, and the bounce exit. **Neither touches r9 or the
   save-and-switch entry** — the published handlers are self-contained.
3. **What W2-3's shim must compose for an EXT delivery to `external_entry=0x50314880`:
   exactly the sc/program 2-SPR shim — SPRG1:=caller r1, SPRG2:=caller LR — and nothing
   else.** The prologue self-loads r7 from [KDP-0x10] (no [KDP+0x660] composition), the
   save target is [KDP-0x14] (NK-maintained), and ExcEnter's SRR1 (= interrupted MSR,
   EE=1 by the delivery gate) satisfies the punch-through guard for free. The DEC-shim's
   ECB/[KDP+0x65c] register-save logic does NOT transfer (same verdict as sc Q-S2).
4. **Level-held latch consequence:** since the NK never EOIs, PIC pending retires only
   when the guest's registered handler (or a mask write) drops it. Pre-installation
   ([[KDP-0x338]+0x20]<2 or empty/short table) every EXT delivery exits through the
   **[KDP+0x5b0] fallback pointer** — runtime value unprobed (residue W2S-R3). W2-3's
   re-delivery tripwire is therefore load-bearing from delivery #1, exactly as rev 2 C1
   anticipated.
5. **Implied follow-up, now with static teeth (carried from F6):** the M3a DEC delivery
   target 0x50412b1c is the save-and-switch body, NOT a handler; the published
   0x50313200 route is self-contained, r9-free, and ends in the same restore tail.
   Re-pointing DEC delivery at the published handler (+ the 2-SPR shim instead of the
   ECB save) is the architecturally clean direction — **out of W2-2 scope, named for
   W2-3+ consideration** (the asymmetry note in M3A stands).

### W2S-3. Q-W3 — the flags-word bit table

The NK's live flags word is **[KDP-0x10]** (init: `oris r7,r8,0xa0; stw r7,-0x10(r1)`
at 0x3113f0 ⇒ boots with 0x00800000|0x00200000 set — matching the probed live
0x00a00006). **[KDP+0x65c]/[KDP+0x660] are the EMULATOR-WORLD interface pair**, written
together by NK switch code at 0x314640/0x314644 (`stw r6,0x65c(r8); stw r7,0x660(r8)`)
and read back by the slot-exit stubs; the published handlers never read +0x660 — the
prologue reads [KDP-0x10]. Bit family ([STATIC] unless noted):

| Bit(s) | CR map (mtcrf 0x3f/0xff) | Meaning (evidence) |
|---|---|---|
| 0xFF000000 | — | pending **event code** (top byte): inserted `rlwimi r7,r8,0x18,0,7` at 0x312ac8, read `srwi r9,r7,0x18`; per-code counters [KDP+0xdc0+code*4]. Codes seen: 0 (alt-table EXT body 0x3146e0), 2 (slot-exit default body 0x3146d0), 8 (double-event error), 9 (EXT's registered-table index, via [KDP-0x238] not the flags byte), 0xc (special-cased at 0x312aec/0x312fc0) |
| 0x00800000 | cr2.lt | **world parity: "current world = emulator ctx"** — init-set, `xoris r7,r7,0x80`-TOGGLED on every save-and-switch (0x312bec); gates the heavy leg's 68k-post-vs-kernel-notify split (0x312cfc) and the `bltl cr2` notify call (0x312b08) |
| 0x00200000 | cr2.eq | **"from emulator"** — the V-seed; switch-service classifier (0x3143a0), dispatcher leg select (`bne cr2`→0x312f70 when CLEAR), and **precondition of the NK's 68k-interrupt post** (0x325518 test before `sth →[[KDP+0x67c]]`) |
| 0x00008000 | cr4.lt | fast-return / skip-reschedule (0x3242ac test; 0x3148f4 `blt cr4`→0x314a38 alternate-space return) |
| 0x00000020 | cr6.eq | post-action secondary flag (cleared in the tree; 0x20+0x10 both set ⇒ code-8 error). Stub-composed: `rlwimi r7,r7,27,0x20` ⇒ bit5 := bit 0x400 of the +0x660 copy |
| 0x00000010 | cr6.so | **post-action pending** (primary): arms the 0x312ccc tree at all |
| 0x00000001 | cr7.so | **heavy-post selector**: with 0x10 ⇒ 68k-interrupt post-and-dispatch (0x00800000 set) or kernel-task notify (clear) |
| low 15 bits (0x7FFF) | cr4–cr7 | per-context bits: REPLACED from new-ctx word[0] on every switch (`rlwimi r7,r8,0,0x11,0x1f`); ctx word[0] bits 0x0F00 also feed the MSR image (`rlwimi r11,r8,0,0x14,0x17`) |
| 0x80000000 | cr0 (via rlwimi.) | stub-derived mirror of 0x00800000 (`rlwimi. r7,r7,8,0x80000000`) — a COMPUTED bit, not stored state |

**Q-W3 verdict: there is no "external pending" bit.** External-ness is carried by the
vector (entry point) and, inside the NK, by the constant source code 9 → the
registered-handler table. **The W2-3 shim sets NO flag bits for EXT** (and none for DEC
via the published handler); the [KDP+0x660] V-seed stays exactly as Task V left it —
its job is the switch-service classification and enabling the NK's 68k posting, not
delivery routing. What the hook composes per class is only the ExcEnter SRR0/SRR1/MSR
transition + the 2-SPR shim.

### W2S-4. Bonus — the [KDP+0x360] publication: it is a FAMILY of per-regime tables

The publication sequence ([STATIC] 0x311500–0x31178x; one [PATCH-DIVERGENT] word — the
`mtspr SPRG3,r9` arming at 0x31154c is NOP'd by `sprg3`) builds **five vector tables**
(48 slots / 0xC0 bytes each, slot = vector>>6, all slots pre-filled with the default
handler **0x50314b80** = `SRR0+=4; rfi` — unhandled vectors SKIP the faulting
instruction!) and the 16-slot slot-exit table:

| Table base | Regime (evidence) | Non-default slots (vector → handler, primary copy) |
|---|---|---|
| **KDP+0x360** | NK/native (the published table M3A/Q-S1 probed; raw SPRG3 target of the m68k_excp_tbl site) | 0x100→0x503272e0 · 0x200→0x50313a04 · 0x300→0x503132c0 · 0x400→0x50313940 · **0x500→0x50314880** · 0x600→0x50313460 · **0x700→0x50314700** · 0x800→0x50313da0 (FP-unavail: enables MSR[FP], **rfi — un-patched rfi exit**) · **0x900→0x50313200** · **0xC00→0x50314ac0** · 0xD00→0x50314b60 · **0xF00→0x50314240** · 0x1600→0x50317440 · 0x1700→0x50314300 · 0x2000→0x50314b60 |
| KDP+0x420 | second regime (68k-emulator-resident? — its EXT differs) | same as +0x360 EXCEPT **0x500→0x503146e0** (`bl 0x313d40; mtcrf 0x3f,r7; bnel cr2→(island b 0x3272e0); li r8,0; b 0x312ab4` — EXT-as-event-code-0 into the central dispatcher) |
| KDP+0x4e0 | **the table SPRG3 is swapped to when dispatching INTO the 68k emulator** (0x312db4 `mtspr SPRG3, KDP+0x4e0`, un-patched) | 0x100→0x503272e0 · 0x200→0x503137c8 · 0x300→0x503135a0 · 0xC00→0x50314ac0; **EXT = the 0x50314b80 skip-and-rfi default** |
| KDP-0x8d0 | third regime | 0x100→0x503272e0 · 0x300→0x503132c0 · 0x400→0x50313940 · 0x600→0x50313460 (then `bl 0x319ce0` continues init) |
| KDP-0x750 | fourth regime | 0x100→0x503272e0 · 0x200→0x50313a04 · 0x300→0x50313b40 · 0xC00→0x50314ac0 |
| KDP+0x5f0 (0x40 bytes, 16 slots) | **slot-exit selector table** | filled with default 0x503146d0 (`bl 0x313d40; li r8,2; b 0x312ab4` — slot-exit-as-event-code-2); live [KDP+0x5f0]=0x50313bf8 / [KDP+0x5f4]=0x503143a0 [PROBE✓ rung 2] ⇒ later init overwrites at least slots 0/1 (writer not located this session — residue) |
| KDP-0x690 | fifth regime | default-fill only in this sequence |

Menu consequence for future classes: a vector's handler is **per-regime** — "the
published handler" must name its table. The M3A generalization
(`probe [KDP+0x360+(vector>>6)]`) reads the NATIVE-regime table; that is the right one
for ExcEnter-style delivery while [0x2810]=0 (the raw NK would have SPRG3=KDP+0x360
there — the m68k_excp_tbl patch site's raw arm). Re-publication sites exist (0x316888 /
0x31697c re-point 0x37c-class slots — debugger/service installs); live table contents
can drift from this init picture (the probed values match it today).

### W2S-5. Residues (what only P1/P2 boots can pin)

- **W2S-R1 (P1, blocking-adjacent):** r9 and [KDP+0x658] at the 0x50412b1c delivery
  instant (shim mismatch #1). **Probe-spec upgrade: P1's 0x50412b1c probe should carry
  `r9,[0x68ffe658],[0x68ffdff0]`** ([KDP-0x10] — the REAL flags input of published
  handlers; the plan's `[0x68ffe660]` field watches the interface copy, still wanted).
- **W2S-R2 (P1):** the XLM_IRQ_NEST −1-per-delivery drift signature (watch 2818 hex).
- **W2S-R3 (P1):** live `[KDP+0x5b0]` (EXT fallback), `[KDP-0x338]` + its +0x20/+0x38/
  +0x44 fields (registered-handler table state at the frontier — decides whether W2-3's
  first EXT delivery dispatches or falls back), `[KDP+0x5a0]` (DEC hook), `[KDP-0x118]`
  (reschedule byte), `[KDP-0x238]`.
- **W2S-R4 (P2):** whether the storm's deliveries traverse the 68k-post leg (probe
  0x50412d04-class visits / `[0x68ffe67c]`-target writes — F9's field already rides P1).
- **W2S-R5:** per-target byte re-confirmation of the staged copies beyond 0x50412b0c–48
  (0x50413200/0x50414880 windows if W2-3 delivers to staged; PRIMARY copies probed
  already per Q-S1).
- **W2S-R6:** live SPRG3 value/regime on the boot path (init arming is patched out;
  swap sites survive) — matters only if anything ever consumes the vector stubs.
- **W2S-R7:** the writer that installs 0x50313bf8/0x503143a0 over the KDP+0x5f0/5f4
  defaults (not located; cheap static follow-up).
- **W2S-R8:** 0x3272e0 serves both as the slot-0x100 (system reset) table entry and as
  the syscall memo's "unlock" — one of the two readings is an alias/misattribution;
  immaterial to W2, flagged for the next static pass.

### W2S-6. Blocking-answer table impact (the W2-2 gate rows, static-half status)

| Blocking answer | Static half | Boot half still owed |
|---|---|---|
| Q-W1 r7 tree + R-9 arm | **PINNED** (§W2S-1 table; R-9 arm = enabler of the NK 68k-post, not a delivery-time branch) | r9/cr6 shim-mismatch live values (W2S-R1); P2 storm behavior |
| Q-W2 IACK/EOI + discrimination | **PINNED** (no NK IACK/EOI; entry-point + constant code 9 + registered table; shim = 2-SPR; F6 external_entry=0x50314880 CONFIRMED static) | [KDP-0x338] table state at frontier (W2S-R3) |
| Q-W3 [KDP+0x660] encoding | **PINNED** (bit table §W2S-3; no EXT-pending bit; shim composes nothing) | [KDP-0x10] live value at delivery (rides W2S-R1) |
| P2 chain READY/BROKEN | — | entirely boot-half (P2) |
| Nest balance (link 8) | drift MECHANISM pinned static (W2S-R2 signature pre-computed) | live confirmation (P1) |

---

## W2-2 probe session results (boot half: P1 census + F7 gate + P2 storm) — 2026-06-11

> **Status:** the bounded live-probe session (plan Task W2-2, Rev 2 F3/F7/F9 applied).
> **Boots used: 2 of the ≤4 budget**, both ≤58s, slot protocol (no pkill):
> P1 = `/tmp/ss-slots/slot0/runs/20260611-212608.4897/`, P2 (storm) =
> `/tmp/ss-slots/slot0/runs/20260611-212903.5448/`. Binary rebuilt at HEAD
> (`dd6ba65f` tree: W2-0 extraction + W2-1 knobs + instrument batch + FE1F default
> all in). Default newworld diagnostic config (9.0.1 ROM, 256MB, nogui).
> **Frontier signature (F8 staleness anchor for every pin below):** the standing NK
> boot ceiling — SIGSEGV ea(guest)=0x0fffff42 at mirror-dispatch host pc, guest pc
> 0x504661a0 (the DSAT-wall-class 68k desync, `8218ee68` recon), ring total
> 4,480,458 records, `delivered_sc=13 delivered_program=2`, `[CUDA] packets=13 …
> i2c=9`. The two boots are byte-deterministic against each other (identical watch
> record numbers #4658/#3391659 across runs).

### W2L-1. Boot P1 — baseline census [PROBE✓]

Env: the plan's P1 probe set + the §W2S-R1 upgrades (r9/[0x658]/[KDP-0x10]) + F9a
[0x68ffe67c] + W2S-R3 frontier-state fields on the 0x50313bf8 probe;
`SS_JIT_WATCH_ADDR=2818` (hex per F3), `SS_JIT_TRACE_RING=1`.

- **(F7 gate) PASS — twice.** Terminal tuple in BOTH boots is byte-identical to the
  pre-W2-0 baseline: `delivered_dec=0 deferred_ee=1 deferred_depth=0
  deferred_native=0 delivered_sc=13 delivered_program=2` (sc selector census also
  unchanged: 9 distinct, 0x3f/0x19/0x14/0x0f/0x27/0x40/0x42/0x50/0x4d). The W2-0
  decision-extraction refactor transposed nothing.
- **0x50313bf8 (the EE-forcer) live, visit 1:** `r3=0x000000ff r7=0x00200000
  r11=0x0002f072`. The stub-composed MSR fiction arrives with EE **already set**
  (0xf072 ⊃ 0x8000) — the `ori r11,r11,0x8000` force is idempotent on this leg;
  r7 carries exactly the V-seed bit (world-parity 0x00800000 CLEAR at this
  instant); selector r3=0xff. Only visit 1 was captured (later traversals — ≥13
  proven by `->50313bf8` watch records — ran chained past the probe dispatch);
  recorded honestly, not over-claimed.
- **0x50412b1c never fired** (delivered_dec=0 — no EE rise on a default boot, as
  expected). W2S-R1's live half (r9/cr6/[KDP-0x10] at the delivery instant) is
  therefore **unobtainable until a delivery can occur** — see W2L-3.
- **(W2S-R3 partially → fully pinned, with P2's table fields):**
  `[KDP+0x5b0]=0x50325f00` (EXT fallback pointer, live), `[KDP-0x338]=0x68ffc1c0`,
  `[[KDP-0x338]+0x20]=0x00000001` (**< 2 ⇒ every EXT delivery at today's frontier
  takes the [KDP+0x5b0] fallback** — the registered-handler table is NOT
  installed), `[+0x38]` base = 0, `[+0x44]` bound = 0 [PROBE✓ P2, visit 1 of
  0x50313bf8]. Also `[KDP+0x5a0]=0x68ffe000` (DEC-hook slot — raw value recorded,
  interpretation deferred), `[KDP-0x118]` word = 0x00020000 (reschedule byte at
  -0x118 itself = 0x00, clear), `[KDP-0x238]=0` (no EXT source code ever posted).

### W2L-2. Nest balance (link 8) — the W2S-R2 signature CONFIRMED, and generalized [PROBE✓]

44 watch transitions on 0x2818 (P1; P2 = same endpoints, 47 with block-attribution
shifts from the extra probe PCs splitting blocks):

- **Start 0 → first transition #4658: 0 → 0xFFFFFFFF at pc=0x50318000** — the
  historic lone restore-tail decrement (M3A finding 3, now timestamped live).
- **Warm switch-backs are BALANCED**: slot-stub entries increment (pc family
  0x5046de08 ×6, 0x504ff010 ×5, 0x5046fa00/0x5046f900/0x5046e120 ×1 each = 14
  increments), each paired with a patched-exit decrement — alternating ±1 around
  −1, exactly the healthy signature.
- **Every hook DELIVERY drifts the counter −1 unmatched**: runs of consecutive
  decrements at the patched trap-return exit family (block-entry attribution
  0x50324524 ×28, 0x503244d4/0x50318000 ×1 each = 30 decrements; the watch `sp`
  values match the `[EXC] SC delivered` r1 values line for line). Final value
  **0xFFFFFFF0 = −16 = 1 historic + 13 SC + 15−13=2 PROGRAM deliveries** — the
  arithmetic closes exactly. Quiescent for the last ~1.09M ring records (the
  mirror-68k phase touches it never).
- **Generalization beyond the static prediction:** W2S-1 mismatch #3 predicted the
  −1-per-DEC-delivery drift; live shows it is −1 per **ANY** ExcEnter-shim delivery
  (SC and PROGRAM included — all exit through the patched `trap_return` family,
  which decrements at the 0x318000 stub with no shim-side increment). Drift
  scales with sc/twi traffic, not just future DEC traffic. Stays negative ⇒
  `HandleInterrupt`'s `>0` early-out unaffected today; unbounded ⇒ **the W2-4
  XLM_IRQ_NEST ownership item is now evidence-backed and quantified.**

### W2L-3. Boot P2 — the EE-storm: verdict **LEVER-DEAD** (the storm cannot run under the current armed state)

Env: `SS_M6A_USER_MSR=1` + the P1 set + 0x50429b40 + 0x50412d04 probes + the R-14
instruments (`[ECB+0xfc]`/`[MMCB+0xfc]` fields on the delivery probe) + the F9c
Ticks watch (`SS_JIT_WATCH_ADDR=2818,16a`).

- **First line of consequence:** `[NW-TRAMP] V: SS_M6A_USER_MSR=1 ignored under
  SS_NW_MM_SWITCH=1 (Q-E verdict: per-context MSR via the NK switch; user-msr
  stays quarantined known-broken diagnostic)`. The quarantine is STRUCTURAL, not
  advisory: rom_patches.cpp:964–976 — Q-E's per-context-MSR verdict plus the
  trampoline word budget (pool+switch+user-msr = 49 > 48 slots before the
  0x429c00 stop stubs). **With the MM switch default-ON (Task Y), the only
  EE-rise lever we own is a no-op.** The boot ran as a pure baseline replica
  (tuple, frontier, nest endpoints all byte-identical to P1).
- **The plan's P2 premise is falsified** — not by the R-2 slide (never reached)
  but one level earlier: there is no storm to bound. The pre-pinned READY/BROKEN
  per-link table was **not scored** (no signature of either class appeared; zero
  DEC deliveries; 0x50412b1c/0x50412d04 probes never fired; Ticks 0x16a watch:
  zero hits — link-7 starvation visible as total tick silence).
- **Why the alternative was NOT taken** (one-iteration discipline, recorded):
  `SS_NW_MM_SWITCH=0 SS_M6A_USER_MSR=1` would re-arm the lever but collapses the
  boot to the pre-Task-V FE01 retry-spin frontier with the V-seed absent — i.e.
  it re-runs the rung-1 night-run conditions (already proven, single-datum) and
  cannot score "the chain under the CURRENT armed state", which is the verdict
  W2-3/W2-4 gate on. A storm there is evidence about a configuration we no
  longer ship.
- **Coherence with the pre-recon (this is the W2S-0 headline validating, not a
  contradiction):** the `trap_return` patch already removed the NK's rfi resume
  (the designed EE-riser) from the interrupt/trap exit path, and now the only
  diagnostic EE lever is structurally quarantined too. Under the current patch
  set + armed state, **EE has no remaining riser at all on the paths the boot
  takes**: §B's "EE first rises at an NK rfi" narrows to the un-patched rfi
  sites (the 0x800 FP-unavail handler's rfi, the vector-stub fast exits, the
  template page) — none on the current boot's path — or a future deliberate
  lever (P3-class `SS_EXC_FORCE_EE_AT`, a SECOND session per Rev-2 F9b +
  Tension 5, source commit + full gates).
- **P3 not attempted** (per Rev-2 F9b it is a second session; per the task
  instruction the session STOPS and reports — the coordinator decides whether to
  schedule it). Boots 3–4 of the budget intentionally unspent: nothing else was
  pinnable without the lever.

### W2L-4. P4 — depth-deferral carry-forward

`deferred_depth=0` in BOTH boots. **M3a carry-forward #1 remains OPEN** (recorded
either way, as specified). No delivery request has ever landed during a nested
EMUL_OP execute on a live boot; unit case U2 + the level-3 telemetry remain its
only coverage.

### W2L-5. The blocking-answer table (the W2-2 gate — final status)

| Blocking answer | Status | Evidence / residue |
|---|---|---|
| Q-W1 r7-flag tree + R-9 arm | **PINNED** (static §W2S-1 + live frontier census) | Tree mapped; R-9 = enabler of the NK 68k-post, not a delivery-time branch. Live residue: r9/cr6/[KDP-0x10] at a delivery instant (W2S-R1) is **inert-by-construction** — no delivery can occur before an EE riser exists; it rides whichever lever next fires a delivery, and W2-3's EXT shim does not depend on it (the 2-SPR shim + published-handler verdict make the 0x412b1c save-and-switch entry's r9 hazard DEC-path-only). |
| Q-W2 IACK/EOI + source discrimination | **PINNED** (static + live) | No NK IACK/EOI; entry-point + constant source 9 + registered-handler table. Live: table NOT installed at the frontier (`[[KDP-0x338]+0x20]=1`, base/bound=0) ⇒ W2-3's first EXT delivery exits via the `[KDP+0x5b0]` fallback = **0x50325f00** [PROBE✓]; the re-delivery tripwire is load-bearing from delivery #1, as rev 2 C1 said. |
| Q-W3 [KDP+0x660] EXT encoding | **PINNED** (static; live interface-copy value r7=0x00200000 confirmed at the switch-back probe) | No EXT-pending bit; the W2-3 shim composes nothing into the flags word. [KDP-0x10]-at-delivery rides the same inert residue as Q-W1. |
| P2 chain READY/BROKEN per link | **NOT SCORED — LEVER-DEAD** (stop-rule event) | The storm lever is structurally quarantined under the default armed state (W2L-3). Per-link table unscorable without a new riser. **W2-3's start decision goes to the coordinator**: either accept the static+W2-0/W2-1 coverage as sufficient de-risking for the EXT path (which does NOT route through the unverified 0x412b1c save-and-switch — it uses the self-contained published-handler shape), or schedule the P3 lever session first. |
| Nest balance (link 8) | **PINNED — drift confirmed** | −1 per delivery, generalized to ALL hook deliveries (W2L-2); −16 at the frontier; stays negative (benign today); W2-4 ownership input quantified. |

**Session residues (new):**
- **W2L-R1:** the `[KDP+0x67c]` resolved target is guest-seeded and was never
  readable this session (the only probes carrying it sit on delivery-path PCs
  that never fired). F9's "P2 watches its resolved target" is satisfiable only
  after a first delivery reveals the pointer — fold into the P3-class session.
- **W2L-R2:** probe sampling vs chaining — 0x50313bf8 captured visit 1 only
  despite ≥14 traversals; future census probes on hot switch-back PCs should
  pair with `SS_PROBE_LINEAR=1` (cap permitting) or accept first-visit-only.
- **W2L-R3 (carried):** W2S-R1/R4/R5/R6/R7/R8 unchanged; R3 retired (pinned
  above).

---

## W2-3 results — OpenPIC bus wiring + EXC_EXTERNAL delivery (links 1 + 11 CONSTRUCTED; flip HELD per stop-rule 3) — 2026-06-11

> **Status:** the ladder's first construction, landed env-gated (`SS_NW_PIC=1`, default
> OFF — **the flip is HELD**, see the verdict). Commits: `b2e0d718` (the LAW edit —
> ExcEnter(EXC_EXTERNAL) consumes `external_entry`, the sanctioned U12 flip, both arms
> pinned; + the `write_ctpr` recompute-equivalence comment debt), `7cafd6ae` (F4 SCC
> interrupt-condition state — NEW model code — + the VIA summary edge, both seams
> transition-only with unit checks), `95d3fc53` (the wiring: bus registration, byte-lane,
> source edges, output flag + kick, the delivery-side EXT branch, tripwires, telemetry,
> harness knob + H6/H7 lane vectors). **Boots used: 4 of ≤5**, slot protocol, all
> ≤45 s (B1 bring-up, B2/B3 [DIAG-FORCED], B4 gated-off A/B).

### W3-1. What landed (per the Rev-2-amended W2-3 body)

- **Byte-lane (F16):** LE VALUE-SWAP in the `openpic_bus_read/write` trampolines,
  [STATIC-oracle] (QEMU maps KeyLargo MPIC little-endian; bus values are architectural).
  Falsifier documented at the seam + `dev_openpic.h`: first live guest FRR read must show
  `0x02003F00`. **Falsifier still UNTESTED live** — `[PIC] reads=0` on every boot (the
  guest has never read the PIC); the first-FRR-read loud line is armed for whenever it
  does. Bus registration: contained overlap in the macio stub (scc/via idiom);
  Reset-then-BindOutput order honored.
- **F4 SCC interrupt-condition state (new model code):** per-channel level predicate
  Rx-available ∧ WR1-Rx-int-enable(0x18) ∧ WR9-MIE(0x08), recomputed at enqueue/drain/
  enable-writes; WR9 chip-wide via a new shared copy (per-channel storage kept for
  read-back compat, divergence documented). VIA summary edge: `(ifr & ier & 0x7F) != 0`,
  recomputed at every read/write + the eager scheduler expiry. Both seams fire on
  TRANSITIONS only under the owning region lock; cross-region lock order documented
  BINDING: device → pic, never pic → device. Unit checks: SCC 63 (+23), VIA 83 (+20).
- **Delivery side:** combined-pending gating through `ExcDeliveryDecision` (one decision
  per poll — tuple semantics preserved; ext constant-0 when gated off ⇒ byte-identical),
  source selected after DELIVER with **DEC-before-EXT** (m11/C1 justification at the
  site); EXT branch = `ExcEnter(EXC_EXTERNAL)` → **`external_entry` = 0x50314880**
  (NK-published [KDP+0x374], primary copy — the F6 default, now CONSUMED; U12 flipped
  honestly with both arms pinned: consumption when nonzero, interrupt_entry fallback
  when 0) + the **2-SPR shim** (SPRG1:=r1, SPRG2:=LR — the sc/program precedent per
  Q-W2; the DEC KDP shim deliberately does NOT transfer). PIC pending NOT cleared
  (level-held, rev 2 C1). SRR1.EE=1 punch-through guard satisfied by construction
  (gate-admitted MSRs only; pinned in test_exc_chain + test_exc_core test 9).
  F5: single-copy-atomic output flag (acquire/release) + TriggerInterrupt kick on the
  assert edge; all SIX EE-edge re-raise sites include the EXT source. Tripwires:
  re-delivery runaway (16, no retirement) + U13 starvation (64 DEC-with-EXT-pending).
  Telemetry: `delivered_ext` as the exc= 7th field, printed ONLY when configured;
  `[PIC]` stats + Q8 first-IACK record on atexit/term-dump/crash-path/heartbeat.

### W3-2. The acceptance-or-downgrade verdict: **PRE-DECLARED DOWNGRADE — flip HELD (stop-rule 3)**

Live acceptance ("controlled trigger → EXT delivered → entry 0x50314880 → guest handler
runs") is **unreachable at the current frontier for two independent, pre-known reasons**,
both now evidence-backed:

1. **PIC initialized: NO** [PROBE✓ B1]: `[PIC] reads=0 writes=0` — the guest never
   touches the PIC region; CTPR stays at the reset 15, every source masked. (Consistent
   with W2L-1's `[[KDP-0x338]+0x20]=1` — no registered-handler table either.)
2. **EE riser: NONE** (the W2L-3 lever-dead verdict, re-confirmed): every EXT kick lands
   as `deferred_ee`; `delivered_ext=0` on all live boots. Host-forced unmask cannot
   manufacture an EE rise and was never going to count as acceptance (tension 1).

Per the plan's tension-1 allowance, ONE `[DIAG-FORCED]` configuration was run (B2/B3,
`SS_NW_PIC_FORCE=1`: host CTPR=0 + IVPR unmask, SCC WR1/WR9 enables re-applied at
inject time because the guest's own WR9 hw-reset clears bring-up forcing — a mechanism
found during implementation, recorded in DIAGNOSTICS.md). **DIAGNOSTIC ONLY, NOT
acceptance.** What it proved mechanically, live:

- **B3 [PROBE✓]: the full SCC leg** — `SS_SCC_RX_INJECT=0:0D` → Rx enqueue → F4
  condition asserts → input **0x25** raised → PIC output asserts → `[EXC] EXT pending
  ASSERTED (edge #1)` → CPU kick → hook poll → `DEFER_EE` (correct: EE=0). The line then
  deasserts when the guest's SCC init resets the chip (condition drops → 0x25 lowers →
  output recomputes) — level discipline correct end-to-end.
- **B2/B3 [PROBE✓]: the VIA leg, at boot scale** — **197 VIA summary edges** traverse
  device→PIC per boot (`raises=197 lowers=197`; B1 shows the same 197 with the sources
  MASKED ⇒ `out=0`, the mask gate verified live). Under [DIAG-FORCED] unmask each edge
  propagates: `out_raises=197/198 out_lowers=197/198`, every assert kick correctly
  deferred at the EE gate (`deferred_ee=198`, was 1 baseline). **The chain
  device→PIC→output→flag→kick→gate is live-proven for BOTH wired source classes**;
  only the EE gate (no riser) and the guest-side retirement stand between the
  current state and a real delivery.
- **Frontier untouched in all four boots:** SIGSEGV ea(guest)=0x0fffff42, guest pc
  0x504661a0, `delivered_sc=13 delivered_program=2` — the F8 staleness anchor
  byte-matches (env-on adds only the expected `delivered_ext=0` field + [PIC] lines).
- **B4 gated-off A/B [PROBE✓]:** zero [PIC]/EXT lines, 6-field `[EXC]` tuple
  byte-identical to the W2-2 baseline class, bus-active line pre-W2-3 shape.

**The EXT delivery itself (the part live boots cannot reach) is PROVEN at harness
level, both modes:** `make test-exc-vectors` gains **H6** (EXT delivery via the mtmsr
EE edge; GATING entry-discrimination — `SS_EXC_ENTRY=0,0,0x1000C000` makes a
wrongly-shared-entry delivery FATAL-unresolved; REGDUMP pins msr=0x1040,
srr0=restart, srr1=0xf072 ⊃ EE=1) and **H7** (dual-pending: exactly one delivery and
it is the DEC — the EXCSTAT tuple `delivered_dec=1 … delivered_ext=0` discriminates;
U13's live analogue). Lane 9/9 score=100, interp-vs-JIT REGDUMPs byte-identical.

### W3-3. Chain-map updates (§A)

| Link | Was | Now |
|---|---|---|
| 1 (device IRQ → PIC) | MISSING | **EXISTS-TESTED** (unit: SCC 63/VIA 83 checks; live: 197-edge VIA traffic + the SCC inject leg, [DIAG-FORCED] B2/B3) — env-gated `SS_NW_PIC`, default OFF |
| 11 (EXC_EXTERNAL delivery) | MISSING | **EXISTS-TESTED-harness / live-blocked-by-EE** (H6/H7 both modes; live blocked by link 9 — no EE riser — and by guest PIC non-init) |

Residues fed to W2-4 / next sessions: the F16 FRR falsifier (armed, untested);
the first live EXT delivery's guest-side retirement behavior (the [KDP+0x5b0]
fallback observation — unreachable until an EE riser exists); W2L-R1 (r9/cr6 at a
DEC delivery instant) unchanged; the nest −1-per-delivery drift now ALSO applies to
future EXT deliveries (same patched trap-return exit family) — W2-4's XLM_IRQ_NEST
ownership item gains the EXT class.

### W3-4. Gates (all green, 2026-06-11)

machine suite 13/13 ALL PASS (test_exc_core 43, test_exc_chain 64, scc 63, via 83,
openpic 206); `make test-jit` plain AND batch 353/353; `make test-exc-vectors` 9/9
score=100; `make e2e-test` 122 passed; **paravirtual `make e2e` PASS** (booted to
Finder, clean shutdown, exit 0); gated-off A/B boot byte-identical (B4).
