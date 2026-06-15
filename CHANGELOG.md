# macemu-jit (macOS arm64) Changelog

Changes specific to the `macos-arm64` branch — this fork of kanjitalk755/macemu, which
adds AArch64 JIT backends. Covers **both emulators** plus shared/build/docs work.

Entries are tagged by component: **[SheepShaver]**, **[BasiliskII]**, **[shared]** (code
used by both, e.g. `ether_unix.cpp`, prefs), **[build]**, **[docs]**. Entries before
2026-06-04 predate this fork-wide reorganization and are SheepShaver-scoped unless noted
(BasiliskII history lives in `BasiliskII/docs/AARCH64_JIT_BRINGUP.md` and
`docs/planning/BasiliskII-MACOS-AARCH64-JIT-PORT.md`).

## 2026-06-15 (Operation NewSheep — Cuda SR-int timer seam: discipline closure)

- **[SheepShaver]** **Track-1 discipline closure for the timer-delayed Cuda SR-int delivery
  seam** (B3's `CudaBindTimerDelivery`/`VIALatchIFRBits`, committed `9e4ae1d0`; the M14
  [ALARM] interrupt-delivery wall, a NewWorld-only boot issue). Audited the seam against the
  "gated-OFF byte-identical" discipline gate and confirmed it is **provably byte-identical for
  the paravirtual 8.6 path on two independent grounds**, so it is left **ungated as-is** (the
  conservative gate was found unnecessary, not skipped):
  (1) **Reachability** — the entire Cuda/VIA device layer (`CudaReset`/`CudaBindADB`/
  `VIABindCuda` in `main_unix.cpp`) is inside the `if (MachineUsesMMIOBus())` block, which is
  FALSE for the paravirtual profile (`MachineUsesMMIOBusParse`: true only for `MACHINE_NEWWORLD`
  or an explicit `SS_MMIO_BUS`). The paravirtual 8.6 e2e boot never instantiates or runs
  `dev_cuda`/`dev_via6522` at all.
  (2) **Dormancy** — even if reached, the seam is opt-in: `arm_sr_int()` with `sr_schedule==NULL`
  (the `memset`-zeroed `CudaReset` default) is exactly the old `sr_int_pending = 1`, and
  `CudaBindTimerDelivery`/`VIALatchIFRBits` currently have **no caller anywhere** (not even the
  NewWorld wiring yet) — the new code is dead until S4 binds it. `via_update_irq` took a
  comment-only change. The correct gating boundary is the device-wiring layer
  (`MachineUsesMMIOBus`) + the default-unbound binding, NOT a `machine_profile` dependency
  inside the pure device model (layering).
  **Test coverage extended** to assert BOTH states the discipline gate cares about
  (`test_dev_cuda.cpp` +20 checks → 4814, `test_dev_via6522.cpp` +6 → 89): gated-OFF
  (unbound ⇒ no one-shot armed, raise delivered only via `CudaSettle` on the IFR-read surface,
  timer never participates) and gated-ON (bound ⇒ arming edge posts a `CUDA_SR_DELAY_NS`
  one-shot; firing it delivers IFR.SR through `VIALatchIFRBits` with no guest IFR read;
  consume-once + mutual-exclusion with the lazy surface; IER masking honored; bit-7 masked).
  Gates: `make build-ss` clean, `make test-jit` score=100 (353), full machine `make test` green.
  The M14 wall fix remains **owed to S4** — the seam is wired but not yet bound to the live
  NewWorld scheduler/VIA (PROSPECTIVE, needs live-S4 validation, per B3's seam contract).

## 2026-06-15 (Operation NewSheep — S1 paged-MMU mechanism, standalone)

- **[SheepShaver]** **Dolphin Dynamic-BAT shadow-arena ported as a standalone, unit-tested
  module** (`src/machine/dolphin_bat_arena.{cpp,h}` + `test_dolphin_bat_arena.cpp`; machine
  Makefile target). Given the 4 DBAT descriptor pairs it maintains a host shadow mapping
  (named SHM store + two aliased VA views + `mach_vm_map(FIXED|OVERWRITE)` sub-range re-alias —
  the primitive PROVEN by `test_shm_arena_spike.cpp`/`test_shm_arena_multientry.cpp`) so a bare
  guest access at `window+EA` lands on `phys+PA`, with no per-access translation (the JIT fast
  path is untouched). Validated three ways: (1) pure BAT decode + the
  `CanCreateHostMappingForGuestPages` 16 KB feasibility gate (incl. the COARSE 256 MB case +
  an adversarial misaligned-PA slow-path row); (2) **agreement with `paged_mmu_translate()`** —
  for every covered EA a sentinel stamped at the oracle PA is read back through the bare window
  (coarse block, two-block RAM+ROM, context switch, identity-revert, out-of-arena slow path);
  (3) a replay-fixture loader that consumes the real harvested NK MMU-trace
  (`/tmp/nk-mmu-trace.log`) AND an `expect EA PA` differential format for the QEMU/NK oracle.
  Reimplemented-to-spec from Dolphin (GPLv2, pinned SHA `144d19433aa734c19c34e5978a1b817d2aa12663`:
  `HW/Memmap.cpp` `UpdateDBATMappings`/`CanCreateHostMappingForGuestPages`, `PowerPC/MMU.cpp`
  `UpdateBATs`, `Common/MemArena.cpp`), cited at the porting site.
  **NEEDS VALIDATION:** the live-NK mapping set is not yet observed (QEMU rig stalls
  pre-NK-MMU-install); coverage adequacy on a real boot is OWED to live S1 integration (G1.e),
  and the trace-replay `expect`-row oracle path is prospective until a trace with oracle PAs is
  harvested. **Inert** — linked only into the machine unit-test harness, not the SheepShaver
  binary; `make test-jit` and the paravirtual build are byte-identical (no kpx_cpu/JIT files
  touched).

## 2026-06-14 (strategy pivot — Operation NewSheep)

- **[docs]** **The M8→M17 "forge" arc is CLOSED (banked NO-GO).** M15 verified the FORGE verdict;
  M16 hit DoD-3 NO-GO (CGRP synthesis target ROM-absent); M17's red-team BLOCKED the host-stub
  approach (EXT regime is MODE_68K) — the series tripwire fired on wall 1, proving per-wall forging
  is bankrupt. Root cause across all of them: the **Trampoline** (producer of the nanokernel
  boot-time init) never runs in our emulation; we synthesize a partial substitute.
- **[docs]** **Opened Operation NewSheep — now the MAIN AIM of this branch:** boot Mac OS 9.2
  (NewWorld; HARD requirement) by running/reproducing the Trampoline producer, not forging its
  outputs. Charter + plan/spec/decisions under `docs/planning/newsheep/`; first milestone (Trampoline
  RE Task-0) brainstormed → spec'd → red-teamed (rev 2), held pre-execution. Baseline tag
  `newsheep-baseline`. NewWorld Mac OS ROM collection (1998→2003) banked to
  `/Users/Shared/macemu/newworld-roms/`.

## 2026-06-14 (M13 close-out — interrupt-delivery artifact retraction)

### [SheepShaver] M13 COMPLETE: native NewWorld interrupt delivery confirmed WORKING

The M13 milestone overturned its own premise. **Native NewWorld 68k interrupt delivery WORKS** — the
load-bearing thesis of M9→M13 ("the 68k interrupt handler `0x5000ED08` never runs / interrupts are
never delivered") was a **probe-granularity artifact**. `SS_PROBE_68K=0x5000ed08` is exact-match, but
the DR's first `lhau` advances r24 `ed08→ed0a` before the dispatch hook samples, so the probe was
blind to handler entry. Re-targeting to `ed0a` matched **8/8 in plain baseline** (interrupt HLE OFF),
with a genuine DR-built `$64` level-1 autovector frame (`[a7+6]=0x0064`, saved `SR=0x2000` = IPL 0 →
legitimate delivery, not forced; saved PC `0x50034cae`); handler region entered ~207×, full
level-dispatch + VBL/deferred pass running, scheduler healthy. Evidence:
`docs/planning/M13-FINDINGS-interrupt-delivery.md` §C-pin.7/8.

- **Reverted** the two dead delivery mechanisms — `SS_NW_DR_AUTOVEC` (the M13 Task C HLE that set the
  DR `cr2lt`) and `SS_M10_CGRP` (the M10 forged CGRP table). Both targeted a non-problem and the
  latter crashed the DR (0xDEADBEEF). Retained only as negative-result records in the findings doc.
- **New frontier (M14):** the real blocker is downstream — the `[ALARM]` model-rejection / pre-System
  gate (~15 s; Gestalt/machine-ID or System-file boot gate), a ROM/OS-version issue, not an interrupt
  one. Leverage: `docs/planning/sheepshaver-research/SYSTEM-BOOT-GATES.md` + the archived System-file
  gate-bypass work. See ROADMAP M14.
- **Meta-lesson** (LEARNINGS 2026-06-14): a load-bearing NEGATIVE result resting on an exact-match
  probe must be ring-confirmed before anything is built on it — the second five-milestone misdirection
  from a measurement artifact in this project (cf. the session-5 HOT-PC retraction).

Docs-only close-out (the source reverts are tracked separately). No harness/gate change.

## 2026-06-13 (session 7 — M12: frontier crash fixes + display driver verification)

### [SheepShaver] M12 Task A: extend NW lowmem + Wave1 24-bit DR alias

Milestone M12: get Mac OS to write pixels to the framebuffer aperture at 0x81000000.
Tasks A/B complete; Task C fails at trap-table bootstrapping wall (M13 input).
(Diagnosis superseded 2026-06-13 — the real wall is NK→68k interrupt delivery, not a trap table;
see `docs/planning/M13-FINDINGS-interrupt-delivery.md`.)

**Wave 0 (Task A)** — extend NewWorld lowmem from 1 MB to 32 MB (`main_unix.cpp`):
- Crash at `ea=0x010020c8` (68k VM Manager VMVectors struct placed at ~0x01002080 by
  Mac OS init; above the old 1 MB window). Fix: `MachineProfileIsNewWorld()` gated
  `vm_mac_acquire_fixed(0, 0x2000000)` and matching `is_mapped()` range update.
- Acceptance: 3/3 consecutive 60s all-on boots, no SIGSEGV, `dec_expiries > 5`.
- Emits: `[WAVE0] low memory extended to 0x0-0x2000000 (NK descriptors + 68k heap)`

**Wave 1** — map 0xFF000000–0xFFFFFFFF as anonymous zero (`main_unix.cpp`):
- Crash at `ea=0xFFFFEFD0` (DR emulator sign-extends 16-bit negative 68k address
  0xEFD0 to 0xFFFFEFD0 instead of masking to 24 bits = 0x00FFEFD0). Probe confirmed
  0x00FFEFD0 contains zero at boot time; anonymous zero mapping gives the DR the same
  result without full 24-bit aliasing.
- Acceptance: 2/3 consecutive 30s boots clean; `dec_expiries=2101+`.
- Emits: `[WAVE1] 24-bit DR alias mapped 0xFF000000-0xFFFFFFFF (zero)`

**Task B (verification)** — `[M12-VIDEO]` log in `VideoOpen` (`video.cpp`):
- `OP_NAME_REGISTRY` confirmed firing via `SS_PROBE_68K=0x500002fa`; `DoPatchNameRegistry`
  runs; video node registered at 0x81000000 640×480×32; `VideoDriverStub` live. No code
  changes needed for the ndrv injection path (already unconditional at name_registry.cpp:371).

**Task C (FAIL)** — `[FB-DIRTY] non_zero_pixels=0` after 180s:
- `irq_fired=0` in all healthy boots: 68k world runs with EE=0 / CGRP uninitialized →
  no interrupt delivery → QuickDraw never initializes → no pixel writes.
- `SS_M10_CGRP=1` arms interrupt delivery, but the ROM interrupt handler at 0x5000ED08
  immediately hits A-traps (e.g., 0xA9A8 at ROM+0xED06) that require the Mac OS Trap
  Dispatch Table — set up by the System file, not yet loaded at this boot stage.
  A-trap without trap table → `bra.s *` stop stub → boot parks/exits early.
- Frontier: 68k world stable 30+ seconds, video node registered, no CGRP delivery.
- M13 input: A-trap dispatcher initialization OR alternative QuickDraw init path.

---

## 2026-06-13 (session 6 — NewWorld coherence tooling)

### [SheepShaver] `make nw-northstar` boot-progress signal + discrete `[NW-PROG]` readout

Closes the NewWorld integration blind spot: nothing previously exercised the all-NW-gates-on
stack end to end, so an earlier machine-layer milestone could be silently regressed by a later
one. The per-opcode harness validates codegen; paravirtual `make e2e` validates "didn't break
8.6"; neither watches the NewWorld boot frontier.

- **`[NW-PROG]` readout** (`main_unix.cpp`, replaces the single `[PROGRESS]` line): discrete,
  self-documenting per-signal lines (config/nk-stage/dr68k/sched/irq), each greppable with a
  score word + inline threshold context. Raw `key=val` tokens preserved. Spec:
  `SheepShaver/docs/DIAGNOSTICS.md` "[NW-PROG] readout".
- **`make nw-northstar`** (`tools/nw-northstar.sh`): boots the all-on cluster via the slot
  protocol, classifies a `[NW-PROG verdict]`. Report-only by default (`--gate` enforces,
  `--history` trends). Verdict keys on **durable in-boot markers** (`[DR68K] first instruction`,
  `EXT delivered #1`), not crash presence — the boot is ~50/50 non-deterministic between a clean
  park and a known post-EXT frontier crash, so a bare SIGSEGV is NOT a regression (LEARNINGS
  2026-06-13).
- **QEMU rig device-tree capture** (`tools/qemu-rig.sh`): auto-dumps `info qtree` + `info mtree`
  to `device-tree.txt` — the topology/wiring/NVRAM oracle (NOT an address oracle).

Gates: build clean · harness 353/353 · machine suite ALL PASS · paravirtual `make e2e` PASS
(the `[NW-PROG]` reformat is paravirtual-reachable; no consumer greps the old `[PROGRESS]` line).

## 2026-06-13 (session 5 — M10 CGRP delivery)

### [SheepShaver] M10 CGRP init + 68k EXT delivery: `SS_PROBE_68K=0x5000ed08` fires

Implemented CGRP initialization for NewWorld NK interrupt delivery to the 68k level-1
handler. Gate: `SS_M10_CGRP=1`. Acceptance: `SS_PROBE_68K=0x5000ed08:5` fires — confirmed
match=1/5, no crash, clean SIGTERM at 60s (run `m10-retry1`).

Key design decisions:

- CGRP TABLE (40B) + STACK (40B) + DESC (8B) + STUB (64B) in guest RAM at 0x68ffc210–68.
  *(KDP-0x338) and CGRP+0x20 are armed at first DR68K dispatch in ppc-cpu.cpp.
- EXT shim in `sheepshaver_glue.cpp` re-syncs CGRP on every EXT delivery (NK overwrites
  TABLE_BASE between deliveries).
- STUB (16 words at 0x68ffc268):
  1. Load old_A7 from KDP+4; guard bltlr if A7 < 32KB (68k stack not initialized)
  2. `mr r12, r24` — save live r24 (interrupted 68k PC, NK-restored at CGRP RFI time)
  3. Push 6-byte 68k exception frame: SR=0 at r1+0, PC=r12 at r1+2 (r1 = old_A7-6)
  4. Set r24=0x5000ed08, update r16+0x1c4, branch to DR_WARM via CTR
- Rationale for `mr r12, r24`: r16+0x1c4 is unreliable — different NK context blocks
  (r16 varies by nesting level) have garbage there. Live r24 at STUB entry is the
  NK-restored interrupted DR PC, correct when the NK fully restores register state.
- Removed all M10-debug fprintf stubs (M10-RFI, M10-EXT-DELIVERY, M10-EXT-KDP,
  M10-CGRP, M10-ILLEGAL-RFI).

Known limitation (M11): `mr r12, r24` is correct when the NK fully restores r24 before
RFI to STUB. Non-deterministic crash can occur if r24 at STUB entry is not the
interrupted 68k PC (NK may modify r24 during CGRP delivery setup in some timing
scenarios). The probe fires in all tested runs; the crash-after-probe is an M11 item.

## 2026-06-12 (session 4 — VIA-IFR baseline fix)

### [SheepShaver] M9 VIA-IFR: fix baseline regression + correct shadow address

Fixed unconditional trampoline write of KDP+0x67c that stalled all NewWorld boots
(dec_expiries=5 regardless of SS_NW_VIA_IFR) by gating tp[25]–tp[26] on SS_NW_VIA_IFR.
Root cause: irq_post `sth r28,0(r23)` fires on every DEC interrupt and writes
`level|0x8000` to the KDP+0x67c target; the old target (hnfo_scratch+0xf8 = 0x68ff50f8)
is inside the NK-owned Hnfo scratch reserve → NK state corruption. Shadow moved to
0x68ff6084 (word after cold/warm discriminator, unallocated gap). No SIGSEGV. Baseline
restored (dec_expiries=2393 with full cluster). Harness: 353/353.

Still open: the via_nw901_int ROM patch itself (OP_IRQ+rte @0x5000ed08) reduces
dec_expiries to 6 with the full cluster — probable cause: 0x5000ed08 is entered via JSR
during early 68k init; rte corrupts the return path. See VIA-IFR-RECON.md §8.

## 2026-06-12

### [SheepShaver][docs] M8 SLOT-4 CONSUMPTION MILESTONE COMPLETE — SHIPPED GATED-OFF-GREEN: the R-II10 livelocks fixed, the consumption round trip live through the 68k handler; the default flip refused on honest criteria

Plan: `docs/superpowers/plans/2026-06-12-slot4-consumption.md` (rev 2). Arc:
`5a40bebd` (plan rev 1) / `f2f78f93` (red-team fold rev 2 — torn-ctx mechanism
statically confirmed pre-implementation) / `66f5298a` (Task 0: fork-(iii) confirmed
LIVE — mid-stub `EXT restart=0x50318018`, torn r10/r11=0x9040 ctx images, SRR0-image
slot pinned ctx+0xfc) / `5e3c4bde` (Task 0.5 coordinator ACK) / `42ce3e0e`+`09b74fe4`
(Task A: deferred EE-edge latch + the passive-latch re-pin) / `d03c6c41` (Task-A
review fold) / `02a0b74e`/`17e0d071`/`377edbf6`/`e6824327` (Task B: Q-C3 staging,
ticks_keepset, MODE_EMUL_OP fence, starvation-backstop re-pin) / `3e42aa82` (Task B
results — stop-rule-2 capture) / `f1fa89bf` (Task C P2 cleanup) / `52b69cd7` (Task C
decision). The instr-hardening trio `05e0d921`/`736ae4b8`/`f1aca585` + the `09821b67`
P0 fix (watch span + [WATCH-SAMPLE], R-II9 downgrade, r24-ring crash flush) rode the
same window — see their own entries below.

- **The decision (`52b69cd7`): `SS_NW_IRQ_CONSUME` stays default OFF** — a standalone
  17th gate (folding an OFF gate into the default-ON M7 cluster would flip it
  implicitly). Flip criteria (c)+(e) failed honestly: the post-flip-candidate config
  shows a NEW deterministic behavior line (**SC#1 r0=0x0d**, 2/2 vs 0/19 — the Q-C3
  stub's unconditional level-0 staging re-arming at the drain) with retirement RED, so
  "unchanged-or-better" is falsified. Flip prerequisites named: the VIA-IFR surface
  lands + the SC#1=0x0d divergence explained-or-fixed; **fold-into-cluster is
  MANDATORY at that flip** (the fix edits the riser path — consume-on+riser-off is
  undefined-by-construction).
- **Acceptance (env-on `SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1`)**: world switch completes
  (livelock-block probe visit=1 vs 10⁹ pre-fix; EXT restart at a REAL out-of-window
  PC); round trip GREEN through leg 7 (post → staging → drain → slot-4 twi → 68k
  level-1 handler at 60 Hz); fence held (zero host writers to per-event state);
  exactly-once arithmetic clean; gated-off A/B byte-identical behavior-line sets.
  Gates: task tier 5/5 on the final commits (incl. plain `make test-jit` 353/353,
  `make e2e-test` 122, machine suite ALL PASS). Boot budgets honored: Task 0 4/≤8,
  A 4/≤4, B 5/≤5, C 3/≤4 (crash/edge-miss reruns disclosed per protocol).
- **Re-score #3's named M3-class surprise adjudicated**: resolved as fork-(iii) — OUR
  riser stub's patch-created rfi non-atomicity, not unmodeled NK protocol; stop-rule 1
  never fired; seeds-not-services held (fix set = one latch + one donor-mirror staging
  stub + one poll kick; zero new service bodies). Re-score #4: trigger state recorded,
  not drafted (gated-off ship — MACHINE-LAYER-PLAN §9).
- **The named next frontier (stop-rule-2 capture): the VIA-IFR surface** — the 68k
  handler's source dispatch finds no VIA IFR source bit in the via6522 model and
  rte's source-less (0x5000eecc); the un-retired post re-traps slot-4 ~1.2k/s; Ticks
  NOT guest-claimed (host keep-set census-proven). Residue table: R-II10 CLOSED;
  R-II7 open-until-PIC-flip; R-II8 unchanged; R-II9 open-downgraded with the
  0x500eXXXX cross-ref; new SC#1=0x0d + EXT-edge-flakiness residues (consolidated
  table: INTERRUPT-INJECTION-RECON.md, M8 Task Z section).

### [SheepShaver] feat: M8 slot-4 consumption Task B — the Q-C3 from-emulator post staging fix + the starvation backstop; round trip live through the slot-4 twi and the 68k level-1 handler

Commits `02a0b74e` / `17e0d071` / `377edbf6` / `e6824327`, all gated
`MachineProfileIsNewWorld() && SS_NW_IRQ_CONSUME` (default OFF, flip-last at Task C).

- **Q-C3 fix (`02a0b74e`, rom_patches.cpp)**: the NK from-emulator interrupt-post leg
  (0x325520) ORed the CR arm into the volatile working r13 only — any ctx-reloading
  exit discarded it (Task-0 Q-C3; shape B's lost arm). The leg now detours through a
  12-word patch-space stub (0x2fd280) that also stages the NK's own deferred-post pair
  (`[KDP-0x440]`/`[KDP-0x43c]`) + task-flag 0x10, closely mirroring the NK's deferred
  leg (donor 0x325668–0x32568c; NOT word-for-word — scratch retargeted to r8 and a
  defensive null-task guard added, and the staging is UNCONDITIONAL where the inline
  leg level-tests; see the Task-C flip record). Verify-EXPECTED-first (exact 6-word match,
  GUARDED-SKIP banners). Live-proven: staging → scheduler-restore drain (0x324720) →
  **the slot-4 twi fires** (`PROGRAM srr0=5046e8d0 word=0fff0004 slot=4` — the
  re-graded PROGRAM#4 gate) → the 68k level-1 handler executes at 60 Hz.
- **Starvation backstop (`e6824327`, main_unix.cpp, one-iteration-rule re-pin)**:
  Task A's "next natural kick" premise falsified live (boot s4tb-b4:
  `deferred=1.43e6 fired=0`, the latched edge WAS the DEC's own delivery and the EXT
  edge is one-shot pre-WLSC). The 60 Hz tick thread now re-kicks `TriggerInterrupt()`
  while the deferred-edge latch is set — a poll kick, fake-poke fence untouched.
- **MODE_EMUL_OP fence (`377edbf6`)**: the M7-named carried item, landed
  single-variable; the host-side nested Execute68k injection arm is fenced under the
  same gate.
- **ticks_keepset census (`17e0d071`)**: host keep-set Ticks(0x16a) increments counted
  and printed in the `[IRQ-CONSUME]` dump — the Ticks-rider confounder census.
- **Honest remainder (stop-rule 2 capture)**: the 68k handler rte's source-less at
  0x5000eecc — the via6522 model presents no VIA IFR source bit, so the 60 Hz proc /
  OP_IRQ retire never runs and the post re-traps slot-4 (~1.2k/s). Next frontier =
  device-model (VIA IFR), not consumption machinery. Ticks NOT guest-claimed (the
  60 Hz movement is the host keep-set — census-proven). Named residue: the EXT edge
  one-shot is a knife-edge race (3/6 env-on boots never engaged). Full record:
  INTERRUPT-INJECTION-RECON.md § "Slot-4 consumption Task B".

### [SheepShaver] feat: M8 slot-4 consumption Task A — rfi-atomicity emulation (deferred EE-edge latch, `SS_NW_IRQ_CONSUME`, default OFF)

The shape-A restore-tail livelock (slot5-recon R-II10; Task-0 fork-(iii)) is patch-created
non-atomicity in OUR riser stub: the trap_return tail raises MSR.EE via mtmsr BEFORE the
ctx reloads + bctr complete, and `execute_mtmsr`'s EE 0→1 re-raise then delivers MID-TAIL
(observed `EXT delivered #1: restart=50318018`), saving a torn ctx (r10/r11 images =
0x9040 riser scratch) whose restore self-loops at 10⁹ visits/s. The raw NK `rfi` raises
MSR and resume PC atomically — this emulates that: the in-stub edge is LATCHED (no
trigger), the guest runs the reload+bctr unmolested, and the latch fires at the next
natural kick's poll (DEC cadence / host edge) at an out-of-window boundary; gate-on, any
in-window delivery poll is suppressed the same way (no boundary inside the windows can
deliver). Windows are SINGLE-SOURCE: `g_exc_riser_window` is filled at the rom_patches
trap_return patch site from the values it emits (stub [0x50318000,0x5031801c), reload
[0x503244e4,0x50324528) on 9.0.1); predicates are pure (`exc_core` +
test_exc_chain U14). Riser-conditional (rev-2 A7): riser opt-out ⇒ empty windows ⇒ latch
dead. **First-iteration falsification recorded in the plan addendum**: the initial
DEFER_NATIVE-idiom HANDLE re-arm hold starved the guest (the JIT exits on non-empty
spcflags before executing — held=1.12e9, guest frozen at 0x318018); re-pinned to the
passive latch per the one-iteration rule. Acceptance (env-on `SS_NW_PIC=1
SS_NW_IRQ_CONSUME=1`): EXT delivered at a REAL restart PC, deasserted (edge #2 unlocked,
edges=1 consumed=1), no livelock (livelock-block probe visit=1 vs 10⁹), boot reaches the
healthy nap-park regime; `[IRQ-CONSUME] deferred=1 held=1 fired=2 latch=0`. PROGRAM#4
slot-4 consumption itself moves to Task B by chronology (the post lands while the DR is
parked — Q-C3's image-selection fix). Gated-off A/B clean (zero new behavior lines).
Files: ppc-execute.cpp, ppc-cpu.cpp, exc_core.{h,cpp}, test_exc_chain.cpp,
rom_patches.cpp (window export only). Plan: `docs/superpowers/plans/2026-06-12-slot4-consumption.md`.

### [SheepShaver] fix: r24-ring crash flush actually wired into the SIGSEGV handler (P0 from review of f1aca585)

`f1aca585` defined `ppc_jit_r24ring_crash_flush()` but never called it — git
grep showed zero callers, so at that HEAD the r24 ring was NEVER flushed on
SIGSEGV (only on clean/SIGTERM exits via atexit): a net regression vs the
pre-f1aca585 ride-along, while three docs asserted the "directly and EARLY"
guarantee. Wired now: the SIGSEGV handler (sheepshaver_glue.cpp) calls the
flush right after the counters-only telemetry, BEFORE the re-fault-prone
`dump_registers()`/`dump_disassembly()`. Verified LIVE: a slot boot with a
deliberate crash trigger (`SS_SEED_MEM=0x68fff074=0x0;0x68fff078=0x0`
re-zeroes the Execute68k emulator pair at trampoline-end, recreating the Q1
wild-jump SIGSEGV) + `SS_DR_R24_RING=1` produced the `[R24RING]` dump
(1,015,149 transitions) positioned after `[EXC]` and before the register
dump. Once-guard composition re-verified: mid-run watch/stall/trigger dumps
call only `ppc_jit_dump_trace_ring()`, which no longer touches the r24 ring.
Two review P2s fixed in the same files: the span announce now prints the
slots actually added (not `len/4`) when the 8-slot cap drops words, and the
[WATCH-SAMPLE] comment now states the observation counter is one shared
record counter (not per-word). Stale pre-f1aca585 wording in DIAGNOSTICS.md
(`SS_RING_DUMP_FROM` section) corrected. Inert unless `SS_DR_R24_RING=1` /
`SS_JIT_WATCH_ADDR` set; crash-handler-path + instrument-init only, no
delivery/exception/codegen logic touched.

### [docs] R-II9 re-test: SS_PROBE_LINEAR no longer crashes under the delivery regime (0/2 at HEAD) — suspicion downgraded

Bounded re-test (2 boots, slot0 20260612-051031/-051246): full env-on cluster +
the 8-probe delivery-chain set + `SS_PROBE_LINEAR=1` + trace ring + r24 ring —
0/2 crashes (vs the original 2/2), both boots reaching the known shape-A
consumption livelock like no-LINEAR boots. The probe path is read-only (stderr
prints only), so LINEAR could only perturb timing; the original crashes ran
pre-`2a452166` (ClearInterruptFlag lost-edge race fix) code — the plausible
retirer. No guard landed (it would suppress exactly the highest-value fires:
delivery entry/restart probes). Status + standing discipline updated in
INTERRUPT-INJECTION-RECON.md (dated note) and AGENT-CONTEXT.md.

### [SheepShaver] instrument: SS_DR_R24_RING crash-dump gap fixed (early direct flush; mid-run dumps no longer eat the once-shot)

The r24 ring's SIGSEGV flush rode `ppc_jit_dump_trace_ring()`, which had two
recorded dump gaps: (a) mid-run trace dumps (watch change dumps, the stall
dump, the ring-dump trigger) consumed the once-guarded crash shot, so a later
real crash printed nothing; (b) the SIGSEGV handler reached that dump only
AFTER `dump_registers()`/`dump_disassembly()`, which can themselves re-fault
and kill the process first. Fixed: the handler now calls the new
`ppc_jit_r24ring_crash_flush()` directly, EARLY (right after the
counters-only dumps), and `ppc_jit_dump_trace_ring()` no longer touches the
r24 ring. Honest trigger conditions documented (DIAGNOSTICS.md): only
SIGSEGV/SIGBUS-routed crashes and clean/SIGTERM exits (atexit) flush the
ring — SIGTRAP deaths still produce no dump. Inert unless `SS_DR_R24_RING=1`
(the flush is a no-op otherwise); no delivery/exception/codegen logic touched.

### [SheepShaver] instrument: SS_JIT_WATCH_ADDR span form + [WATCH-SAMPLE] periodic sampling (watch blindness fixes)

Two watch blindness classes cost ~3 boots in the M7 cycle: (a) value-identical
stores (zero-over-zero) never trip the change detector; (b) multi-word lowmem
longs watched on the wrong word (Ticks' LSB lives in 0x16c, agents watched
0x168). Landed in `jit_ring_record()` (`ppc-cpu.cpp`): **span form**
`SS_JIT_WATCH_ADDR=ADDR:LEN` (hex bytes, cap 0x10) expands to per-word watch
slots; slot cap raised 4→8 (per-record cost = one `vm_read_memory_4` + compare
per slot, trivial next to the ~30-field ring record this rides on, and the
recorder only runs under `SS_JIT_TRACE_RING=1`); **`[WATCH-SAMPLE]`**
edge-independent value samples at logarithmic record counts (1, 10, 100, …)
per watched word — frozen-vs-moving provable without a change edge. Honest
limitation documented in DIAGNOSTICS.md: the watch remains a sampling
change-detector; true store-event detection (codegen store instrumentation) is
out of scope. Evidence (slot0 20260612-050606): `[WATCH] span 00000168:8 -> 2
word slot(s)`; samples show 0x16c moving (Ticks `000b0000`) while 0x168 stays
0 post-init — the exact previously-blind case. Inert when unset (same
parse-once pattern; paravirtual unaffected — the watch block is reached only
with `SS_JIT_TRACE_RING=1`). Gates: inner PASS (353/353, machine ALL PASS).

### [SheepShaver][docs] M7 INTERRUPT-INJECTION MILESTONE COMPLETE — first host→guest interrupt through the guest's own chain; first guest IACK of the OpenPIC model; riser/published/host-irq cluster newworld DEFAULT

Milestone summary (per-task detail in the entries below + the plan
`docs/superpowers/plans/2026-06-12-interrupt-injection.md`; evidence
`docs/planning/machine/INTERRUPT-INJECTION-RECON.md`). Commit arc: `153c088b`
(plan) / `c57d87f9` (rev 2, two red-team rounds folded) / `533a3c43`
(entry-gate actuals) / `2668ead9` (Task 0 recon — EXT route verdict, first
forced EXT deliveries ever, the A1 level-source livelock proven in vivo
68.6M deliveries/40 s) / `b124556f` + `de659b6c` (Task A — `SS_NW_HOST_IRQ`
once-per-assert-edge latch, Q-I6 fence narrowing, FIRST live host-sourced EXT
delivery) / `eee33f16` + `77d8ac6d` (Task B verify-don't-build — break pinned
to the NK post's zero-level test; level-source staging signed off, shape (i)) /
`68c8fc3e` + `3de826b2` (Task B-2 — PIC-rail level staging, FIRST guest IACK,
the level test PASSES) / `b3e51b8d` (slot5-recon — the park is the NK idle nap
loop; new frontier = the slot-4 consumption livelock, three shapes pinned) /
`2a452166` (pre-flip checklist 4/4 incl. the ClearInterruptFlag lost-edge race
fix) / `81d60cc1` (THE CLUSTER FLIP). **Acceptance numbers:** exactly-once per
assert edge (`edges=1 consumed=1 deasserts=0 pending=0`; zero TRIPWIRE lines
across every boot); env-on watch 68fff070 trips `value=80010000` (the level
test passing); `first-iacks: src=0x3f vec=0x3f` (guest-traversed retirement —
the IACK lowers the line); deferred_native 97→0 under the narrowed fence; DEC
regime healthy at ~2.0 mtspr/delivery; full gates 6/6 PASS pre- AND post-flip;
all-OFF opt-out byte-identical to the E4 baseline class (blocks=7354 exact).
**The flip:** `SS_NW_EE_RISER` + `SS_NW_DEC_PUBLISHED` + `SS_NW_HOST_IRQ` are
the newworld DEFAULT (explicit-"0" opt-out each); post-flip default boots
deliver DEC #1–4 (2-SPR) + the first host-sourced EXT delivery with NO env
vars and still reach the PROGRAM#5 park; `SS_NW_PIC` stays default-OFF/HELD
(env-on test cluster only — it carries the B-2 level staging, so default-boot
EXT deliveries carry level 0, the real chain's correct pre-init behavior).
**Honest remainder, named:** consumption — the armed 68k post is never
consumed/retired by the DR/68k world (the slot-4 round trip = the named next
task); Ticks has never been guest-claimed (the host keep-set ticks it; the
guest `addq.l #1,$16a` has never run). Docs close-out (this entry's commit
group): DIAGNOSTICS M7 section, MACHINE-LAYER-PLAN re-score #3 (draft for
coordinator confirmation), wave2 plan CLOSED/SUPERSEDED with the final W2-4
disposition table, LEARNINGS, AGENT-CONTEXT frontier refresh.

### [SheepShaver] M7 Task C — THE CLUSTER FLIP: SS_NW_EE_RISER + SS_NW_DEC_PUBLISHED + SS_NW_HOST_IRQ are the newworld DEFAULT — the first host→guest interrupt delivered through the guest's own chain on a default boot

Interrupt-injection milestone Task C (`2a452166` checklist + `81d60cc1` flip; plan "Task
C results"). Pre-flip checklist 4/4: (1) run-exc.sh gate guard — BASE_ENV pins
`SS_NW_DEC_PUBLISHED=0` per lane + a loud abort on gate-on-without-`SS_EXC_ENTRY`
wiring + a glue-side warning for ad-hoc SS_TEST_HEX runs; (2) stub=1 six-words
verified-accepted (defined zero tail after blr, harness-only path); (3) the
ClearInterruptFlag lost-edge race FIXED (re-check `InterruptFlags!=0` after Deassert,
re-run the full assert-edge path); (4) tripwire-counter masking accepted-with-note
(split per source before any default co-arm of SS_NW_PIC with device sources).
**The flip** (explicit-"0" opt-out each, SS_NW_SC_SURFACE polarity): the W2-4-reserved
riser/published flip + the host-irq default land together; `SS_NW_PIC` stays HELD
(default OFF — the B-2 level staging remains test-cluster-only, so default-boot EXT
deliveries carry level 0 until the PIC flip, the real chain's correct pre-init
behavior). **Battery:** full gates 6/6 PASS pre- AND post-flip (incl. paravirtual e2e),
task 5/5, exc lane 14/14; 4 of ≤4 boots — env-on re-asserts Task A+B-2 (EXT delivered
exactly-once, watch 68fff070=80010000, first-iacks 0x3f, zero tripwires, DEC
~2.0 mtspr/delivery); **post-flip default boot delivers DEC #1–4 via 0x50313200 (2-SPR)
+ the first host-sourced EXT delivery with NO env vars and still reaches the PROGRAM#5
srr0=50324fec park with the full E4 census**; post-flip opt-out (`SS_NW_*=0`)
reproduces the pre-flip default byte-identically (blocks=7354 exact). The block/hit
counters are recorded as host-timing-coupled and dropped from the baseline-class
definition (behavior lines only). Dispositions written in the plan: XLM_IRQ_NEST drift
documented-as-dead (both consumers' predicates unreachable from −1 downward),
MODE_EMUL_OP injection arm stays (fencing = named follow-on with the next milestone's
HandleInterrupt rework), R-II3/R-II10 → the slot5-recon section (`b3e51b8d`: the park
is the NK idle nap loop; the new frontier is the slot-4 consumption livelock).
M3A-ENTRY-TABLE dual-mode row updated (KDP shim = opt-out path; retirement a named
follow-on).


### [SheepShaver] M7 Task B-2 — the host source joins the PIC rail (sign-off shape (i)): THE LEVEL TEST PASSES — first guest IACK of the OpenPIC model, post writes level|0x8000, CR bits SET

Interrupt-injection milestone Task B-2 (plan "Task B-2 results" + recon "M7 Task B-2").
The signed-off level-source staging: the Task-A host latch now also wiggles a RESERVED
PIC input (`OPENPIC_IRQ_HOST` 0x3F — no real DEC/timer PIC input exists,
KEYLARGO_MAX_TMR=0, QEMU openpic.h:41 @ de5d8bfd…) on exactly the assert edges, and the
init-time platform constants Mac OS's native MPIC init would write are staged:
IVPR[0x3F]=edge|prio8|vec0x3F + IDR=CPU0 + CTPR=0 (model, bring-up single-threaded) and
`[[KDP-0x20]+0xf18]`=0xF3040000 (NK-held PIC base, fallback read 0x50325f48) +
`[0x3f3f]`=1 (vector→level byte, 68k level-1) in the NW trampoline. Every per-interrupt
EVENT stays guest-traversed (assert → EXT delivery → fallback IACK lwbrx-over-MMIO →
vector → level → post; retirement = the guest's own IACK consuming the edge source —
zero host writes to pending/CR/per-delivery state). **LIVE (env-on test cluster
+SS_NW_PIC=1; default flip HELD):** `first-iacks: src=0x3f vec=0x3f` (the model's first
guest consumer, F16 byte-lane validated end-to-end), `[WATCH] addr=68fff070
value=80010000` (Task B's break link RETIRED — R-II7 closed env-on), exactly-once
re-proven, zero tripwires, gated-off A/B byte-identical (blocks=7354 exact). **The break
moved one link PAST the level test (R-II10, frontier-record per stop-rule 2):** the
parked NK spin regime never runs the DR/68k world again post-delivery, so the armed
0x8001 is never polled — via_int/OP_IRQ probes 0 matches. **Ticks correction of record:**
Ticks HAS been moving via the host HandleInterrupt MODE_68K keep-set (glue:3399); Task
B's "Ticks did not move" watched 0x168 = Ticks' HIGH half (LSB is in word 0x16c). The
"first guest addq.l #1,$16a" headline remains unclaimed. dev_openpic.cpp untouched
(header constant only; 206 oracle checks unchanged). Gates: task tier 5/5 + exc lane
14/14; 4 of ≤5 boots.

### [SheepShaver][docs] M7 Task B — consumption round trip verified to its break link: the NK post fires on host-sourced EXT deliveries, the round trip dies at the post's zero-level test; Ticks unmoved (verify-don't-build, zero source changes)

Interrupt-injection milestone Task B (plan "Task B results" + the recon's "M7 Task B"
addendum). 4 slot boots, env-on. **PROVEN LIVE:** host edge → `EXT delivered #1` →
the [KDP+0x5b0] fallback service body (its `[KDP+0xe80]` entry counter incremented
delivery-adjacent and for nothing else all boot — the watch discriminator) → the 68k
post body 0x3254e0 (r23=0x68fff070, r7 from-emulator bit SET, skip leg zero visits).
**THE BREAK LINK, instruction-pinned:** at the current frontier the posted level r28=0
(R-II7 — PIC IACK unmapped + lowmem 0x3f00 table zero), and at r28=0 the post stores
0x0000 (a value-invisible write — the change-detecting watch on 68fff070 is
structurally blind to it, instrument fact recorded) and actively CLEARS the
emulator-CR interrupt bits (`and r13,[KDP+0x678]`) — the DR dispatch poll gets a null
signal, via_int/OP_IRQ/Ticks never run (`SS_PROBE_68K` 0 matches ×2 boots; Ticks=0;
`[0xcfc]` pre-WLSC throughout). Per the coordinator re-grade this is the "dies exactly
at the level test" branch: the **level-source staging proposal** (the two guest-init
words `[[KDP-0x20]+0xf18]` + `[0x3f00+vector]`, trampoline-staged-constant class, not
the fenced fake-poke) is written in the recon addendum and the task STOPPED for
sign-off. Invariants green: zero tripwires ×4, exactly-once re-proven, deferred_native=0,
DEC/sc/program censuses at baseline class. New named anomaly R-II9: `SS_PROBE_LINEAR=1`
env-on boots crashed 2/2 (vs 0/2 without) — the instrument is suspect under the
delivery regime. Doc-only commits (no gates per the stated-reason rule).

### [SheepShaver] M7 Task A — the FIRST host→guest interrupt delivered through the guest's own exception path (`b124556f`)

Interrupt-injection milestone Task A
(`docs/superpowers/plans/2026-06-12-interrupt-injection.md` rev 2 + Task 0 pins). New
gate `SS_NW_HOST_IRQ` (default OFF, newworld-only): host interrupt posts
(`SetInterruptFlag`, the `InterruptFlags≠0` level) are forwarded through a dedicated
**deliver-once-per-assert-edge latch** (`exc_host_irq_latch` — a THIRD semantics
beside the one-shot DEC latch and the level-held PIC seam, which stays untouched per
W2-3 C1) with a TriggerInterrupt kick on each 0→1 edge; the delivery hook consumes the
latch at EXC_EXTERNAL delivery. Sources OR-compose at the poll site + the six EE
re-raise compose sites (A5 — no clobber). The bare-level alternative is Task 0's
proven livelock (68.6M deliveries/40 s). Companion change (Task 0 Q-I6 verdict (ii)):
the DEFER_NATIVE run_mode fence is **narrowed route-aware** — skipped for
published-handler deliveries (2-SPR shim writes SPRG1/SPRG2 only), kept for the legacy
KDP-shim DEC route. Acceptance (slot0 boots, riser+published+host-irq on): first live
host-sourced `EXT delivered #1 → entry=50314880`; exactly-once-per-edge proven
(`host-irq: edges=1 consumed=1 pending=0`); zero tripwires; DEC chain at real cadence
under the narrowed fence (delivered_dec=4340@50 s all (2-SPR), deferred_native=0,
mtspr_dec ≈ 2.0/delivery); gated-off A/B byte-identical to the E4 baseline class
(blocks=7354, PROGRAM#5 srr0=50324fec, identical sc census, zero new output). Harness:
`SS_TEST_HOST_IRQ` knob + H9 vector pin the once-per-edge consume (lane 14/14). Gates:
task tier 5/5.

### [SheepShaver] NewWorld 9.0.1 — P-M5 SIGSEGV cleared: Execute68k ported to the trampoline boot via [KDP+0x1074]/[KDP+0x1078] staging (`34d3d441`)

M7-critical item 1 (INTERRUPT-INJECTION-RECON.md Q1/Q5). The P-M5 crash (guest
pc=0x100000 ~0.11s in) was NOT the suspected host-TM-expiry injection: the boot
sequencer's OP_NAME_REGISTRY step calls FindLibSymbol → Execute68k, which reads the
kernel-data emulator pair `[KDP+0x1074]` (68k opcode dispatch table) / `[KDP+0x1078]`
(emulator code base) — both NULL on the trampoline boot, so `execute(0x558f*8)` ran
zeroed low RAM into the 0x100000 fetch fault. Fix: stage the mirror-world values
(0x50480000 / 0x50460000 — the mirror equivalents of `patch_nanokernel_boot`'s
LA_DispatchTable/LA_EmulatorCode pair, rom_patches.cpp:1514-1515) in the newworld
trampoline staging block. **New default-boot baseline**: 44s full-window session
parking at `PROGRAM #5 srr0=50324fec slot=5` with the sc-selector storm
(0xffffffff x233) — the recon's seed-proven frontier, now unconditional. Also de-mines
the MODE_EMUL_OP injection arm and OP_IRQ's TimerInterrupt task calls (Execute68k as a
synchronous tool now works on newworld). Structurally inert on paravirtual (newworld
trampoline block). Gates: inner.

### [SheepShaver] Machine Layer W2-4 item 1b — post-DEFER_NATIVE wake-up edge: first live published-route DEC deliveries (`3cb3b16e`)

M7-critical item 2 (INTERRUPT-INJECTION-RECON.md Q4 option (c)). A native-window
deferral consumed the HANDLE spcflag and nothing re-asked — the latched DEC starved
(D-7: deferred_native=97, delivered_dec=0). The delivery hook now re-arms
`SPCFLAG_CPU_HANDLE_INTERRUPT` on `EXC_DECIDE_DEFER_NATIVE`, **bounded** by a
per-episode budget (cap 65536, reset on any non-native decision) after the recon's
"bounded by window length" claim was falsified live — the post-item-1 frontier parks
inside a native window that never exits (unbounded variant: 5.3e9 re-polls in 50s,
regressed frontier; falsification + re-pin recorded in the recon's RESULTS addendum).
check_spcflags (newworld branch) skips the legacy HandleInterrupt fall-through on a
re-armed flag (no SDL_PumpEvents / MODE_EMUL_OP-arm amplification). Host-side only —
exc_core untouched. Riser-on acceptance: **delivered_dec=3, the first live deliveries
on the published 0x50313200 (2-SPR) route; pending drained to 0; mtspr_dec=25 (no
storm); sc/program exactly at the item-1 baseline. Ticks still frozen (expected —
consumption is item 3, the EXC_EXTERNAL/PIC routing).** Default-boot A/B: baseline
signature reproduced, cap fires once, no spin. Gates: task tier; boots 6/6.

### [SheepShaver] NewWorld 9.0.1 — SysError-12 wall (P-M4) cleared: HLE Time Manager via trap-table image population (`f808a7fb`, `adea99bc`)

The 9.0.1 ROM ships NULL trap-table-image entries for the TM cluster {0x58
InsTime/InsXTime, 0x59 RmvTime, 0x5a PrimeTime, 0x93 Microseconds}; upstream's exact
EMUL_OP stub bodies now live in new ROM patch space (TIME_MANAGER_PATCH_SPACE=0x2fd240)
with the image entries populated verify-zero-first, so the guest's own installer
(ROM 0xe0c8) installs them at boot. Gate `SS_NW_TM_TRAPS`: newworld default-ON ("0"
opt-out). Also lifts the `patch_68k()` .Sony DRVR-4 hard abort (lenient mode) that
silently dropped the whole EMUL_OP tail (ADBOp/PowerOff/scrap now apply on 9.0.1);
every resumed-tail write site is verify-target-first (14-site audit in `f808a7fb`;
the SERD-0 ROM-offset-0 clobber hazard guarded). Enable60HzInts now completes; new
frontier **P-M5** = SIGSEGV guest pc=0x100000 ~0.13s in (suspected host-TM-expiry
Execute68k delivery — see TRAP-TABLE-RECON.md fix record). Paravirtual: live
`make e2e` PASS.

### [SheepShaver] Machine Layer W2-4 — DEC reload storm RESOLVED: NK scheduler timebase-frequency global was never staged (`21704614`, `f31d475e`)

The D-6 EE-riser DEC storm (250K deliveries/s, 68k world starved) is fixed. Root
cause: `[KDP+0xf2c]` is the NK scheduler's timebase-frequency global (ticks/second
— units pinned by the ROM's own duration→ticks helper at 0x50323708, which divides
it by 250000 for µs conversions); NK cold-init zeroes it and the hardware config
path that loads it never runs in the trampoline boot, so every timeslice deadline
computed `now + 0` → `mtdec 0` → instant re-expiry. Fix: stage
`[KDP+0xf2c] = TimebaseSpeed` in the newworld trampoline block (same family as the
existing `KDP+0xf6c` staging; structurally inert on paravirtual). Also lands a
default-on capture-only instrument: VirtClockWriteDEC value+PC ring and value-class
buckets in the `[VCLK]` dump — the channel that pinned the zero writes to the NK
timeslice delta path in one boot. Riser-on acceptance: mtspr_dec 14.68M → 8 in 60s,
reload values = the genuine 1.042ms NK timeslice, 68k world un-starved
(jDR 14 → 4.8B; sc=173/program=4 = exact gated-off baseline). New successor item:
delivery now defers on the run-mode native fence in the cold 68k world
(deferred_native=97, `[XLM_RUN_MODE]` never cleared) — see EE-CHAIN-RECON.md D-7.
Gates: task tier 5/5 PASS (test-jit plain 353/353, machine suite 13/13, e2e-test 122).


### [SheepShaver] Machine Layer W2-4 steps 1+2 — the EE riser lands; FIRST DEC deliveries ever (12.4M, zero crashes), storm-bounded (`10b1b3e8`)

The trap_return replacement stub at ROM 0x318000 gains an env-gated **EE riser**
(`SS_NW_EE_RISER=1`, newworld-only, default OFF — the flip is W2-4 final acceptance):
an EE-only MSR compose `mfmsr r10; rlwimi r10,r11,0,16,16; mtmsr r10` consuming the
SRR1 image in r11, per EE-CHAIN-RECON "W2-4 entry decision" candidate (a) with all
coordinator sign-offs. mtmsr's interpreter EE-edge re-raise drives delivery through the
proven W2-0/W2-1 machinery — no new mechanism. Word budget verified against the RAW
ROM (the stub area is the NK AltiVec lvebx thunk table; +3 words = the same
reachability class as the accepted upstream clobber). Gate-off byte-identical
(SS_DUMP_ROM A/B: delta is exactly the 4 designed words; paravirtual structurally
inert). **Scored boot** (`SS_NW_EE_RISER=1 SS_NW_DEC_PUBLISHED=1`, the deferred P3
session): **delivered_dec > 0 for the first time — 12.4M deliveries in 50s, all on
the published 0x50313200 2-SPR route, zero corruption** — but a DEC re-expiry storm
(~250K/s; mtspr_dec ≈ dec_expiries = 14.68M) starves the 68k world (jDR=14, Ticks
frozen, nest drifting −1/delivery). Links 3/5/6/10 score READY, links 7/8 BROKEN as
predicted; falsifications NONE. W2-4's remaining body re-shaped: DEC reload cadence
first, then XLM_IRQ_NEST ownership, then the tick consumption path. Full per-link
table + storm anatomy: EE-CHAIN-RECON.md D-6.

### [SheepShaver] Machine Layer — tm_task ROM-patch misalignment fixed (the 0x505bb060 slide wall); new frontier = SysError 12 at _InsXTime (`2ff7765f`)

The 0x505bb060 slide (SLIDE-WALL-RECON.md `f786fd99`) is fixed with a
**verify-EXPECTED-first guard** on the `tm_task` patch's +28 NOP write
(rom_patches.cpp): the 6 NOPs are written only when both covered slots start a
`bsr.l` (0x61ff), the 1.1 layout — pinned offline by decoding the 1.1 ROM with a
throwaway `rom_decode.hpp` host tool (no boot needed; base=0x2f8, two TM-install
`bsr.l` calls at +28). On 9.0.1 the lenient-relocated anchor (0x262) puts +28
mid-instruction (observed `038e 2e48 90fc 2000 a02d 6100`) — the guard fires a loud
`[ROMPATCH] tm_task GUARDED-SKIP (9.0.1 misalignment)` and skips; `SS_NW_TM_TASK_FORCE=1`
restores the old write for A/B (reproduces the baseline crash tuple exactly).
Sibling sweep: all 25 lenient-RELOCATED patterns tabled (SLIDE-WALL-RECON appendix);
tm_task was the only mid-instruction corruptor; one SUSPECT-semantic residue recorded
(scsi_mgr base+0x20 second-entry stub, unguarded). Gating: ROMType-gated (not
profile-gated) — paravirtual's 1.1 boot reaches the guard and passes it (byte-identical
ROM); live `make e2e` smoke PASS. **New frontier P-M4**: a deliberate guest park —
SysError 12 (dsCoreErr) at 0x500047ae `bra.b *`; probe-pinned D0=0x0c, D1=0xa458 =
**_InsXTime**: Enable60HzInts now RUNS and dies because OS trap #0x58 (InsTime) has a
NULL trap-table entry — the 60Hz TM task does not install yet. The chain to ticks is
now the guest's own InsTime → TM-task → timer-interrupt path (trap-table population +
real delivery, not ROM patching) — feeds W2-4 directly.

## 2026-06-11

### [SheepShaver] Machine Layer Wave-2 W2-4 step 0 — DEC delivery re-pointed to the NK-published handler 0x50313200 + 2-SPR shim (gated, default OFF)

DEC delivery's target is now gate-selectable from the save-and-switch body 0x50412b1c
(KDP register-save shim — the M3a shape) to the NK-published DEC handler **0x50313200**
(`[KDP+0x384]`, [PROBE✓] re-verified live this task; primary copy per the publication
precedent), harmonizing all four exception classes (DEC/sc/program/EXT) on the
published-handler + 2-SPR-shim pattern (SPRG1:=caller r1, SPRG2:=caller LR). Retires the
W2S-R1 r9 hazard and the cr6/cr7 flag-composition hazards on the published route
(EE-CHAIN-RECON.md D-3/D-4, coordinator sign-off item 2). Gate: `SS_NW_DEC_PUBLISHED=1`
(default OFF — the flip is W2-4's final acceptance); the gate selects the shim shape,
`SS_EXC_ENTRY` overrides the entry value only (precedence unchanged); the KDP shim stays
as the gate-off fallback path. Harness: new run-exc.sh **H8** vector trio (gate-on DEC
delivery end-to-end with `SS_EXC_BARE=0` — a KDP-shim regression would fault; extended
capture stub `SS_TEST_EXC_STUB=2` pins the SPRG1/SPRG2 rows in the REGDUMP; "(2-SPR)"
delivery-tag discrimination vs the H1 control), lane 12/12. Live: gate-OFF boot
signature md5-identical to the pre-change baseline; gate-ON differs only in the
`[NW-DEC]`/entry-table lines (live-inert — delivered_dec=0 until the W2-4 step 1 riser).
Honest scope: the harness cannot execute 0x50313200 itself (no NK there — entry
overridden to the capture stub); end-to-end execution through the published handler
lands with step 1. test_exc_chain not extended (decision logic untouched — entry value +
host-side shim only). Records: M3A-ENTRY-TABLE.md "W2-4 step 0", EE-CHAIN-RECON.md D-5.

### [SheepShaver] Machine Layer M6 — 68k PC-desync: DR r0≡0 invariant re-assert; DSAT wall PASSES; boot 0.16s→4.8s, sc 13→169; newworld DEFAULT (`c8429b23`, `ac50b2c9`, `959e209e`, `86b1b1f7`, `25be4342`, `2024a835`)

The 68k PC-desync wall (post-FE1F frontier, System Error ID 10 → DSAT underflow) is
**resolved**. Root cause (Task 0, `c8429b23`): the DR's **r0≡0 invariant is poisoned by the
FE1F `twi`-delivery ctx save**. The NK's exception prologue (0x313d40) saves the in-flight
r0=selector (0x31/0x36 for the two FE1F callouts) into the 68k ctx slot +0x104 — but the
NK's save-and-switch (0x312b0c) deliberately omits r0 (it is protected by invariant, not
slot), so the ctx slot retains the stale selector. The merged restore tail then re-poisons
r0=0x36 on every subsequent switch-in, and the e388 selector shim's `addco.`-class writeback
mis-dispatches: 0x5c + 0x36 = 0x92 → jmp at e380+0x92 = 0xe412 → line-1111 → SysError 10.
On real hardware the slot-8 callout is a parcels-patched direct call (no exception, no ctx
save) — the placeholder-twi delivery is what introduces the poisoned save. The patched chain
constants (`trap_return` / `m68k_excp_tbl` / `sprg3`) are all **exonerated** [RING✓].

Fix (Task A, `ac50b2c9`): 3-word stub at rom ROM+0x429da0 (verify-zero-first) —
`li r0,0; lwz r1,0x10c(r3); b 0x5046e1a4` — patched at the slot-exit re-entry site
0x5046e1a0 (the `SS_NW_DR_R0_INVARIANT` gate, polarity MachineEnvFlag, `be0e02cb`-style
bring-up). Apple's own db7c idiom; NW-gated; legacy patch bodies untouched.

**Boot transformation (Task B evidence `86b1b1f7`, Task C acceptance `25be4342`+`2024a835`):**
- Boot time: 0.16s (old DSAT wall) → **~4.8s** JIT-time (new frontier)
- PROGRAM deliveries: 2 → **4** (both FE1F invocations' $31/$36 pairs, all conformant)
- `sc` deliveries: 13/9-distinct → **169/16-distinct** (new: 0x1b/0x1c/0x07/0x0c/0x08 ×3;
  0xfffffffe ×17; 0xffffffff ×103 — the negative-selector candidate surface, flagged)
- `deferred_ee=5`, CUDA quiet (13 packets), VIA/SCC MMIO traffic — machine layer composing

**New frontier (stop-rule 2 captured, NOT chased):** `pc=0x505bb060` — beyond the staged-copy
end 0x50500000; control flow slides through zeros to 0x55590000 → host SIGSEGV. Captured in
`DSAT-WALL-RECON.md` Task A results section.

**`SS_NW_DR_R0_INVARIANT` is the newworld profile DEFAULT** since Task C (`25be4342`);
`=0` opt-out restores the byte-identical DSAT baseline. All gates green throughout
(batch+plain test-jit 353/353 score=100, machine 13/13, e2e-test 122; paravirtual
byte-identical by inertness argument + gated-off A/B boot). Zero falsifications,
one-iteration rule never invoked. Plan: `docs/superpowers/plans/2026-06-11-68k-pc-desync.md`.

### [SheepShaver] Machine Layer tooling: gates.sh tiered runner + ss-slot-boot --expect + ring-walk analysis script + §6b dispatch economics (`50ba9eb5`, `5859c729`, `5f7cae93`, `7d7ae3ae`)

Workflow and analysis tooling landed alongside the desync and W2-3 milestones:

- **`tools/gates.sh` tiered gate runner** (`50ba9eb5`): `gates.sh <inner|task|full> [--reason "…"]`
  runs the appropriate gate tier (inner = build+batch-jit+machine; task = +plain-jit+e2e-test;
  full = +paravirtual-e2e). Prints one `GATE …: PASS|FAIL` line per gate and a final
  `GATES <tier>: PASS|FAIL` verdict; on FAIL, prints the failing gate's last 20 lines. Per-gate
  logs preserved in the printed tmpdir. Agents read the summary lines, not raw gate output.
- **`tools/ss-slot-boot.sh --expect`/`--absent`** (`5859c729`): boot-log assertion contract —
  `--expect 'PAT;;…' [--absent 'PAT;;…']` checks grep patterns against the boot log, prints
  `EXPECT: n/m present, k absent-violations` + `BOOT-VERDICT: PASS|FAIL` (exit 0/3). Agents
  grep targets, not log reads.
- **`tools/ring-walk.py` trace-ring analysis** (`5f7cae93`): dump-analysis script for
  trace-ring + R24RING output — `--window START:END`, `--r24-flow` (68k-PC transitions;
  odd-PC/odd-delta flagged DESYNC-CANDIDATE), `--find-pc 0xPC`, `--regs-at REC`. Agents stop
  reading raw ring text — point at a boot log or slot rundir.
- **`docs/MILESTONE-WORKFLOW.md` §6b dispatch economics** (`7d7ae3ae`): model tiering (economy
  tier for doc sweeps/read-only recon; pro tier for implementation; ultra for adversarial review
  only), task-card pattern, verdict-over-logs principle.

### [SheepShaver] Machine Layer M3b Wave 2 W2-3 — OpenPIC bus wiring + EXC_EXTERNAL delivery: the interrupt chain's first construction, shipped gated-off (`SS_NW_PIC`, flip HELD) (`b2e0d718`, `7cafd6ae`, `95d3fc53`, `81e3ea4a`)

The EE-chain ladder's first construction (links 1 + 11 of EE-CHAIN-RECON.md §A): the
already-landed OpenPIC model (206 checks) registered on the M1 bus at 0xF3040000+0x40000
(contained overlap in the macio stub), with **new device interrupt-condition state** —
the SCC Rx-int predicate (Rx-available ∧ WR1 Rx-int-enable ∧ WR9 MIE, chip-wide WR9 copy;
rev 2 F4) and the VIA IFR&IER summary edge — feeding PIC inputs 0x25/0x24/0x19
(transition-only seams, device→pic lock order BINDING). PIC output → a single-copy-atomic
EXT pending flag + CPU kick (F5). The delivery hook gains the level-held EXT source
beside the DEC latch (DEC-before-EXT, one-shot-vs-level-held justification; runaway +
U13-starvation tripwires); **`ExcEnter(EXC_EXTERNAL)` now consumes `external_entry`
(0x50314880, the NK-published [KDP+0x374]) — the sanctioned U12 flip, both arms pinned**
— with the sc/program 2-SPR shim per the Q-W2 verdict. Byte-lane = LE value-swap
[STATIC-oracle] (F16; FRR-read falsifier armed at the seam, untested live — the guest
never reads the PIC). exc= tuple gains `delivered_ext` as a 7th field printed only when
configured; `[PIC]` stats/first-IACK record on atexit/term-dump/crash/heartbeat.

**Acceptance = the pre-declared downgrade (stop-rule 3, flip HELD):** live acceptance is
unreachable — "PIC initialized: NO" ([PIC] reads=0 writes=0, CTPR still 15) AND "EE
riser: NONE" (every EXT kick lands as deferred_ee). One sanctioned `[DIAG-FORCED]` boot
pair proved the chain mechanically live for BOTH source classes (SCC inject → 0x25 leg;
**197 VIA summary edges per boot** traverse device→PIC, mask-gated correctly when
unforced); the EXT delivery itself is proven at harness level both modes — the
test-exc-vectors lane gains **H6** (EXT delivery + entry discrimination + SRR1.EE=1 pin)
and **H7** (dual-pending: DEC delivers, EXT survives), 9/9 score=100. New knobs:
`SS_NW_PIC`, `SS_NW_PIC_FORCE` ([DIAG-FORCED], never acceptance), `SS_TEST_EXT_PENDING`,
`SS_EXC_ENTRY` third field. Gates: machine 13/13 (test_exc_core 43, test_exc_chain 64,
scc 63, via 83), test-jit batch+plain 353/353, e2e-test 122, **paravirtual e2e PASS**,
gated-off A/B byte-identical (4 of ≤5 boots). Full record: EE-CHAIN-RECON.md "W2-3
results"; knob reference: SheepShaver/docs/DIAGNOSTICS.md "Wave-2 W2-3".

### [SheepShaver] Machine Layer M6 — FE1F service surface: the "placeholders" were trap trampolines; the THIRD exception class (0x700) delivered; the first DR native callout round trip — newworld DEFAULT (`46649a23`…`03222907`)

**Headline — unknown ≠ dead:** the 16 entry-vector "placeholder" slots that rung-2 Task U
overwrote with loud parked stops (on the theory they were dead) turned out to BE the
design — each raw `twi 31,r31,N` word is a **trap-to-NK-dispatch trampoline** encoding its
slot id. Executing one raises a **program interrupt (vector 0x700, the third real
exception class after interrupt and syscall)**, which the NK's own published handler
(`[KDP+0x37c]`=0x50314700 [PROBE✓]) decodes into the exit-pointer dispatch — the same
selector-service gateway the sc surface traverses. With the placeholders restored and
0x700 delivered, **the DR emulator's FE1F native callout completes its first round trip
ever**: 68k `dc.w $FE1F` selector $31 → marshal → `blrl` into slot 8 → trap → 0x700 →
NK gateway 0x5031aca0 → the 'EVNT' kernel-object service 0x5031d204 → return r3=0 (noErr)
/ r4=0x00120001 (kernel-object ID handle, `(dir-index<<16)|generation`) → the 68k tail
stores the handle into the ExpandMem slot `[0x100037dc]` and the CFM-prep routine
proceeds to the $36 sibling callout. The boot transforms: **839k → 4.48M ring records**
past the old 0x5000f248 park. **`SS_NW_FE1F_SURFACE` is the newworld profile DEFAULT**
(`=0` opt-out, SS_NW_SC_SURFACE polarity). Plan:
`docs/superpowers/plans/2026-06-11-fe1f-service-surface.md` (revs 2/3 — two red-team
rounds, then the Task-0 re-scope ratified); full evidence:
`docs/planning/machine/M6A-ONGOING-ENTRY-DESIGN.md` "FE1F native callout" + Task A/B/C
results. **Zero falsifications milestone-wide** (Task B's two corrections judged honest
predicate REFINEMENTS — the mechanism held; the slot recipe +4 and handle-not-pointer
mis-pins were static misreads corrected in the same evidence boot).

The arc (all gates green throughout — batch+plain test-jit 353/353 score=100, machine
suite 12/12 incl. test_exc_core 25→37 checks, e2e-test 122, gated-off A/B byte-identical):

- **Plan + red teams + rev-3 re-scope** (`46649a23`, `103c7def`, `eba8f0a3`): rev 2
  settled the two load-bearing contracts statically (the seed theory wrong; the success
  predicate un-inverted); rev 3 ratified Task 0's bigger falsification — restore the
  placeholders + deliver 0x700, superseding the population framing.
- **Task 0 recon** (`74fdc067`): all blocking answers pinned in 3/8 boots — the
  trap-placeholder mechanism, the 0x700 handler + exit-pointer publication (closing
  rung-2's `[ECB+0x9e0]`-writer mystery: the NK cold-init publication cluster at file
  0x3117xx), the selector-$31 'EVNT' service fully staged (seed-class fix list EMPTY —
  the fix is route-class), the H1 park named (blrl → slot-8 stop, never returns).
- **Task A** (`89a28c15` EXC_PROGRAM in exc_core, `669ccf7a` restore + delivery,
  `3b27fb9c` results, APPROVED review): raw `twi` restore over slots {4,6–15}
  (verify-EXPECTED), trap-taken `twi`/`tw` → `ExcEnter(EXC_PROGRAM)` →
  `program_entry=0x50314700` + the 2-SPR shim (the sc-shim sibling); SRR1 trap bit
  0x00020000 test-pinned; `delivered_program` as the 6th `exc=` heartbeat field.
  **Rung-2's "dead slots get loud stops" policy RETIRED** (dated notes in the design
  doc + rom_patches site); slot-15 exhaustion diagnostics moved to delivery telemetry.
- **Task B** (`9443a568`, evidence-only): round trip PASS end-to-end with **two honest
  predicate refinements** (slot = base + 4·index, so the watch word is 0x100037dc;
  r4 = the ID handle, not the EVNT pointer — the sc ID-directory precedent); all
  rung-2 + sc-surface invariants carried; the DSAT wall named.
- **Task C acceptance + flip** (`2949ec32` per-selector sc counter, `be0e02cb` flip,
  `03222907` records): battery green pre/post-flip, revert-on-red not invoked.
  **Fix-budget finding: the baseline "5 sc deliveries" was always the cap-5 PRINT
  artifact** — true totals are 8/7-distinct parked, 13/9-distinct FE1F-armed
  (0x3f, 0x19×2, 0x14, 0x0f×3, 0x27, 0x40, 0x42×2, 0x50, 0x4d).

**THE new frontier (stop-rule trigger 2 — captured + named, recon in flight): the DSAT
stack-underflow wall.** Right after the $36 callout's return path, a 68k System Error
ID 10 is raised (saved PC `[$C70]`:=0x5000e448, record-for-record reproducible) and the
Deep-Shit-Alert machinery itself underflows RAMBase from a 0x100000a0 fallback stack →
host SIGSEGV `ea=0x…0fffff42`. Capture:
`docs/planning/machine/M6A-WAVE2-SHIM-RECON.md` "Frontier update (FE1F Task C closeout)".

Knob reference (`SS_NW_FE1F_SURFACE`, the dependency matrix, the `[EXC] PROGRAM` /
per-selector lines): `SheepShaver/docs/DIAGNOSTICS.md` "Machine Layer M6 — FE1F service
surface". Registered ticket (rev 2 T-M2, out of this milestone): glue's ECB halfword-table
loop (`sheepshaver_glue.cpp:2163-2166` era) is dead-on-arrival AND latently buggy
(`hw | page_base` OR-corrupts page bits; primary-world base) — a real builder overwrites
it; fix deferred.

### [SheepShaver] Machine Layer M6 — NK syscall surface: the FIRST GUEST SYSCALL EVER RESOLVED; vector 0xC00 runs real NK code, newworld DEFAULT (`dad9a557`…`52928958`)

**Headline:** MPLibrary's first kernel service call — `sc` at pc=0x500d638c, selector
r0=0x3f, the wall every boot died on since the MixedMode round trip closed — now
**delivers into the staged NK's own syscall handler and returns r3=0 to the caller**.
Vector 0xC00 is resolved to `syscall_entry=0x50314ac0` (the PRIMARY copy, NK-published at
`[KDP+0x390]` — a deliberate cross-copy asymmetry vs `interrupt_entry=0x50412b1c`
staged-copy, recorded per plan P-m5), delivered through bare `ExcEnter(EXC_SC)` plus a
**2-SPR shim** transcribed from the real 0xC00 vector stub's postconditions: exactly
`SPRG1 := caller r1`, `SPRG2 := caller LR` — nothing else. Five syscalls delivered per
boot (selectors 0x3f/0x19/0x14/0x19/0xf), every sampled resume r3=0 with full register
continuity; **MPLibrary's MixedMode excursion RETURNS to the 68k world**. The surface is
the **newworld profile DEFAULT** (`SS_NW_SC_SURFACE=0` opt-out). Plan:
`docs/superpowers/plans/2026-06-11-nk-syscall-surface.md` (rev 2, two red-team rounds);
full evidence: `docs/planning/machine/M3A-ENTRY-TABLE.md` "Syscall entry resolution" +
Task B/C results — **the M3a `syscall_entry=0` descope is formally CLOSED**.

The arc (all gates green throughout — batch+plain test-jit 353/353 score=100, machine
suite 12/12 incl. the new test_dev_openpic 206 checks, e2e-test 122, paravirtual
`make e2e` PASS byte-identical; **zero falsifications, zero fix-budget consumed**):

- **Plan + two red teams** (`dad9a557`, `3e9682c9`): rev 2 folded both rounds — the
  probe-PC fix (probe the block at 0x500d6388, not the sc), the blocking-answer map,
  static-RE/boot budgets, the env matrix as Task-A *behavior*, and the free static wins
  (selector 0x3f pre-pinned, result register r3 / error r3≠0, the 12-byte-stride
  syscall-stub table).
- **Task 0 recon** (`780bbc34` telemetry + `3e7b04ca` addendum): the FATAL capture
  extended with r0/r3..r10 (the dying sc samples its own conformance vector); all
  blocking answers pinned in 2/8 boots — the NK's per-vector handler table at
  `KDP+0x360` (indexed `vector>>6`, via SPRG3), `[KDP+0x390]=0x50314ac0` [PROBE✓],
  the handler-entry ABI table, the shim verdict (two SPRs, host-side per the DEC-shim
  precedent), the save-target finding (**the syscall path saves into the `[KDP-0x14]`
  ctx — NOT `[KDP+0x65c]`** — so the DEC-shim's KDP logic does NOT transfer), exit via
  the scheduler restore (SRR0=sc+4 never re-incremented), and the Q-S5 staged-surface
  audit: the no-shim probe boot ran selector 0x3f END-TO-END on staged state (r3=0) —
  stop-rule NOT fired.
- **Task A** (`bcce26c2`): `NW_SYSCALL_ENTRY_DEFAULT` + the `SheepExcSyscallShim` glue
  helper (env-gated `SS_NW_SC_SURFACE=1` bring-up), the P-M1 env-matrix as behavior —
  **fixed the no-comma `SS_EXC_ENTRY=0xINT` trap** (it used to zero the syscall entry;
  now PRESERVES the default), override×gate 2×2 pinned, loud inert-`legacy` warning —
  plus the delivered-sc counter as the **5th `exc=` heartbeat field**. Handler-entry
  probe conformed exactly to the Q-S2 register table; gated-off boot byte-identical
  to the FATAL baseline.
- **Task B** (`c41b9ea3`, evidence-only): first-sc round trip PASS — no FATAL,
  handler conformance re-asserted, resume at 0x500d6390 with r3=0 and r1/LR/r4..r10
  preserved; rung-2 invariants carried (cold-once, guest[0]/[4] stable, slot-15
  unvisited, delivered-DEC=0); bounded selector map (the full `li r0,SEL / sc` stub
  table file 0xd6298..0xd6d3c statically enumerates selectors 0x00..0x84 —
  per-selector resume probes pre-answered).
- **Task C acceptance + default flip** (`7a079079` + records `52928958`): full battery
  green env-on, then **the flip** (explicit-"0"-only opt-out, SS_NW_MM_SWITCH
  polarity), then the battery re-run with NO env vars — all green, no revert.
  Opt-out boot reproduces the sc-wall FATAL baseline byte-identically (diff-verified).

**THE new frontier (stop-rule trigger 2 — captured + named, not chased):** after sc #5
the boot parks 68k-side at **0x5000f248** — the instruction after an **F-line NK/DR
service trap `$FE1F` with selector d0=0x31** (a CFM/accelerator slot-fill request into
an ExpandMem-anchored array), issued by the ROM's CFM-prep routine via the A-line trap
dispatcher at 0x5000dfa2. The FE1F service surface is the next milestone's named wall —
not another sc selector, not an MMIO poll. Capture:
`docs/planning/machine/M6A-WAVE2-SHIM-RECON.md` "Frontier update (Task C closeout)".

Knob reference (`SS_NW_SC_SURFACE`, the env matrix, the legacy reproduction recipe):
`SheepShaver/docs/DIAGNOSTICS.md` "Machine Layer M6 — NK syscall surface".

### [SheepShaver] Parallel landings: M3b Wave-2 OpenPIC model + tests (`b86449c9`); EE-chain recon memo (`89fd0642`)

- **OpenPIC (KeyLargo MPIC) model + 206-check unit suite** (`b86449c9`, + target
  pre-wire `884586a5`): pure module + tests only per the Wave-2 stop-rule disposition —
  no live consumer; EXC_EXTERNAL wiring stays gated on the W2.0 NK-handler recon
  (`OpenPICBindOutput` is the future seam). Behavioral reimplementation against the
  QEMU oracle (`hw/intc/openpic.c` @ `de5d8bfd`, OPENPIC_MODEL_KEYLARGO), with the
  oracle's corrections baked in: **the KeyLargo register file is mapped
  little-endian** (documented in the header for the wiring task) and **CTPR resets
  to 15** (all sources masked until the guest lowers it). Boot-critical surface
  (BRR1/FRR/GCR/VIR/PIR/SPVE, per-source IVPR+IDR, CPU0 CTPR/WHOAMI/IACK/EOI) with
  QEMU's quirks matched (edge-clear on bad IACK, EOI re-raise without CTPR recheck);
  absent-device-consistent timers/IPI holes. Machine suite is now **12 suites**;
  mutation probes 5/5 killed.
- **EE-chain recon memo** (`89fd0642`, `docs/planning/machine/EE-CHAIN-RECON.md`):
  the honest 11-link chain map (real through delivery, fictional after it), the
  first-EE-rise verdict (**EE rises by `rfi`, not guest `mtmsr` — the NK forces
  EE=1 via `ori r11,r11,0x8000` at 0x50313bf8; the pending DEC latch makes the first
  rise an instant delivery**), a 3-level test spec, and the Wave-2 reorder
  recommendation: verification rungs W2-0..W2-2 BEFORE construction W2-3..W2-4.

**Headline:** the 68k→PPC Mixed Mode switch works in BOTH directions on the newworld
profile and is now the profile **default** (`SS_NW_MM_SWITCH=0` opt-out). MPLibrary's
PPC TVector (`0x500cef8c`) executes for the first time; the boot is transformed —
jNK collapses **116M (FE01↔NK bounce spin) → 4104**, comp unfreezes 3573 → 3672, the
boot advances through TWO full MixedMode round trips (literal 68k resume PCs observed
in the r24 ring, e.g. completion-written `[saveblk+0x3c]=0x50033776`) and the CFM
parcel-by-name caller region runs. Plan:
`docs/superpowers/plans/2026-06-11-m6a-rung2-mixedmode-switch.md`; full results in
`docs/planning/machine/M6A-ONGOING-ENTRY-DESIGN.md` (Task T…Y results sections).

The arc (all gates green throughout — batch+legacy test-jit 353/353 score=100, machine
suite 11/11, e2e-test 122, paravirtual `make e2e` PASS byte-identical):

- **Task 0 contract recon** (`c8178095`): FE01/FE02 contracts pinned from static RE +
  bounded probe boots — the retry-spin root cause named (`[KDP+0x660]` classification
  gate: every slot-1 call classified "not from emulator"), the TVector entry-register
  table, the 16-slot entry-vector resolution, FE02's direction re-scope (it's
  PPC-calls-68k, not the switch-back).
- **Task T pool default + collision fix** (`735f775c`): `SS_NW_MM_POOL` → newworld
  default-ON, and the staged pool base `0x68ff5000` — claimed "free gap, no other
  users" — was **falsified by our own Hnfo-scratch seed** (machine-detect writes landed
  inside MM save record 0). Pool relocated to `0x68ff5800`; the sub-KDP **occupancy
  map** is now the authoritative tracked table; slot-15 allocator-exhaustion loud stop.
- **Task U slot stops** (`b271f161`): dead NK entry-vector slots get unique parked-PC
  loud stops (zeros never silently consumed again).
- **Task V the switch seeds** (`1c10d968`): the entire forward switch reduced to TWO
  trampoline-resident KDP seeds (`[KDP+0x660]|=0x00200000` + MRU pair-0
  `[KDP+0x340/0x344]=MMCB`) — the DR's FE01 service + the staged NK's own
  switch/save/scheduler + the ROM's native glue do everything else. **The TVector
  executes** (probe: exact Q-D register conformance); the FE01 retry spin is GONE.
- **Task W stop-rule + discriminator** (`3dd1550b`): verify-first caught
  the illusion — pre-W, every "TVector excursion running" was actually a **68k reset
  per excursion** through the always-cold table[0] (a faster reboot loop, not
  progress). Landed the table[0] cold/warm discriminator + `[KDP+0x658]` cold seed;
  the second falsification (the `[KDP+0x65c]` self-switch) correctly fired the
  stop-rule → rev 3 re-scope.
- **Rev 3/3.1 red-team** (`cc8302f8`, `b468b07d`): the W2 design GO-WITH-CHANGES —
  including **striking the `[KDP+0x5f0/4]` retarget that every prior doc recommended**
  (live values are NK-rebuilt staged addresses; retargeting would have destroyed both
  switch directions) and pulling the DEC fence forward.
- **Task W2 the world-flip** (`43d42b83`): `[KDP+0x65c]` current-world flip discipline
  (warm arm := MMCB; retargeted slot-1 region := ECB) — **the round trip CLOSES**: the
  parked FE01 service resumes, FE07 reads command byte 0xff live, the 68k resumes at
  the completion-written PC. Plus the **DEC fence**: `deliver_pending_dec_exception`
  defers while `[XLM_RUN_MODE]` (0x2810) != 0 — new `deferred_native` counter, the
  `exc=` heartbeat tuple is now 4-wide (`delivered/deferred_ee/deferred_depth/`
  `deferred_native`).
- **Task X verify-and-leave + re-census** (`aaaa9a02`): `[KDP+0x5f0/4]` probed at cold
  AND warm entry = the NK staged pair (glue's primary-world seeds are dead-on-arrival,
  comment-only); post-W2 census ratifies the R3 stub route — the only warm entrant is
  the legitimate completion signature (r3=0xff).
- **Task Y acceptance + default flip** (`296c3661`): all five PASS/FAIL gates green
  pre- AND post-flip; opt-out boot reproduces the pre-switch baseline byte-identically;
  fix budget consumed: zero.

**THE new frontier (stop-rule trigger 2 — captured, not staged):** MPLibrary's first NK
syscall — `sc` at `pc=0x500d638c` with unresolved syscall_entry (`SRR0=0x500d6390
SRR1=0x00007072 lr=0x500cf108 r1=0x103ffb50`), identical pre- and post-flip. Vector
0xC00 / the NK syscall surface is the next milestone. Baseline at the wall: comp=3672,
jNK=4104, exc=0/1/0/0, CUDA quiet (13 packets / 9 i2c — the V-era "MMIO storm" reading
was the reset cycle, now dead).

Knob reference (incl. the `SS_M6A_USER_MSR` quarantine and the `SS_DR_R24_RING` /
`SS_INTERP_RING` capture rings): `SheepShaver/docs/DIAGNOSTICS.md` "Machine Layer M6a".

### [SheepShaver] Machine Layer M3b Wave 1: Cuda protocol model + minimal ADB stub live on newworld (`143f66e7`…`21b6410a`)

M3b Wave 1 (the live consumer half of MACHINE-LAYER-PLAN M3b) landed on the newworld profile:

- **`dev_cuda` pure module** (`143f66e7`, review follow-ups `b7a3445b`): Cuda (Apple MCU)
  SR-handshake state machine + command dispatcher — ADB dispatch, GET/SET_TIME RTC,
  READ/WRITE_PRAM + MCU_MEM (in-memory 256-byte PRAM), autopoll control, FILE_SERVER_FLAG/
  POWER_MESSAGES acks, RESET/POWERDOWN loud-latched, I2C (below). Behavioral extraction
  (NOT a code port) from two oracles, both SHAs cited in `dev_cuda.h`: QEMU
  `hw/misc/macio/cuda.c` @ `de5d8bfd6105d3dd3ae668df9762df244a6d1506` and DingusPPC
  `devices/common/viacuda.cpp` @ `92bb6d10549529f9f4031a85c2bc136149535bdc`.
  `test_dev_cuda` 2483 → 3924 checks (conformance vectors CV-0…CV-10).
- **`adb_stub` minimal ADB bus** (`e7120368`, `bb09269f`, 90 checks): keyboard@2 + mouse@3,
  Talk R3 identification, Listen-R3 address-move (the boot scan MOVES devices), Talk R0 =
  empty — **full host-input-over-ADB deferred** per the donor study §7.2 decision; the full
  implementation replaces this module behind the same interface (tracked in ROADMAP D3).
- **Task 3 integration** (`b7a3445b`, `912f27cb`, `94c4a0f0`, `8c7f6796`): `VIABindCuda`
  SR/ORB seam on the M1 VIA (region-lock covered, loud stub replaced), main_unix bring-up,
  `cuda_init_dat` + `adb_init_dat` ROM-patch retirements (profile-gated; **no-op on the
  9.0.1 parcels ROM by construction** — both patterns miss their search windows there, the
  inits always ran unpatched; live behavior change on 1.1/OldWorld-window ROMs only).
- **CV-10 deferred SR-int delivery** (`d3e60d88`) — THE acceptance-unlocking fix. QEMU
  delays every Cuda-raised SR interrupt 20 µs so it lands *after* the host's sync-byte SR
  read; our synchronous raise was consumed by that read before the ROM's 15000-budget IFR
  wait even began, so the wait expired and the boot parked. Fix: pending raise stays
  latched and is delivered only via `CudaSettle` on the VIA's R_IFR read path (full
  root-cause story in the dedicated entry below).
- **I2C 0x22/0x25** (`4e544aff`): READ_WRITE_I2C + COMB_FMT_I2C per DingusPPC, absent-device
  error replies (`CUDA_ERR_I2C`) — no I2C devices modeled (stop-rule; the boot sweeps 9
  addresses and moves on). Retired `unknown` 9108 → 0.
- **Diagnostics** (`132b020d` + Task 3/4): `[CUDA]` stats line, `[VIA] orb:` write-value
  trace (the C3 polarity forensics that pinned the live Cuda-polarity engine, DDRB=0x30),
  `SS_TERM_DUMP=1` (SIGTERM → exit(1) so timeout-killed boots reach atexit dumps), `[M3b]`
  retirement banners with pattern found/absent reporting (`f4c53f80`). Reference:
  `SheepShaver/docs/DIAGNOSTICS.md` "Machine Layer M3b".

**Acceptance** (records `6ffb467e`, `d80155ae`, `21b6410a` in
`docs/planning/machine/M6A-WAVE2-SHIM-RECON.md` "M3b Wave 1 acceptance"): the M6a-named
18338-read ORB sync frontier is CROSSED (syncs=1, then 734+/60 s as the boot cycles).
Latest boot: `packets=9529 responses=9529 pram_rd=2199 pram_wr=733 i2c=6597` (all absent)
`unknown=0`; comp 849→4786+ climbing, no park. ADB/GET_TIME/autopoll not yet issued — the
boot cycles its Cuda probe sequence (~80 ms/cycle); the loop-ender is under recon
(Ticks-starvation hypothesis live; per the plan's stop-rule it gates Wave 2's shape).

**Dual-PRAM inconsistency window (deliberate):** Cuda READ/WRITE_PRAM serves in-memory
zeros while the `nvram*` EMUL_OP HLE stays applied until M4 — two divergent PRAM sources
until the M4 NVRAM model unifies them.

**Build gotcha (do NOT pattern-match mid-struct inserts as safe):** the I2C work grew
`CudaDevice` mid-struct and the Unix build's `main_unix.o`/`dev_via6522.o`/
`sheepshaver_glue.o` did not rebuild on the header change (no header dependency
tracking) — first boot showed garbage counters (`powerdowns=6114308096`). Treat any absurd
`[CUDA]` counter as a stale-object symptom first; force-remove the consuming `.o` files
after any machine-header struct change.

**Gates throughout:** machine suite 11/11 (now incl. `test_dev_cuda` 3924,
`test_adb_stub` 90, `test_dev_via6522` 70); batch + legacy `make test-jit` 353/353
score=100; e2e-test 122; paravirtual `make e2e` PASS; paravirtual byte-identical/inert
(no `[CUDA]` lines).

### [SheepShaver] Machine Layer M3b: CV-10 deferred SR-int delivery — sync park root-caused + fixed (`d3e60d88`)

M3b Wave-1 acceptance root cause: the 9.0.1 ROM's 68k Cuda startup sync (file 0x9584, guest
0x50009584 — counter-reconciled exactly: ORB=3340, IFR=15002, SR=2, syncs=1; parked 68k PC
live-verified via `SS_PROBE_PC` r24) reads the SR sync byte AFTER the TACK-negate edge and then
waits for one more SR int with a 15000-poll IFR budget. Eager seam delivery let that SR read
consume the int before the wait began → budget expired → park at 0x9754 `bra.b *`. Oracle: QEMU
cuda.c @ `de5d8bfd` delays every Cuda SR int 20 µs (`cuda_delay_set_sr_int`) past the host's SR
read. Fix (deterministic lazy equivalent): raises latch in `sr_int_pending` and deliver ONLY via
`CudaSettle` on the VIA R_IFR read surface; ORB reads no longer settle; SR access clears latched
IFR.2 only. New CV-10 conformance vector (TDD red→green). Gates: machine suite 11/11, test-jit
353/353 (batch + plain). Acceptance boot: park GONE — 13156 Cuda packets, 48k/55k bytes in/out,
1013 syncs, PRAM traffic, SCC active, comp 849→4912+ climbing. New frontier: Cuda I2C pseudo
command 0x22 (DingusPPC `READ_WRITE_I2C`) unknown ×9108. Full chain:
`docs/planning/machine/M6A-WAVE2-SHIM-RECON.md` "M3b Wave 1 acceptance".

## 2026-06-10

### [SheepShaver] Machine Layer M3a: exception core + DEC delivery (`3a3d5380`–`a2dd1ff8`)

M3a implements the first half of MACHINE-LAYER-PLAN §2d — a real PPC OEA exception model on the
newworld profile — and delivers the first real PPC exception ever fired by this emulator.

**`exc_core` pure module** (`d3abf130`, `d6ae4b74`) — `ExcEnter` (SRR0 capture + sc +4 ownership;
SRR1 = msr & 0x0000FFFF; MSR cleared of POW/EE/PR/FP/FE0/SE/BE/FE1/IR/DR/RI via
`EXC_MSR_CLEAR_MASK=0x0004EF32`; entry from table or `EXC_PC_UNRESOLVED`), `ExcRfi` (MSR =
(SRR1 & 0xFF73) | (MSR & ~0xFF73); PC = SRR0 & ~3), `ExcDeliverable` (MSR[EE]). Anti-vacuity test
suite (25 checks): all-ones/complement inputs; ExcEnter(0xf072)→MSR 0x1040, SRR1 0xf072 (honest
composition equals the legacy fiction byte-for-byte); round-trips incl. the documented POW-loss case.
`EXC_SYSCALL` renamed `EXC_SC` (macOS SDK `<mach/exception_types.h>` collision).

**Glue fences** (`7d30c73c`) — `[NW-INT]` tick-50 injection deleted (was already newworld-gated;
real delivery replaces it). `HandleInterrupt` MODE_68K arm: fake-delivery machinery
(`WriteMacInt16(KDP+0x67c,1)` + CR-mask injection through `interrupt_copy`) fenced off on newworld
(would corrupt live guest state). MODE_NATIVE arm fenced on newworld entirely (stale static entry).
Paravirtual body byte-identical. Gates: test-jit 353/353.

**sc + rfi real semantics** (`d2b43d17`) — `sc` reclassified `CFLOW_TRAP` (was CFLOW_NORMAL; an
absolute-PC sc inside a decoded interpreter block would execute stale continuations). `execute_syscall`
on newworld: `ExcEnter` absolute-PC, no `increment_pc` (kills the double-increment class structurally);
unresolved → `SS_EXC_SC=abort|legacy`. `execute_rfi` on newworld: `ExcRfi` full MSR restore.
EE 0→1 edge re-raise at `mtmsr` and `rfi` (load-bearing: the 60 Hz net is dead on this boot —
`XLM_IRQ_NEST=0xFFFFFFFF`, Task 0 finding 3). `Makefile.in SRCS += exc_core.cpp`. Gates: 353/353,
machine 9/9.

**DEC delivery hook** (`3b4c05ff`, `6a6b37c6`) — `check_spcflags` HANDLE arm gains a newworld
branch (before `processing_interrupt`/`interrupt_copy`/`HandleInterrupt`): if
`VirtClockDECPending()` + `ExcDeliverable(msr)` + `execute_depth==1`, performs the KDP
register-save shim (offset-by-offset from `interrupt()`: KDP+0x004/+0x018, the [KDP+0x65c] context
block +0x13c..+0x16c, r1/r7/r13 splice) with honest upgrades — r10/r12=real restart PC (block-start,
the poll contract), r11=real composed SRR1 (byte-equal to 0xf072 for the boot-real case) — then
`ExcEnter`. Deferred: latch held, re-raised at EE-edges + the four nested-execute returns. Entry
table `g_exc_entry_table`: interrupt_entry=0x50412b1c (probe-verified vs static 0x312b1c,
M3A-ENTRY-TABLE.md); `SS_EXC_ENTRY` override for no-rebuild iteration; `SS_EXC_BARE=1` skips the
shim (direct-entry experiment). DEC unresolved guard (I1: guards wild-jump after latch clear + KDP
mutation). Telemetry: `exc=delivered/deferred_ee/deferred_depth` in the `[HB]` heartbeat (rides the
heartbeat because SIGALRM skips atexit) + crash-path dump. Gates: 353/353, machine 9/9,
test-opcodes inert (zero `[EXC]` lines on paravirtual).

**Cold-MSR EE=0 fix** (`ab8e5ac6`) — Boot-A root cause: `0xf072` has EE=1 from instruction zero,
so the first DEC expiry delivered into NK cold-init (all registers zero, LR=0) → handler's r7-flag
`blr` exit → jump to 0 → ignoreillegal zero-page march → SIGSEGV at 0x100000 mapping edge.
Architecturally, reset MSR has EE=0. Fix: newworld trampoline seeds MSR=0x7072 (fiction minus EE).
Verified: `exc=0/1/0` in the heartbeat — cold-init expiry defers, boot reaches the console spin
intact. Paravirtual keeps 0xf072 (untouched). Boot frontier identified as the NK Thud debug console
(designed wake = serial character, not timer; EE stays honestly masked there).

**SCC Rx queue + SS_SCC_RX_INJECT** (`a2dd1ff8`) — per-channel 16-byte Rx FIFO in the SCC 8530
model (RR0 bit0 = queue non-empty; data read pops; M1 conformance defaults intact when empty;
M3b's real serial input will reuse this queue). `SCCInjectRx` (caller-holds-region-lock contract).
Debug knob `SS_SCC_RX_INJECT=DELAY_S:HEXBYTES` (M2 scheduler one-shot → `MMIOBusWithRegion` →
inject). SCC unit test 19 → 37 checks.

**End-to-end machine-layer demonstration (airtight A/B, one boot):** `SS_SCC_RX_INJECT=25:0D` fed
one CR to the NK Thud console at T+25s; JIT compile counter frozen at 781 (two pre-injection
heartbeats) → 791 (two post-injection heartbeats) — 10 new code blocks compiled and executed in
direct response. M2 scheduler → M1 bus/backpatch → SCC Rx → `check_work` → console. Every
machine-layer milestone composing in one observable event. M1's carried-forward consumer-(b)
Rx-path coverage is closed.

**Gates throughout:** batch + legacy test-jit 353/353 score=100; machine suite 9/9 (scc 37 checks);
e2e-test PASS; paravirtual `make e2e` lifecycle PASS; paravirtual byte-identical (all changes
newworld-gated).

### [SheepShaver] Wave 0: the 0x50326050 MMU/SR wall is CROSSED (`b27aa6de`, `54a5d04a`, `29859b52`)

- **SR0–15 + MSR stored state** in the CPU core (the SPRG store-the-write/return-the-read
  pattern; fields appended last; MSR cold value 0xf072 = the old `mfmsr` hardcode, byte-
  identical; MSR stays stored-only — DR/EE semantics land in M3a). Decode entries for
  mtmsr/mtsr/mtsrin/mfsr/mfsrin (previously `execute_illegal` no-ops via the default-true
  `ignoreillegal` pref); native JIT mfsr/mfsrin/mfmsr constants neutralized to interpreter
  fallback (red-team C1 — left native they'd defeat the stored state). Also fixed: sdr1/bat/
  srr0/srr1 were malloc-garbage at cold start. Harness +3 round-trip vectors (353 total);
  batch per-vector reset extended to the trailing supervisor block.
- **Newworld low memory extended to 0x0–0x100000** (single profile-sized acquire — red-team
  C2: a second Mach acquire at 0x3000 page-truncates into an overlap failure) backing the
  NK's low-physical descriptor probes (~0x200a0).
- **Live result (`M5-MMU-SR-WALL-ANALYSIS.md` §8):** zero SIGSEGV (was: instant fault at
  0x50326068); identity confirmed (EA = flat 0x200a0, probe r22=0; saved SR == programmed
  SR); no handler re-entry; rung 3/4 stays deferred. **New frontier = M3's acceptance
  target (closed by M3a ✅ — see the M3a entry above):** the runtime-staged (parcels-relocated,
  0x504xxxxx) NK idle loop polling our real SCC 8530 model at 0xF3012000 through the backpatched
  MMIO bus — M1's carried-forward consumer-(b) acceptance is live; the loop awaited real
  interrupt delivery, delivered in M3a (first real PPC exception; end-to-end demo via
  SS_SCC_RX_INJECT).
- Gates: batch + legacy test-jit 353/353; machine suite 8/8; test-opcodes 353/353;
  e2e-test 122/122; paravirtual e2e lifecycle PASS; paravirtual byte-identical.

### [build][SheepShaver] Developer-speed tooling: ccache recipe, batch harness, SS_SEED_MEM (`c9b5f926`, `311b0920`, `1fcdc297`)

Three developer-productivity tools landed in one session:

- **ccache recipe** (`c9b5f926`): SheepShaver's autoconf build wires ccache by re-running
  configure with `CC="ccache gcc" CXX="ccache g++"` (plus the standard flags). Per-checkout
  config state (not committed; a fresh clone/worktree must re-run configure with those
  CC/CXX values). Measured: clean-tree rebuild ~5.88s cold → ~0.48s warm (~12×). See
  CLAUDE.md and CONTRIBUTING.md for the one-line recipe to add to configure.

- **Batch harness** (`311b0920`, `SS_HARNESS_BATCH=1 make test-jit`): all 350 vectors in ONE
  emulator process per mode instead of one process per vector (~700 launches → 2). Timing:
  **~3s vs ~32s** (legacy). Per-vector state is fully reset between vectors (GPR/CR/XER/LR/CTR
  + FPR/VR/FPSCR/vrsave + interpreter cache + JIT cache). Equivalence proven by byte-identical
  per-vector REGDUMP content in both interp and JIT modes across all 350 vectors, plus a
  deliberate corruption test confirming the diff logic is live. Usage: batch for the inner
  loop, plain `make test-jit` as the authoritative gate (process-per-vector isolation is the
  stronger contract). `SS_HARNESS_KEEP=1` preserves REGDUMPs for auditing.

- **SS_SEED_MEM** (`1fcdc297`): no-recompile guest-memory poke knob with two forms —
  immediate (`0xADDR=0xVAL`) applied at NW-trampoline-end, and PC-triggered
  (`0xPC:0xADDR=0xVAL`) applied at the first JIT block-entry visit of 0xPC. Up to 16
  semicolon-separated entries; 32-bit writes; MMIO ranges refused; `[SEED]` stderr lines.
  Born from the NK spike where a KDP field needed re-seeding after the nanokernel's own
  cold-init zeroing clobbered it. Full reference: `SheepShaver/docs/DIAGNOSTICS.md`.

### [SheepShaver] NK-boot ceiling root-caused + fixed (d8932203); new frontier is the 0x50326050 MMU/SR wall

Post-M1 diagnostic spike (2026-06-10):

- **Root cause of M1 carry-forward ceiling (pc=0x503123fc, ea=0xffffffff):** the nanokernel
  page-descriptor build loop (ROM 0x3123a8–0x312424) reads its trip-count cap from `KDP+0x6b4`;
  the field was un-seeded (=0), clamping the loop to ~16k iterations and driving the stride-8
  pointer walk at `KDP+0x80` ~64 entries past the valid region into `0xFFFFFFFF` poison at
  `KDP+0x340` → faulting `stw r30,0(r8)` at `0x5031240c`. The pre-M0 paravirtual path had
  silently skipped these faults via `ignoresegv` for the entire page-init stage; the fidelity
  profile's abort-loudly design exposed the fault immediately (working as intended). DEC and
  `[KDP-0x900]` were both checked and ruled out as clobbering sources — the trampoline-time
  seeding is byte-identical to pre-M0.
- **Fix** (`d8932203`): ROM instruction patch in `rom_patches.cpp` (gated `g_rom_904_lenient` +
  newworld profile): replaces `lwz r8,0x6b4(r1)` at ROM 0x3123ac with
  `lis r8,<ceil(phys_pages/0x10000)>` (cap=65536 pages for 256 MB). Trampoline-time seeding is
  ineffective because the NK cold-init zeroing clobbers it; the patch site fires after the zeroing
  completes. (Commit subject says "in NW trampoline" — the fix is in `rom_patches.cpp`.)
- **New frontier:** boot advances one full stage further before SIGSEGV at `0x50326050–0x50326068`
  — `lwbrx` of hardcoded physical `0x200a0` (below RAMBase) inside the NK's MMU/segment-fault
  handler (`mtdbatl/mtdbatu`, `mtsrin`, `mtmsr` translation toggling, byte-reversed PTE accesses).
  This is the genuine SR/BAT/supervisor-environment wall — M3/M5 territory. Consumer-(b) live
  acceptance remains blocked, now by a root-caused, documented hard wall instead of a mystery.
- Probe logs: `/tmp/nk-probe.out`, `/tmp/nk-run3.out` (ephemeral).

### [SheepShaver] Machine Layer M2: virtual clock (TB/DEC) + DingusPPC event scheduler + VIA timer state machine

- **`virt_clock` module** (`5ccfb070`, `07caef23`) — guest-visible TB and DEC backed by an
  injected host-monotonic-ns source at a fixed ratio (`tb_freq_hz = TimebaseSpeed`). DEC
  expiry raises an exception **CONDITION only** (latch + telemetry; delivery landed in M3a). `SS_SYNTH_DEC`
  absorbed as a deprecated force-override (`=0` escape hatch preserved); cold state bit-identical
  to M1's synthetic down-counter. Fused generation+armed single-CAS prevents stale scheduler
  events from stealing a fresh arm. Standalone unit test (`test_virt_clock`) validates the
  `check_work` DEC-deadline consumer contract (SPIKE-S3 §2.4).

- **`event_sched` module — DingusPPC TimerManager port** (`27747fb9`, `07caef23`) — ported from
  DingusPPC `core/timermanager.{cpp,h}` at commit
  `92bb6d10549529f9f4031a85c2bc136149535bdc` (https://github.com/dingusdev/dingusppc),
  GPL-3.0-or-later; combined work GPLv3 per `DINGUSPPC-EVALUATION-PLAN.md`. Adaptations
  (`[SS]`-marked in source): class renamed `TimerManager` → `EventScheduler`; singleton
  `get_instance()` removed (machine layer owns the instance); `loguru` → `fprintf(stderr,...)`
  on `cancel_all` warning path only. Queue, ordering, re-arm, and callback semantics preserved
  verbatim — including the donor's contract that `process_timers()` never holds the queue mutex
  while invoking a callback (the M2 lock-order rule: device/region lock → scheduler queue only,
  never the reverse). Standalone unit test (`test_event_sched`) exercises ordering, cyclic
  drift-correction, cancel, and the no-lock-during-callback contract.

- **VIA 6522 timer state machine + N6/N7/N8 fixes** (`96bbb53a`, `b61acf0e`) — IDLE/RUNNING/FIRED
  state machine replaces the lazy expiry-on-read model. Resolves three M1-deferred conformance
  notes from `M1-DEVICE-CONFORMANCE.md` §6:
  - **N7 ✅** expiry now at N+1 ticks (`dt > cnt`; hardware-correct per 6522 datasheet)
  - **N6 ✅** T2CL (and T1CL, added for symmetry) read now clears the respective IFR bit (6522 ack path)
  - **N8 ✅** IFR write-1-clear clears the flag only; FIRED one-shot state prevents re-assert (matches QEMU/DingusPPC)
  Two execution-time quality fixes also landed: count-read TOCTOU single-sample; IFR blind-clear
  settles the deadline before broadcasting. New `MMIOBusWithRegion` bus API (runs a callback
  under the owning region's lock — scheduler callbacks mutate device state safely; lock-order
  rule: device → queue only).

- **Integration** (`0293ba6a`, `dcf61ddd`) — clock init on both normal + harness paths (after
  `get_system_info()` so the `cpuclock` pref is honored); scheduler pump thread (10ms cap,
  kicked-predicate); DEC eager-expiry hook with stale-one-shot cancel; VIA re-clocked off
  `VirtClockNowNS`. Paravirtual profile inert (no thread, no output — byte-identical).

- **`mdec_dat` patch retirement on newworld** (`47a82c78`) — gate live for 1.1-ROM newworld
  (profile check at the site, M1 `scc_init` idiom). **Honest scope:** the pattern is absent
  in the 9.0.1 parcels ROM (byte-verified — `[ROMPATCH] SKIP mdec` in every prior 9.0.1 run);
  the 9.0.1 fidelity boot exercises the M2 clock seams directly, not the retirement gate.
  `9.0.1 diagnostic boot 2026-06-10: mtspr_dec=3 / mfspr_dec=0 / tb_writes=0 / dec_expiries=0; boot dies at the unchanged pre-existing 0x50326050–68 MMU/SR wall (ea=0x200a0), identical in baseline (SS_SYNTH_DEC=0) and acceptance runs — DEC is not load-bearing pre-wall on the current path; the S3 §2.4 check_work DEC consumer (0x50326520+) lies beyond the ceiling, so mfspr-DEC live coverage carries forward with it (unit-level: the check_work deadline-math simulation in test_virt_clock)`

- **Interpreter seams** (`6f4930f7`) — `mfspr`/`mtspr` for DEC (SPR 22), TBL/TBU writes
  (SPR 284/285), and `mftb` routed through `g_virt_clock` on the newworld profile; `SS_SYNTH_DEC`
  honored as a deprecated alias (deprecation warning printed). JIT unmodified (all these forms
  fall back to the interpreter via `return false` — verified). Paravirtual path pays nothing
  (profile check on the slow path only).

- **Known accepted M2 risks (documented in plan):** VIA `timer_arm` allocates on a
  Mach-fault-reachable path (§2g); M3a hardening = pre-allocated slots or skip-eager-arm-on-
  handler-thread. Crash-path `pthread_join` hazard in `sched_pump_stop` addressed in M3
  (bounded-join).

- **Gates:** machine suite 8/8 binaries (192+ checks) ALL PASS; `make test-jit` 350/350
  batch+legacy score=100 throughout; `make test-opcodes` inert (interpreter determinism
  unchanged); rom-harness builds clean; `make e2e-test` 122/122; paravirtual `make e2e` lifecycle PASS (boot to Finder, clean shutdown — final binary); positive seam check
  (`mfspr r3,DEC` force-on returned nonzero through the real interpreter seam).

### [SheepShaver] Machine Layer M1: MMIO bus + SCC 8530 + VIA timer/IFR surface + JIT backpatch

- **MMIO bus core** (`7fa756c0`, `e369405a`) — region registry with trapped-MMIO and
  mapped-aperture region kinds; locked dispatch; per-region fault-rate telemetry + idle-detection
  hook; `SS_MMIO_BUS=1` named third config (paravirtual + bus + devices − serial-skips);
  PROT_NONE MacIO reservation at `0xF3000000–0xF3080000`; `SS_JIT_VERIFY` hard-incompatible with
  the bus (device reads are side-effecting — never double-execute).
- **AArch64 MMIO access decoder** (`da1a59c4`) — standalone-tested pure module; decodes the
  exact JIT-emitted `LDR/STR Wt, [RMEMBASE, Xm, UXTW]` + `REV`/`REV16` forms; the JIT
  memory-emitters are the decoder's contract; unmapped undecodable faults abort loudly, no silent
  zero-reads or blind `pc += 4`.
- **Mach fault path** (`bc3029d3`) — S2 productionized: decode JIT accesses → bus dispatch →
  AArch64 thread-state writeback; adds `SIGSEGV_RETURN_STATE_MODIFIED` return code + lazy
  thread-state fetch to `sigsegv.cpp`; `[KDP-0x900]` set to `0xF3012000` (real SCC base) on
  the newworld profile (`sheepshaver_glue.cpp`) — `SS_NW_NO_SCC` env escape preserved.
- **SCC 8530 model** (`4f2ba41f`) — legacy `+2/+6` port layout, WR-pointer state machine,
  RR0/RR1 status bits; SPIKE-S3 conformance write-vectors pass. **`scc_init` ROM patch retired
  on newworld profile** (`0f2e83f2`): guest SCC init now reaches the model; profile check at
  the patch site guards the paravirtual path.
- **VIA 6522 timer/IFR surface** (`44529d82`) — lazy T1/T2, IFR/IER register decode; Cuda =
  loud stub (state latched, no stdio on fault path per §2g).
- **JIT backpatch** (`ab1d008c`) — generic thunk emitted at JIT-init; strong symbol overrides
  weak stubs so the emulator build never links the no-op fallback; verified AArch64 encodings
  (MOV/BL/NZCV save-restore); thunk re-emitted on JIT cache flush; hot MMIO fault sites become
  direct bus calls — THUNK-SELFTEST PASS.
- **Interpreter range check + host-accessor guards** (`2694c1e6`) — `vm.hpp` MMIO dispatch is
  branch-gated per profile (paravirtual interpreter path pays zero); `Mac2HostAddr` aborts via
  `mmio_mac2host_abort` on device-range addresses; `SS_PROBE_PC` / `SS_JIT_WATCH_ADDR` refuse
  device ranges loudly; interpreter bench delta indistinguishable from environmental noise (the
  paravirtual check is a single predicted-untaken branch; medians 22.44 s → 23.41 s on a loaded
  machine with min-run delta −1.9% — within measurement noise).
- **Minimal DEC tick** (`43f61082`) — synthetic decrementer default-on for newworld profile so
  `check_work`'s timeout loop has a ticking DEC; full clock/scheduler deferred to M2.
- **Conformance audit** (`0a3007a9`) — QEMU `escc.c` / `mos6522.c` and DingusPPC
  `escc.cpp` / `viacuda.cpp` checked against the M1 scope fence; zero model fixes warranted;
  9 note-level deltas (N1–N9) documented in `docs/planning/machine/M1-DEVICE-CONFORMANCE.md`.
- **rom-harness link fix** (`fca3b259`) — stub `ss_stub_trace_dump` for standalone link; fixes
  pre-existing break from `127d54d8`; rom-harness `a64`/`op` deltas remain +0.000.
- **Crash-path MMIO stats dump** (`e7db6336`) — the JIT SIGSEGV handler dumps per-region MMIO
  counters on crash, so a boot that dies before clean shutdown still yields device-traffic
  evidence (used by the acceptance run below).
- **e2e log-fixture fix** (`686a4771`) — e2e test fixtures were silently swallowed by the
  `*.log` gitignore pattern, breaking the offline unit suite on fresh clones; fixtures
  un-ignored, offline suite now 122/122.
- **Gates:** `make test-jit` 350/350 score=100 throughout; machine unit suite 6 binaries
  (28/19/18/64/21 checks + profile test) ALL PASS; THUNK-SELFTEST PASS (through the real
  patch write path).
- **Acceptance (Task 12): PARTIAL.** Validated: bus activates on `machine newworld`;
  `[KDP-0x900]` wiring correct per-run (0 vs `0xF3012000`); `[M1] scc_init ROM patch retired`
  fires; no undecodable-access or macio-stub aborts; no regression vs baseline; paravirtual
  `make e2e` smoke **PASS** (boot → Finder → clean shutdown, exit 0, zero `[MMIO]` lines).
  **Blocked:** consumer (b)'s end-to-end "`check_work` polls the real SCC without a fault
  storm" could NOT be exercised — the 9.0.1 diagnostic boot crashes at the pre-existing M0
  ceiling (guest pc=`0x503123fc`, ea=`0xffffffff`, nanokernel page-descriptor build loop)
  before the kernel idle phase, identically with SCC enabled and disabled (SCC/VIA region
  counters zero on both runs). The fault path + backpatch + thunk ARE validated by unit
  tests (21-check machfault dispatch; THUNK-SELFTEST); only the live boot exercise is
  missing — carried forward as the first work item when the fidelity-profile boot resumes
  (M3/M5 territory, HANDOFF §2.8 obstacle map). Logs: `/tmp/m1-baseline.log`,
  `/tmp/m1-accept.log`, `/tmp/m1-e2e.log`.

### [SheepShaver] Machine Layer M0: pref-selected machine profile (paravirtual / newworld)

- **`machine` pref + profile module** — new `machine` pref (`paravirtual` default /
  `newworld`) resolved once at startup (`src/machine/machine_profile.cpp`, standalone unit
  test: `make -C src/machine test`). Env override `SS_MACHINE`; legacy `SS_NW_TRAMPOLINE`
  is now a deprecated alias that maps to the newworld profile (warning printed).
- **`SS_NW_*` env-gate sprawl consolidated** — all 9 getenv sites (rom_patches,
  sheepshaver_glue, main_unix, name_registry) now consult the profile module.
- **Fidelity-profile fault honesty** — on `machine newworld`, the legacy PC-keyed
  serial/installer skip hacks and the `ignoresegv` blanket skip are disabled (a silent
  skip would eat MMIO accesses the future bus must see); zero-page skip kept on both.
- **M0 design artifacts** — `docs/planning/machine/CORE99-MACHINE-DESCRIPTION.md` (the
  cited address-map/interrupt-tree/device-tree contract M1–M5 implement against) and
  `docs/planning/machine/ROM-PATCH-AUDIT.md` (22 device-neutralizing patches with
  retire-at-milestone dispositions).
- **Paravirtual non-regression verified** — byte-identical default behavior: `make
  test-jit` 350/350 score=100, machine unit test ALL PASS, e2e lifecycle PASS (boot →
  Finder → clean shutdown, exit 0). See `docs/planning/MACHINE-LAYER-PLAN.md` (M0).

## 2026-06-09

### [SheepShaver] NewWorld: DR Emulator entry wall cleared

- **NewWorld: DR Emulator entry wall cleared** — promoted ECB/KDP field writes (ECB ptr at
  +0x65c, EMUL_RETURN at +0x5f0/+0x5f4, 68k vectors) from `SS_NW_SYNTH_ENTRY` diagnostic to
  `SS_NW_TRAMPOLINE` Path A; boot now enters DR Emulator cold-start (region 50460000
  compiles). Next wall: 68k dispatch table setup. Stop-rule revised — hybrid approach
  validated.

### [SheepShaver] NewWorld nanokernel init — complete (forcing function closed)

- **SegMap/PMDT spike**: PPC stub at ROM+0x30d600 writes minimal SegMap pointers + PMDT
  data (one 256MB RAM area + sentinels for 16 segments) immediately before
  CreateAreasFromPageMap, bypassing KDP+0x80 corruption during page-init. Both `bl` call
  sites (0x3124e4, 0x312568) redirected. Replaces NOP_AREAS and MARKER_TEST diagnostics.
- **Fake VIA page**: Mapped page at 0x68FAF000 with IFR timer bit set, stored at
  KDP-0x900. Nanokernel's SchIdleTask exits idle loop, VIA interrupt handler runs,
  scheduler dispatches to DR Emulator.
- **Result**: PPC nanokernel init sequence fully complete — boot reaches DR Emulator entry
  (0x5046f900) at 568 compiled blocks, 54M blocks/s. Crash at PC=0 is expected (68k
  environment not initialized; `SS_ROM_SKIP_JUMP68K` still active).
- **Forcing function closed**: Stop-rule fires — remaining work (68k HLE shim porting) is
  ROM-specific byte-patching with no general emulator fix on the horizon. The NW boot
  harvested: SPRG0-3, fctiw rounding, SDR1/HTAB, page-descriptor free-list, SegMap/PMDT,
  VIA idle loop. Milestone captured as `make test-nw-init` regression gate.
- All changes env-gated on `SS_NW_TRAMPOLINE`, default off. No regression to OldWorld
  path (test-jit score=100, 350/350).

## 2026-06-08

### [shared] RPC mem_search overflow + New World mapping visibility in ss_rpc_is_mapped

- **Correctness Fix:** Fixed an infinite loop in `ss_rpc_handle_mem_search` caused by `uint32_t` wrap-around in the loop variable `addr`. When `addr` reached `0xFFFFFFFC`, `addr + 4` overflowed to `0x0`, remaining `<= end_addr` and causing an infinite loop. Changed the loop iterator to `uint64_t` to resolve this.
- **Diagnostics Extension:** Extended `ss_rpc_is_mapped` to recognize the custom memory regions dynamically mapped under `SS_NW_TRAMPOLINE` (HTAB, sub-KDP pool, and page descriptors), enabling diagnostics tools to query these regions via RPC memory search/read commands.

### [SheepShaver] move SS_DUMP_ROM from patch_68k to PatchROM (`ecd341f3`)

- Moved `SS_DUMP_ROM` execution into the general `PatchROM()` function. This ensures that the decompressed ROM image is successfully dumped for offline analysis even when the versioned patch stages in `patch_68k()` fail or are skipped.

### [SheepShaver] SDR1 register + HTAB allocation — nanokernel page-table init unblocked

- **General correctness fix (all guests):** `mfspr`/`mtspr SDR1` (SPR 25) now read/write a real
  register (`ppc-registers.hpp`, `ppc-execute.cpp`). Previously, `mfspr SDR1` returned a hardcoded
  sentinel `0xdead001f`; `mtspr SDR1` was silently ignored. SDR1 field appended LAST in
  `powerpc_registers` (preserves JIT hardcoded offsets). `test-jit=100`.
- **HTAB allocation (New World only, env-gated):** the `SS_NW_TRAMPOLINE` trampoline maps + zeros
  64 KB at `0x68FE0000` (below sub-KDP pool) and seeds `SDR1=0x68FE0000`. The nanokernel's own
  `mfspr SDR1` now returns this real base, and its zeroing loop writes to mapped memory (microseconds
  instead of 131s of faulting stores).
- **ROM-patch skip:** `rom_patches.cpp` `sdr1_read` and `pgtb_clear` patches (which replaced
  `mfspr SDR1` with `lis r8,0xdead` and NOP'd the `stwx` zeroing) are now skipped for parcels
  (gated on `g_rom_904_lenient`). The 1.1 LZSS path is byte-identical.
- Boot advances past the HTAB wall to **420 compiled blocks**, settling at a new wall (`0x50312250`).
  OldWorld Mac OS 8.6 boot verified (no regression).

### [SheepShaver] Sub-KDP pool region mapped — nanokernel heap allocator unblocked

- **Memory-layout fix (New World only, env-gated):** the nanokernel's heap/pool allocator initializes
  a free-list at `KDP - 0x7000` (guest `0x68FF7000`). `KERNEL_AREA_SIZE = 0x2000` — the shmem mapping
  only covers `[0x68FFC000, 0x69000000)` after SHMLBA alignment. The pool region at `0x68FF7000` is
  **unmapped**, so pool init's `stw` stores silently fault (the default `ignoresegv=true` skips them
  via `SIGSEGV_RETURN_SKIP_INSTRUCTION`), the pool data structure is never written, and the allocator
  reads garbage → infinite zeroing loop at `0x50322990` (the "128-PC wedge").
- **Fix:** `vm_acquire_fixed` + zero 32 KB below the kernel-data shmem base, inside the
  `SS_NW_TRAMPOLINE` gate (OldWorld path is byte-identical). **Verified** via SIGSEGV-handler
  instrumentation (probe present during both before/after runs): **10 faults** in
  `[0x68FF5000..0x68FF7000)` before fix → **zero faults** after. `test-jit=100` (350/350).
  OldWorld boot to Finder verified (~9.2s).
- **Advances the parcels nanokernel through its complete init sequence:** pool free-list allocation,
  pool-init (`0x50322784`), zeroing loop (`0x50322990`), serial debug output, SR/BAT loading, and
  address-space context switch. Boot reaches **373 unique PCs** (up from 128), **410 compiled blocks**.
  Hits the **SDR1/HTAB wall** at block 14000: `mfspr SDR1` returns `0xdead001f` (sentinel) →
  nanokernel computes HTAB at `0xDEAD0000` (2 MB) → zeroing loop at `0x50311ff4` → 524K faulting
  stores → ~131s effective stall. Next: implement SDR1 read/write (general correctness fix).

### [SheepShaver] SPRG0-3 registers implemented — general JIT correctness fix

- **Correctness fix (all guest OSes):** `mfspr`/`mtspr` for SPRG0-3 (SPR 272-275) were previously
  **dropped** (writes ignored, reads returned 0) — simply wrong; SPRG0-3 are OS scratch / per-CPU
  pointer registers. Added real `sprg[4]` storage (appended LAST in `powerpc_registers` so no JIT
  hardcoded offset moves), wired interp `mfspr`/`mtspr`, zero-init in `init_registers`. `test-jit=100`.
- **Found via the New World parcels nanokernel** (the correctness forcing-function), which stashes its
  per-CPU/KernelData pointer in SPRG0 and reads it back — with SPRG dropped it read 0 → garbage pointer
  → spinlock deadlock. See `docs/planning/NEW-WORLD-ROM-SUPPORT-PLAN.md`.
- **Build note:** changing `ppc-registers.hpp` needs all PPC TUs recompiled; the Makefile doesn't track
  that header dep, so a stale incremental build silently mismatches struct offsets (manifests as
  `test-jit=0`). `rm obj/ppc-*.o obj/sheepshaver_glue.o obj/ppc-jit.o` after struct changes.

### [SheepShaver] New World parcels-ROM correctness probes (first runtime boot of a parcels ROM)

- **`:715` CPU-detect skip + `sr_load` skip** (gated on `g_rom_904_lenient`; 1.1 path byte-identical):
  the parcels ROM self-handles the faked G4 PVR, and SR/BAT loads are JIT no-ops, so both 1.1-era
  patches are skippable on parcels. Got the **parcels (9.0.1) PPC nanokernel to RUN under the JIT for
  the first time** (diagnostic config `SS_ROM_SKIP_JUMP68K` skips the un-ported 68k handoff + 68k HLE).
- **`SS_SYNTH_DEC`** (env, default off): synthetic free-running decrementer (mfspr DEC was 0). Fidelity
  tool; ruled out "time-based" for the parcels wedge.
- **Diagnostics** (env, zero-cost off): `SS_LOG_FIRST_BLOCKS=N` dumps the first N block-entry PCs (boot
  path) + a one-shot lock-state dump at the parcels spinlock-acquire. Reusable for any boot bring-up.
- Root cause of the parcels wedge traced to the SPRG/Trampoline supervisor-environment gap (above +
  plan doc). The 9.x boot is treated as a **forcing function for PPC/JIT correctness**, not an end.
- **`SS_NW_TRAMPOLINE`** (env, default off, 1.1-safe): emulates the Trampoline-established supervisor
  environment — seed `SPRG0=KernelDataAddr`, back+zero the negative KDP scratch, `[SPRG0-4]=KDP`. This
  advances the parcels nanokernel **27 → 128 distinct PCs** past the block-12 spinlock deadlock (clean,
  no derail) to a new wedge at `0x50322990`/`0x503251e4`. See `sheepshaver-research/SPRG0-KDP-DESIGN.md`.
- **`fctiw` honors dynamic FPSCR[RN]** (general fix, separate entry below in spirit): selects
  FCVT{AS,ZS,PS,MS} by RN instead of fixed FCVTAS. Resolved+promoted the `fp_fctiw_dynround` harness
  quarantine; test-jit now 350/350.

## 2026-06-07

### [SheepShaver] Boot-stall watchdog — early-boot dead-ends now alarm in the log

- **Problem:** early-boot dead-ends (the ROM "This startup disk will not work on this Macintosh
  model" alert, a hang, a sad Mac) were invisible — a ~0.2s burst of JIT compilation then silence,
  because `[BOOT]`/`[APP]`/`[SYSV]`/`[READY]` all ride the `SynchIdleTime` idle hook, which a wedged
  guest never reaches. Operators were left waiting on a GUI screen the log couldn't see.
- **Fix:** `ss_boot_stall_check` (`emul_op.cpp`), driven by the host-side JIT heartbeat (which keeps
  ticking through the wedge), raises a loud **`[ALARM]`** the moment the pre-idle stall shape holds
  (blocks spinning fast, no new compiles, idle never reached), then re-states **`[STALL]`** every 30s
  so a `tail` shows the live state instead of silence. Verified: `[ALARM]` at 15.0s on the failing
  9.2.1 NewWorld boot.
- **False-positive safe:** scoped strictly pre-idle (unlike the forbidden post-boot "same-PC" rule —
  cf. CLAUDE.md), self-disarms when `[BOOT] idle` fires (healthy boot ~10s « 15s threshold), re-arms
  if compilation resumes. Names the front dialog when the WindowManager is up; for a pre-System
  DSAlert it says so and points to a screenshot. Tunable via `SS_BOOT_STALL_SECS` (default 15; 0 off).
  Docs: `SheepShaver/docs/DIAGNOSTICS.md`.
- **`SS_NW_MODEL` probe (default off):** inject a New World device-tree identity (`model` +
  `compatible`) to test booting 9.2 on the 1.1 ROM without the parcels-ROM port. FINDING (via the new
  watchdog): **insufficient alone** — 9.2.1 still rejects the model; needs the real New World ROM
  environment. Kept as default-off groundwork. See `NEW-WORLD-ROM-SUPPORT-PLAN.md`.

### [SheepShaver] Preliminary AltiVec support — opt-in `altivec` pref; real app runs vector code

- **Real-app AltiVec achieved.** The AArch64 JIT already compiled PPC AltiVec → ARM64 NEON (validated by
  `make test-jit`); the missing piece was *detection* — under the OldWorld 1.1 ROM the guest never
  registers the `'ppcf'` gestalt, so apps ran scalar. New opt-in **`altivec` pref** (default off;
  `prefs_items.cpp`) registers `'ppcf'` with the vector-feature bit via `_NewGestalt $A3AD`
  (`emul_op.cpp force_altivec_idle_service`; `SS_FORCE_ALTIVEC` env = dev override). With it, **AltiVec
  Fractal Carbon detects the Velocity Engine and runs its vector kernel through the JIT** —
  `SS_JIT_PROFILE` → `[JIT-COMPILED-MIX] AltiVec=160`, AltiVec hot blocks.
- **New profiler metric** `[JIT-COMPILED-MIX]`: global per-class op-compile histogram (AltiVec/FP/int/…)
  — the definitive "did vector code actually run" signal.
- **Opt-in / preliminary by design:** Mac OS 8.6/9.0 here don't VR-context-switch, so it's safe for a
  focused compute app, not general multitasking vector use; `VSCR[SAT]` unmodeled. Roadmap §B5 tracks
  making it fully safe. Docs: README "AltiVec" section, USER-HANDBOOK pref+caveat, ALTIVEC-DETECTION-RESEARCH.md.
- **Bug-hunt note:** first attempt set gestalt bit `0x40` (= 64-bit-support) instead of `0x10` (vector,
  `1<<gestaltPowerPCHasVectorInstructions`), yielding a self-consistent wrong "FC ignores gestalt"
  conclusion until a research agent checked Apple's `Gestalt.h`. Lesson: a bit-number constant is `1<<N`.

### [SiliconSheep] UX polish — Gatekeeper quarantine button + fullscreen escape overlay

- **Gatekeeper mitigation**: on startup, checks SheepShaver binary for `com.apple.quarantine`
  xattr. Shows amber warning banner with one-click "Clear Quarantine" button (`xattr -cr`).
- **Fullscreen escape overlay**: 3-second fading HUD on VM launch showing Ctrl-F5 (release
  mouse) and Esc (exit fullscreen) shortcuts.

### [SheepShaver] B1 execution profiler + fallback trace + P3 block timing

- **B1 profiler**: per-block execution counter (`SS_JIT_PROFILE=1`). Hot Blocks panel.
- **Fallback trace**: per-PC interpreter fallback counts with opcode ID. Fallbacks panel.
- **P3 block timing**: wall-clock per-block via `mach_absolute_time()`. Block Timing panel
  with flame chart SVG visualization (top 40 blocks, red→yellow heat scale).
- **Instruction mix**: per-opcode execution breakdown via `RPC_METHOD_GET_OPCODE_MIX` (21).
  Ranked table with execution counts, percentages, and bars.
- **Region heat map**: execution density by 64K address region via `RPC_METHOD_GET_HEATMAP`
  (22). Bar chart with ROM/RAM/DR labels.
- **Session comparison**: load two `.sheepshaver-profile` files for side-by-side diff.
  Duration, event count, per-metric table, and dual sparkline rate-over-time charts.
- All via C2.0 RPC (methods 18-22). Zero overhead when profiler is off.

### [docs] SiliconSheep Inspector — Tier 2 machine state + P1 session recording

- **Register Inspector** — GPR r0-r31, SPR (PC/LR/CTR/CR/XER) snapshot via
  `RPC_METHOD_DUMP_REGISTERS`. Change-highlighting between snapshots (Snow-style).
  `dump_regs_json()` method added to `sheepshaver_cpu` class.
- **Memory hex viewer** — read guest RAM at any address via `RPC_METHOD_READ_MEMORY`
  (capped at 4K). Classic hex+ASCII dump with preset address buttons (Low Memory,
  CurApName, WindowList, ROM Base, RAM Base).
- **Guest State panel** — live OS version, window list (title/kind/bounds/visibility),
  screen size, modal state, front window via `RPC_METHOD_UI_SNAPSHOT`. Reuses the
  existing `ui_introspect.cpp` `serialize_snapshot()` backend directly.
- **Session recording** — Record button captures events + JIT stats + guest state
  every 2s. Saves to `.sheepshaver-profile` JSON. Load button replays with sparkline
  chart (block rate over time), event timeline with timestamps, stats table.
- **RPC status indicator** — Inspector toolbar shows connection state. Friendly errors
  instead of raw "No such file" messages.

### [SheepShaver] LIVE FP bug: mtfsf/mtfsfi code bodies were swapped vs their XOs

- Extended the XO audit to the scalar-FP switch (primary 63), which — unlike dormant AltiVec — runs
  in **every boot**. Found `mtfsf` (XFL XO 711) and `mtfsfi` (X XO 134) with their **code bodies
  swapped**: `case 711` ran the mtfsfi decode (`crfD`/`imm`), `case 134` ran the mtfsf decode
  (`fm`/`frB`). Both write FPSCR + sync rounding, so a guest `mtfsf`/`mtfsfi` would set the wrong FPSCR
  fields. Fixed by swapping the case labels (correct by inspection — each body decodes the other
  instruction's fields). **Severity: latent — no observed boot impact** (Mac OS boots cleanly, so these
  are off the boot hot path or were benign in practice); a correctness fix, not evidence prior boots were
  wrong. `fsel`/`fsqrt`/`frsqrte` (A-form in the X-form switch) audited as benign (frC=0 or interp fallback).
- New QUARANTINE repro `fp_fctiw_dynround`: JIT `fctiw` uses a fixed rounding (FRINTA) and ignores the
  dynamic FPSCR RN that `mtfsfi` sets (interp rounds 2.25→3, JIT→2). Separate minor limitation
  (non-default `fctiw` rounding is rare); flips to xpass when fctiw honors dynamic RN. `make test-jit`
  349/349, quarantine 1 xfail.

### [SheepShaver] AltiVec XO audit tool + FP round/compare fixes (vrfin/vrfiz/vcmpgefp) + vmsum→interp

- New tool `tools/altivec-xo-audit.py`: cross-checks every JIT `case N:` (mnemonic from its comment)
  against the authoritative `{mnemonic→XO}` in `ppc-decode.cpp`. One run found 13 more mismatches.
  (Blind spot, documented: catches XO/label mismatch only — right-XO-wrong-codegen is invisible;
  `make test-jit` is the codegen gate.)
- **Fixed** (emitted wrong code for real ops): `vrfin`@522 ran FRINTP, `vrfiz`@586 ran FRINTM,
  `vcmpgefp`@454 ran FCMGT. Remapped all four FP rounds to authoritative XOs + both FP compares.
  `vrfin` is round-half-**away** (FRINTA, matches interp `frsin`), not ties-to-even — the differential
  tie test caught the wrong choice. Routed the broken `vmsum*`/`vmhaddshs`/`vmhraddshs`/`vmladduhm`
  VA-form block (horizontal multiply-sums / swapped operands) to the interpreter; kept the correct
  `vmaddfp`/`vnmsubfp`/`vsel`/`vperm`.
- **Deferred** (already correct via interp fallback; logged): 6 dead-XO cases (`vupkhsb/hsh/lsb/lsh`,
  `vexptefp`/`vlogefp`) — moving them to the right XO only accelerates dormant code.
- 6 new differential vectors (`av_vrfin/vrfiz/vrfip/vrfim/vcmpgefp/vcmpgtfp`). `make test-jit` 349/349.
  Note: these are wrong results *if/when AltiVec is enabled* (currently dormant); record forms'
  CR6 is unmodeled (pre-existing family-wide gap, like VSCR).

### [SheepShaver] AltiVec whole-vector shifts fixed — vsl/vslo/vsro had scrambled XOs + per-lane codegen

- A coverage audit (JIT `case` labels vs test-vector names) found a second scrambled family. The
  whole-128-bit shifts `vsl` (XO 452), `vslo` (1036), `vsro` (1100) were running the *wrong* ops'
  **per-lane** NEON codegen (vsl→vsldoi EXT-by-constant, vslo→per-byte SSHL, vsro→per-byte NEG+USHL);
  `case 1356/1420` were dead (non-existent XOs). `vsr` (708) and `vsldoi` already fell back correctly.
- **Fix**: route `vsl/vslo/vsro` to the interpreter (correct) and remove the dead cases; native
  TBL-based codegen documented as a perf follow-up. Confirmed by 4 new vectors (`av_vsl/av_vslo/av_vsro`
  diverged before the fix; `av_vsr` already passed). `make test-jit` 343/343, score=100.

### [SheepShaver] AltiVec sum-across family fixed — scrambled XO map + missing saturation (5 ops)

- The horizontal-reduce-and-saturate ops (`vsum4ubs`/`vsum4sbs`/`vsum4shs`/`vsum2sws`/`vsumsws`)
  were doubly broken in `ppc-jit.cpp` and had **zero test coverage**:
  - **Scrambled XO→op map**: real guest `vsumsws` (XO 1928), `vsum2sws` (1672), `vsum4sbs` (1800)
    each got a *different* op's codegen; `vsum4ubs` (1544) had no case (fell back to interp);
    a dead `case 1932` matched no real op. Authoritative XOs are in `ppc-decode.cpp`.
  - **No saturation**: every variant added `vB` / reduced with a plain `ADD.4S`/`ADDV` that wraps
    at the int32/uint32 boundary instead of clamping.
- **Fix**: correct XOs + saturating codegen — `UQADD`/`SQADD.4S` for the per-word ops; 64-bit
  accumulation (`SADDLP.2D`/`SADDLV` → `SQXTN.2S`) for `vsum2sws`/`vsumsws` (2-/4-word sums can
  exceed int32). All NEON encodings verified offline (`as -arch arm64` + `otool`).
- **Caught + gated** by 5 new boundary vectors in `gen-altivec-vectors.py`; the interpreter
  (int64 accumulation, `v4si_sat_operand`) is independent ground truth. `make test-jit` 337/337,
  score=100. Detail in LEARNINGS.md (2026-06-07).

### [SheepShaver] AltiVec 1-5-5-5 pixel family fixed (vpkpx/vupkhpx/vupklpx) + convert-op XO bugs (`ff8e9504`, `e8e30c2e`)

- `vpkpx` (pack, 782): new `emit_vpkpx` builds each 1-5-5-5 pixel from a word via USHR.4S + AND-mask
  field extraction, then the word→halfword pack tail. `vupkhpx`/`vupklpx` (unpack, 846/974): new
  `emit_vupkpx` selects+widens 4 halfwords (UXTL/UXTL2) and expands the 5-5-5 channels (AND/SHL) +
  synthesizes the sign-bit alpha via `SSHR((h&0x8000)<<16, 7)` → `0xff000000`. All capstone-verified.
- **Uncovered + corrected two pre-existing XO bugs:** `vcfsx` is XO 842 (not 846) and `vcfux` is 778
  (not 910) — the 846 entry was actively miscompiling `vupkhpx` as `SCVTF`. Removed both wrong cases;
  `vcfsx`/`vcfux` now fall to the interpreter (their plain `[SU]CVTF.4S` ignored the UIMM scale anyway).
- 2 differential vectors (8 distinct pixels, sign-bit mix). `make test-jit` **332/332**. The AltiVec
  pixel family is complete; remaining open AltiVec: sum-across, scaled converts, fctiw rounding.

### [SheepShaver] AltiVec halfword multiplies + vpkuwum fixed (ev_mixed multiply/pack family complete) (`92594ea4`, `808c26db`, docs `c5f4e81c`)

- `vmul{o,e}{u,s}h` (even/odd halfword multiplies, 72/328/584/840): were broken (no ev_mixed
  even/odd select; unsigned encoding `0x0E60A000` isn't UMULL). Fixed via `emit_vmul_hword` — the
  halfword analog of the working `emit_vmul_byte`: REV32.8H normalize → UZP1/2.8H even/odd select →
  `[SU]MULL.4S` widen (16×16→32) → word output. 4 vectors with high-bit halfwords (signed≠unsigned).
- `vpkuwum` (word→halfword modulo pack, case 78): ignored vA; routed through `emit_vpk_w2h` with
  truncating `XTN`/`XTN2`. 1 vector.
- With these, the entire **ev_mixed multiply/pack family is complete** (7 packs + 4 byte mults + 4
  halfword mults). `make test-jit` **329/329**. Remaining open AltiVec are *new derivations* (not
  pattern-extensions): pixel `vpkpx`/`vupk{h,l}px` (1-5-5-5 bit-field) + sum-across.

### [SheepShaver] AltiVec saturating packs fixed — all 6 (vpk{sh,uh,sw,uw}{ss,us}) (`17a73fac`, `f6326070`, docs `a3a72c66`)

- The six saturating pack ops were broken (single-source narrow ignoring vB, no ev_mixed
  normalize, and a mislabeled `0x2E212800` "UQXTN" that is actually SQXTUN → unsigned packs
  clamped negatives-as-signed to 0). Fixed via two ev_mixed-aware helpers: `emit_vpk_h2b`
  (halfword→byte: REV32+REV16 normalize → `[SU]QXTN`/`QXTN2` → REV32 back) and `emit_vpk_w2h`
  (word→halfword: no input normalize — raw `.4S` already holds correct word values — →
  narrow → REV32+REV16 halfword-output normalize).
- Derived empirically against the interpreter REGDUMP with **saturation-crossing operands**
  (negatives + over-range) so the three signednesses are distinct (non-saturating operands make
  SQXTUN/UQXTN identical — the false-PASS trap the earlier reverted attempt hit). 6 committed
  differential vectors. `make test-jit` **324/324**. Remaining open AltiVec: pixel + sum-across
  families. See `docs/planning/ALTIVEC-SHIFT-ROTATE-BUGS.md`.

### [SheepShaver] fsel/frsp/frsqrte/fsqrt → zero-copy FP RA (P5b follow-up, completes FP-RA op conversion) (`e67b3913`, docs `3cb5bb9f`)

- Converted the last bridge-using FP ops to direct FP-RA access: `frsp`/`fsel` (differentially
  tested — added `fp_fsel_pos`/`fp_fsel_neg` exercising both select arms) and `frsqrte`/`fsqrt`
  (double; mechanical/prospective — `frsqrte` is an estimate, the emulated 603/604/750 interpreter
  doesn't implement `fsqrt`, so neither is differentially testable). `make test-jit` **317/317**.
- The FP path now has **no `emit_*_fpr` bridge uses in any hot op**; the bridge remains only for
  genuinely struct-resident reads (mffs/mtfsf/fcmp). See OPTIMIZATION-PLAN §P5b.

### [SheepShaver] FP indexed/update memory → zero-copy FP RA (P5b follow-up) (`d1ca48ae`, eviction test `fa66964a`, docs `6ad47bc7`)

- Converted the FP **indexed** loads/stores (`lf{s,d}x`/`lf{s,d}ux`/`stf{s,d}x`/`stf{s,d}ux`,
  opcode-31 cases 535/567/599/631/663/695/727/759) and **D-form update** forms
  (`lf{s,d}u`/`stf{s,d}u`, cases 49/51/53/55) from the FMOV bridge to direct zero-copy FP-RA access,
  matching the already-converted non-update D-form. Removes a per-op FMOV/V0 round-trip when the FPR
  is RA-resident and keeps FP values RA-resident across loads/stores in FP-heavy loops.
- **Coverage gap closed:** these 12 ops were untested (only D-form `lfd/lfs/stfd/stfs` had vectors).
  Added 12 differential vectors; verified non-vacuous (GPR result) + rA-writeback (REGDUMP). `make
  test-jit` **315/315** (was 303). FC renders identically (pHash unchanged), guest-MIPS within noise
  (FC's loop is register FP arith — the win accrues to array-heavy FP code). See OPTIMIZATION-PLAN §P5b.

### [SheepShaver] mfmsr JIT/interp divergence fix + AltiVec-detection finding (mfmsr[VEC] is NOT the gate) (`cd6df179`, docs `65588767`)

- **Fix (`cd6df179`):** the aarch64 JIT's `mfmsr` (case 83) returned `0`, while the
  interpreter's `execute_mfmsr` returns `0xf072` — a latent JIT/interp divergence for any
  guest that reads MSR and branches on it. JIT now returns `0xf072`. `make test-jit` 303/303.
- **Finding (falsified hypothesis, task #21):** advertising MSR[VEC]=1 via `mfmsr`
  (`0x0200f072`) does **not** enable AltiVec. Booted Fractal Carbon with the change →
  profile still shows **0 AltiVec blocks** (hot mix unchanged: 24 integer / 11 load-store /
  4 branch / 1 FP, ~1105 guest-MIPS). The gate is the gestalt `'ppcf'` (`0x70706366`) vector
  bit; *where* it is computed is open (#23, see the `112481f2` probe below). Experiment reverted.
- **Reframing:** the aarch64 AltiVec NEON codegen is **mature and dormant**, not missing
  (54 differential vectors, 27 bugs fixed; open: pack/pixel/sum + `fctiw` rounding). AltiVec
  enablement is a *detection* problem, not a codegen one. Codegen-first ordering adopted: close
  the open families via the `SS_TEST_HEX` differential harness before attempting gestalt
  enablement. See LEARNINGS 2026-06-07, ROADMAP B5, tasks #22/#23.

### [SheepShaver] `SS_LOG_ILLEGAL` diagnostic + `mtmsr`/MSR[VEC] AltiVec-detection probe (`112481f2`)

- **Diagnostic:** env-gated (`SS_LOG_ILLEGAL=1`) log at the top of `execute_illegal` recording every
  undecoded opcode reaching the handler, decoding `mtmsr` (op31/XO146) and testing the MSR[VEC] bit
  `0x02000000` of rS. Reusable for any undecoded-opcode triage; zero cost when unset.
- **Finding (falsified, task #21):** the "OS enables AltiVec via an `mtmsr` WRITE we silently drop"
  theory is **false**. Confirmed `mtmsr` is *not* NOP-stubbed (JIT `compile_one`→false→inline-interp
  →`execute_illegal`; validated with `SS_TEST_HEX=7C600124` in JIT+interp), then booted **Mac OS 9 +
  Fractal Carbon** via the E2E workload: **zero `mtmsr`, zero illegal opcodes** all run. `mtmsr`-enable
  is downstream of detection.
- **Measurement (not a full correction):** the `'ppcf'` **selector byte-string** appears **0× in the
  OldWorld ROM, 5× in the Mac OS 9 boot disk** (measured 2026-06-07) — so the selector is *referenced*
  on-disk. This does **not** locate where the vector bit is *computed*: a ROM/NanoKernel handler the
  System invokes is consistent with the selector living on-disk, so the prior entry's "computed in ROM"
  is **unverified, not disproven** (#23 is open precisely to settle this). Either way, enabling AltiVec
  is Phase-3 "widen emulation" work (System `'ppcf'` path + VR context-switch save/restore), not a quick
  ROM patch. `make test-jit` **332/332**. See LEARNINGS 2026-06-07 "AltiVec detection is NOT an
  `mtmsr`/MSR[VEC] path", ROADMAP B5, the in-code DORMANT banner.

### [shared] C2.0 Bidirectional UDS RPC — sub-16ms launcher↔emulator IPC

- Emulator becomes an RPC server on `rpc_unix.cpp` UDS framework (`/tmp/sheepshaver-<pid>`).
  Non-blocking poll at 60 Hz. New methods: INPUT_LOCKOUT, FRAMESKIP, GET_STATS, READ_MEMORY.
  `rpc_listen_socket_nb()` for non-blocking accept. SiliconSheep RPC client (`rpc_client.rs`)
  with lazy connect and fallback to file polling. Same pattern as QEMU QMP.

### [docs] SiliconSheep Inspector + Profiler + HIG behavioral reference

- Inspector as separate window (4 panels: Overview/Timeline/Log/Debug + session recording).
  Profiler plan: P1 session recording, P2 per-block (B1), P3 flame chart.
- Classic Mac OS HIG behavioral patterns reference. Snow evaluation revised (S1 crosswalk).

### [docs] SiliconSheep — full Tier 1 feature set + major UX restructure

- Master-detail layout, immediate-apply settings (Mac OS 9 HIG), multi-VM support (C5),
  Platinum styling, pixel art icons (slimes.ca), 4 consolidated tabs, disk resize, clipboard
  status, OS target presets, description + dates, bug report bundle, VNC screenshots, guest
  OS version detection, SavePrefs comment preservation, 19 Rust tests. See
  `docs/planning/DESKTOP_INTEGRATION_PLAN.md` for full feature list.

### [e2e][docs] E2E toolkit review — `drive.py` refactor, agent API, discoverable doc map

Tidy-up + coherence pass on the E2E harness before handing it to parallel work, adversarially reviewed
(`docs/archive/2026-06/planning/E2E-TOOLKIT-REVIEW-AND-MCP-PROPOSAL.md`). The review flipped the plan: the first job was a
**refactor**, not docs. Shipped:
- **`sse2e/drive.py`** (`f97310bd`) — extracted the boot/drive/gate primitives + the dedup'd `quit_app` /
  `clean_shutdown` / `reactor_shutdown` (was verbatim-duplicated across `_quit_to_finder`/`_quit_workload`,
  the verdict ×3, the reactor teardown ×4); `scenario.py` 753→609 ln. Behaviour-preserving: 122 offline +
  `make e2e` smoke + `make e2e-workload` all PASS (render 18.29s, Δ +0.00s vs pre-refactor).
- **`SheepShaver/e2e/AGENT-API.md`** + the e2e README "toolkit at a glance" map (3 gates, 2 sensors + the
  Carbon-vs-dialog decision line, entry-point table, the two-history note) (`16fd110c`). The MCP-server idea
  is parked: introspection is a launch-time contract (`SS_UI_DUMP_DIR`) so an attaching server can't use it,
  and the existing `sse2e` Python API already *is* the agent surface.
- **Tracked doc map in the root `README.md`** (`18c14a16`) — the comprehensive index had lived only in the
  gitignored `CLAUDE.md`, so a fresh clone/agent had no map to `docs/`, ROADMAP, the e2e harness, or
  AGENT-API; now it's in the tracked README (16 links verified). Audit fixes in passing: README test count
  79→122; unused `field` import.

### [SheepShaver] FP register allocator (P5b) — Speedometer Math +16%, the top throughput lever

The JIT had no FP register cache, so every FP op round-tripped the guest FPRs through the
`powerpc_registers` struct (store→load serialization). Added a **block-local FP register
allocator** mirroring the integer RA, mapping PPC FPRs → ARM64 **V16–V23** (caller-saved →
no prologue change → integer blocks stay byte-identical, zero regression). `emit_load_fpr`/
`emit_store_fpr` made RA-aware (FMOV bridge) so unconverted FP handlers stay coherent;
`ra_fp_flush_all` coupled into `ra_flush_all`. Converted to zero-copy: **double + single-
precision arithmetic** (fadd…fnmsub / fadds…fnmsubs / fres / fsqrts), **moves**
(fmr/fneg/fabs/fnabs), and **D-form memory** (lfs/lfd/stfs/stfd). Results: **Speedometer
Math 13,000 → 15,075 (+16%)** — JIT Math vs interpreter ~1.29× → **~1.89×**, in line with
integer (2.2×); **Fractal Carbon +8% guest-MIPS**; `fp-add`/`fp-fma` microbench **a64/op
4/5 → 1.0** (ns/insn ~14×/~9×). Validated: `make test-jit` **303/303** (added an
eviction-writeback vector); two adversarial reviews clean (V16–V23 confirmed exclusively
owned; all flush sites block-terminating; the integer RTMP/NZCV-across-`ra_store` landmine
cannot recur on the FP side). See OPTIMIZATION-PLAN §P5b. Follow-ups: FP update/indexed
memory, cross-block FP pinning.

### [SheepShaver] Native `lwarx`/`stwcx.` (P3a) + the 0f/0h codegen sweep

- **P3a — native `lwarx`/`stwcx.`** (single-CPU reservation in the shared regs struct): were
  interpreter fallbacks (two JIT→interp transitions per atomic iteration); now compiled
  natively. Correctness-validated (differential success/fail/reservation-cleared paths).
  Boot-profiler evidence showed atomics were the #1 *concentrated* hot block; the e2e-bench
  profile then showed they're ~5% of compute (not idle-driven) but **not** the dominant
  throughput cost — so this is banked as a correctness/architecture win, not a measured speedup.
- **0f sweep**: `divw` / `mulhw` / `mulhwu` drop their trailing MOV (write result via the final
  CSEL / shift directly into the dest). Plus a divw `ra_store` hoist removing a latent
  NZCV-clobber hazard (adversarial-review finding).
- **0h**: `rlwinm` `slwi`/`srwi` → single ARM64 `LSL`/`LSR` (UBFM) and `clrlwi`/`clrrwi` →
  AND-direct-from-rS (drop the mov). Hot (4× `slwi` in the Speedometer matrix block);
  microbench a64/op 2.0 → 1.0. Exhaustively validated (slwi/srwi 248/248 differential).

### [SheepShaver] Deterministic `a64/op` microbench metric + `SS_JIT_PROFILE` run-profile

- **`a64/op`** (`make bench`): emitted ARM64 instructions per PPC op, from the JIT's own
  `code_size` — **zero host-noise, machine-independent, CI-gateable**. The prior "<1% noise"
  claim corrected: it holds only on a *quiet* host (a fixed binary swung ±25% under load —
  P/E-core migration + DVFS). Added a CV-gate (`NOISY` instead of a bogus delta), QoS P-core
  pin, and `CLOCK_THREAD_CPUTIME_ID`. New `compute`/`shift` kernels model real hot blocks.
- **`SS_JIT_PROFILE` run-profile**: at exit emits `[JIT-RUN-PROFILE]` — empirical **guest-MIPS**
  (ops/sec for the whole run) + deterministic execution-weighted `a64/guest-op`. Captured per
  workload (boot 1040 MIPS, Speedometer 2413, Fractal Carbon 1132). `SS_JIT_PROFILE_DISASM`
  dumps hot-block PPC words for offline capstone disassembly.

### [docs] Findings: AltiVec dormant (MSR not modeled); JIT-vs-interpreter headline; benchmark series

- **AltiVec is dormant for real guest software** despite PVR=0x000c0000 (G4): MSR isn't modeled
  (`mfmsr`→`0xf072`, VEC bit clear), so the OS can't enable AltiVec → all software takes the
  scalar/FP path. Our AltiVec codegen is correct but **only exercised by the test harness**.
  Tracked as a fix (model MSR[VEC]) — would unlock real vector workloads.
- **`SS_E2E_TIMEOUT_SCALE`** env knob for profiled workload runs (profiler ~2× slows the guest,
  tripping the 45s launch gate). Documented benchmark series in BENCHMARKS.md (codegen-density /
  run-profile / Speedometer, with the interpreter floor). Host recorded: M5 MacBook Air, 32 GB.

## 2026-06-06

### [SheepShaver] P0 execution-weighted hot-block profiler (`SS_JIT_PROFILE`) — Track B start

First step of the performance pivot (OPTIMIZATION-PLAN §P0): a mix-aware, execution-weighted
hot-block profiler so optimization priorities come from *what actually runs* rather than the biased
compile-frequency proxy. Gated behind **`SS_JIT_PROFILE`** — zero codegen + runtime cost when off
(`make test-jit` 302/302 unchanged). When on, each compiled block emits a 64-bit exec-count
increment at its **chain entry** (counts dispatched *and* chained entries via RTMP0/RTMP1, free
pre-body); a pc-keyed slot table accumulates the count + a compile-time **instruction-mix tag**
(integer-ALU / AltiVec / FP / load-store / branch); at exit it dumps the **top-40 hottest blocks**
(pc, exec, %, mix, insns, ROM/DR/RAM region) to stderr or the path in `$SS_JIT_PROFILE`. Implemented
in `ppc-jit.cpp` (`jit_prof_*`/`jit_mix_classify`/`jit_profile_dump`). **Validated without a boot**
(another agent owns the emulator): the rom-harness activates it and produced a real dump with correct
mix tags, and its differential interp-vs-JIT Score is **identical on vs off (489/496)** — the counter
codegen doesn't corrupt block results. Done on branch `p0-profiler` (git worktree). Remaining: a real
boot run for true hot data + routine-name attribution. (Track B prereq; feeds the `jit-diff-sweep`
perf-join — hot × microbench ns/insn.)

### [e2e] Generic real-world-app workload scenario — Fractal Carbon is entry #1

`scenario.run_workload` + `sse2e/workload.py`: the reusable framework for benchmarking a real app —
boot a workload disk + attach the apps disk, launch by Finder type-select, and time the render via the
**screenshot perceptual hash** (not the window list — a Carbon/fullscreen app has no standard
`WindowRecord` while rendering). Gates: launch = the app **takes the menu bar** (region-pHash of the
menu-bar strip — classic Mac OS gives it to the frontmost app); a sustained Finder dialog with the menu
bar intact = **launch failure** (the missing-library alert emits no `[APP]`/introspection signal, so the
menu bar is the only tell — see `LEARNINGS.md`); render-done = pHash stabilizes; quit = pHash converges
back to baseline; then the honest clean-shutdown verdict. Perf signal = `render_s` (launch→stable) + the
result-frame pHash (visual fingerprint), appended to a per-workload `history.csv` with a trend line.
Entry point `run_workload.py` (+ `make e2e-workload WL=<name>`); pure logic (launch-classify, render-stable,
history) offline-unit-tested (`tests/test_workload.py`, 15 cases; suite 122). **Boot-validated BOTH paths
against AltiVec Fractal Carbon:** on the CarbonLib-1.6 disk → launched (menu-bar Δ=28), render stable 18.3 s,
clean shutdown, result pHash + history; on the old-CarbonLib disk → correctly FAILS with "app did not take
the menu bar … likely a launch failure" + a `launch-error.png`. Commits `b4a0a594`, `bbb53e80`.

### [SheepShaver][e2e] Guest UI introspection — window control-list items (non-dialog windows)

**Dogfooding fix.** Driving the CarbonLib installer over VNC exposed a real hole: the Apple Installer's
"Continue"/"Install" buttons live in a movable-modal/document window, but Backend A only emitted dialog
DITL `items` for `dialogKind` windows — so a non-dialog window's controls were absent from the JSON and
the driver had to fall back to blind Return. Now `serialize_window_controls()` walks
`WindowRecord.controlList` (+0x8C) → the `ControlRecord` chain (`nextControl`/`contrlRect`/`contrlHilite`/
`contrlValue`/`contrlTitle`, reusing Plan 2b's layout) for **non-dialog** windows too, emitting each
control as an `item` (type/rect/text/value/hilite, same schema) so `find_item`/`click_item` work on them
unchanged. **Verified by re-running the installer on the new build**: its Continue button is now clicked
**by name** via introspection (`click 'Continue'`) — a non-dialog window's controls now surface as `items`,
where before the feature they were absent and the driver fell back to blind Return (`no dialog button;
Return`, observed in earlier-session runs). `make test-jit` score=100; e2e offline suite 107 passed;
`ui-introspect-test` ALL OK — and the branch is now **offline-unit-tested** against a mock RAM (see the
serializer-harness entry below). Commit `ac4363a2`.

### [SheepShaver][test] Offline unit harness for the memory-walking UI serializers

Paid down tracked test-debt: the serializers (`serialize_snapshot`/`_window_controls`/`_dialog_items`/
`_menu_bar`) read guest memory via `ReadMacInt*` and were validated **boot-only**.
`ui_introspect_serialize_test.cpp` now compiles the **real** `ui_introspect.cpp` against a flat
big-endian **mock RAM** (stub `sysdeps.h`/`cpu_emulation.h` in `src/uitest/`, selected purely by `-I`
order — the real build is untouched) and asserts the JSON for hand-built Toolbox structures, so each
offset is a regression-tested fact. Fixtures cover all four serializers: non-dialog `controlList` → items (titled button globalized,
dimmed/untitled control, degenerate-rect skip), dialog DITL items (text/rect/refCon/defaultItem/modality/
default), and the `menuBar` walk (apple role + File ▸ New=⌘N / Open=⌘O). Wired into `make ui-introspect-test`;
standalone `make ui-introspect-serialize-test`. Commits `492607c4`, `b6ee595c`.

### [e2e][docs] Real-world workload bring-up: Fractal Carbon install + CarbonLib via introspection

First steps toward the S4/S5 real-app workload library. Installed **AltiVec Fractal Carbon** onto the
E2E apps disk host-side (forks/type intact; `docs/HOST-SIDE-MAC-SOFTWARE-INSTALL.md`), then resolved its
**CarbonLib ≥1.3** dependency the hard way — **drove the Apple `.smi` installer over VNC with guest-UI
introspection** (`SheepShaver/e2e/run_carbonlib_install.py`): CarbonLib → 1.6 (`INIT/cbon … Jun 2002`).
Extracted the installed extension as a reusable MacBinary (`/Users/Shared/macemu/CarbonLib_1.6_extension.bin`)
so future installs are a one-line `hcopy -m` — no SMI, no boot. Procedure + the SMI/dialog-vs-document-window
gotchas documented in the install doc; saga in `LEARNINGS.md`. Discovery harness commit `3c0368c8`;
install driver committed with `ac4363a2`.

**Validated end-to-end (2026-06-06):** baked CarbonLib 1.6 into a clean clonefile workload boot disk via
the reuse artifact (host-side, no boot), then booted it + the apps disk — **AltiVec Fractal Carbon now
launches and renders a live fractal** (`e2e/artifacts/fc_discover.png`), where it previously died on
`CarbonLib--GetPortBitMapForCopyBits could not be found`. Finding for the workload-scenario build: the
running Carbon app's fullscreen canvas is **not** a standard `WindowRecord`/`MenuList` (Backend-A returned
degenerate windows), so a Carbon workload must gate on screenshot/pHash + the app-change signal, not the
window list.

### [SheepShaver][e2e] Guest UI introspection — Plan 2c: menu bar + depth + desktop role

The last app-automation enabler: a top-level `menuBar` (menus + items + **Command-key equivalents** +
enabled + apple role) read by a research-backed, read-only `MenuList` ($0A1C) walk — **no Toolbox
traps** (the idle-hook reentrancy the repo already avoids), robust via **handle-anchoring** (each
entry must deref to a plausible `MenuInfo`, else stop). Offsets verified vs Carbon `Menus.h`
(`SheepShaver/e2e/MENUBAR-READ-SPEC.md`). The harness can now discover "File ▸ Open ⌘O" and fire it by
keystroke (`find_menu_item(snap, "Open").cmd_key`). Also: real `screen.depth` from the GDevice;
`role:"desktop"` on the Finder backdrop window. **Live-verified**: 7 Finder menus
(File/Edit/View/Special/Help…), File's New=N/Open=O/Close=W. A subtle bug fix along the way: extent-END
bounds checks must use a range-only guard (`guest_range_ok`) — `guest_ptr_ok` requires even alignment,
so odd-ended title/text/menu extents were spuriously rejected (this also hardens Plan 1/2a/2b reads).
Commits 4ca2da4d, 8aeb9a2f, ecb0ec42, 706ce769, d2fa9602, e73eb6a6, 5026c2b6.

### [docs] Stress-workload catalog → living capability matrix (stretch goals + frontier metric)

Extended `docs/MACOS9-STRESS-WORKLOADS.md` from a runnable-software list into a **capability matrix
that benchmarks reach over time**. Added a "Capability matrix & stretch goals" section that
**deliberately includes targets we can't run today** (Quake III / OpenGL games — no 3D accel;
FireWire capture; 9.2.x-only software) as **frontier markers**, scored not pass/fail but by *how far
they get* (🌑 won't launch → 🌒 quit → 🌓 menu → 🌔 one frame → 🌕 usable), measurable via the e2e
harness + heartbeat/HOT-PC/trace-ring/`SS_JIT_VERIFY`. Each gated row lists its unlock path (3D bridge,
virtual DV source, New-World ROM → D3). Names the 3D-acceleration bridge as the major unplanned
north-star and "how far Quake III gets per build" as its motivating proxy.

### [docs] Curated Mac OS 9 stress-test software catalog (downloadable, AltiVec-first)

Added `docs/MACOS9-STRESS-WORKLOADS.md` (cross-linked from `TESTING.md`): a linkable catalog of
demanding period-correct PowerPC Mac OS 9 software for stress-testing the JIT, prioritized by
AltiVec-relevance + emulation-feasibility, with download sources (Macintosh Garden / Repository /
archive.org) and per-title caveats. Web-researched, with myth-corrections carried: official
**SETI@home is NOT AltiVec**, **Photoshop 6.0 removed AltiVec** (5.5+AltiVecCore plug-in / 7 have it),
**stock POV-Ray Mac is NOT AltiVec**, **OpenGL games (Quake III) are non-starters** (no 3D HW) but
software-renderer games run, and **iMovie/FCP render-export works camera-free** (only capture needs
FireWire). Top zero-friction AltiVec picks: AltiVec Fractal Carbon (built-in scalar oracle),
SoundJam MP (AltiVec MP3 encode), POV-Ray 3.6 (scalar-FP). Ties to ROADMAP A2/A3/A5-V + B1.

### [docs] Fold in two external threads — JIT stress workloads + hot disk loading

Digested two user-shared sources and folded the actionable parts:
- **emaculation #7159** (the actual provenance of the "pin SheepShaver to one core" lore): specific
  titles crashed on **upstream** multicore (Royal Flush, The Dig, A-Train; Sandy Bridge, Mac OS
  7.5–9.0.4), single-core affinity "fixed" them. Added to `docs/TESTING.md` as documented
  emulator-breakers (concurrency + timing stress; Royal Flush also a timing/speed-calibration probe),
  and to `SheepShaver/docs/CONCURRENCY-MODEL.md` as the lore's provenance + an **empirical falsification
  test** of our "not multicore-sensitive" claim (run them on a busy multicore Apple Silicon host under
  SS_JIT_VERIFY; crashes = a real race to root-cause; the originals were the upstream x86 build).
- **Infinite Mac disk-streaming write-up** (persistent.info): folded a "Hot disk/CD insertion +
  software library" feature into `DESKTOP_INTEGRATION_PLAN.md` Tier 2. Validated that SheepShaver's
  `disk.cpp` already has runtime mount machinery (`DiskMountVolume`/`to_be_mounted`/`mount_mountable_volumes`)
  so hot insertion is feasible; Infinite Mac's runtime injection uses the **same `extfs.cpp`** we have +
  a Downloads/Uploads/Saved watched-folder convention, while its browser streaming (256 K chunks/service
  worker/IndexedDB) does **not** transfer to native (real files + APFS clonefile instead). Cross-linked
  from the Infinite Mac eval plan.

### [SheepShaver][e2e] Guest UI introspection — Plan 2a (dialog items) + 2b (control state) + harness integration

Backend A now emits each **dialog** window's DITL items — `button`/`checkbox`/`radio`/`staticText`/
`editText`/etc. with **globalized (VNC-clickable) rects**, titles, `enabled` (itemDisable), and the
`default` item — plus the window `refCon` + `defaultItem`. Python `uidump.py` gains `Item`,
`find_item`, and `click_item(vnc, snap, win, text="OK")` to click a named button. **Live-verified**
against the Speedometer choose-disk dialog (8 items, `OK`/`Cancel` titles, `refCon='sped'`, plausible
global coords). Review-driven Plan-1 hardening also landed: honest modality from the window *variant*
(not `windowKind`), `frontWindowIndex`=first-visible, junk-title sanity. E2E harness now boots with
`SS_UI_DUMP_DIR` and uses introspection in its gates — `run_lifecycle` logs the desktop window list,
and the benchmark's quit-to-Finder is now a definitive "Speedometer gone + Finder front" check
(heuristic kept as fallback). DX: `make ui-dump` CLI + `Snapshot.render()` ASCII layout,
`wait_for_window`, `click_point`. Canonical reference `SheepShaver/docs/UI-INTROSPECTION.md` updated;
action plan in `docs/archive/2026-06/planning/UI-INTROSPECTION-REVIEW-SYNTHESIS.md`. Plans: `…-p2a-dialog-items.md`.
**Plan 2b (control state):** control-type items also emit `value`, `hilite` (255 = dimmed/disabled),
and a self-checking `crect` (the `ControlRecord` rect, which must equal the item rect); text with
`^0`–`^3` is flagged `hasParams` (resolution deferred to Backend B). Python `Item.checked`/`.dimmed`/
`.has_params`. **Live-verified** on the choose-disk buttons: `crect_ok=True` on all controls, dimmed
buttons reported `hil=255`. Commits 81c3f9e6, e901beb2, 320a0eb6, 57a8b9af, 80491e28, 325501c2,
06b68fce, 39afb740, 4234bec0, 0299b22a, 31a3177c.

### [docs] Fold the "Developer Inspector" into SiliconSheep + correct DingusPPC license to GPL-3.0

- **Folded the Snow-inspired build/debug "chrome" recommendation into the SiliconSheep plan** as a
  tracked feature: `DESKTOP_INTEGRATION_PLAN.md` → "Developer Inspector / Debug Chrome". A live
  *observability inspector* (not a step-debugger) in the Tauri UI, fed by the **B1 profiler + existing
  diagnostics** (heartbeat/ring/HOT-PC/`SS_JIT_WATCH_ADDR`/window-title stats), riding the Tier-4
  Layer-A launcher↔emulator RPC. Cross-linked from the Snow plan (rationale), ROADMAP B1 (data layer),
  DingusPPC plan, and OPTIMIZATION-PLAN §P0. Unifies the three eval plans' debug-tooling crosswalks.
- **License re-validation (primary source): DingusPPC is GPL-3.0, NOT BSD-3-Clause.** Verified against
  the repo's root `LICENSE` (GPLv3 verbatim) and GitHub metadata (`spdx_id: GPL-3.0`). This **corrects
  the prior CHANGELOG/plan note (`9affe3a4`)** that said BSD-3-Clause — that was a bad scrape; the
  original plan's "similar GPL family" was right (same copyleft family as our GPL SheepShaver).

### [docs] Validate + sharpen the external-emulator evaluation plans (Snow / DingusPPC / Infinite Mac)

Web-verified the factual claims in the three eval plans (`b2bf2c0e`) and corrected them:
- **Snow** is **68K-only** (Mac 128K–II; 68000/020/030), **MIT**, Rust/egui, deliberately
  hardware-level — so its *emulation* lessons apply to **BasiliskII only, never SheepShaver's PPC**;
  only its *debugger/observability UX* transfers. Added a **"Build/debug presentation" recommendation**
  answering "should we adopt Snow-style chrome": build a **live observability *inspector*** (not a
  step-debugger) hosted in **Silicon Sheep (Tauri)**, fed by the **B1 profiler + existing diagnostics**
  (heartbeat / ring / HOT-PC / `SS_JIT_WATCH_ADDR` / window-title stats) — profiling-first, no
  emulator-core churn. Unify the three plans' separate debug-tooling crosswalks into one.
- **DingusPPC license corrected: BSD-3-Clause (permissive), not "GPL family"** — and flagged as the
  **most directly relevant** reference (PowerPC interpreter with real MMU + setjmp/longjmp exceptions);
  cross-linked to `MMU-NANOKERNEL-MP-PLAN.md`.
- **Infinite Mac** runs **our own SheepShaver lineage** (downstream WASM sibling) — best host-integration
  reference for Silicon Sheep; "includes Snow" is true-but-secondary; AppleTalk-over-Cloudflare-Durable-
  Objects zones, off by default; runtime media injection is a product feature not in repo docs.

### [SheepShaver][e2e] Guest UI introspection — Plan 1 walking skeleton (SS_UI_DUMP_DIR)

A read-only, env-gated host-side dump of the guest `WindowList`: front→back window list with
global (VNC-clickable) bounds, title (MacRoman→UTF-8), and dialog/active/visible flags, serviced
at the idle safe point and written as nonce-stamped JSON. Python consumer `sse2e/uidump.py`
provides the snapshot handshake and query/occlusion helpers. Live boot smoke verified: backend A,
screen 800×600, 2 windows (CD volume + Desktop), clean exit 0. Activate by setting
`SS_UI_DUMP_DIR` to a writable directory (feature is off when unset). Commits: `245d9fee`
(text transforms), `ee21de74` (shared guard), `7665d73a` (Backend-A walk + transport, hardened),
`13c0aa0c` + `07271080` (Python uidump consumer), `5d2bc4cf` (integration smoke). Spec:
`docs/superpowers/specs/2026-06-06-guest-ui-introspection-design.md`.

### [docs] Canonical concurrency-model reference — "SheepShaver is not multicore-sensitive, and why"

Added `SheepShaver/docs/CONCURRENCY-MODEL.md`: the citable, code-grounded answer to a recurring
question. States plainly that this fork runs the guest on one host thread (single logical guest CPU)
with synchronized host helpers (atomic `spcflags`/P0d, `atomic_or` `InterruptFlags`, mutex'd
framebuffer, signal-driven `SIGUSR2` interrupts) — so it is **not** multicore-sensitive and needs no
core-pinning (that folklore is upstream/older builds). Documents the one real caveat: the JIT cache
is single-writer *by construction, not by lock* (no locks on `jit_cache_wp`/`jit_bc_pool_next`/chain
pool; per-thread W^X), a property to preserve — adding a compiler thread needs R8→R9 first. Linked
from `docs/ARCHITECTURE.md` and the MULTICORE plan's concurrency baseline.

### [SheepShaver] LR-prediction icbi-safety fix + two stale-comment refreshes (code review)

A code-review pass (`72c5e525`) found one latent bug + two stale comments in `ppc-jit.cpp`:
- **icbi/SMC stale-translation bug:** the R1 LR-prediction fast-path emitted a raw `B chain_code`
  to the return block's chain entry **without `record_chain_site`**, so
  `ppc_jit_aarch64_invalidate_range` couldn't revert it — a RAM return-target that got SMC/icbi-
  invalidated left the predicting block branching into a stale translation (the historical icbi-hang
  class). Naive registration is unsafe (the B is mid-hit-path; the revert writes a lone LDP word →
  double epilogue). **Fix:** only direct-chain LR predictions to never-invalidated **ROM** targets;
  RAM returns fall back to the standard dispatcher (correct, unoptimized). Boot-verified (ISO→Finder,
  no hang), `make test-jit` 302/302. (= CROSS-EMULATOR-IDEATION finding #L, now ✅.) Restoring the
  RAM optimization via a registered revert-to-miss-path site is filed as a perf follow-up.
- **Stale comments:** the block-chaining header claimed "default OFF / flip to 1 to test" (it's ON
  + boot-verified — rewritten, with a do-not-disable note); the KNOWN-AltiVec-BUG (PARKED) block
  claimed "STILL BROKEN: vmuloub/vmuleub" (fixed long ago) — updated to current status + an explicit
  "approach A (global REV32 at load/store) REJECTED" guard so it isn't re-attempted.

### [docs] Fold external multicore analysis into the plans (+ code-verified concurrency baseline)

- Reconciled a web-sourced external analysis against our plans. It **independently converged** on the
  host-side-parallelism-not-guest-SMP thesis — validation for `MULTICORE-OFFLOAD-PLAN.md`. Folded in:
  - **Concurrency baseline (code-verified):** debunked the external "pin the JIT to one core" claim
    for this fork — the runtime is cleanly single-writer-guest (atomic spcflags, mutex'd framebuffer,
    signal-driven interrupts). But the JIT cache is single-writer-*by-assumption* (no locks on
    `jit_cache_wp`/`jit_bc_pool_next`/chain-site pool; per-thread `pthread_jit_write_protect_np`), so
    Tier-1 background compile has named hard prerequisites: **R8 dual-W^X → atomic cache allocation →
    cross-thread invalidation**. This makes the R8→R9 dependency mechanical, not aspirational.
  - **New Tier-2 device ideas:** **hardware cursor** (host overlay, decoupled from guest 60 Hz
    redraw — added as idea-bank grid #11) and the **"synthetic devices"** framing for the HLE layer.
  - **Deterministic event-queue corollary** for interrupt/device-completion (also the record/replay
    substrate).
- Cross-EMULATOR-IDEATION.md gets the external-corroboration note + grid #11; noted that the external
  "no-MMU ⇒ no 9.1+" line is common-wisdom — our New-World-ROM plan found parcels-ROM is the first wall.

### [SheepShaver] tools: jit-diff-sweep.py — differential op-sweep + auto-referee (instrumentation #11)

The unified-instrumentation payoff (ROADMAP A1 #11, Phase 0+2 in practical form): a committed,
reusable tool (`SheepShaver/tools/jit-diff-sweep.py`) that formalizes the throwaway sweep scripts +
by-hand `SS_TEST_HEX` triage that found the session's 27 codegen bugs. For each registered AltiVec/FP
op it injects one instruction (crafted lane-distinct, sign/saturation-crossing operands) through the
**real emulator twice** — interp (`SS_TEST_JIT=0`) vs JIT (`SS_TEST_JIT=1`) — and diffs the result
register. The interpreter IS the trusted oracle, so the tool is its own **auto-referee** (no second
pass): `interp == JIT` ⇒ correct; `!=` ⇒ a real divergence with a copy-paste repro.

- **Run-stamped JSONL output** under `$SS_RUN_DIR` (default `/tmp/macemu-runs/<ts>-sweep/` + a
  `latest` symlink), one record per op (`{schema,tool,kind,family,op,verdict,oracle,interp,jit,hex}`)
  + a `summary.json` — the #11 schema. FAIL records carry the exact `SS_TEST_HEX` repro.
- **Regression-tracks known-broken ops** (the pack family, A2) as `known-broken`, not `FAIL`, and
  **skips un-referee-able ops** (`fsqrt`/`fres`/`frsqrte`: interp lacks them / estimates).
- Lessons baked in (operands non-saturating so lane bugs can't hide; see
  `docs/planning/ALTIVEC-SHIFT-ROTATE-BUGS.md`). Current run: **pass=38, FAIL=0, known-broken=3**.
- **Remaining for full #11:** Phase 1 orchestrator (`make test-session` aggregating all oracles)
  and the in-emulator C++ JSONL emitters (so `SS_JIT_VERIFY`/heartbeat feed the same run dir) — this
  tool delivers the high-value referee piece standalone. Spec:
  `docs/superpowers/specs/2026-06-06-unified-test-session-instrumentation-design.md`.

### [SheepShaver] FP differential sweep — fctiw/fctiwz conversion bug fixed; rest of FP clean

Pivoted the proven sweep method to the under-tested FP ops (vs the real interpreter, with
edge-case operands: signed zero, NaN, inf, rounding boundaries). **`fsel`, `fnabs`, and the single
fused ops `fmsubs`/`fnmadds`/`fnmsubs` all passed** — clean. One real bug found:

**`fctiw`/`fctiwz` (double→int32) were both wrong** (`ppc-jit.cpp` case 14/15): both emitted the
*same* `FCVTZS Xd` (64-bit, toward-zero), so (a) `fctiw` ignored its FPSCR-rounding contract and
behaved identically to `fctiwz`; (b) overflow mis-saturated (2³¹ → `0x80000000` instead of
`0x7FFFFFFF`) because of the 64-bit-convert-then-truncate; (c) NaN → 0 instead of `0x80000000`.
Fix: 32-bit `FCVTAS Wd` for `fctiw` (round-nearest-ties-**away** = PPC `frin` = FPSCR default RN=0)
and 32-bit `FCVTZS Wd` for `fctiwz` — the 32-bit form gives PPC's INT32 overflow saturation for
free — plus an `FCMP`/`CSEL` NaN→`0x80000000` fixup. Verified interp==JIT across 2.5/3.5/−2.5/2³¹/
inf/NaN/±1 (`fctiw` now correctly rounds half-away 2.5→3, distinct from `fctiwz` 2.5→2). All NEON
capstone-verified. **5 committed vectors** (`fp_fctiw_round`/`_ovf`/`_nan`, `fp_fctiwz_trunc`/`_nan`);
`make test-jit` **302/302**. **Known limitation (documented in-code):** only the default FPSCR
RN=0 is honored for `fctiw`; non-default dynamic rounding modes aren't read yet (still a strict
improvement — the old code was wrong for *all* rounding). `fsqrt`/`fres`/`frsqrte` remain
un-sweepable (interp doesn't implement them / they're estimates).

### [SheepShaver] AltiVec saturating add/sub + signed averages fixed — 14 more bugs (broad sweep)

A broad differential sweep (every untested VX-form AltiVec op, JIT vs real interpreter, distinct
operands) + a sub-agent audit surfaced a second large bug cluster, all written without differential
validation (scrambled comments again). Fixed in `ppc-jit.cpp`, all NEON capstone-verified:

- **Saturating add** `vaddubs/uhs/uws` (512/576/640), `vaddsbs/shs/sws` (768/832/896): emitted
  **SABA/UABA** (absolute-difference-accumulate) — a completely different op — instead of
  **UQADD/SQADD**. Also signedness was transposed vs the canonical XO.
- **Saturating sub** `vsububs/uhs` (1536/1600), `vsubsbs/shs/sws` (1792/1856/1920): correct opcode
  but **signed/unsigned swapped** (unsigned XOs emitted signed `SQSUB`, and vice versa) → wrong
  clamp. Now `UQSUB`/`SQSUB` per the canonical op.
- **Signed averages** `vavgsb/sh/sw` (1282/1346/1410): emitted **SMAXP** (pairwise max!) instead of
  the signed rounding average **SRHADD**. (Unsigned `vavgu*` were already correct.)

Validated against the real interpreter across mid-range + saturation-boundary + signed-overflow
operands (42/42 agree). **14 strong committed vectors** with boundary-crossing operands (new
`load_bytes`/`satop` generator helpers — `load_pattern`'s linear slope made subtraction vacuous);
`make test-jit` **296/296** (was 282). Sweep also confirmed integer **compares** all correct, and
flagged the pack/pixel/sum families (structural rework, overlap ROADMAP A2) + FP-conversion scale
handling as remaining — see `docs/planning/ALTIVEC-SHIFT-ROTATE-BUGS.md`.

### [docs] Cross-emulator ideation capture + effort/payoff grid + R3 stale-note fix

- Added `docs/planning/sheepshaver-research/CROSS-EMULATOR-IDEATION.md`: a reality-checked capture
  of a 6-persona lateral-ideation workflow (Dolphin/RPCS3/Cemu = PowerPC; QEMU; Rosetta/FEX = same
  host; wildcard), 88 ideas distilled to an effort/payoff/blocked **grid**. Nominated first picks:
  `CopyBits` HLE (games/media, →Metal) and idle-skipping (battery/thermal/desktop citizenship);
  do-anyway: dual-W^X (R8). Records what's already shipped (chaining, AltiVec byte-mults) so it's
  not re-chased, defers the persistent-ROM-cache idea (re-file under Silicon Sheep snapshot/resume),
  and logs strategic anchors (TSO-for-SMP, record/replay time-travel, static AOT recomp).
- Wired: ROADMAP B4 + OPTIMIZATION-PLAN (HLE section) point to the idea bank; the two nominees are
  promoted to first-class tracked items **ROADMAP B5 (`CopyBits` HLE)** and **B6 (idle-skipping)**.
- **Fixed OPTIMIZATION-PLAN R3 stale note** — it claimed "no W^X toggling exists"; the code uses
  `MAP_JIT` + `pthread_jit_write_protect_np` (`jit-target-cache.hpp:34-39`), superseded by R8.
- Surfaced (for the JIT owner, not fixed here): two stale `ppc-jit.cpp` comments (chaining
  "default OFF"; AltiVec "STILL BROKEN: vmuloub") and a latent bug (unregistered LR-prediction
  `B chain_code` at ~:3052). Handoff prompt prepared.

### [SheepShaver] AltiVec shift/rotate family COMPLETE — halfword/word shifts + all rotates fixed

Finished the family started with the byte ops below. `ppc-jit.cpp` case 324/388 (`vsl{h,w}`),
580/644 (`vsr{h,w}` logical right), 836/900 (`vsra{h,w}` arith right), 4/68/132 (`vrl{b,h,w}`):

- **Halfword/word shifts:** same fix as the byte ops at `.8H`/`.4S` (mask mod 16/32, truncating
  `USHL`/`SSHL`, `+NEG` for right). Validated against byte-**asymmetric** lvx operands with amounts
  exceeding the element width — so the **ev_mixed byte order is empirically covered** (a non-issue
  for per-element shifts: `vslw` already passed asymmetric data pre-fix).
- **Rotates:** NEON has no vector rotate — synthesized `rol(x,k) = (x<<k)|(x>>>(w-k))` with
  `k=amt&(w-1)` (mask, copy, left `USHL`, `SUB` width, right `USHL` by the negative, `ORR`). The old
  code emitted a plain shift (dropped the wrapped bits) and didn't mask.

All capstone-verified. **9 more strong committed vectors** (`av_vsl{h,w}`/`av_vsr{h,w}`/`av_vsra{h,w}`
/`av_vrl{b,h,w}`); `make test-jit` **282/282** (was 273). The full 12-op variable shift/rotate
family is now correct and covered. `docs/planning/ALTIVEC-SHIFT-ROTATE-BUGS.md` marked ✅ ALL FIXED.

### [SheepShaver] AltiVec variable BYTE shifts fixed — vslb/vsrb/vsrab (3 confirmed bugs)

Fixed the first family of the AltiVec shift bugs found below — the **byte** ops, which are
byte-order-safe (each lane independent). `ppc-jit.cpp` case 260/516/772, capstone-verified NEON:

- **Mask the shift amount mod element width** (`DUP #7`→v2, `AND`) before the shift — AltiVec wraps
  the amount mod 8; the old code passed it raw so amount ≥ 8 shifted the bits out (`vslb` by 9 gave
  0 instead of `<<1`).
- **Use truncating, correctly-signed shifts**: `vslb`→`USHL`; `vsrb` (logical right)→`NEG`+`USHL`
  (was a signed *left* shift — wrong direction); `vsrab` (arith right)→`NEG`+`SSHL` (was `SRSHL`,
  which *rounds* — AltiVec truncates). The old code used rounding/signed variants throughout.

Validated: the `SS_TEST_HEX` repros now agree interp==JIT; **3 strong committed vectors added**
(`av_vslb`/`av_vsrb`/`av_vsrab`, distinct high-bit data + amounts 0..15 exercising the mask — 11–12
distinct result bytes, non-vacuous), `make test-jit` **273/273** (was 270). **Still open** (tracked,
`docs/planning/ALTIVEC-SHIFT-ROTATE-BUGS.md`): halfword/word variants (same fix, but the ev_mixed
byte order needs lvx-built byte-asymmetric validation) and the rotates `vrl{b,h,w}`.

### [SheepShaver] AltiVec shift/rotate codegen bugs found (hunt paid off) — repros recorded, fix pending (`c7b0c98c`)

Acting on the "AltiVec/FP is the highest residual-bug surface" strategy verdict, led a differential
hunt with the zero-coverage variable-shift family (where ARM64 NEON is *not* 1:1 with AltiVec).
Found **real, oracle-validated** codegen bugs in minutes — same lane/width-sensitive category that
produced the 9 `ev_mixed` bugs. Validated against the **real emulator** interp (`SS_TEST_HEX
SS_TEST_JIT={0,1}`), not the rom-harness subset interp:

- **Missing mod-element-width masking** — `vslb v2,v1,v3` with amount 9: interp `0x08` (9 mod 8 →
  <<1), JIT `0x00` (NEON `USHL` by 9 ≥ 8 shifts out). Affects all variable shifts/rotates at
  amount ≥ width.
- **Logical right shifts emit the wrong shift** — `vsrb` `0xF0>>1`: interp `0x78` (logical), JIT
  `0xe0` (shifted **left**!); `vsrh`/`vsrw` give arithmetic (sign-filled) results instead of
  logical. `vsra{b,h,w}` (arithmetic right) are correct.
- **Rotates `vrl{b,h,w}`** use a plain shift by inspection (lose wrapped bits) — suspected, needs an
  lvx-built repro.

The codegen's own inline comments are scrambled (`vsrb`↔`vsrab`), confirming this path was written
without differential validation. **Repros + fix plan: `docs/planning/ALTIVEC-SHIFT-ROTATE-BUGS.md`.**
Fix deferred to a focused, differentially-validated pass (add xfail repros → fix `ppc-jit.cpp`
~3320–3331/3446–3447 op-by-op → `make test-jit` 270 + boot smoke). ROADMAP A1.

### [docs] Unified test-session instrumentation design + planning reconciliation (`88c3311d`)

- **New design spec** `docs/superpowers/specs/2026-06-06-unified-test-session-instrumentation-design.md`:
  one `SS_RUN_DIR` run-stamp convention + a JSONL record schema (with an `oracle`-trust field) so the
  four correctness/perf oracles (test-jit, rom-harness, `SS_JIT_VERIFY`, E2E/bench) + diagnostics
  write to one analyzable place; a reconciliation analyzer **auto-referees disagreements via
  `SS_TEST_HEX`** (automating the manual 2026-06-06 triage) and **joins HOT-PC "hot" with microbench
  "slow"** for optimization leads. Phased (0: schema + 2 tools; 1: orchestrator; 2: auto-referee;
  3: perf join), building on the benchmark-export run-stamp pattern + `jit-analyze.py`.
- **Planning reconciled:** ROADMAP A1 gains the unified-instrumentation bullet and reflects the
  rom-harness arc (span gate + triage ✅, integer path clean) and AltiVec/FP promoted to 🔜;
  OPTIMIZATION-PLAN P0 cross-links the perf-join half of the spec; ROADMAP "Updated" → 2026-06-06.

### [docs] Multi-core offload plan ("multithreading light") (`4e8cc8f8`)

- Added `docs/planning/MULTICORE-OFFLOAD-PLAN.md`: what guest/emulator work can move to other host
  cores. Core finding: the guest is one logical cooperative CPU, so the high-ROI wins are
  emulator-*internal* parallelism (background JIT compile R9, dual W^X R8, Metal compositing R10,
  async devices) — not splitting guest execution; and games/large apps benefit more from getting the
  emulator's housekeeping *off* the hot core than from any guest SMP. Tiered 0–3 with
  effort/payoff/risk; "true guest SMP" (MP tasks on separate cores) is Tier 3, gated on the
  supervisor-fidelity plan's MP work + a cross-core coherence project (real `lwarx`/`stwcx.` +
  PPC→ARM64 barriers).
- Wired bidirectionally: ROADMAP B4 pointer; back-pointer from `MMU-NANOKERNEL-MP-PLAN.md` sub-plan C
  (Tier 3 is its multi-core extension).

### [docs] Supervisor-fidelity plan (MMU / nanokernel / MP) + New World ROM "second wall" (`36e45a74`)

- Added `docs/planning/MMU-NANOKERNEL-MP-PLAN.md`: specs the three privileged layers SheepShaver
  deliberately *stubs* (MMU faked V=P; nanokernel exception/interrupt model bypassed; preemptive
  MP tasks absent), grounded in the actual stub sites (`rom_patches.cpp` `patch_nanokernel*`,
  `main_unix.cpp` `sigill_handler`/`tick_func`, `ppc-execute.cpp` `sc`→illegal). Key finding: the
  dependency order is *not* MMU→nanokernel→MP — MP rides on nanokernel exception/decrementer
  fidelity, which mostly does **not** need real MMU translation (identity mapping suffices), so the
  cheapest path to MP/timing fidelity skips the MMU. Includes effort/payoff/risk per sub-plan and a
  cheap "stub-pressure" probe to run before committing.
- Linked it as the possible "second wall" behind `NEW-WORLD-ROM-SUPPORT-PLAN.md` (Phase 3 pointer),
  and added **ROADMAP D3** ("break the 9.0.4 ceiling") tying the two plans together. New World ROM
  support was previously unlinked from the roadmap; now it is.

### [SheepShaver] rom-harness — span gate cleans the differential signal (~46 → ~7–8 failures/seed) (`fe378c5d`, triage `980cf4df`)

Follow-up to the skip-not-abort fix: the harness's failures were dominated by a **block-model
mismatch**, not codegen bugs — the scanner ends a block at `bc` (opcode 16) but the JIT runs
*past* it (bc is not a JIT terminator), so the two compared different instruction spans from the
same start PC (the same root cause as `SS_JIT_VERIFY` fix-(i)).

- **Span gate (`rom-harness.cpp`):** compare only when `jblk.n_insns == blk.n_insns`; dropped
  blocks are reported as **`Span mismatch`** (visible, not a silent cap). On the OldWorld ROM
  (`--count=10000`, seeds 1/7/42) this cut failures from ~43–49/seed to **~7–8 span-matched,
  trustworthy failures** (≈710 bc-terminated blocks skipped). The remaining failures are now
  genuine span-matched divergences (PC/CR-dominated — branch-target/condition), no longer cascade
  artifacts from the span mismatch, so they're worth refereeing via `SS_TEST_HEX`.
- **Tactical hardening:** `alarm(0)` now cancels the per-block timeout on all exit paths (normal,
  SIGSEGV, fallback longjmp). In-code block-model notes at the compare site + `is_block_terminator`.
- **Survivor triage done — integer path differentially CLEAN, zero real JIT bugs.** Refereed 3
  distinct span-matched survivors against the real emulator (`SS_TEST_HEX`/`SS_TEST_INIT`,
  `SS_TEST_JIT={0,1}`): pure `bl`, `extsh r3,r7`+`bl` (harness claimed `jit` clobbered source r7),
  `mr;addi;li;bl` (harness claimed `jit` zeroed untouched r9/r11). In all three real-interp == JIT
  == correct (e.g. `extsh r3,r7` → r3=`0000197d`, r7 unchanged). Every survivor is a **harness
  artifact**: all `b`/`bl`-terminated, the harness's *JIT* run follows the branch at runtime (into
  real ROM at the relative target, which mutates registers) while its reference interp stops at the
  block end — a *runtime* branch-follow the *compile-time* span gate doesn't catch.
- **Open follow-ups (ROADMAP A1, both rom-harness polish, neither a JIT bug):** stop the harness JIT
  following the branch (disable chaining / snapshot at block exit); and/or recover the span-gate
  coverage by running the interp for the JIT's instruction count. Higher-priority residual-bug
  surface remains AltiVec/FP operand vectors in `test-jit`.

### [SheepShaver] rom-harness — skip-not-abort on fallback blocks (broad sweeps unblocked) (`c1a10c0a`)

The standalone differential rom-harness `abort()`ed the entire run the moment a JIT block that
the compiler marked `complete == true` hit the inline-interp fallback bridge (`ppc_jit_interp_one`)
at runtime — which happens whenever the linear ROM scan picks up a mis-scanned data region or an
op handled only via fallback. Under random seeds this killed the run on the *first* such block
(no Score line printed), making broad differential sweeps impossible.

- **Fix (harness-only, `rom-harness.cpp`, zero emulator/boot risk):** the fallback bridge now
  `siglongjmp`s back to a per-block guard (a shared `sigsetjmp` buffer, mirroring the existing
  SIGSEGV protection) instead of `abort()`ing. Such blocks are counted as **`JIT fallback`**
  skips and the sweep continues to completion. Verified across 3 seeds (1/7/42, `--count=10000`):
  all now finish cleanly (~110 fallback blocks gracefully skipped/seed, was: abort on block 1).
- **Honest characterization of the now-visible failures (root cause found via review — block-model
  mismatch, not codegen bugs):** ~43–49 differential failures/seed are surfaced, but the dominant
  cause is **structural, not a bug**. The scanner ends a block at the first terminator and counts a
  conditional branch (`bc`, opcode 16) as one — but the JIT does **not** treat `bc` as a block
  terminator, so it compiles/runs *past* it. The harness then compares a short interp run against a
  longer JIT run from the same start PC; registers diverge because the two ran **different
  instruction spans** (the exact analog of the `SS_JIT_VERIFY` fix-(i) block-exit problem).
  Verified: the flagged single-instruction `bc` block `42424642` "fails" only because the JIT ran 5
  instructions past it while the harness interp ran 1; the `bc` codegen is **correct** — a
  non-vacuous real-emulator test (`SS_TEST_HEX="38600002 7C6903A6 42424642"` = li/mtctr/bc) gives
  identical interp and JIT REGDUMP (CTR 2→1, branch to the correct AA=1 target). So this is a
  harness block-comparison artifact, **not** a JIT bug and **not** a harness-interp ISA bug. The
  GPR-diff "minority" is largely **cascade** from the same span mismatch in multi-insn blocks, not
  independent bugs. A trustworthy differential would compare only when `jblk.n_insns == blk.n_insns`
  (or run the interp for the JIT's instruction count) — tracked as a follow-up. Treat the absolute
  count as a noisy regression-delta upper bound, **not** an "N JIT bugs" figure. README
  "Interpreting Results" updated with the block-model caveat + the worked `bc` example.
- **Corroborates fix-(ii) must-fix (a):** the root cause (`complete` not distinguishing
  fallback-ending blocks) is the same `ends_in_fallback` signal gap noted in `ppc-jit.cpp:4792`
  and `docs/superpowers/specs/2026-06-05-verify-memory-snapshot-fix-ii-design.md`.

### [SheepShaver] SS_JIT_VERIFY oracle — X1 fix (i): replay mirrors the JIT single-block exit (`5ac5e676`, dedup `7314bc9c`)

The differential oracle's interp replay used "stop when pc leaves [start,end)", which
re-iterated intra-block loops, followed blr returns, and ran into post-bc dead code — the
structural false-positive classes 2/4/5 (OPTIMIZATION-PLAN §0b-extra4). Root cause (from the
compiler): a JIT block runs LINEARLY until the first conditional branch (`bc`, opcode 16 —
**both** arms `emit_epilogue_with_pc` to the dispatcher, taken or not) or an unconditional
terminator/taken branch; `bc` is not a terminator, so the compiler emits dead code past it
that inflates `n_insns`.

- **Replay now mirrors the JIT's single-block exit** (`ppc-cpu.cpp`): execute each insn, stop
  when PC left the sequential path (`pc != cur+4`) OR the insn was a `bc` (opcode 16). Plus a
  no-op-skip guard (`memcmp(jit_state, pre_state)` for the entry-spcflags-poll bail) and a
  one-time warning when `SS_JIT_NO_CHAIN=1` isn't set. **Boot-validated** (`SS_JIT_NO_CHAIN=1`):
  the 7 known control-structural blocks now verify **clean boot-wide** (ARTIFACT-PC count 0,
  was 7); design adversarially challenged by a sub-agent across two rounds (it caught the
  not-taken-`bc`/opcode-16 case).
- **Per-block report dedup**: a memory-RMW block (e.g. `100fd0e0`, a `lwz/addi/stw` counter)
  diverges every visit and previously consumed the whole report budget, blinding the oracle to
  the rest of boot. Now each distinct block is reported once. The oracle now sees past it; the
  newly surfaced divergences are **all memory-contamination, no codegen bug**: SUSPECT
  `10106b50` (`lhz/addi/sth`), `1011e734` (stw to a just-loaded pointer slot), `1018b04c`
  (`lwzx/stwx`), plus memory-dependent-`bc` ARTIFACT-PC blocks.
- **Remaining**: only the memory-RMW confound (class 6) is left — the replay reads guest memory
  `fn()` already wrote. Fix (ii) (memory snapshot/restore, verify-gated) is the next step; it
  also doubles as the proof of "no real bug" (if it cleans them all, they were artifacts).
- **Verify sweep (2026-06-05, `SS_JIT_VERIFY=1 SS_JIT_NO_CHAIN=1`, HD boot) — honestly scoped:**
  with fix (i) + dedup, the oracle reports **exactly four `SUSPECT` blocks**
  (`100fd0e0`/`10106b50`/`1011e734`/`1018b04c`) and four `ARTIFACT-PC` blocks. **All four
  `SUSPECT`s are class-6 memory-RMW**: three *encoding-certified* (load+store same base reg &
  offset — `0(r8)`, `10(r31)`, `4(r3)`), and `1018b04c` most-likely (indexed `[r3+r4]`, `stwx`
  base reloaded → contingent; value-chain confirms artifact, broken-`lwzx` hypothesis refuted —
  the JIT's own load returns nonzero). **No real codegen bug in the covered region — but NOT
  whole-boot:** under verify the guest stalled in an early-boot spin ~10 s in (`comp=10826` frozen
  10 s→3 min, only 8 distinct block PCs ever verified ≈0.07 % of compiled blocks, never reached
  Finder — the documented timer-starvation trap; "nothing new after" = nothing executed). **Deeper
  differential coverage is cheaper offline** (`make test-jit`, `rom-harness --passes/--seed/--count`,
  no timer dependency) — preferred over more boot sweeps. Fix (ii) re-framed: **DEFER pending
  greenlight** — the tool that would *certify* the artifacts + unblock deeper boot-time verify,
  not mere polish. Ready-to-implement design with the two landing-blocker must-fixes
  (`ends_in_fallback` bit; 16384-entry journal) and the register-only caveat:
  `docs/superpowers/specs/2026-06-05-verify-memory-snapshot-fix-ii-design.md`.

- **P1a substantially strengthened — RA eviction path validated (targeted surfaces).** The
  >8-live-GPR `ra_evict` path (never hit by the harness's small vectors) is validated on
  **oracle-independent** evidence: the eviction battery (below) passes the harness JIT-vs-interp
  REGDUMP (separate clean executions, no replay confounds), and a full chaining-on boot reaches
  Finder with eviction firing continuously. **P8 (cross-block pinning) unblocked.** Calibration:
  targeted harness coverage is strong for mid-block, pure-register, single-exit, Rc=0 eviction;
  two surfaces have functional (boot) coverage but no targeted vector yet — eviction at
  control-flow exits/terminators, and eviction × deferred CR0/XER state under pressure (next
  cheap no-boot increment). Detail + residual: OPTIMIZATION-PLAN §P1a.
- **Eviction harness battery broadened (264 -> 270 vectors).** Six pure-register vectors
  widen `ra_evict` coverage beyond the single `lmw_stmw_wide` load/store-multiple case, each
  using 16 distinct GPRs > `RA_NUM_REGS=8` (capstone-verified encodings, no memory/chaining/loop
  confound). Mid-block, pure-register: `evict_wb16` (dirty spill+reload), `evict_rd_eq_ra`
  (ra_load-before-ra_store ordering under pressure), `evict_mixed_alu` (add/subf/and/or/xor).
  Edge surfaces: `evict_branch_exit` (a conditional `bc` **terminates the block** — verified
  `blocks=2` — exercising `ra_flush_all` on a branch exit, not just `blr`), `evict_rc1_cr0`
  (Rc=1 `add.` → `emit_update_cr0` under pressure, CR captured), `evict_adde_carry` (`addic.`+
  `adde` chain → XER carry under pressure). All pass `make test-jit` (270/270, score=100); each
  confirmed fully JIT-compiled (not an interp fallback) with hand-checked REGDUMPs. The
  confound-free, no-boot way to harden P1a -- the right instrument vs. the verify oracle. Not
  exhaustive by design (interp-fallback/spcflags/FP-VR/bclr-bail edges remain boot-only).
- **Mapped the `SS_JIT_VERIFY` differential oracle's 6 false-positive classes** across three
  whole-boot sweeps (chaining-on; `SS_JIT_NO_CHAIN=1`; raised-budget ~92k lines). Every reported
  divergence is structural, **zero real codegen bugs**: block chaining, blr/bclr returns,
  intra-block loops, mid-block conditional paths, PC bookkeeping, and **memory-RMW** (the replay
  restores registers but not guest memory, so a `lwz;addi;stw` counter shows a PC-matching,
  step-by-2 divergence that fools a naive PC-match filter — `100fd0e0`). A literal whole-boot
  clean verify is **unachievable** here (low budget → oracle goes dark mid-boot; high budget →
  verify-every-block starves the guest timer into an early-boot ROM spin). Full taxonomy + the
  X1 fix plan (replay mirrors JIT path/terminator; snapshot+restore memory; targeted/sampled
  verify): OPTIMIZATION-PLAN §0b-extra4, ROADMAP A1.
- **Diagnostic knobs added** (`ppc-jit.cpp`, `ppc-cpu.cpp`): `SS_JIT_NO_CHAIN=1` now logs a
  startup marker so a no-chain run self-confirms; new `SS_JIT_VERIFY_BUDGET=N` overrides the
  divergence report budget (default 20) for whole-boot coverage. The earlier "exactly one blr
  residual" baseline was itself a latch artifact (§0b-extra5) — superseded by the taxonomy above.
- **`SS_JIT_VERIFY` made triagable + targetable (partial X1, tooling).** Each divergence is now
  classified inline — **`[VERIFY] SUSPECT`** (interp/JIT agree on exit PC ⇒ same control flow ⇒
  real-bug candidate) vs **`[VERIFY] ARTIFACT-PC`** (exit PC differs ⇒ structural confound, not a
  codegen bug) — so a whole-boot log triages to `grep '[VERIFY] SUSPECT'` instead of a wall of
  noise (memory-RMW still lands in SUSPECT; disambiguate by value stepping by 2, not 1). New
  `SS_JIT_VERIFY_PC=LO:HI` scopes the oracle to one PC range to re-check a suspect block without
  the whole-boot slowdown. These make the *existing* confounded oracle usable; the replay-mirror
  + memory-snapshot fixes (to make it *clean*) remain the X1 follow-on. **Boot-validated against
  ground truth (2026-06-05):** two scoped `SS_JIT_NO_CHAIN=1 SS_JIT_VERIFY=1` boots tagged the 7
  known structural blocks `ARTIFACT-PC` (20/20) and the lone memory-RMW block `100fd0e0` `SUSPECT`
  (200/200, `GPR10` confirmed stepping by 2 — correct codegen, oracle double-apply); `SS_JIT_VERIFY_PC`
  scoping confirmed working. Harness unaffected (270/270).

### [SheepShaver] Guest OS version detection via SysVersion low-memory global (`ce725b75`)

- The emulator's idle hook now reads the guest's `SysVersion` ($015A) — a BCD-packed
  OS version set by Mac OS during boot. Emits `[SYSV] osVersion=0x0860 (8.6.0)` to
  stderr on first detection. Verified firing on Mac OS 8.6 (E2E ISO) and Mac OS 9.0
  (benchmark disk). This is a generic guest-state introspection hook, reusable for
  other runtime queries.

### [shared] SavePrefs now preserves comments, blank lines, and ordering (`99b9b720`)

- Previously `SavePrefs()` dumped the in-memory prefs list, stripping all `#` comments
  and user ordering. New `SavePrefsToStreamMerging()` reads the original file, passes
  through comments/blanks, replaces known-key lines with current values, appends new
  keys. Write-to-temp + rename avoids the truncation race. Verified: comments in
  `~/.sheepshaver_prefs` survive a full E2E boot+shutdown cycle.

### [docs] SiliconSheep — full feature set shipped and documented

SiliconSheep Tauri v2 launcher progressed from scaffold to functional app:

- **First-run wizard**: 4-screen flow (Welcome → ROM picker with SHA-256 verification →
  Disk creation + optional CD → Review & Boot). ROM validation accepts compressed/trimmed
  ROMs (512K–8MB), shows "proceed anyway" for unrecognized files.
- **VM library**: Card grid with live VNC screenshots (captured every ~10s while running,
  final frame on shutdown), OS version (from `[SYSV]` hook), last-booted date.
- **Complete settings panel**: 7 sections (General, Display, Storage, Network, Input,
  Advanced with Expert fold-out, Debug). All 30+ SheepShaver prefs configurable.
  Custom resolution support (any width×height). Explainer text on all expert settings.
- **Debug panel**: GUI controls for JIT diagnostics — SS_JIT_VERIFY, SS_JIT_NO_CHAIN,
  SS_JIT_NO_ROM, SS_USE_JIT, SS_JIT_TRACE_RING, watch addresses, skip opcodes. Env vars
  passed to the emulator child process on launch.
- **Log retention**: Timestamped logs in `<vm>.sheepvm/logs/`, last 10 kept, symlink to
  latest, "View Logs" button in Debug section.
- **Drag-and-drop**: ROM files, disk images, CD images, prefs files — drop on the window,
  routed to the right context (wizard step or import).
- **Import from prefs**: "Import Prefs" button or drag-drop a `sheepshaver_prefs` file to
  create a VM from an existing config.
- **VM duplicate**: APFS `clonefile` for instant zero-copy, warns on shared external disks.
- **Disk backup**: APFS clonefile snapshot of disk images (available when VM stopped).
- **19 Rust tests** (10 prefs parser + 9 VM integration). TypeScript strict mode. Vite 8 +
  TypeScript 6. Zero compiler warnings.
- **Deps**: Vite 8.0, TypeScript 6.0, all Tauri packages at latest (2.11.x).
- **Renamed** "Silicon Sheep" → "SiliconSheep" (one word, like SheepShaver).

### [SheepShaver][BasiliskII] JIT harness & diagnostic integrity hardening (`d52d4103`, `8d101b2e`, `c5cc8b0d`)

Tooling-only (no runtime/codegen change); hardens the signals used to judge the JIT:

- **[SheepShaver] SS_JIT_VERIFY suppression latch fixed.** The differential interp-vs-JIT
  oracle used a `bool verify_suppressed` that, once set on the first divergence, gated its
  own clearing branch — so verify went silent for the rest of the run after one report.
  Replaced with a `verify_suppress_blocks` countdown decremented only on in-range (RAM)
  blocks (execution is overwhelmingly ROM at `0x50xxxxxx`, so a per-block decrement would
  drain it before any RAM block is skipped). Checking now auto-resumes. Tunable with a
  symptom guide in the code comment. (`ppc-cpu.cpp`)
- **[SheepShaver] rom-harness bench no longer blanket-masks stderr.** `make bench` shows
  the JIT engine chatter by default; masking is opt-in via `make bench BENCH_QUIET=1`. The
  bench's own warnings/errors (baseline-stale, init-fail, compile-fail) are routed to
  **stdout** so they survive even under `BENCH_QUIET=1` — the previous unconditional
  `2>/dev/null` silently hid them, including the advertised "baseline OLDER than
  ppc-jit.cpp" warning.
- **[BasiliskII][SheepShaver] preflight vacuousness guard (shallow tier).** Both
  `jit-test/run.sh` preflights now reject a vector whose body is entirely NOPs (`4E71` /
  PPC `60000000`) — it exercises only decode/dispatch and asserts nothing under the
  differential. Intentional decode/dispatch sanity vectors (`nop`, `nop_triplet`) are
  allow-listed. Verified: rejects synthetic all-NOP vectors, zero false positives across
  the real tables (B2 452, SS 264). The **deep** tier (real-opcode body whose result hides
  in an FPR/VR/memory the REGDUMP can't see) remains deferred to the `gen-*-vectors.py`
  generators / a future sentinel-mutation redesign.

### [SheepShaver] E2E harness — first-run DX: shared entry scaffolding, GUI-session + configure preflight (`3ab8bd59`)

From a developer-advocate review of the onboarding cliff:

- **New `sse2e/harness.py`** — shared entry-point scaffolding so each run script (and any new
  scenario) is a thin "check preconditions, run the drive fn": `hard_exit(code)` (the os._exit that
  stops a stuck vncdotool reactor from hanging the process — previously duplicated verbatim with a
  long comment in each script) and `check_preconditions(need_iso/need_disk)` (build + GUI + assets,
  failing fast with actionable messages). `run_smoke.py`/`run_benchmark.py` now use it.
- **GUI-session fast-fail** (`runner.gui_session_ok`, `launchctl managername == "Aqua"`,
  conservative) — a headless/SSH run now fails immediately with "no GUI session" instead of waiting
  out the 90s boot timeout on a cryptic disk error. Surfaced in `make e2e-setup` too.
- **`configure`-not-run detection** (`runner.is_configured`) — `make build-ss` fails on an
  un-configured tree; the doctor + a tracked README **"Building the emulator"** section now carry
  the one-time `configure` incantation (it previously lived only in gitignored `CLAUDE.md`,
  invisible to a fresh clone). README also gains an env-var reference table. +8 unit tests.

### [SheepShaver] E2E harness — trustworthy gates: FakeRunner tests, settle-based gate, honest PASS (`daaa1965`)

Hardens the *integrity* of the benchmark's pass/fail signal (this session exposed runs printing
PASS when the automation hadn't actually worked unattended):

- **Drive/gate logic is now unit-tested offline.** `scenario.py`'s save/quit sequence was
  extracted into testable helpers (`_save_text_report`, `_quit_to_finder`, `_await_front_app`);
  new `tests/test_scenario.py` exercises them with a **`FakeRunner`** (scripted `[APP]` log lines)
  + **`FakeVnc`** (records keys, reacts to input) + a fake clock — 14 deterministic tests, no
  emulator/boot. The most error-prone part of the harness finally has coverage.
- **`back-to-finder` gate hardened against the noisy `frontApp` signal.** It used to fire on a
  single spurious `'Finder'` frame (false confidence). `_await_front_app` now requires Speedometer
  to be ABSENT across a window of consecutive `[APP]` frames (and Finder present) — it only
  succeeds once Speedometer has really quit. Unit-tested against the exact false positive.
- **Honest benchmark PASS.** A green `make e2e-bench` now requires the guest's real clean-shutdown
  signatures (`saw_clean_shutdown`: "Shutdown complete." + the atexit session line), not just a 0
  exit code — so PASS means the harness genuinely drove an unattended shutdown. 67 unit tests pass.

### [SheepShaver] E2E benchmark history export (`cad1af88`, SS_E2E_RUNS `0dc302cf`)

`make e2e-bench` now saves Speedometer's text report in-guest (Cmd-T → Return, accepting
the default name "Power Macintosh Report"), extracts it host-side via hfsutils (no extra
boot — reads the unmounted run-copy image directly, MacRoman-decoded, hfsutils state
isolated via a throwaway HOME), and archives each run under gitignored
`SheepShaver/e2e/artifacts/benchmark-history/<timestamp>/` (raw `report.txt` + `scores.csv`
+ result PNG) plus an append-only `history.csv`, printing the CPU/Graphics/Disk/Math delta
vs the previous run. Collect+report only — a save/extract failure never flips a PASS to FAIL.
The benchmark now **shuts down unattended**: since the Power-key hook only fires at the
Finder (not over a frontmost app), the harness quits Speedometer keyboard-only (Cmd-Q →
Return through the save dialogs) to the Finder first — keyboard is reliable. (VNC *clicks*
work fine too — verified on both SDL2 and SDL3; an earlier "clicks don't register" was a
misdiagnosis, see LEARNINGS.)
**Less-noisy measurement:** `SS_E2E_RUNS=N` runs N times (each in its own subprocess — vncdotool's
Twisted reactor can't restart in-process) and prints a batch summary — the **median** per metric
+ each metric's **CV%** (run-to-run noise), flagging >5%. CV% is host-state dependent (quiet host:
CPU/Math <1%, Disk noisy; under load: everything noisy) — the honest "is this batch trustworthy?"
signal. `PR`/PowerRating is **deliberately not trended** (disk-weighted composite → inherits
Disk's noise, misleads as a perf number; also panel-only). New `sse2e/bench_export.py` (+24 unit
tests); throwaway run-copies are now cleaned up; `make e2e-setup` gains an optional hfsutils
check. Design + plan:
`docs/superpowers/specs/2026-06-05-benchmark-result-export-design.md`,
`docs/superpowers/plans/2026-06-05-benchmark-result-export.md`.

### [docs] BasiliskII → SheepShaver JIT cross-pollination triage (`33ffbea6`)

- Added `docs/planning/sheepshaver-research/BASILISKII-CROSS-POLLINATION.md`: which techniques
  from the BasiliskII 68K JIT (the more mature, same-host ARM64 lineage) are worth borrowing for
  the SheepShaver PPC JIT, filtered by whether they come over clean or drag the heavy
  lazy-state / multi-PC / lifecycle machinery in. Top borrows: verify/bisection tooling (X1,
  Track A), intra-block backward CR0-liveness as the safety proof for re-enabling lazy CR0 (X2,
  §0g), and the light §R2 inline-cache form of guarded indirect-branch resolution for `bcctr`
  (X3, §P2/§P9) — explicitly *not* the heavy edge-profiling/jmpdep apparatus.
- Wired into the trackers (bidirectional, per the doc-lifecycle rule): ROADMAP A1/B2/Track-B
  intro, and back-pointers in OPTIMIZATION-PLAN §0g/§P2/§R2.
- Includes a **fast-target-hardware re-ranking** (two independent sub-agent reviews): on Apple
  Silicon the emulator already runs the 1990s guest faster than period hardware, so the four
  non-throughput borrows (X1 tooling, X4 latency, X5 clarity, X6 methodology) top the list and
  the two throughput borrows (X2 lazy-CR0, X3 bcctr) defer — also premature by our own rule
  (gated on the unbuilt B1 execution-weighted profiler). Re-ranked order: X1 → X6 → X4 → X5.

### [shared] Repository tidy-up — relocate the BasiliskII harness, remove the superseded VNC-QA scaffold, document `cxmon/` (`22710b39`)

- **Moved `jit-test/` → `BasiliskII/jit-test/`.** The root `jit-test/` was the *BasiliskII*
  68K opcode harness (distinct from `SheepShaver/jit-test/`, the PPC one — they only shared a
  name). Relocating it gives clean per-emulator symmetry. Fixed the scripts' internal relative
  paths (`run.sh`: `../BasiliskII/src/Unix` → `../src/Unix`; `rom-harness.sh`:
  `$DIR/BasiliskII/src/Unix` → `$DIR/src/Unix`) and repointed `Makefile` (`make test-jit`),
  `autoresearch.sh`, `JIT-STATUS.md`, and `docs/planning/BasiliskII-next-phase-plan.md`.
  SheepShaver-context `jit-test/` references (run from `SheepShaver/`) are unchanged.
- **Removed the repo-level VNC/Gherkin QA scaffold** (`qa/` and `BasiliskII/qa/`). It was an
  Xvfb/Linux-oriented story-runner experiment, superseded by the macOS VNC E2E harness at
  `SheepShaver/e2e/` (isolated prefs + pristine disk, `[BOOT]`/`[READY]` signals). `BasiliskII/qa/`
  depended on root `qa/tests/vnc/`, so the two were one system and went together; BasiliskII also
  doesn't build on macOS. Repointed the surviving references (`JIT-STATUS.md`, both
  `*/docs/AARCH64_JIT_GOLDEN_WORKLOADS.md`, `BasiliskII/docs/AARCH64_JIT_BRINGUP.md`,
  `docs/planning/sheepshaver-research/COMPATIBILITY-TESTING-PLAN.md`) at `SheepShaver/e2e/`.
- **Documented `cxmon/`** with `cxmon/README.macemu.md`: it's vendored upstream source (cxmon 3.2,
  the optional `mon` debugger), pinned at the repo root because all three `configure.ac` files
  hard-code `../../../cxmon/src` for `--with-mon`; currently compiled **off** (`config.h` →
  `/* #undef ENABLE_MON */`); frozen upstream (last change 2017) — keep as-is, don't relocate.

### [SheepShaver] SDL3 is now the genuinely-built default backend (+ the bugs that surfaced)

- **The build had been silently linking SDL2.** `configure.ac` defaults to SDL3, but the *generated*
  `configure` was stale (generated before the SDL3-default flip), so every build linked SDL2 — the
  E2E harness had been validating SDL2 the whole time. Re-bootstrapped (`NO_CONFIGURE=1 ./autogen.sh`)
  so the build actually links SDL3 (`otool -L` confirms `libSDL3`). See LEARNINGS / spec §17.
- **SDL3 boot regression fixed.** The live-JIT-stats title-bar update called `SDL_SetWindowTitle`
  from the redraw thread; on macOS that Cocoa call is main-thread-only and stalled the redraw thread
  → VBL stops → guest hangs in early boot (commit `5d87d713`, the "Live JIT stats" entry below).
- **SDL3 shutdown crash fixed.** `Quit()` calls `VideoExit()` twice (directly + via `ExitAll()`);
  `VideoExit()` destroyed `frame_buffer_lock`/`sdl_palette_lock`/`sdl_events_lock` without NULLing
  them, so the 2nd pass double-destroyed freed mutexes → `os_unfair_lock is corrupt` abort (SDL2
  tolerated it; SDL3's os_unfair_lock-backed mutexes don't). Fix: NULL after `SDL_DestroyMutex`
  (`video_sdl3.cpp`, commit `3daa9c98`). Diagnosed from a user-captured crash backtrace (the earlier
  Metal-deadlock theory was wrong — host VBL degradation had masked the real bug). Spec §18.
- **VNC server ported to SDL3** (`vnc_server.cpp`, `video_sdl3.cpp`, commit `01bc52fe`). It was
  SDL2-only (`#if SDL2 && !SDL3`; the SDL3 branch was empty stubs), so on SDL3 the benchmark +
  headless screenshots failed (VNC `ConnectionRefused`). Guard widened to
  `#if SDL_VERSION_ATLEAST(2,0,0)`; SDL3 adaptations for the renamed keymod/condition-variable
  symbols, `SDL_EVENT_KEY_*`/float mouse coords, and `SDL_GetPixelFormatDetails`; the 4 call sites
  wired into `video_sdl3.cpp` mirroring SDL2. `make e2e-bench` PASS on SDL3 (port 5950,
  pixel-correct capture). **SDL3 now has full harness parity.** Spec §20.

### [SheepShaver] E2E harness — signal-gated benchmark, richer signals, robustness fixes

- **Harness no longer hangs after PASS** (commit `f55dd7f6`). vncdotool starts a non-daemon Twisted
  reactor on connect; a swallowed screenshot-connect failure skipped `close()`, leaving the reactor
  to block interpreter exit (print PASS, then hang / lingering Python). Fix: entry points
  `flush` + `os._exit(code)`; the boot screenshot always `api.shutdown()` in a `finally`.
- **Richer `[APP]`/`[BOOT]` idle signals** carry the front-window pointer (`win=`) and **title**
  (`title=`); garbage frames (background-extension pseudo-windows under cooperative MT) are
  suppressed, cutting the benchmark's signal lines from hundreds to ~32 and making the log a readable
  state story. Guest reads are sanitized (a literal `'` → backtick so it can't break the
  `frontApp='…'`/`title='…'` parsers) and bounds-checked (a wild titleHandle no longer SIGSEGVs the
  host). (`emul_op.cpp`.)
- **Benchmark drive steps are signal-gated** (`_await_since`/`_drive_until`) instead of fixed sleeps:
  each step proceeds the instant the guest reaches the next window state, prints its elapsed time
  (`[gate] step: Xs`), and self-corrects (resends a key the splash silently dropped — one continuous
  watch, no cursor race). Done-detection gates on the `All Done!` alert title (unambiguous), not a
  modal count. (`observe.py`, `scenario.py`; commits `8972c0f3`, `b170415d`.)
- **`[READY]` settled-desktop signal** — emitted once the Finder has been frontmost + non-modal for a
  ~2 s dwell (more robust than the first `[BOOT] idle`, which can fire mid-draw); carries `MBarHeight`
  for evaluation. A better "desktop actually usable" marker. (`emul_op.cpp`, `observe.saw_desktop_ready`.)
- **Pre-flight disk-availability check** (`runner.preflight`): `make e2e`/`e2e-bench` now kill **and
  reap** stray SheepShaver instances (bare `pkill` returns before the OS releases file handles) and
  refuse to launch if the boot image is still held open — so a stray session can no longer make a run
  boot to the "?" no-boot-disk icon; it fails fast with a clear message instead.

### [docs] SiliconSheep plan — Tier 4 Automation & Scripting

- Added a **Tier 4 — Automation & Scripting** section to `docs/planning/DESKTOP_INTEGRATION_PLAN.md`:
  a launcher control surface (`siliconsheep` CLI / AppleScript / Shortcuts / MCP server / headless
  CI mode), a screenshot-free guest control bridge (structured input + observation + an AppleEvents
  bridge), and a "SiliconSheep Tools" guest agent for *managed* images that reframes part of the
  Infeasible list. Grounded in prior art (UTM scripting, Lume's HTTP+MCP control, Tart, VirtualBuddy)
  with sources inline for future agents. Feeds the E2E harness (ROADMAP A5 / A5-V).
- **Built out with a per-layer integration design** (parallel codebase investigation): `file:line`
  anchors, RPC method additions, phased build orders + effort/risk per layer, and a "Shared
  architecture" synthesis (one wire protocol, one broker, one idle-hook command mailbox on the emul
  thread; the three execution contexts). Surfaced a premise correction — the bidirectional launcher
  RPC does **not** exist yet (emulator is an outbound-only RPC client; Tauri launcher stops VMs via
  SIGUSR1), so making that channel bidirectional is Layer A's true first task.

### [SheepShaver] E2E harness — host→guest shutdown hook + ISO medium + Speedometer benchmark (A5)

- **Host→guest shutdown hook.** `SIGUSR1` → the emulator's idle hook injects the ADB Power key
  (with dwell) → waits for the Shut Down dialog → Return (confirms) → real OS shutdown → clean
  exit. Replaces VNC menu-clicking; verified on Mac OS 8.6 and 9.0.4. (`emul_op.cpp`,
  `runner.request_shutdown()`.)
- **Read-only ISO is the default medium** — can't get dirty (no repair-prompt/dialog
  false-positives, no pristine-copy), reproducible. Disk-boot retained (`SS_E2E_MEDIUM=disk`,
  instant APFS clonefile copy) for the benchmark.
- **`make e2e-bench` — Speedometer benchmark automation.** Boots a small stripped Mac OS 9.0.4 +
  Speedometer disk, drives the full suite over VNC (gated on a new `[APP] frontApp` change signal
  so it doesn't race the variable app launch), captures the results image (PR/CPU) + emulator log,
  and shuts down via the hook. Verified PASS (PR 29.375).
- **Benchmark-finished hook (no fixed sleep).** The idle hook's `[APP]` signal now also fires on a
  front-window **modal change**, so Speedometer's "tests are done!" dialog (modal 0→1) is a
  deterministic finish signal. `run_benchmark` waits for that dialog instead of a fixed 105 s sleep
  and reports the measured suite duration (a coarse perf signal). App-change `[APP]` emits are
  debounced to ~0.5 s (CurApName churns ~6/s among background extensions); modal changes are always
  emitted. (`emul_op.cpp`, `observe.is_app_dialog()`, `scenario.run_benchmark`.) Score *parsing*
  (Speedometer text export) is the next step.
- **Fixes found building it:** `Vnc.close()` calls `api.shutdown()` (vncdotool's reactor otherwise
  hung every capture ~2 min); host-FS (`extfs`) mount disabled in the test prefs; verbose `pytest`.
- Run guide: `SheepShaver/e2e/README.md`; design: `docs/superpowers/specs/2026-06-04-e2e-vnc-harness-design.md`
  (§12–§15).

### [SheepShaver] Live JIT stats in SDL window title bar — periodic update reverted (boot-breaking on macOS)

- New `ppc_jit_aarch64_get_stats()` API exposes block count, pool size, cache used/total;
  `set_window_name()` gained an optional `status_suffix`. **The periodic title update in
  `do_video_refresh()` was removed** (both SDL2 and SDL3 backends): `do_video_refresh()` runs on
  the "Redraw Thread", and `SDL_SetWindowTitle` calls into Cocoa, which asserts *"NSWindow … should
  only be modified on the main thread!"* — aborting the emulator (SDL2) or stalling the redraw
  thread so the 60 Hz VBL stops and the guest hangs in early boot (SDL2-default build; `SS_JIT_VERIFY`
  run reproduced the abort). The getter + param are retained for a future reimplementation that
  applies the title on the **main thread** (e.g. a pending-title mailbox drained by the main loop).

### [docs] SiliconSheep — Tauri v2 launcher scaffolded + plan expanded

- **Framework pivot:** Desktop integration plan (`DESKTOP_INTEGRATION_PLAN.md`) revised from
  Cocoa/ObjC to **Tauri v2** (Rust + pnpm + TypeScript). Rationale: cross-platform door stays
  open, CLI-buildable (no Xcode.app needed), modern web UI for free.
- **Tauri scaffold:** `SiliconSheep/` at repo root — Rust backend with VM profile CRUD
  (`vm.rs`), Tauri command registrations (`main.rs`), web frontend with VM card grid UI
  (`main.ts`, `styles.css`), dark/light mode. Both `cargo check` and `vite build` pass.
- **Feature roadmap expanded** with competitive teardown (Parallels, VMware, UTM, Infinite Mac,
  DOSBox-X), detailed UX flows (4-screen first-run wizard, VM library cards, settings sidebar,
  error states, coach marks), Tauri sidecar architecture (IPC via existing `rpc_unix.cpp` UDS),
  APFS `clonefile` for instant VM duplication, and an explicit "Infeasible" tier.
- **Host-Guest Channels reference** (`HOST-GUEST-CHANNELS.md`): all existing host↔guest
  interaction channels, readable guest OS structures (WindowList, CurApName, MenuList,
  ScrnBase), achievable new channels, and hard limits.
- **ROADMAP Track C** updated from "⏸ Researched" to "🟡 Active — scaffolded".

## 2026-06-04

### [docs] Doc hygiene sweep — stale-claim fixes, /workspace paths, archive (`61306fa5`)

- **Corrected misleading status claims.** SheepShaver `AARCH64_JIT_GOLDEN_WORKLOADS.md`
  Workload 3 said the JIT only "reaches the Mac OS Welcome splash" — updated to the verified
  reality (boots Mac OS 8.6 to Finder with the full native JIT, 2026-06-04), plus the
  Speedometer/blocker rows. Added Linux-lineage caveats to the BasiliskII
  `AARCH64_JIT_GOLDEN_WORKLOADS.md` and `AARCH64_JIT_BRINGUP.md` (their `301/301` figures are
  Linux history; B2 does not build on macOS arm64).
- **Fixed stale `/workspace/projects/macemu` paths** in both GOLDEN_WORKLOADS docs → `<repo>`.
- **Archived three one-off artifacts** into `docs/archive/` (with an index):
  `0x50467E00-CODEGEN-BUG-ANALYSIS.md`, `PR-SUBFE-FIX-DRAFT.md`, `JIT-OPCODE-TABLE.md` — their
  conclusions live in LEARNINGS/CHANGELOG / `make harness-count`.
- Added `docs/superpowers/README.md` namespace index; fixed OPTIMIZATION-PLAN reference links
  to point at local copies (`PERFORMANCE_AUDIT.md`, `JIT-FPU-PLAN.md`).

### [docs] Documentation lifecycle convention + status/provenance headers (`e7b444c9`)

- **New `CONTRIBUTING.md` → "Documentation Lifecycle" section**: how to keep docs honest when
  work lands — log to `CHANGELOG.md` (component-tagged), flip the item's status marker instead
  of deleting it, bump the doc's `Updated:` date, and fold-and-retire (not silently delete)
  dated specs.
- **Standardized header on all 43 docs under `docs/planning/`**: a `Status / Created / Updated /
  Why this doc exists` block plus the canonical marker legend (✅ done · 🟡 in progress ·
  ⏸ blocked/deferred · ☐ todo). Created dates recovered from git history (following renames);
  existing rich intros (ROADMAP, NEW-WORLD plan) harmonized, not clobbered.

### [SheepShaver] AltiVec even/odd byte multiplies fix — ev_mixed class complete (vmuloub/vmuleub) (`ed1fb0bd`)

- **Bug fix.** `vmuloub`/`vmuleub` (odd/even unsigned byte multiply → halfword products) had two
  bugs: they emitted a **non-widening `MUL.8B`** (must widen 8×8→16) and ignored the `ev_mixed`
  even/odd element selection. Fix (`emit_vmul_byte`): `REV32.16B` normalize → `UZP1`(even)/
  `UZP2`(odd)`.16B` select into the low 8 lanes → `UMULL.8H` widen → `REV32.8H` to ev_mixed
  halfword storage. Test vectors strengthened to distinct operands (vA=00..0F, vB=10..1F), which
  exercise **both** bugs at once — even-lane products like 0x0A×0x1A=260 exceed 255, so a
  non-widening op truncates visibly. `xfail→xpass`, promoted (**262→264, score=100**). **This
  empties the AltiVec ev_mixed quarantine lane** — the whole element-order class (splats, merges,
  pack, byte multiplies) is now fixed and scored.
- **Also corrected the scrambled multiply XO→op comment labels** (e.g. case 520 was labelled
  `vmulesb` but is `vmuleub`). Remaining, untested (no test vector, flagged in code + ROADMAP A2):
  halfword multiplies `vmul*h`, word pack `vpkuwum`, signed byte multiplies `vmulosb`/`vmulesb`
  (the latter share `emit_vmul_byte` with `SMULL` — emitted as *prospective*).
- Boot-verified (`SS_JIT_VERIFY`, Mac OS 8.6 clean exit): zero VR/FPR divergence for the pack +
  multiplies (only the documented `blr`-boundary GPR false positive). The whole ev_mixed class is
  now boot-confirmed.

### [SheepShaver] AltiVec vpkuhum pack fix (ev_mixed) (`d43313ac`)

- **Bug fix.** `vpkuhum` (pack 8+8 halfwords to their low bytes, modulo) was doubly wrong: it
  **ignored vA entirely** (loaded only vB) and used the wrong NEON op. PPC keeps each halfword's
  low byte = PPC byte 2i+1 = the odd byte lane in natural order, so on `REV32.16B`-normalized
  inputs that is **`UZP2.16B`** (odd-lane deinterleave; vA → result high half). Reuses the merge
  family's `emit_vmrg` helper (`REV32.16B` → permute → `REV32.16B`). Verified `xfail→xpass` with
  distinct operands, promoted to the scored gate (**261→262, score=100**). Sibling `vpkuwum`
  (word→halfword modulo pack, case 78) has the same ignore-vA bug — flagged in code + ROADMAP A2,
  not yet vectored. Remaining in the `ev_mixed` class: the even/odd byte multiplies
  (`vmuleub`/`vmuloub`).

### [SheepShaver] AltiVec byte/halfword merge fix (vmrgh/l b,h) + distinct-operand test strengthening (`023870cd`)

- **Bug fix — completes the merge family.** Following the word-merge fix below, the byte and
  halfword merges (`vmrghb`/`vmrglb`/`vmrghh`/`vmrglh`) needed more than a correct ZIP
  encoding: the VR is stored `ev_mixed` (bytes reversed *within* each 32-bit word), which at
  the byte level is exactly `REV32.16B` relative to natural PPC element order. New
  `emit_vmrg()` helper normalizes both inputs with `REV32.16B`, merges with
  `ZIP1`/`ZIP2.{16B,8H}` (PPC element 0 = MSB = NEON's lowest lane after the rev, so PPC "high"
  merge = ZIP1 / "low" merge = ZIP2), then `REV32.16B` back before the store. Word merges keep
  the plain `ZIP.4S` — `word_element` is the identity under `ev_mixed`, so they need no rev.
- **Test strengthening (closes a masking gap).** All six merge vectors moved from self-operand
  (`v1,v1`) to **distinct operands** (vA=`00..0F`, vB=`10..1F`), so a wrong ZIP1↔ZIP2 or A↔B
  swap can no longer pass coincidentally; the full 128-bit result is diffed via the REGDUMP VR
  line. The four byte/halfword merges flipped `xfail→xpass` and were promoted to the scored
  gate (**257→261, score=100**); the word merges stayed green under distinct operands —
  retroactively proving that fix was not coincidental.
- **Boot-verified** under `SS_JIT_VERIFY` (Mac OS 8.6, 235K blocks, 98.7% JIT coverage): **zero
  VR/FPR divergence**. The single GPR/LR/PC divergence is the documented `blr`-block-boundary
  false positive (LEARNINGS, "SS_JIT_VERIFY false positives"), not a codegen bug.
- **Remaining in the `ev_mixed` class:** `vpkuhum` (halfword→byte pack) and the even/odd byte
  multiplies `vmuleub`/`vmuloub` (also need `UMULL.8H`, not `MUL.8B`) — still quarantined
  (xfail). See `docs/planning/ROADMAP.md` A2.

### [SheepShaver] AltiVec word-merge codegen fix (vmrghw/vmrglw) (`b1bb6b52`)

- **Bug fix.** The JIT's AltiVec merge cases (`vmrgh*/vmrgl*`) emitted `0x..C400`/
  `0x..C800` — bit 15 set makes those three-same *arithmetic* encodings, not the
  ZIP1/ZIP2 permutes the inline comments claimed. Merges silently produced wrong
  results. Corrected all six cases to the real ZIP1/ZIP2 `{16B,8H,4S}` encodings.
  The word-granular `vmrghw`/`vmrglw` are now fully correct (word order is preserved
  under the interpreter's `ev_mixed` VR layout, so `ZIP.4S` needs no byte remap):
  verified by the differential harness (xfail→xpass) and an `SS_JIT_VERIFY` boot
  (zero VR divergence), then promoted from the quarantine lane to the scored gate
  (255→257, score=100). The byte/halfword merges, packs, and even/odd multiplies
  remain quarantined (`ev_mixed` byte-within-word reordering — see `ROADMAP` A2 and
  the in-code note above `case 12` in `ppc-jit.cpp`). The harness gained a
  quarantine lane (xfail/xpass, not scored) so known-diverging vectors are tracked
  as regressions-in-waiting rather than silently dropped.

### [SheepShaver] Wayland detection (upstream backport) (`f5a96e0e`)

- **Wayland detection without GTK** (backport of kanjitalk755/macemu `91d58b12`, Dave
  Vasilevsky): `init_sdl()` previously forced `SDL_VIDEODRIVER=x11` only under
  `#if REAL_ADDRESSING && defined(GDK_WINDOWING_WAYLAND)`, so a `--without-gtk`
  SDL build never got the XWayland workaround that avoids a Wayland mmap/fixed-
  low-address-mapping crash. The guard is now `#if REAL_ADDRESSING &&
  defined(__linux__)` plus a runtime `getenv("WAYLAND_DISPLAY")` check, so the
  workaround applies in non-GTK SDL builds. **Inert on macOS**: the block is
  gated on `defined(__linux__)`, which is never defined on Darwin (only
  `__APPLE__`/`__MACH__`), so it compiles out entirely on the macOS arm64 build
  (`REAL_ADDRESSING` is not defined here regardless — see `docs/UPSTREAM-LINEAGE-SYNC.md`
  §6.1). Brought in for a future
  Linux/Wayland target; runtime Wayland behavior is not verifiable on macOS.
  Harness unaffected: `make test-jit` 257/257, score=100.

### [shared] Networking (`3bfa680a`)

- **VDE virtual networking** (backport of kanjitalk755/macemu `06d8bc02`): SheepShaver can now use
  a VDE switch for Ethernet. The destination VDE link is configured directly in the
  `ether` pref via a new `vde:` prefix (e.g.
  `--ether 'vde:cmd://ssh root@server vde_plug tap://tap0'`), so it persists with the
  rest of the prefs. Two correctness fixes in the shared `ether_unix.cpp` send path:
  outgoing packets now send the actual frame length (was `sizeof(packet)`, which
  appended trailing garbage), and the infinite `do {} while (len < 0)` send-retry was
  replaced with a proper `excessCollsns` error return. SheepShaver's `configure` gains
  `--with-vdeplug` (default yes) and an `AC_CHECK_LIB(vdeplug, vde_close)` probe that
  defines `HAVE_LIBVDEPLUG` and links `-lvdeplug` when the library is present (Homebrew
  `vde`, header `libvdeplug.h`). The bare `vde` ether pref (no destination) still works.
  Boot/packet-flow on real hardware is unverified by this change.

### [SheepShaver] Video backend (`dda61521`)

- **SDL3 is now the default video backend** (was SDL2). `configure` selects SDL 3.x
  when no `--with-sdlN` flag is given; pass `--with-sdl2` to opt back to SDL 2.x.
  Requires the `sdl3` pkg-config module (Homebrew `sdl3`, tested with 3.4.10). The
  SheepShaver binary now links `libSDL3.0.dylib`.
- Picked up kanjitalk755/macemu `e596e215` ("SDL3: blit not required in `SDL_UnlockTexture()`"):
  the SDL3 texture is unified to `ARGB8888` and the big-endian→host swap is done in
  software (`__builtin_bswap32`) inside the `SDL_LockTexture`/`UnlockTexture` copy,
  removing the `SDL_GetMasksForPixelFormat` round-trip.
- macOS SDL3 build fixes (this backend had never been compiled on the fork before):
  `video_sdl3.cpp` used `dynamic_cast` (needs RTTI, but the build uses `-fno-rtti`) →
  changed to `static_cast` to match `video_sdl2.cpp`; the three macOS Objective-C++
  files (`prefs_macosx.mm`, `VMSettingsController.mm`, shared `utils_macosx.mm`) used
  a raw `#include <SDL.h>` that does not resolve under SDL3's `sdl3/SDL.h` layout →
  switched to the version-aware `my_sdl.h` shim.

> **Status:** SDL3 is **boot-verified** — Mac OS 8.6 boots to the Finder desktop on the
> SDL3-default build (2026-06-04). The JIT harness validates codegen, not video, so this
> confirmation is the boot test, not the harness. If you hit display problems on a future
> build, `--with-sdl2` falls back to the SDL2 backend.

### [BasiliskII] Build

- **macOS arm64 build (partial; does not yet build).** The configure host-routing was
  fixed (Apple Silicon `arm-apple-darwin` → AArch64, not ARM32; `c71100d0`) and Linux-only
  code in `main_unix.cpp` guarded (`2f967f4b`), but the B2 AArch64 JIT backend
  (`compemu_support_arm.cpp`) is still unported — **BasiliskII does not yet build on macOS
  arm64.** Full detail, remaining errors, and the pick-up plan are in
  **`docs/planning/BasiliskII-MACOS-AARCH64-JIT-PORT.md`**.

### [SheepShaver] JIT correctness

- **AltiVec `vsel` fix**: `vsel` (vector select) emitted ARM64 `BSL` with its two
  source operands swapped — it computed `(vC & vA) | (vB & ~vC)` instead of PPC's
  `(vB & vC) | (vA & ~vC)`, returning vA wherever the mask bit was set. This would
  silently corrupt any AltiVec software that uses `vsel` (the emulator advertises a
  G4, so AltiVec is live). One-token operand swap; caught and regression-tested by
  a new differential vector. (`086226f3`)

- **AltiVec `vspltb`/`vsplth` element-order fix (ev_mixed)**: byte/halfword splats
  selected the WRONG element. VRs are stored in the interpreter's `ev_mixed` byte
  order (`byte_element(i)=(i&~3)+(3-(i&3))` — bytes reversed *within* each 32-bit
  word, word order preserved), but `emit_load_vr` loads it raw via `LDR Q`, so the
  JIT indexed NEON lanes with the raw PPC element. Fixed by remapping the DUP index
  through `ev_mixed` (verified across indices 0/3/15 and 0/3/7); `vspltw`/`vsldoi`/
  element-symmetric ops were already correct. The same mismatch still affects
  `vmrg*`/`vpk*`/even-odd multiplies — parked and signposted in code (`emit_load_vr`)
  and tracked as **P1b** in OPTIMIZATION-PLAN.md (two fix approaches documented).
  Repro vectors in `jit-test/gen-altivec-vectors.py`. (`9117e789`)

- **`emit_update_cr0` cleanup (B1)**: CR0 field construction reduced from 19 to
  11 ARM64 instructions.  Replaced 3x `emit_load_imm32` + 3x CSEL + LSL + AND +
  ORR with 3x CSET + 3x shifted ADD + BFI.  Every Rc=1 instruction benefits. (`fd616e5b`)

- **LogicalImm encoder (B2)**: ARM64 bitmask-immediate encoding for AND masks in
  rlwinm, rlwimi, rlwnm, andi., andis.  Saves 1-2 instructions per masked op
  (992/1024 PPC masks are encodable).  Encoder from `ppc-logical-imm.hpp`. (`c71cd0f2`)

- **mullwo overflow detection (A3)**: Case label was 715 (wrong XO), should be
  747.  The instruction was never JIT-compiled — silently fell to interpreter.
  Now uses SMULL + ASR/CMP to detect 32-bit overflow and sets XER OV/SO. (`c71cd0f2`)

- **SS_JIT_VERIFY cascade fix**: Reduced false divergences from 20+ to 1 per
  boot.  Skips verifying blocks ending with link-setting branches (bl/bctrl),
  and suppresses cascade after any divergence until a clean block is found.
  Mixed Mode Manager dispatch causes unavoidable interpreter/JIT path divergence
  that is not a codegen bug. (`0cda0257`)

- **Software link stack (R1, partial)**: Compile-time infrastructure for
  Dolphin/RPCS3-style blr return prediction added.  Finding: `bl` always
  terminates a block, so the compile-time stack is empty by the callee's `blr`
  — the fast-path never fires.  Runtime variant (R1b) documented in the
  optimization plan as a follow-up. (`e256c871`)

### Code Quality

- **Technique attribution**: Added source credits (Dolphin, RPCS3, MAME,
  upstream PERFORMANCE_AUDIT) to all major JIT optimizations as inline code
  comments — RA, CR0 cleanup, LogicalImm, ADCS carry, mullwo, atomic spcflags,
  link stack. (`2f2347cd`)

### [SheepShaver] Testing & benchmarking

- **End-to-end VNC test harness (P1, ROADMAP A5)**: `make e2e` boots an isolated copy of
  Mac OS 8.6 in a checked-in config (not `~/.sheepshaver_prefs`), waits for a deterministic
  boot-ready signal, drives the Finder's Special ▸ Shut Down over VNC, and asserts a clean
  exit — the first *system-level* regression gate, complementing `make test-jit`/`SS_JIT_VERIFY`.
  A new one-shot `[BOOT] idle frontApp='Finder' modal=0` signal is emitted from the guest idle
  hook (`OP_IDLE_TIME`/`OP_IDLE_TIME_2` = the `SynchIdleTime` patch), enriched with `CurApName`
  (0x910) + a front-window modal check (0x9d6) so the harness distinguishes "idle at the desktop"
  from "idle blocked on a modal dialog". Python + `vncdotool`, env-resolved asset paths for CI,
  pristine-disk-per-run isolation, 12 offline unit tests. A host→guest shutdown *hook*
  (`ShutDwnPower` trap / ADB power-key) was explored and reverted — see spec §12. Requires a
  logged-in macOS GUI session (SDL needs a WindowServer; no Xvfb equivalent). `SheepShaver/e2e/`. (`acd37114`)

- **18 real FP-arithmetic test vectors**: the pre-existing `fp_*` vectors were
  *vacuous* — they ended at `stfd` and never loaded the result into a GPR, but the
  harness REGDUMP captures GPRs, not FPRs, so a wrong FP result was invisible and
  the JIT-vs-interpreter diff passed trivially. FP arithmetic was effectively
  untested. The new vectors load the result back into a GPR (fadd/fsub/fmul/fdiv,
  the fma family, frsp/fctiwz/fneg/fabs/fmr, and single-precision forms). Generated
  by `jit-test/gen-fp-vectors.py` (documented, reproducible). The 9 vacuous `fp_*`
  originals were then removed (kept `fp_lfd_stfd`/`fp_lfs_stfs` — those *do* round-
  trip the value back into a GPR, so they are real load/store tests). (`c1e26ee2`)

- **AltiVec coverage rebuilt (corrected over several review rounds)**: an initial
  15-vector batch (14 vacuous — VX-form doubled-XO no-ops) *and* all 12 pre-existing
  `vec_*` vectors were found vacuous (results never reached a checked GPR). Replaced
  with correctly-encoded, verified-non-vacuous vectors via the documented
  `jit-test/gen-altivec-vectors.py` (VX-form XO is unshifted; operands must be
  distinct per-lane to avoid the masking trap). The `vspltb`/`vsplth` probe that
  exposed the ev_mixed divergence was **fixed** (see JIT correctness above), not
  merely flagged; the still-broken `vmrg*`/`vpk*`/multiply ops have repro vectors
  parked in the generator's bug set.

- **Harness integrity preflight**: the SheepShaver `jit-test/run.sh` had no
  self-validation (unlike BasiliskII's). Added a preflight that aborts on a
  malformed/missing/duplicate-name vector before the run. It immediately caught
  three real pre-existing **duplicate vector names** (`crand_basic`/`mcrf_basic`/
  `orc_basic`) where the second `T_` definition shadowed the first, so one vector of
  each pair never ran (silent lost coverage); fixed by renaming the shadowed ones.
  Vacuousness itself is not caught (a vacuous vector still touches scratch GPRs) —
  the `gen-*-vectors.py` generators are the practical defense there. (`f49df948`)

- **`make harness-count`**: single source of truth for the harness vector count,
  derived from `jit-test/run.sh` (the count had drifted across several docs). The
  *gate* references in the testing docs (TESTING.md, OPTIMIZATION-PLAN.md,
  CLAUDE.md, CONTRIBUTING.md) were de-hardcoded to reference it; dated historical
  snapshots in session logs and baseline tables are intentionally left as-is. (`797a9242`)

- **FP microbench kernels**: `make bench` gains `fp-add`/`fp-fma`. They measure the
  FPR store/load round-trip (the JIT has no FP register allocator), not raw FP-unit
  latency — useful as the baseline an FP register allocator would improve against. (`64258de7`)

### [docs] Documentation

- **Paranoia FP conformance**: concrete manual run steps documented in TESTING.md,
  with the honest caveat that automation needs a guest binary + boot. (`6ab3f194`)

- **IMPROVEMENT-CYCLE-1.md**: prioritized, collision-aware improvement plan from a
  multi-agent audit (read-only auditors → adversarial verification → synthesis). (`4053d111`)

## 2026-06-03

### Emulator Features

- **Clean shutdown**: Special > Shut Down now exits the host process cleanly,
  running all cleanup handlers (JIT miss report, cache free, SDL teardown).
  Previously the process hung with a black screen after the guest powered off.

- **Restart**: Deferred — requires deeper ROM reset state management. Special >
  Restart still has no effect (same as upstream).

- **Prefs: K/M/G suffixes**: Integer prefs now accept human-readable sizes
  (e.g., `ramsize 256M`). Also supports `0x` hex prefix via `strtol` base 0.
  Shared by both BasiliskII and SheepShaver (symlink).

- **Prefs: comments**: `#` and `;` line comments were already supported by the
  parser but undocumented. Now noted in CLAUDE.md and prefs examples.

- **Networking**: `ether slirp` provides outbound NAT networking (web, FTP)
  with built-in DHCP. No host configuration required.

- **JIT code cache sizing**: Default increased from 64 MB to 256 MB, eliminating
  recompilation churn during boot and app launch (previously 2+ full flushes per
  session).  Configurable via `jitcachesize` pref or `SS_JIT_CACHE_KB` env var.

- **Startup prefs readout**: SheepShaver now prints each loaded pref value on
  startup, showing what configuration is active.

### JIT Performance

- **Register allocator (RA)**: Re-enabled and fully converted. All 32-bit GPR
  accesses go through `ra_load`/`ra_store`, operating directly on ARM64
  x21-x28 registers — no MOV bounce through temporaries. Benchmark Mix
  improved **+15.6%** (548 → 634), Dhrystones **+9.4%**, CPU **+2.2%**.

- **TBZ bclr optimization**: Mixed Mode guard on function returns uses single
  `TBZ` instruction instead of `AND` + `CBZ` (1 instruction saved per `bclr`).

- **Atomic spcflags (P0d)**: Replaced spinlock-based spcflags with
  `std::atomic`, eliminating lock contention between the 60 Hz VBL timer
  and the JIT dispatch loop. CPU score 65.2 (new high).

- **Trailing MOV elimination (P0f)**: Carry, overflow, and immediate-carry ops
  (subfc, addc, addco, subfco, addo, subfo, nego, addze, subfze, addic, addic.,
  subfic) now compute ADDS/SUBS directly into the RA destination register,
  eliminating a redundant MOV per instruction.  Mix 638 (new high).

- **RA eviction test**: New `lmw_stmw_wide` harness vector exercises 12 live
  GPRs, forcing mid-block RA eviction. Harness now 236/236.

- **JIT miss report via atexit**: The opcode coverage histogram now prints
  reliably on process exit (via `atexit` hook), regardless of how the guest
  shuts down. Includes session wall-clock time.

- **JIT coverage**: 98.4% of compiled instructions run natively. The remaining
  1.6% (opc=19: bcctr/isync) accounts for 98.5% of all misses — making native
  bcctr the single highest-impact remaining optimization.

### JIT Correctness

- **RA flush on bclr bail path**: Added `ra_flush_all()` before the Mixed Mode
  bail epilogue — prevents dirty cached GPR values from being lost when bclr
  bails to the interpreter mid-block.

- **RA ordering constraint**: `ra_load` for source operands must precede
  `ra_store` for the destination. Otherwise `ra_store` allocates without
  loading the old value, and a subsequent `ra_load` for the same GPR (when
  rD == rA) returns uninitialized data.

- **gpr64 coherence warning**: Documented that `emit_load_gpr64` /
  `emit_store_gpr64` bypass the RA cache for the low word. Safe today (PPC64
  ops unreachable from 32-bit guests), must be fixed before G5 support.

### Testing & Benchmarking

- **`make test-jit`**: New target that runs the harness in JIT equivalence mode
  (`SS_HARNESS_MODE=jit`), actually testing JIT codegen against the interpreter.
  The old `make test-opcodes` only tested interpreter determinism.

- **`make bench` (jit-bench)**: Microbenchmark for fast A/B testing of codegen
  changes — reports ns/insn for targeted kernels (carry-chain, rc1/CR0, ALU).
  Supports `--save-baseline` / `--compare` for differential timing. No boot needed.

- **TESTING.md maintenance contract**: Freshness rules, per-change checklist,
  harness mode awareness. Prevents test/doc rot.

### Debug Output

- **Disk driver**: Suppressed per-poll DiskStatus flood for csDriverGestaltCode
  (status 43). Replaced two-line output with single-line format showing the
  four-char gestalt selector: `csDriverGestaltCode 43: 'flus'`.

### Documentation

- **OPTIMIZATION-PLAN.md**: Updated with completed items (RA, TBZ), new
  entries (lazy CR0, trailing-MOV elimination, cross-block register pinning,
  indirect bclr chaining), post-RA benchmark baseline, and miss-data-driven
  reprioritization of P2 (native bcctr).

- **USER-HANDBOOK.md**: New user guide covering prefs reference, networking
  setup, environment variables, benchmarking, and troubleshooting.

- **CHANGELOG.md**: This file — tracks user-visible changes per session.
