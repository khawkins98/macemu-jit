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
