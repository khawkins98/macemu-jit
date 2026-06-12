# M3b Wave 2 — the EE/interrupt-delivery chain, verification-first: prove the chain under the current armed state, then wire the OpenPIC, then restore the tick

> **STATUS: CLOSED / SUPERSEDED (2026-06-12).** W2-0…W2-3 shipped from this plan;
> **W2-4's remainder — including the reserved riser/published flip — shipped via the M7
> interrupt-injection milestone** (`2026-06-12-interrupt-injection.md`, arc
> `153c088b`…`81d60cc1`), per its rev-2 B1 supersession table. Final per-item
> dispositions: the table in this plan's W2-4 section below. Current state: the
> SS_NW_EE_RISER + SS_NW_DEC_PUBLISHED + SS_NW_HOST_IRQ cluster is the **newworld
> DEFAULT** (`81d60cc1`); EXT delivery + guest IACK live; consumption (Ticks) is the
> named next task (slot-4 round trip, slot5-recon `b3e51b8d`). Body text below is
> historical — claims like "EE has never risen" describe the 2026-06-11 state.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps
> use checkbox (`- [ ]`) syntax for tracking. **Dispatch ONE task at a time — W2-0 edits
> `sheepshaver_glue.cpp`/`ppc-execute.cpp`/`exc_core.*`, W2-3/W2-4 edit
> `sheepshaver_glue.cpp`/`main_unix.cpp`/`rom_patches.cpp`; the concurrent FE1F
> service-surface milestone ALSO edits `sheepshaver_glue.cpp` + `rom_patches.cpp`
> (its plan's own same-files rule). NOTHING here parallelizes against FE1F tasks that
> touch those files — the coordinator serializes (see "Coordination constraint" below).**

**Goal:** Re-score #2's named likeliest next M3-class surprise is the EE/interrupt-delivery
chain: EE has never risen on the newworld boot path, and everything downstream of the NK
handler entry — the from-emulator handler arm (R-9), the guest tick dispatch, the entire
EXC_EXTERNAL/PIC side — is unexercised guest code or simply absent
(`EE-CHAIN-RECON.md` §A: links 1/7/11 MISSING, 3/6 PARTIAL, 8 UNOWNED, 9/10
NEVER-EXECUTED/UNTESTED). The recon's §B verdict: **EE first rises at the NK's first `rfi`
carrying an EE=1 SRR1 image — likeliest during the now-unlocked post-syscall MPLibrary/NK
service phase, with the EE bit originating at the 0x50313bf8 `ori r11,r11,0x8000` force.
The pending DEC latch makes the first rise an INSTANT delivery, onto the never-executed
R-9 handler arm, with a starved tick path behind it.** This milestone adopts the recon's
recommendation verbatim: fold the EE chain into Wave 2, REORDERED VERIFICATION-FIRST —
prove the chain at unit level (W2-0), at harness level (W2-1), and live under the current
armed state in one bounded probe session (W2-2) BEFORE the first construction (W2-3
OpenPIC wiring + EXC_EXTERNAL) and the evidence-gated retirement work (W2-4 tick
restoration). Better to fire the surprise on purpose in a bounded session than mid-boot
during M4/M5. Paravirtual byte-identical throughout.

**Architecture:** No new delivery mechanism — the M3a `deliver_pending_dec_exception`
hook (glue:785), the exc_core transition math, and the EE-edge re-raises
(ppc-execute.cpp:1336ff/:1639ff) are the chain; this milestone (a) extracts the gating
COMPOSITION into the pure module so it is law-testable (W2-0), (b) gives the SS_TEST_HEX
harness the three missing knobs so real mtmsr/rfi edges drive real deliveries without
boots (W2-1), (c) spends one sanctioned probe session confirming-or-falsifying the §B
verdict and pinning the original W2.0 recon contracts (W2-2), (d) registers the
already-landed OpenPIC model (`b86449c9`, 206 checks) on the M1 bus and wires its output
as a SECOND, level-held pending source beside the DEC latch — env-gated `SS_NW_PIC`,
flip-last, revert-on-red (W2-3), and (e) retires the tm_task/via_int suppressors so a
delivered interrupt finally becomes a guest-visible tick (W2-4, evidence-gated, with an
explicit out to spin off as its own milestone).

---

## Authoritative inputs

| Doc | What it fixes |
|---|---|
| `docs/planning/machine/EE-CHAIN-RECON.md` (THE seed, `89fd0642` + resolved housekeeping note) | §A the 11-link chain map + verdicts (this plan's scope IS closing its gaps); §B the EE-rise verdict (first rise = instant delivery onto the R-9 arm; the `SS_M6A_USER_MSR` lever, the ruled-out candidates); §C.1 the unit spec — `ExcDeliveryDecision`/`ExcEdgeReRaise` signatures + the U1–U12 case table VERBATIM; §C.2 the harness spec — the 3 missing knobs + H1–H5 vectors + the capture-stub + the test-exc-vectors lane + the honest depth-deferral exclusion; §C.3 the probe plan P1/P2/P3/P4 incl. the READY/BROKEN signature tables; the W2-0..W2-4 ladder + per-rung sizes (adopted as this plan's structure) |
| `SheepShaver/src/include/dev_openpic.h` + `src/machine/dev_openpic.cpp` (`b86449c9`) | The as-landed model W2-3 wires: `OpenPICBindOutput`/`OpenPICOutputAsserted` output seam (transitions only); region 0xF3040000+0x40000 (header); CTPR reset 15 (= nothing deliverable until the guest lowers it — dev_openpic.cpp:199); Q8 inputs VIA=0x19/ESCCB=0x24/ESCCA=0x25; the LE-register-file warning (header endianness note: QEMU maps KeyLargo little-endian; the module speaks NATURAL values — byte-lane mapping is THIS plan's wiring decision); `OpenPICFormatFirstIACKs` (the Q8 tripwire); the review minor owed: `write_ctpr` (dev_openpic.cpp:314–325) recomputes output by a different (equivalent-by-analysis) path than QEMU's — a comment is owed, folded into W2-3 |
| `docs/superpowers/plans/2026-06-11-machine-layer-m3b.md` | The original Wave-2 section + **Task W2.0's recon gate (STILL REQUIRED, folded into W2-2 here)**: NK-handler r7-flag tree at the delivery entry, IACK/EOI on the KDP-shim path, `[KDP+0x660]` "external pending" encoding; rev 2 C1 latch semantics (DEC one-shot-clear vs PIC LEVEL-HELD) + m11 (OEA priority External ABOVE Decrementer); the Wave-1 stop-rule precedent; the Wave-2 status note (model landed, wiring gated on the recon) |
| `docs/planning/machine/M3A-ENTRY-TABLE.md` | The delivery-hook contract W2-3 extends (`interrupt_entry=0x50412b1c`, DEC and EXT SHARE this target per exc_core); Task 7 delivery #1 evidence; the syscall-surface results (Q-S1..Q-S5 — the copy resolution, `[KDP+0x390]` publication precedent for finding NK publications; Task B/C frontier baselines this plan's boots are compared against); probe recipes |
| `docs/planning/machine/M6A-ONGOING-ENTRY-DESIGN.md` | The EE-relevant rung-2 facts: the `[0x2810]` run-mode DEC fence rationale; the `[KDP+0x65c]` world-flip discipline (shim save target = interrupted-world ctx) + `[KDP-0x14]` MRU discipline; residues **R-7** (ctx SRR1 the NK rfi uses — unresolved), **R-9** (the interrupt handler's `[KDP+0x660]` flag tests never run on nonzero flags — THE armed arm), **R-14** (mid-switch DEC window, benign-by-same-values, unobserved); XLM_IRQ_NEST imbalance history |
| `docs/planning/machine/M3-PIC-CUDA-DONOR-STUDY.md` §5 | The wiring-side facts: region resolution (MacIO BAR+0x40000, span 0x40000 ⇒ 0xF3040000..0xF307FFFF); §5.2 what the guest plausibly exercises (source-bank mask sweep at init, IACK/EOI hot path, FRR/GCR); the reimplement-vs-oracle verdict (already executed) |
| `SheepShaver/src/include/mmio_bus.h` (M1) | The bus API W2-3 uses: `MMIOBusRegister` contained-overlap rule ("most-specific wins" — the OpenPIC region 0xF3040000+0x40000 is fully contained in the macio-stub 0xF3000000+0x80000, main_unix.cpp:1860 — same idiom as scc/via); region locks cover lock-free devices (§2g rule 1); `MMIODevice` read/write shape = the `OpenPICRead/Write` trampoline shape (already matching) |
| Process template (BINDING): `docs/superpowers/plans/2026-06-11-nk-syscall-surface.md` body + Rev 2 | The discipline this plan replicates: blocking-answer table; ALL blocking answers pinned before ANY implementation task; static-RE time-boxes with written residue fallbacks (P-C2); boot arithmetic — TOTAL cap binds, co-scheduling mandated, canonical-gate boots outside the budget (P-C3); env-gate + flip-LAST + revert-on-red; the canonical gate set (**now 12 machine suites**); the P-M4 capture-artifact requirement; the one-iteration mechanics; the empty red-team record awaiting a round |

## Codebase facts (verified this session; implementers re-verify before editing)

- **The delivery hook** (`sheepshaver_glue.cpp:785–915`): gate order pending → depth
  (`current_execute_depth() != 1`, :791) → EE (`ExcDeliverable(msr_reg())`, :795) → native
  fence (`ReadMacInt32(XLM_RUN_MODE) != 0`, :814), then `VirtClockClearDECPending` (:818),
  `ExcEnter(restart, msr, EXC_DECREMENTER, &g_exc_entry_table)` (:824), EXC_PC_UNRESOLVED
  abort (:825), `SS_EXC_BARE` bypass (:836), the KDP shim (:858–896, save through
  `r6=[KDP+0x65c]` with zero-abort guard), transition apply (:902–905). **The gate ORDER is
  contract** — the `exc=` 5-tuple counts per-gate (`deferred_depth`/`deferred_ee`/
  `deferred_native`/`delivered_dec` + `delivered_sc` as out[0..4], :1176–1184, heartbeat
  :1353). Poll site: `ppc-cpu.cpp:1649` (newworld-only).
- **EE-edge re-raises**: `execute_mtmsr` 0→1 edge (`ppc-execute.cpp:1336–1362`,
  `trigger_interrupt()` at :1359), `execute_rfi` edge (:1639–1660, :1654), nested-return
  recheck (glue:744–748 per recon). All check `VirtClockDECPending`; none has fired with
  effect on a default boot.
- **exc_core is LAW**: PEM masks pinned by literal-value tests (`test_exc_core`, 25
  checks); `ExcDeliverable` is `(msr & 0x8000) != 0` (exc_core.h:113); EXC_DECREMENTER and
  EXC_EXTERNAL **share `interrupt_entry`** (exc_core.h:57–65); ExcEnter owns SRR0 semantics;
  `EXC_PC_UNRESOLVED=0xFFFFFFFF`. The W2-0 extraction must not change a single existing
  check — the 25-check suite is the behavior-preservation ratchet.
- **virt_clock**: `VirtClockInit(c, tb_freq_hz, now_fn, opaque)` /
  `VirtClockDECPending` / `VirtClockClearDECPending` (virt_clock.h:53–72); `dec_pending`
  is the latch word (:42). 26 checks green. On the live boot the latch is pending FOREVER
  (set at cold expiry, `delivered_dec=0`).
- **Machine suite is 12 binaries** (machine/Makefile TESTS, incl. test_dev_openpic 206
  checks). W2-0 adds `test_exc_chain` ⇒ 13. The recon's §0.4 broken-`make test`
  housekeeping is ALREADY RESOLVED (`7a52b6a1`) — not in scope.
- **OpenPIC model**: pure, lock-free, no consumer; `local_pipe`/`update_irq`/IACK/EOI
  oracle-derived (QEMU @ de5d8bfd…); output seam fires on transitions only, "under
  whatever lock the caller of the mutating entry point held" (dev_openpic.h:124). CTPR
  resets to 15 ⇒ after reset NOTHING is deliverable until the guest writes CTPR — a
  silent-PIC boot is EXPECTED until guest init, and W2-3's observability must distinguish
  "CTPR still 15" from "wiring broken".
- **Bus registration site**: `main_unix.cpp:1853–1888` — macio-stub 0xF3000000+0x80000,
  scc 0xF3012000+0x1000, via 0xF3016000+0x2000 registered as contained overlaps. OpenPIC
  at 0xF3040000+0x40000 is contained in the macio stub ⇒ registers the same way (no
  carve needed — most-specific wins, mmio_bus.h:38).
- **Bus values are ARCHITECTURAL** (mmio_bus.h header note: "what the PPC load yields
  after its REV; the JIT fault path byte-swaps at the injection layer"). QEMU maps
  KeyLargo with the LITTLE-ENDIAN ops table — so a BE guest `lwz` of a natural-value
  register must observe byte-swapped lanes (or the guest uses `lwbrx`). **The byte-lane
  decision (W2-3 first sub-item) is: value-swap inside the OpenPIC bus trampolines vs
  natural pass-through, decided from guest-access evidence + the QEMU oracle, recorded
  with a falsifiable probe (the first guest FRR read's observed value).**
- **The controlled external-interrupt source already exists**: `SS_SCC_RX_INJECT=DELAY:HEX`
  (main_unix.cpp:1891–1955) injects SCC ch-A Rx bytes — the M3a end-to-end demo lever.
  W2-3's acceptance trigger: SCC Rx assert → PIC input 0x25 → output → EXT delivery.
- **The guest-side suppressors W2-4 retires**: `tm_task` patch (rom_patches.cpp:3139/:3153,
  NOPs Enable60HzInts); `via_int`/`via_int2`/`via_int3` (rom_patches.cpp:3474–3511,
  rewrite VIA level-1/60Hz handlers to the paravirtual `M68K_EMUL_OP_IRQ` path). The 68k
  boot world's device service is POLLED (ROM trampolines 0x6d58/0x6e90, SCC poller 0x6ea0).
  XLM_IRQ_NEST parked at 0xFFFFFFFF; live balance unknown since the MM-switch flip
  (watch addr 10264 = 0x2818). The tick-thread trigger gate `==0` (main_unix.cpp:2446)
  is permanently false ⇒ no 60 Hz safety net; the EE-edge re-raises are load-bearing.
- **Harness facts (recon §C.2, carried)**: `ss_run_one_vector` glue:1455ff; REGDUMP
  glue:1713ff; `SS_MACHINE=newworld` wins (machine_profile.cpp:35); `SS_EXC_ENTRY=0xINT,0xSC`
  override (no-comma form PRESERVES the syscall default since Task A);
  `reset_supervisor_for_test` pins MSR=0xf072 (EE=1); the SS_TEST gate at main_unix.cpp:1440
  exits before `VirtClockInit` (:1817) ⇒ harness clock is dead without the new knob.
- **Canonical gate set ("full gates")**: `make -C SheepShaver build-ss`;
  `SS_HARNESS_BATCH=1 make -C SheepShaver test-jit` AND plain (both 353/353, legacy
  authoritative); `make -C SheepShaver/src/machine test` ALL PASS (12/12, 13/13 after
  W2-0); `make -C SheepShaver e2e-test` (122); paravirtual `make -C SheepShaver e2e`
  PASS + byte-identical (no new [EXC]/[PIC]/[NW-*]-shape lines on paravirtual). Smaller
  sets only with a stated reason.
- **Standing rules**: per-task commits; struct fields appended LAST; clean recompile after
  kpx/machine header changes (stale-.o awareness for glue/rom_patches); boots authorized
  but budgeted per task; probe-PC limit 8/run; `SS_JIT_WATCH_ADDR` is hex + needs
  `SS_JIT_TRACE_RING=1`; one shared checkout.

## Coordination constraint (BINDING)

W2-0 edits `sheepshaver_glue.cpp` + `ppc-execute.cpp` + `exc_core.{h,cpp}`; the FE1F
service-surface milestone's Tasks A/B/C edit `sheepshaver_glue.cpp` + `rom_patches.cpp`.
**The coordinator serializes any W2-0/W2-3/W2-4 implementation window against FE1F tasks
touching the same files** (the established same-files rule — one task in flight per file
set). W2-1 (harness block of glue + Makefile + script) also touches glue — same rule.
W2-2 is boots + docs only (no source edits beyond capture-only telemetry, which still
queues). The ladder is strictly ordered anyway (W2-0 → W2-1 → W2-2 → W2-3 → W2-4); the
constraint is about interleaving with the OTHER milestone, not within this one.

## Budgets (template P-C2/P-C3 discipline)

| Task | Size | Time-box | Boots |
|---|---|---|---|
| W2-0 | S | half-day incl. extraction + suite | 0 |
| W2-1 | S–M | 1 day (3 knobs + stub + 5 vectors + make lane) | 0 (harness runs are not boots) |
| W2-2 | S | one sanctioned session | **≤4 diagnostic boots TOTAL, each ≤60s** (P1=1, P2=1, P3/P4 contingent within the cap); canonical-gate boots outside the budget |
| W2-3 | M | 2–3 days | ≤4 bounded boots (bring-up + acceptance + gated-off A/B + 1 fix-iteration boot) |
| W2-4 | M | 2 days IF evidence-gated green; else spun off | ≤4 bounded boots |

Static-RE inside W2-2 is time-boxed per question (≤2h each, written residue fallback);
the one-iteration rule applies everywhere: a falsified pinned contract gets a dated
addendum entry + ONE bounded re-pin boot; a second falsification of the same contract
escalates to the stop-rule.

---

## Tasks

### Task W2-0: the decision-chain extraction + `test_exc_chain` (unit; links 3 + 10)

The recon §C.1 spec, adopted verbatim. **Risk note: exc_core is LAW** — masks tested by
literal pins; this extraction is behavior-preserving with `test_exc_core`'s existing 25
checks as the ratchet (zero existing checks change).

> **W2-0 DONE (2026-06-11).** Live-tree adjustments (drift accommodation, recorded in the
> commits): (a) FE1F landed `program_entry`/EXC_PROGRAM after this plan was written —
> `external_entry` is the FOURTH ExcEntryTable field (rev 2 F6's "third" is stale), the
> existing exc_core ratchet is **37** checks (not 25), the exc= tuple is **6** fields, the
> machine suite was 12 binaries (13 with test_exc_chain); (b) the live tree has FOUR
> nested-return re-raise sites (interrupt / execute_68k / execute_macos_code / execute_ppc,
> all the M3a Task 4.4 idiom), not one — all four routed through ExcEdgeReRaise via the
> synthetic-full-edge form (behavior-identical); (c) the hook caller uses the two-phase
> run-mode sampling idiom (pinned in the F13 caller-obligation comment) so [XLM_RUN_MODE]
> is still only read after pending/depth/EE pass (harness lowmem stays untouched pre-W2-1/F2).
> test_exc_chain = 62 checks (U1–U13; U12 per F6 mask-parity + unconsumed-external_entry pin;
> U13 per tension 2 with the starvation predicate documented for W2-3). **Addendum pointer:
> the F7 baseline-tuple gate is NOT discharged here — it rides W2-2's P1 boot** (the pre-W2-0
> exc= tuple class comparison; W2-0's job was only to not break what it would catch).

- [x] Extract into exc_core (pure; caller samples guest memory and passes words in):
  ```c
  enum ExcDecision { EXC_DECIDE_NONE, EXC_DECIDE_DEFER_DEPTH, EXC_DECIDE_DEFER_EE,
                     EXC_DECIDE_DEFER_NATIVE, EXC_DECIDE_DELIVER };
  ExcDecision ExcDeliveryDecision(int pending, int execute_depth,
                                  uint32_t msr, uint32_t run_mode_word);
  int ExcEdgeReRaise(uint32_t old_msr, uint32_t new_msr, int pending);
  ```
  **The gate ORDER is part of the contract** (pending → depth → EE → native — the `exc=`
  tuple counts per-gate; pin it in the header comment).
- [x] Refactor `deliver_pending_dec_exception` (glue:787–817) to call
  `ExcDeliveryDecision` — behavior-identical: same counters incremented per decision,
  same early-returns, latch untouched on deferral, cleared exactly once on DELIVER.
  Refactor the three re-raise sites (`execute_mtmsr` :1353–1360, `execute_rfi`
  :1644–1655, glue nested-return :744–748) to call `ExcEdgeReRaise` — behavior-identical
  (same `VirtClockDECPending` sampling, same `trigger_interrupt()`).
- [x] `test_exc_chain.cpp` (links exc_core + virt_clock; CHECK-counted, test_exc_core
  style): the recon's **U1–U12 verbatim** — U1 no-pending; U2 depth-outranks-EE; U3
  DEFER_EE; U4 EE-outranks-native; U5 DELIVER; U6 deferral retains the latch /
  delivery consumes exactly once (real VirtClock); U7 edge-predicate truth table
  (6 sub-cases); U8 the mtmsr contracts (0x7072→0xf072 fire, →0xd032 fire — the
  night-run pair; 0xf072→0x7072 no-fire); U9 rfi composition (ExcRfi from srr1=0xf072
  at msr 0x1040 → edge fires with pending); U10 the full deliver→rfi→deliver storm
  round-trip (never double-consumes); U11 the [0x2810]-fence release (mode 1→0);
  U12 EXC_EXTERNAL parity (same decision logic for an EXT-pending input; ExcEnter
  EXC_EXTERNAL hits interrupt_entry with the same masks).
- [x] Makefile: add `test_exc_chain` to TESTS (suite ⇒ 13 binaries).
- [x] Gates: full gates (machine 13/13; test_exc_core's 37 checks (live count; the plan's 25 predates FE1F) UNCHANGED is the
  explicit ratchet assertion; test-jit both modes 353/353 proves the glue/ppc-execute
  refactor is behavior-preserving on the harness surface). Commit.

### Task W2-1: the deliverability harness lane (links 4/5/10 end-to-end, no boots)

The recon §C.2 spec, adopted verbatim. **Honesty carried: depth-deferral
(`execute_depth>1`) is NOT harness-reachable** (needs a nested EMUL_OP execute) — covered
at unit level (U2) and live telemetry (W2-2 P4); recorded, not hidden.

> **W2-1 DONE (2026-06-11, commits 8355c943 + 4b995906).** As-landed notes:
> (a) the F1 fix shape: the boot parse extracted to the shared helper
> `exc_entry_table_apply_env_override()` (glue), called from both init_emul_ppc and the
> harness knob block; (b) the F2 fix shape: **MAP** (not zero-substitute) — guest
> [0x0,0x4000) mmap'd zero at NATMEM_OFFSET+0 so the hook's two-phase [0x2810] read runs
> the boot-identical code path (zero = MODE_68K = deliverable); REAL_ADDRESSING refuses
> loudly; (c) two knobs beyond the named three: `SS_TEST_EXC_STATS=1` (the EXCSTAT
> 6-tuple line — the H4 observable) and harness-side `MachineProfileInit()` when
> SS_MACHINE is set (the profile resolver is boot-path-only; without it every
> delivery-chain gate reads paravirtual); per F11 knob 1 shrank to dec_pending=1 +
> per-vector re-arm (VirtClockInitHost already runs at the SS_TEST gate); (d) H1's
> pinned restart 0x1000400C verified EXACTLY — realized by ending the vector in `b .`
> (the interpreter delivers two block-boundary polls after the edge; the b-self parks
> the PC deterministically); H3's restart = the rfi target (0x1000401C in the landed
> layout — the body's 0x10004010 was layout-dependent; the semantic — delivery AT the
> rfi target — is what held); (e) H4 green non-gating (deferred_ee=1 observed; the
> legacy HandleInterrupt fall-through proved read-safe on the F2 page); H5-unresolved
> per F12 (process-isolated, FATAL capture + exit 134, both modes, no REGDUMP);
> (f) lane `make test-exc-vectors` = METRIC pass=5 fail=0 total=5 score=100,
> interp-vs-JIT REGDUMPs byte-identical for all gating vectors (JIT-mode honesty:
> mtmsr/rfi/sc are non-compilable — the diff proves the JIT dispatch path delivers
> identically, not mtmsr codegen). Gates: 353/353 both modes (legacy table
> byte-unaffected knobs-off), machine 13/13, e2e-test 122; paravirtual e2e substituted
> by structural inertness (only boot-reachable change = the behavior-identical parse
> refactor inside init_emul_ppc's MachineProfileIsNewWorld() branch).

- [x] Three knobs, all inside the harness path only (`ss_run_one_vector` setup /
  SS_TEST block):
  1. `SS_TEST_DEC_PENDING=1` — `VirtClockInit(&g_virt_clock, 25000000, stub_now_ns, 0)` +
     set `dec_pending=1`, reset per vector in batch mode (the harness never reaches
     main_unix's `VirtClockInit`).
  2. `SS_TEST_MSR=0xHEX` — initial-MSR control (default 0xf072 ⇒ byte-compatible with
     all 353 legacy vectors; the EE-edge vectors start at 0x7072).
  3. `SS_TEST_EXC_STUB=1` — plant `mfmsr r20; mfspr r21,srr0; mfspr r22,srr1; blr` at
     guest 0x1000C000 host-side (the REGDUMP has no MSR/SRR0/SRR1 — captured via GPRs).
- [x] The five vectors, env
  `SS_MACHINE=newworld SS_EXC_BARE=1 SS_TEST_DEC_PENDING=1 SS_TEST_MSR=0x00007072
  SS_EXC_ENTRY=0x1000C000,0 SS_TEST_EXC_STUB=1`, each run interp vs JIT with REGDUMP
  diff:
  - **H1 mtmsr-edge**: `lis r3,0; ori r3,r3,0xf072; mtmsr r3` ⇒ r20=0x1040,
    r21=0x1000400C (restart = next insn), r22=0xf072; LR intact → stub blr → sentinel.
  - **H2 no-edge control**: same with 0x7072 ⇒ r20/r21/r22 untouched (anti-vacuous).
  - **H3 rfi-edge**: srr0:=0x10004010, srr1:=0xf072, `rfi` from 0x7072 ⇒ delivery at
    the rfi target: r21=0x10004010, r22=0xf072, r20=0x1040.
  - **H4 deferral telemetry** (nice-to-have, not gating): H2 + a counter-dump hook
    asserting deferred_ee incremented.
  - **H5 sc-class regression**: `sc` with entry unresolved vs resolved-to-stub —
    guards EXC_PC_UNRESOLVED + SRR0=sc+4 ownership end-to-end.
- [x] New make lane `test-exc-vectors` (dedicated run.sh stanza or sibling script,
  METRIC-formatted) — these vectors are env-dependent and MUST NOT enter the 353-table
  (env-free determinism contract).
- [x] Gates: full gates + the new lane green in both modes (H1/H2/H3/H5 PASS/FAIL;
  interp-vs-JIT REGDUMP identical). Files: glue harness block, Makefile, the script.
  Commit.

### Task W2-2: the bounded live-probe session (links 5/6/8 + the §B verdict + the original W2.0 recon — BINDING gate for W2-3/W2-4)

One sanctioned session, diagnostic prefs (`/tmp/m2accept.prefs`-class), **≤4 boots total,
each ≤60s**. Deliverable: a written addendum — new section in `EE-CHAIN-RECON.md`
("Wave-2 W2-2 probe results + NK-handler recon") — with every answer tagged
[STATIC]/[PROBE✓]/[CRASH]/residue, ending in the blocking-answer table below. Capture-only
telemetry commits allowed (full gates).

- [ ] **Boot P1 — baseline census (no behavior change):**
  `SS_PROBE_PC='0x50313bf8:r3,r7,r11;0x50412b1c:r6,r7,[0x68ffe660],[0x68ffe65c]'`
  `SS_JIT_WATCH_ADDR=10264 SS_JIT_TRACE_RING=1`. Pins: (a) the 0x313bf8 EE-force live
  (r11 before/after, selector r3 per visit); (b) `[KDP+0x660]` live value at the delivery
  entry (the R-9 arm input); (c) the XLM_IRQ_NEST balance post-MM-switch-flip (watch hits
  alternate ±1 = balanced; monotonic drift = finding, link 8).
- [ ] **The original M3b Task W2.0 recon items (static RE primary, ≤2h/question,
  residue fallback in writing):**
  - **(Q-W1) The NK handler r7-flag tree**: disassemble the delivery target 0x50412b1c
    (and its primary-copy sibling 0x50312b1c — the syscall milestone's copy-resolution
    precedent applies; per-target byte re-confirmation required, T-M1 rule) — map the
    `[KDP+0x660]`-derived branch tree, specifically the arm taken with bit 0x00200000
    set (the V-seed; the bounce at 0x50312cb0 `andi. r8,r7,0x30` + the rlwinm
    CR-mutation leg is the known entry to this family).
  - **(Q-W2) IACK/EOI on the KDP-shim path**: locate (or RULE OUT) a PIC IACK read /
    EOI write reachable from the delivery entry — DEC and EXT share one entry + one shim;
    how the handler discriminates sources is THE load-bearing unknown for W2-3. Also pin
    the NK's published external handler `[KDP+0x374]=0x50314880` vs the shared entry
    (reconciliation scope decided here, not improvised in W2-3).
  - **(Q-W3) `[KDP+0x660]` encoding for "external pending"**: the bit family, and what
    (if anything) the shim must compose differently for an EXT delivery vs DEC.
- [ ] **Boot P2 — THE EE-storm experiment (the §B verdict, falsifiable; THE NAMED
  DANGEROUS BOOT — bounded explicitly):** `SS_M6A_USER_MSR=1` (the quarantined lever,
  re-run deliberately now that MM-switch + sc-surface have landed; its rung-1 datum
  predates both; known-broken side effect: the un-root-caused R-2 zero-page slide) + the
  P1 probe set + `SS_PROBE_PC=0x50429b40`. Hard bounds: ≤60s wall clock (perl-alarm),
  `SS_TERM_DUMP=1`, diagnostic prefs only, env quarantined to this one boot, run LAST in
  the session so a wedge costs nothing downstream. Pre-pinned signatures (the recon's
  per-link table, BINDING):
  - **READY**: `exc=` delivered_dec climbing (≥1 per DEC reprogram cycle); ring shows
    continued 68k execution (no `100266f2 → 0 → 1` reset signature); comp/jDR climbing;
    `[KDP+0x67c]`-target writes appear (the from-emulator arm posting); frontier ≥ the
    sc wall.
  - **BROKEN (each maps to a link)**: SIGSEGV ea≈small-negative (R-2 slide — trampoline,
    not chain); 68k reset ring (handler arm corrupted the world — link 6/R-9);
    deferred_native climbing with delivered flat ([0x2810] fence stuck — link 3); nest
    drifting positive then Ticks frozen (link 8); ECB/MMCB save-slot clobber across a
    delivery in a switch window (R-14 real — link 5).
  - **ONE falsification → stop, record, re-scope** (the one-iteration discipline; this
    boot exists precisely to fire the surprise in bounds).
- [ ] **Boot P3 (contingent — only if P2's R-2 slide blocks the verdict):** land the
  sharper lever `SS_EXC_FORCE_EE_AT=0xPC` (OR 0x8000 into live MSR at first block-entry
  visit of PC; one line in the probe hook; capture-only-class commit) and re-run the
  storm at a chosen site.
- [ ] **P4 (rides any boot, no extra instrumentation):** first nonzero `deferred_depth`
  closes M3a carry-forward #1.
- [ ] **Gate — the blocking-answer table (template P-C1 form):**

  | Blocking answer | Gates |
  |---|---|
  | Q-W1 r7-flag tree + the R-9 arm's observed/static behavior | W2-3 (shim composition for EXT) + W2-4 (what the handler posts toward the 68k world) |
  | Q-W2 IACK/EOI presence + source discrimination verdict | W2-3 (whether the NK reads the PIC itself, or the hook must do more) |
  | Q-W3 `[KDP+0x660]` EXT encoding | W2-3 |
  | P2 verdict: chain READY/BROKEN per link under the current armed state | W2-3 start at all; W2-4 shape |
  | Nest-balance finding (link 8) | W2-4 (XLM_IRQ_NEST ownership) |

  ALL blocking answers pinned before W2-3 starts; a residue on a blocking answer invokes
  the stop-rule (re-scope, not improvisation). Commit the addendum.

### Task W2-3: OpenPIC bus wiring + EXC_EXTERNAL delivery (links 1 + 11 — the FIRST construction; env-gated `SS_NW_PIC`, default OFF, flip-LAST)

Blocked on W2-2's table. All new machinery inert when gated off (paravirtual + gated-off
newworld byte-identical).

> **W2-3 DONE (2026-06-11, commits b2e0d718 + 7cafd6ae + 95d3fc53) — SHIPPED GATED-OFF,
> FLIP HELD per stop-rule 3.** Coordinator decision honored: EXT delivers to
> `external_entry` = the published [KDP+0x374]=0x50314880 (the sanctioned U12 flip, both
> arms pinned), NOT the shared interrupt_entry; the P3 EE-lever session stayed superseded.
> Acceptance verdict = the PRE-DECLARED DOWNGRADE: live acceptance unreachable on TWO
> independent evidence-backed grounds — "PIC initialized: NO" ([PIC] reads=0 writes=0,
> CTPR still 15) AND "EE riser: NONE" (every EXT kick lands as deferred_ee; W2L-3
> re-confirmed). One [DIAG-FORCED] config ran inside the budget (tension 1): the full
> chain device→PIC→output→flag→kick→EE-gate is LIVE-PROVEN for both source classes
> (SCC inject → 0x25 leg; 197 VIA summary edges per boot — a live finding: the VIA
> 0x19 input carries real boot traffic, mask-gated correctly when unforced). The EXT
> delivery itself is proven at HARNESS level both modes (new lane vectors H6 entry-
> discrimination + H7 dual-pending priority; 9/9 score=100). 4 of ≤5 boots used; F16
> byte-lane = LE value-swap [STATIC-oracle], FRR falsifier armed but untested live
> (guest never reads the PIC). Gated-off A/B byte-identical (B4). Full record:
> EE-CHAIN-RECON.md "W2-3 results". W2-4's entry evidence: the chain is mechanically
> ready; what's missing is an EE riser + guest PIC init + tick consumption (link 7).

- [x] **The LE byte-lane decision FIRST (in writing, at the seam):** QEMU maps KeyLargo
  little-endian; our bus speaks architectural values; the model speaks natural values.
  Decide value-swap inside the OpenPIC bus trampolines vs natural pass-through, from the
  guest's first observed accesses (FRR read value is the falsifiable probe: a natural
  pass-through shows 0x003F0002, the LE mapping shows 0x02003F00-class lanes) + the
  oracle. Record the decision + evidence in the header's endianness note (replacing
  "W2.0-gated wiring work").
- [x] Bus registration in main_unix bring-up (the scc/via idiom):
  `MMIOBusRegister(0xF3040000, 0x40000, MMIO_TRAPPED, &openpic_dev)` — contained overlap
  inside the macio stub, most-specific wins; `OpenPICReset` then `OpenPICBindOutput`
  (reset clears the binding — order matters, header contract); region lock covers the
  lock-free model (§2g rule 1). The `[MMIO] bus active` line gains the pic region.
- [x] **Source edges**: VIA IFR-summary → `OpenPICRaise/LowerInput(0x19)`; SCC ch B/A →
  0x24/0x25 — assertion edges from the device models' existing interrupt-condition
  state, called under the owning region's lock (cross-region: device → pic ordering
  documented; never pic → device).
- [x] **The delivery-hook extension** (glue): a level-held EXT pending check BESIDE the
  DEC latch — `OpenPICOutputAsserted` (sampled via the bound-output flag, not a
  cross-thread struct walk), same depth/EE/native gating via `ExcDeliveryDecision`
  (U12 already tests the parity), `ExcEnter(..., EXC_EXTERNAL, ...)` → the same
  interrupt_entry + the KDP shim composed per Q-W1/Q-W3's pinned answers.
  **Latch semantics (rev 2 C1, BINDING): the hook must NOT clear PIC pending DEC-style** —
  EXT pending is level-held until guest IACK/EOI/mask drops the output; the hook delivers
  while asserted and relies on the handler's PIC service (Q-W2) to retire it. Re-delivery
  runaway guard: a delivered-EXT counter + a loud tripwire if EXT delivers N times with
  no intervening IACK (the Q8 first-IACK record is the discriminator).
  PLUS the U13 EXT-STARVATION tripwire (rev 2 Tension 2; W2-0 review Minor): EXT
  pending across >N DEC deliveries with no EXT delivery -> loud line.
  **Priority: DEC before EXT, justified locally per m11/C1** — OEA ranks External ABOVE
  Decrementer, but our DEC latch is one-shot-clear-on-delivery while PIC pending is
  level-held and safely waits one poll; the comment carries this justification verbatim.
- [x] **Observability**: CTPR first-write logged loud (the reset-15 gate — distinguishes
  "guest hasn't initialized the PIC" from "wiring broken"); `OpenPICFormatFirstIACKs` +
  `OpenPICFormatStats` wired into the heartbeat/term-dump; the first EXT delivery logged
  like `[EXC] DEC delivered #1`. **Fold the review minor: the `write_ctpr` recompute
  divergence comment** (dev_openpic.cpp:314–325 — equivalent-by-analysis to QEMU's CTPR
  path; write the analysis down at the function).
- [x] **Acceptance (PASS/FAIL, env-on `SS_NW_PIC=1`): a device-sourced external interrupt
  delivered through PIC→ExcEnter→NK on a CONTROLLED trigger — `SS_SCC_RX_INJECT` is the
  existing controlled source** (SCC Rx → input 0x25 → output → EXT delivery →
  handler-entry probe conforms to the Q-W1/Q-W3 pinned shim table → guest resumes; first
  IACK recorded for 0x25 with a Q8-consistent vector). Honest caveat: this requires the
  guest to have unmasked 0x25 + lowered CTPR — if the boot never initializes the PIC by
  the frontier, the acceptance downgrade is pre-declared: deliver-on-asserted with a
  HOST-forced unmask is NOT acceptance; instead record "PIC initialized: NO" as a W2-4
  evidence input and hold the flip (the stop-rule decides).
- [x] **Gated-off A/B**: one boot without the env var byte-identical to the current
  baseline class.
- [ ] *(HELD — stop-rule 3, see the DONE note)* **THEN flip** `SS_NW_PIC` to the newworld profile default (opt-out `=0`, the
  SS_NW_MM_SWITCH polarity), re-run full gates + acceptance with no env vars;
  **any red ⇒ revert the flip in the same task** (machinery stays env-gated, failure
  recorded). Gates: full gates throughout. Commit per sub-step.

### Task W2-4: tick restoration — tm_task/via_int retirement + XLM_IRQ_NEST ownership (link 7 + 8; EVIDENCE-GATED on W2-2/W2-3 findings)

> **FINAL DISPOSITION (2026-06-12, M7 close-out — the supersession table, per
> M7 rev-2 B1 + M7 Task C results):**
>
> | W2-4 item | Final disposition |
> |---|---|
> | tm_task retirement A/B | **MOOTED** by `2ff7765f` (verify-EXPECTED-first guard; the 9.0.1 misalignment was the real bug) — no retirement A/B owed |
> | via_int cluster disposition (rom_patches :3474–3511) | **RETAINED, load-bearing** — M7 Q-I3 pinned the patched chain as THE consumption path (level-1 @0xec50 → via_int 0xef2c → 60 Hz proc 0xbbb8 → fe6b OP_IRQ → `addq.l #1,$16a`); B-2's level fix arms exactly its trigger inputs. It is the next milestone's consumption rail, not dead code |
> | XLM_IRQ_NEST ownership (link 8) | **DOCUMENT-AS-DEAD** (M7 Task C disposition 1): both consumers' predicates unreachable from −1 downward; ~2³¹-delivery wrap caveat recorded; re-base/retire not owed |
> | Polled-trampoline + SDL_PumpEvents | **NAMED-DEFERRED** — ROADMAP follow-on row (default: leave / keep+decouple); untouched by the M7 chain |
> | Acceptance headline — Ticks advancing | **ABSORBED into M7 Task B/B-2 rider, honestly NOT achieved by the guest**: the host keep-set ticks Ticks (watch-word 0x16c correction); the guest `addq.l #1,$16a` has never run — the break is the slot-4 consumption livelock (the named next task) |
> | The reserved riser/published flip | **SHIPPED as the M7 Task-C cluster flip** (`81d60cc1`): SS_NW_EE_RISER + SS_NW_DEC_PUBLISHED + SS_NW_HOST_IRQ newworld DEFAULT, explicit-"0" opt-outs; all-OFF byte-identical A/B |
> | Wave2 Task Z (docs) | **ABSORBED into the M7 Task-Z close-out** (DIAGNOSTICS M7 section, MACHINE-LAYER-PLAN re-score #3, this table) |

**Entry gate (explicit):** W2-2's P2 verdict READY-or-mapped + W2-3's acceptance (or its
pre-declared downgrade verdict) determine this task's shape. **If the evidence says the
68k consumption side needs more than retirements** (e.g. the R-9 arm posts into
paravirtual-patched handlers that need real NK→68k forwarding through the DR emulator —
the M3b rev 2 M5 coupling to M6's boot-past-console work), **the stop-rule gives this
task out as its own follow-on milestone: capture, name the frontier, stop.** No tunneling.

- [ ] **tm_task retirement A/B** (rom_patches.cpp:3139/:3153): newworld-only,
  profile-gated (paravirtual keeps all patches forever); Enable60HzInts runs real.
- [ ] **via_int cluster disposition** (rom_patches.cpp:3474–3511): retirement per the
  W2-2 Q-W1 evidence of what the NK posts (the `[KDP+0x67c]` + CR-bit path) — REPLACE@M3
  per the audit, gated identically; if the evidence shows the paravirtual rewrite is
  still load-bearing, record the dependency and stop (the M5 coupling).
- [ ] **XLM_IRQ_NEST ownership decided in writing** (link 8): who resets/owns the
  counter on newworld now that warm switch-backs traverse the slot-0 stub — informed by
  P1's watch-balance finding; the delivery hook keeps ignoring it (real MSR[EE] gate),
  but `HandleInterrupt`'s Ticks keep-set and the tick-thread trigger gate get an owned
  disposition (retire, re-base, or document-as-dead).
- [ ] **Polled-trampoline disposition**: the ROM VIA/SCC pollers (0x6d58/0x6e90/0x6ea0)
  vs interrupt-driven service — recorded decision (likely: leave; polling coexists with
  delivery). **SDL_PumpEvents relocation decision** re-evaluated against what the 68k
  world actually needs (recon option (c) keep+decouple is the default).
- [ ] **Acceptance — THE MILESTONE'S HEADLINE DIAGNOSTIC: the first REAL guest-visible
  tick — `Ticks` (0x16a) advancing** under delivered interrupts (watch/probe on 0x16a;
  sustained deliver→rfi→deliver cadence in `exc=`; no nest drift; no reset ring).
  PASS/FAIL gates are the regression invariants (full gates, rung-2 invariant carry-over,
  byte-identical gated-off boots); Ticks advancing is recorded as the diagnostic outcome
  with the full HB/CUDA/ring baseline — the next milestone's opening evidence either way.
- [ ] Env-gated + flip-last + revert-on-red, same discipline as W2-3. Commit per sub-step.

**W2-4 progress (the EE-riser staging per EE-CHAIN-RECON "W2-4 entry decision", coordinator-signed):**
- [x] **Step 0 — DEC re-point to the published handler** (`181efc02`): live `[KDP+0x384]`=0x50313200
  verified first; `SS_NW_DEC_PUBLISHED` gate (default OFF) selects the 2-SPR shim, gate-off path
  byte-inert; harness lane H8 pins the SPRG round-trip (12/12); gated-off live A/B md5-identical.
  Review: **APPROVED-WITH-NOTES** — carry-forward: (P2) run-exc.sh wants a guard/comment for
  gate-ON-without-SS_EXC_ENTRY (2-SPR shim toward the unmapped legacy default) before any
  default-ON flip; (P3) stub=1 writes 6 words not 4 (inert, fresh-process zero RAM). Flip-risk
  note: the first end-to-end execution of 0x50313200 arrives with step 1's riser — it reads
  NK-maintained [KDP-0x10]/[KDP-0x14] state never exercised under emulated delivery.
- [x] **Step 1 — the EE riser in the 0x318000 stub** (`10b1b3e8`, 2026-06-12): candidate (a),
  partial-rfi, EE-ONLY compose per sign-off item 3 (mfmsr r10; rlwimi r10,r11,0,16,16;
  mtmsr r10 — 3 words after the nest decrement). `SS_NW_EE_RISER` gate (newworld +
  env, default OFF, applied at patch time). Word budget verified [RAW-ROM]: the stub
  area is the NK AltiVec lvebx thunk table; +3 words = same reachability class as the
  accepted upstream clobber. SS_DUMP_ROM A/B: gate-on delta is exactly the 4 designed
  words at 0x31800c-0x318018; gate-off byte-identical. Fresh gated-off baseline (post
  tm_task-guard): exc=0/5/0/0/173/4, SIGTERM park, HOT-PC 0x50467ed4/r9=0x50004a9e.
- [x] **Step 2 — scored boot against the W2-2 READY/BROKEN table** (deferred P3
  DISCHARGED, 2026-06-12; 4 of ≤5 boots): `SS_NW_EE_RISER=1 SS_NW_DEC_PUBLISHED=1` →
  **delivered_dec > 0 for the FIRST TIME — 12.4M deliveries in 50s, all on the
  published 0x50313200 2-SPR route, zero crashes** (no SIGSEGV / no 68k reset ring /
  deferred_native=0). BUT a DEC delivery STORM: expire→deliver→handler mtspr DEC→tail
  exit→riser EE-raise→immediate re-expiry (~250K/s; mtspr_dec=14.68M ≈ dec_expiries);
  68k world starved (jDR=14, sc=0, mmio=0 — frontier REGRESSED vs the gated-off park);
  nest drift −1/delivery at storm scale ([0x2818] → −10.0M ≈ −deliveries); Ticks [0x168]
  frozen at 0. Per-link table + full storm anatomy: EE-CHAIN-RECON "W2-4 step 1+2".
  Riser+published-route mechanics PASS; links 7/8 confirmed BROKEN as predicted — the
  W2-4 body (nest ownership + DEC reload cadence + tick path) is the remaining work.
  Default stays OFF in tree. Falsifications: NONE (storm = the predicted intended risk).
  Side evidence (boot 3): riser WITHOUT SS_NW_DEC_PUBLISHED → SIGTRAP crash at
  0x50412be0 (the legacy-KDP r9 hazard) — step 0's re-point is load-bearing, as designed.
- [x] **Item 1b — run-mode fence vs deferral starvation: RESOLVED as the post-DEFER_NATIVE
  wake-up edge** (`3cb3b16e`, 2026-06-12, with `34d3d441` Execute68k staging as prerequisite):
  per the pm5-recon CORRECTION (INTERRUPT-INJECTION-RECON.md Q4 — D-7's "[0x2810] never
  cleared in the cold 68k world" mechanism FALSIFIED by the balanced 151/151 NK-writer
  watch; fence semantics correct, run-mode clearing a non-fix), the fix is a HANDLE-spcflag
  re-arm on EXC_DECIDE_DEFER_NATIVE — re-pinned once to a bounded per-episode budget
  (cap 65536; the recon's "bounded by window length" claim falsified at the post-P-M5-fix
  frontier, which parks inside a never-exiting native window). Riser-on acceptance:
  **delivered_dec=3, the first live published-route deliveries (0x50313200, 2-SPR);
  pending drained to 0; mtspr_dec=25 (no storm); sc/program at baseline; Ticks still
  frozen (expected — consumption is the item-3 EXC_EXTERNAL/Ticks work).** Full record:
  INTERRUPT-INJECTION-RECON.md "RESULTS" section.

### Task Z: docs

- [ ] DIAGNOSTICS.md: `SS_NW_PIC`, `SS_TEST_DEC_PENDING`/`SS_TEST_MSR`/`SS_TEST_EXC_STUB`,
  `SS_EXC_FORCE_EE_AT` (if landed), the test-exc-vectors lane; CHANGELOG (first EXT
  delivery + first real tick numbers, if achieved); EE-CHAIN-RECON status flip (links
  closed per task); MACHINE-LAYER-PLAN M3b row + §9 re-score (the named surprise:
  fired/de-fused, honestly); ROM-PATCH-AUDIT rows (tm_task/via_int) with evidence;
  M3A-ENTRY-TABLE (the delivery-hook contract note "stays with HandleInterrupt until
  M3b's PIC" — superseded); LEARNINGS; cross-tracker grep for stale "EE has never risen"/
  "no PIC consumer" claims (historical sections stay). Commit.

## Stop-rule (triggers per MACHINE-LAYER-PLAN §9)

1. **W2-2 P2 falsification**: ONE broken signature → stop the session, record per-link,
   re-scope W2-3/W2-4 against the finding (this is the de-risking working as designed —
   the surprise fired in bounds).
2. **A residue on a W2-2 BLOCKING answer** → W2-3 does not start; re-scope (no
   improvised shim composition or source discrimination).
3. **W2-3 acceptance unreachable because the guest never initializes the PIC** by the
   current frontier → ship W2-3 gated-off (model wired, flip held), record "no live
   consumer yet", W2-4 evidence-gates on its own findings — the Wave-1 stop-rule
   precedent verbatim.
4. **W2-4 entry gate red** (consumption side needs NK→68k forwarding beyond
   retirements) → W2-4 becomes its own named follow-on milestone; capture and stop.

Within tasks: the one-iteration mechanics (dated falsification entry → ONE bounded
re-pin boot → resume; second falsification of the same contract escalates).

## Self-review record

Spec coverage: the EE-CHAIN-RECON ladder adopted verbatim (W2-0..W2-4, sizes, the
verification-before-construction rationale); §C.1's U1–U12 and §C.2's knobs/H1–H5/lane
folded as written contracts; §C.3's P1/P2/P3/P4 with the READY/BROKEN tables BINDING;
the original M3b Task W2.0 recon folded into W2-2 (Q-W1/Q-W2/Q-W3) with a P-C1-style
blocking-answer table; rev 2 C1/m11 latch + priority semantics carried into W2-3;
the as-landed OpenPIC seams (BindOutput-after-Reset, transitions-only, CTPR-15,
Q8 tripwire, the write_ctpr comment debt) consumed; the bus containment fact verified
(0xF3040000+0x40000 ⊂ macio stub — no carve needed, correcting the older "carve the
macio-stub overlap" phrasing); env-gate + flip-last + revert-on-red + canonical gates
(12→13 suites) per the template; the EE-storm boot named and bounded as required;
honest gating separation maintained (W2-0/1/2 gates = test counts + falsifiable probe
signatures; W2-3 = controlled-trigger delivery; W2-4 = invariants, with Ticks-advancing
as the headline DIAGNOSTIC).

Known tensions flagged for the red team:
1. **W2-3's acceptance depends on guest PIC initialization** (CTPR 15 + masked sources at
   reset): if the boot frontier never programs the PIC, the SS_SCC_RX_INJECT trigger
   cannot legally deliver — the pre-declared downgrade (trigger 3) is load-bearing; is
   "host-forced unmask is NOT acceptance" the right line, or should a bounded forced-
   unmask DIAGNOSTIC boot be allowed inside the budget?
2. **DEC-before-EXT inverts OEA priority** — justified by one-shot-vs-level-held latch
   mechanics (m11/C1); a reviewer should check the justification survives the
   storm cadence (U10 + P2) when BOTH sources are pending.
3. **The shared interrupt_entry for DEC and EXT** vs the NK's published external handler
   `[KDP+0x374]=0x50314880` — Q-W2 decides, but if the NK genuinely discriminates by
   entry POINT (not by flags/IACK), the exc_core single-entry table shape itself is
   wrong and the re-scope is bigger than a shim tweak.
4. **W2-0's extraction touches the LAW module** — behavior-preservation rests on the
   existing 25 checks + 353-vector harness; is that ratchet sufficient, or should the
   refactor land with a pinned before/after `exc=` tuple from one canonical boot?
5. **P2 rides a known-broken lever** (SS_M6A_USER_MSR's R-2 zero-page slide) — the P3
   contingency exists, but P3 lands NEW code (SS_EXC_FORCE_EE_AT) mid-recon; is a
   capture-only-class commit the right classification for a knob that mutates live MSR?
6. **Same-files serialization vs FE1F** is stated but coordinator-enforced — no
   mechanical guard exists.

## Red-team record

*(empty — a red-team round follows this draft; findings to be folded as rev 2 markers)*

## Rev 2 (2026-06-11) — red-team round folded (BINDING; overrides the body where in conflict)

**Critical fixes (the plan would have failed as written):**
- **(F1) `SS_EXC_ENTRY` never parses on the harness path** (the parse lives in the boot
  init block, glue:2223; the harness gate returns at main_unix:1445 before it) — W2-1
  gains a FOURTH knob (or folds into SS_TEST_EXC_STUB): harness-side entry-table setup
  inside `ss_run_one_vector`, re-using the parse as a shared helper. The Codebase-facts
  harness bullet is corrected.
- **(F2) The delivery hook reads `[XLM_RUN_MODE]` (guest 0x2810) — unmapped in the
  harness** (only test RAM at 0x10000000 exists) → SIGSEGV on the first delivery
  attempt. The W2-1 knob must map (or zero-substitute) a lowmem page in harness mode —
  named scope, not invisible.
- **(F3) P1/P2 watch address corrected: `SS_JIT_WATCH_ADDR=2818`** (the parser is HEX,
  ppc-cpu.cpp:909; the body's `10264` would watch 0x10264 and read "balanced" as
  silence). The Codebase-facts decimal gloss is corrected.

**Major fixes:**
- **(F6 + tension 3) EXT entry-point discrimination is the EXPECTED verdict, not the
  surprise**: the NK publishes a per-vector handler table at KDP+0x360 (DEC
  [KDP+0x384]=0x50313200 ≠ EXT [KDP+0x374]=0x50314880 ≠ SC [KDP+0x390]=0x50314ac0,
  all [PROBE✓] in the syscall milestone), and a bounded capstone look confirms
  0x314880 is a distinct EXT body (shared 0x50313d40 save prologue, then SRR1-bit +
  selector dispatch into the bounce family). **W2-3 pre-positions `external_entry` as
  a THIRD ExcEntryTable field (appended last), Q-W2-gated, default candidate
  0x50314880 (primary copy per the publication precedent).** U12 re-worded to test
  mask parity, not the shared-entry shape. The addendum also records the implied
  follow-up: whether DEC's 0x50412b1c target (vs published 0x50313200) is long-term
  right — out of W2 scope, named not silent.
- **(F4) The SCC has NO interrupt-condition state** (dev_scc8530 models raw WR bytes +
  an Rx queue only): the SCC→0x25 edge is NEW device-model code (Rx-available ∧ WR1
  Rx-int-enable ∧ WR9 MIE, assert/deassert at enqueue/drain/enable-writes) with unit
  checks in the SCC suite; the VIA summary-output edge is also new (small — ifr/ier
  exist). W2-3's sizing notes this sub-scope explicitly.
- **(F5) The EXT output needs a CPU-thread KICK + an atomicity contract**: the
  bound-output callback's asserted edge calls the existing cross-thread trigger
  (the DEC-expiry TriggerInterrupt idiom); the output flag is a single-copy-atomic
  word (written under the PIC region lock from fault/pump threads, read lock-free at
  the CPU-thread poll) — never a plain bool.
- **(F7 + tension 4) The W2-0 ratchet gains the missing leg**: the 353-vector suite
  exercises only paravirtual arms; W2-2 P1 therefore carries an explicit gate — the
  boot-baseline `exc=` tuple class unchanged vs the pre-W2-0 capture (a transposed
  gate/counter shows as a tuple-class change; rides the already-budgeted boot).
- **(F8) Evidence-staleness rule vs the concurrent FE1F milestone**: each W2-2 pinned
  answer records the frontier signature it was pinned at; a frontier-moving FE1F
  landing before W2-3 starts triggers one bounded re-confirmation boot (or a written
  still-valid-by-argument note) per affected pin.
- **(F9) Probe-spec repairs**: P1 adds `[0x68ffe67c]` ([KDP+0x67c]) and P2 watches its
  resolved target (hex); P3 is explicitly a SECOND bounded session (build+boot after
  P2 — "P2 last" applies per-session); the R-14 signature regains its instruments
  (`[ECB+0xfc]`/`[MMCB+0xfc]` probe fields); P2 adds the 0x16a watch for the
  Ticks-frozen leg.
- **(F10) W2-3 budget restated**: the four named boots are bring-up (+byte-lane FRR
  probe co-scheduled), acceptance env-on, gated-off A/B, post-flip acceptance re-run;
  the fix-iteration boot is a conditional +1 (≤5 total).

**Adjudications adopted:**
- **(Tension 1) Forced-unmask is allowed as a NAMED DIAGNOSTIC** (`[DIAG-FORCED]` tag,
  one boot inside the cap, only after "PIC initialized: NO" is recorded) — it
  distinguishes wiring-broken from guest-hasn't-initialized, which CTPR logging alone
  cannot; it is never acceptance. Trigger 3 consumes it.
- **(Tension 2) DEC-before-EXT kept WITH the starvation guards**: new unit case U13
  (dual-pending → DEC delivered, EXT survives) + an EXT-starvation tripwire (EXT
  pending across >N DEC deliveries with no EXT delivery → loud line) symmetric to the
  re-delivery runaway guard.
- **(Tension 5) P3's `SS_EXC_FORCE_EE_AT` is a DIAGNOSTIC LEVER commit, not
  capture-only** (it mutates live MSR): env-gated default-off, full gates, queues
  under the same-files rule like any source edit; W2-2's "boots + docs only" sentence
  carries this named exception.
- **(Tension 6) The mechanical same-files guard is approved as Stream-D tooling**:
  `docs/superpowers/.claims/<milestone>.claim` files + `tools/check-claims.sh` in a
  pre-commit hook (fails commits staging another live claim's files; no-op when no
  claims exist). Until it lands, each W2 task preamble runs
  `git log --oneline -5 -- <shared files>` with "unexpected foreign commit ⇒ stop
  and re-sync".

**Factual corrections (F11/F12/F13/F14/F15/F16/F17):** the harness clock is NOT dead
(VirtClockInitHost at main_unix:1443 initializes it; only `dec_pending=1` is missing —
knob 1 shrinks accordingly; ":1817" was the scheduler block); H5-unresolved's
observable is the FATAL capture + exit code, run process-isolated (never in a batch
file); U11/U6 retained with the caller-obligation doc-comment carrying the real
contract; W2-4's entry-gate evidence list gains the VIA lazy-settle/IER-push deadlock
risk (the header's own warning); stop-rule middles added — a W2-1 interp-vs-JIT
divergence is a codegen bug (systematic-debugging route, not re-pin mechanics), and
the byte-lane decision defaults to [STATIC-oracle] (QEMU's LE ops table ⇒ value-swap)
with the first live FRR read as the FALSIFIER (the guest-evidence framing was
circular under CTPR-15 silence). All other cited facts verified (F17).
