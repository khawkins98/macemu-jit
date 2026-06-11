# M6 next milestone — the NK syscall surface: resolve vector 0xC00 (`syscall_entry`) so MPLibrary's parcel init can make its NK system calls and return

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. **Dispatch ONE task at a time — Tasks A/B/C all edit the same three files (`ppc-execute.cpp`, `sheepshaver_glue.cpp`, `rom_patches.cpp`); NOTHING in this plan parallelizes.**

**Goal:** On the newworld profile (MixedMode switch default-on since rung 2), MPLibrary's
PPC init runs to its first kernel service call and dies on the M3a abort-with-capture:
`[EXC] FATAL: sc at pc=500d638c with unresolved syscall entry (SRR0=500d6390
SRR1=00007072 msr=00007072 lr=500cf108 r1=103ffb50)` — identical pre/post-flip; nothing
else sits between the MixedMode round trip and this wall. `g_exc_entry_table.syscall_entry`
is still 0 (the M3a descope carry-forward). This milestone resolves vector 0xC00 the
machine-layer way: **real NK code handles the `sc`** — locate the staged NK's own syscall
handler, deliver the `sc` to it through the already-real `ExcEnter(EXC_SC)` transition
(SRR0=+4 ownership pinned in exc_core), seed whatever KDP/ctx state its real vector stub
would have provided (the rung-2 seed idiom), and let the NK's own service + `rfi` return
the caller to SRR0. Milestone acceptance DIAGNOSTIC (not a gate): MPLibrary's init
RETURNS — the MixedMode excursion completes after the syscall(s) and the 'pwpc' parcel
chain advances toward CodeFragmentMgr (observed via SS_DR_R24_RING 68k-PC evidence + the
CFM caller-region signature, file 0xf4xx — 68k PCs are PPC-probe-blind). PASS/FAIL gates
are the regression invariants plus the falsifiable sub-contracts below. Honest framing:
MPLibrary's init may issue MANY syscalls; this milestone owns the syscall *surface* (entry
resolved + first-sc round trip conformant), not every selector's semantics — subsequent
walls are captured, named, and stopped on per the stop-rule. Paravirtual byte-identical
throughout. Wave-2 backlog (OpenPIC wiring etc.) stays parallel and untouched.

**Architecture:** No new host-side delivery mechanism and no powerpc_cpu changes beyond
the existing profile-gated slow-path seam (MACHINE-LAYER-PLAN §2d CPU-core honesty —
`execute_syscall` is already that seam). The likely implementation is SMALL: a resolved
`NW_SYSCALL_ENTRY_DEFAULT` next to the interrupt entry (glue), plus a pre-entry shim
and/or KDP seeds per the NK handler's pinned ABI — host-side in the sc delivery path
(the `deliver_pending_dec_exception` KDP-shim precedent) or guest-side trampoline seeds
(the rung-2 idiom: PatchROM-time writes, verify-zero-first, new regions above the W2
slot-1 region 0x429d80..0x429d9c). **Task 0 is a BINDING pre-implementation recon gating
Tasks A..C** — it pins the handler address, its entry ABI, MPLibrary's first-sc
conformance vector, and the staged-surface audit in writing before implementation
freezes. New machinery is env-gated during bring-up (`SS_NW_SC_SURFACE=1`, default OFF;
the existing `SS_EXC_ENTRY=0xINT,0xSC` override is the no-rebuild iteration channel) and
flipped to the newworld profile default only as Task C's LAST step with explicit
revert-on-red. Gated OFF, the abort-with-capture baseline is byte-identical.

---

## Authoritative inputs

| Doc | What it fixes |
|---|---|
| `docs/planning/machine/M3A-ENTRY-TABLE.md` | THE entry-table contract: `interrupt_entry=0x50412b1c` (probe-verified, KDP-SHIM mode), `syscall_entry=0` descope + revisit clause ("revisit when a live sc fires" — that moment is now); the two-anchor probe-verified methodology (static+0x100000 relocation delta, byte-identical copy); vector pages 0x100/0x300/0x500/0x900 EMPTY (DIRECT-VECTOR out — **0xC00 was never dumped**); `[KDP+0x65c]`=ECB / `[KDP+0x660]` shim inputs; probe recipes. **Task 0's addendum lands here.** |
| `docs/planning/machine/M6A-ONGOING-ENTRY-DESIGN.md` — "Rung 2 contracts" + Task T/V/W/W2/X/Y results | The rung-2 facts that touch the syscall question: the NK switch service 0x503143a0 disassembly + the live `[KDP+0x5f0/4]`=0x50313bf8/0x503143a0 NK-rebuilt publications; the NK save/restore protocol (§2.1–2.2: save through r6, scheduler restore via ctx+0xfc); `[KDP-0x14]` current-ctx discipline (cold=ECB seed, forward-switch sets MMCB); the `[KDP+0x65c]` world-flip (holds **MMCB during native excursions** — the regime the `sc` fires in); the `[0x2810]` run-mode DEC fence; residues R-1..R-15 (esp. **R-7**: the ctx SRR1/EE the NK rfi uses for the MixedMode ctx — unresolved); sub-KDP occupancy map (AUTHORITATIVE — extend before placing anything); the Task-Y captured baseline |
| `docs/planning/machine/M6A-WAVE2-SHIM-RECON.md` — "Frontier update (2026-06-11, rung-2 Task Y closeout)" + the night-run/bail sections | The frontier capture verbatim (the `[EXC] FATAL` line + HB/CUDA baseline numbers); the r24-ring/probe methodology + corrections (SS_JIT_WATCH_ADDR parses hex, needs SS_JIT_TRACE_RING=1; probe-PC limit 8/run); the `SS_EXC_SC=legacy` wedge datum (52M/s comp-frozen spin at 3672 — survives the sc, not a bridge) |
| `docs/planning/HANDOFF-NEWWORLD-SUPERVISOR-MMU.md` §2.8 | Background obstacle map ONLY — its sc analysis ("a real PPC exception vector for sc … is a much larger change") **predates M3a and is superseded** by exc_core; cited for history, NOT a spec |
| `SheepShaver/src/kpx_cpu/src/cpu/ppc/ppc-execute.cpp` `execute_syscall` (:1119–1155) | The live sc seam: newworld → `ExcEnter(pc(), msr, EXC_SC, &g_exc_entry_table)`; unresolved ⇒ capture-abort (or `SS_EXC_SC=legacy` no-op); resolved ⇒ **bare** SRR0/SRR1/MSR/PC transition, NO KDP shim, NO increment_pc (CFLOW_TRAP — JIT falls back) |
| `SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp` (`g_exc_entry_table` + `NW_INTERRUPT_ENTRY_DEFAULT` :86–93, `deliver_pending_dec_exception` + KDP shim + [0x2810] fence :769–899, `SS_EXC_ENTRY` parse :2135–2154) + `SheepShaver/src/include/exc_core.h` / `src/machine/exc_core.cpp` + `test_exc_core` | EXC_SC semantics: **ExcEnter owns the +4** (SRR0=pc+4 for SC — exc_core.h:71,84); PEM masks are LAW; ExcRfi real; the DEC-path shim is the transcription precedent for any sc-side shim; `SS_EXC_ENTRY=0xINT[,0xSC]` is the designed no-rebuild iteration channel |
| `docs/planning/MACHINE-LAYER-PLAN.md` §2d + M3/M6 rows | Where this sits: M6's named next milestone; §2d's CPU-core honesty rule (profile seam on slow paths only); §9 stop-rule |
| `SheepShaver/src/rom_patches.cpp` (`PatchROM_NW_trampoline` :714ff; W discriminator :999–1160; W2 slot-1 region :1160–1240) | The guest-side seed idiom every rung-2 task used: PatchROM-time only, verify-zero-first (verify-EXPECTED for nonzero sites), regions above 0x429c30; current high-water mark = the W2 slot-1 region **0x429d80..0x429d9c** — new regions start at/above **0x429da0** |

## Codebase facts (carried; implementers re-verify sites before editing)

- **The captured baseline (the milestone's opening evidence; re-confirm once before Task A):**
  default newworld config (switch on), boot dies at
  `[EXC] FATAL: sc at pc=500d638c … SRR0=500d6390 SRR1=00007072 msr=00007072
  lr=500cf108 r1=103ffb50`. HB: comp=3672 jNK=4104 jRAM=0 exc=0/1/0/0
  mmio=S:65540/V:70183(IER=65539,ORB=3848); CUDA quiet (13 pkts / 9 i2c to absent
  addrs, pram_rd=3). Chain: TVector → MPLibrary init → CFM caller region (file 0xf4xx)
  → second MixedMode round trip → `sc`. `SS_EXC_SC=legacy` survives the sc but wedges
  in a 52M/s comp-frozen spin at 3672 — evidence the caller checks a syscall result
  it never gets; NOT a bridge.
- **The sc fires MID-NATIVE-EXCURSION**: `[0x2810]` (XLM_RUN_MODE) = 1 (NK-maintained
  forward), `[KDP+0x65c]` = MMCB (W2 world-flip), `[KDP-0x14]` = the MRU pair (=MMCB,
  set by the forward hit path), SRR1=0x7072 ⇒ **EE=0** — no DEC can deliver during the
  excursion (fence + EE both closed). MSR fiction 0x7072 also has IR/DR=1, PR=0.
- **The sc delivery path applies NO KDP shim today** (deliberate M3a asymmetry vs the
  DEC path — pending exactly this milestone's Q-S2). If the NK syscall handler's real
  vector stub provides a prologue (the interrupt handler's body at static 0x312b0c
  expects r6=save-area/r7/r8 etc. — the KDP-shim idiom exists because of this), the
  sc path needs its equivalent. Site discipline per §2d: profile-gated slow path; a
  glue helper called from `execute_syscall` (the DEC-path precedent) keeps powerpc_cpu
  honest.
- **`SS_EXC_ENTRY=0xINT[,0xSC]` already parses both fields** (glue :2135–2154) — Task 0
  candidate-entry boots need NO rebuild. The default lives at glue :92–93
  (`{ NW_INTERRUPT_ENTRY_DEFAULT, 0u }`).
- **ExcEnter owns SRR0=+4 for EXC_SC** (exc_core.h: caller passes the sc's own address;
  verified live in the capture: pc=0x500d638c → SRR0=0x500d6390). The handler MSR =
  pre-MSR with POW|EE|PR|FP|FE0|SE|BE|FE1|IR|DR|RI cleared (0x7072 → 0x1040 expected —
  same as the DEC deliveries). Any NK-handler expectation that IR/DR stay on is a Task-0
  falsification target, not a mask edit (masks are LAW).
- **THE COPY AMBIGUITY (flagged contradiction — Task 0 Q-S1 resolves):**
  M3A pinned `interrupt_entry=0x50412b1c` = static 0x312b1c + 0x100000 ("staged = static
  +0x100000, byte-identical") and delivery there WORKS (Task 7 evidence). But rung-2's
  live probes show the NK's OWN publications point at the **primary-range copy**:
  `[KDP+0x5f0/4]` = 0x50313bf8/0x503143a0, and the switch/save/scheduler legs execute
  at 0x5031xxxx/0x5032xxxx (10M-visit probes). Both copies are byte-identical so either
  executes, but the live syscall entry should follow **what the NK itself publishes/
  reaches**, not an assumed delta. Do not silently "fix" interrupt_entry — out of scope;
  record the resolution.
- **M3A's "vector pages EMPTY" never dumped 0xC00** — one probe word closes it.
- **NK save/restore protocol** (design doc §2): handler-side saves through r6; scheduler
  restore resumes via ctx+0xfc/mtlr r12/mtctr r10/bctr; the M3a shim feeds it
  r10=r12=restart. Whether the SYSCALL exit uses rfi (SRR0/SRR1 → ExcRfi) or the
  scheduler path is Q-S4's question — both mechanisms are real in our core.
- **Guest-side seed budget** (if Task A needs trampoline-resident seeds): the trampoline
  proper is full (46/48); the W region is 0x429d00..0x429d74, W2 slot-1 region
  0x429d80..0x429d9c. New regions start at/above **0x429da0**, verify-zero-first,
  PatchROM-time only (rev 2 C4/C6 carried), env-gated structurally inert when off.
- **Sub-KDP occupancy map is AUTHORITATIVE** (design doc) — any new data word goes
  through the map first (free ranges: 0x68FF6084..0x68FF7000).
- **ROM-dump provenance**: `/tmp/rom901_inventory.bin` = the only RAW image (16 `twi`
  placeholders at 0x36e8c0); `/tmp/rom901.bin` + `/tmp/rom901_patched_t0.bin` = PATCHED
  (md5-matched 2026-06-11). These are /tmp files — **re-verify existence + provenance
  before tagging anything [RAW-ROM]/[PATCH]; re-dump via SS_DUMP_ROM if evaporated.**
  The 4 MB dump covers the primary copy only; mirror facts are [PATCH-source] or
  [PROBE✓] only.
- **Canonical gate set ("full gates" below means exactly this):** `make -C SheepShaver
  build-ss`; `SS_HARNESS_BATCH=1 make -C SheepShaver test-jit` AND plain
  `make -C SheepShaver test-jit` (both 353/353, legacy authoritative);
  `make -C SheepShaver/src/machine test` ALL PASS (11/11 suites);
  `make -C SheepShaver e2e-test` (122); paravirtual `make -C SheepShaver e2e` PASS +
  byte-identical (no new [NW-*]/[EXC]-shape lines on paravirtual). A task may name a
  SMALLER set only with a stated reason.
- **Standing rules**: per-task commits; struct fields appended LAST (JIT hardcodes
  offsets; clean PPC recompile after register-struct changes); stale-.o awareness
  (rom_patches/glue not covered by machine-header deps — clean-rebuild after header
  changes there); boots authorized; one shared checkout; probe-PC limit 8/run;
  SS_JIT_WATCH_ADDR is hex + needs SS_JIT_TRACE_RING=1.

## Tasks

### Task 0: NK syscall-entry recon (BINDING — gates Tasks A..C; static RE + bounded probe boots)

Pin each contract in a written addendum (`M3A-ENTRY-TABLE.md`, new section "Syscall
entry resolution (vector 0xC00) — NK-syscall-surface plan Task 0"). Budget honesty:
static RE (capstone, PPC BE, the raw + patched dumps with provenance re-established)
is the primary tool; **≤8 bounded diagnostic boots total, each ≤60s, at most 2 boots
per question before its residue status is decided.** Candidate-entry boots use
`SS_EXC_ENTRY=0x50412b1c,0xCANDIDATE` (no rebuild). Capture-only telemetry commits
allowed (full gates).

- [ ] **Provenance first**: confirm `/tmp/rom901_inventory.bin` (raw) +
  `/tmp/rom901.bin`/fresh `SS_DUMP_ROM` dump (patched) still exist and match the
  recorded md5s; re-dump if evaporated. Tag discipline [RAW-ROM]/[PATCH]/[STATIC]/[PROBE✓]
  carried unchanged.
- [ ] **(Q-S1) The live syscall handler entry + the copy question**: locate the staged
  NK's vector-0xC00 handler. Method (in order): (a) static RE of NK cold-init's
  entry-vector publication cluster — the writers of `[KDP+0x5f0]/[0x5f4]` (live values
  0x50313bf8/0x503143a0) and their sibling stores; find the analogous syscall-handler
  publication (KDP slot, NKSystemInfo field, or direct dispatch constant); (b) static RE
  around the interrupt save body (static 0x312b0c; note the sibling prologues at
  0x312a91/0x312ab4/0x312afc — `mfspr r1,SPRG0` + `mtcrf 0x3f,r7` idioms — and the
  `mtspr/bcl` chain at 0x312afc that falls INTO 0x312b0c) for the per-vector-class
  entries; (c) one probe word at live `[0xC00]` (closes the M3A vector-page gap);
  (d) live-probe the candidate publication slots at the sc wall. **Deliverable: the
  entry address in the copy the NK itself reaches (resolve the 0x503xxxxx-vs-0x504xxxxx
  ambiguity in writing — Codebase fact above), with [PROBE✓] confirmation that the
  candidate words match the static bytes.**
- [ ] **(Q-S2) The handler entry ABI** (what its real vector stub provides): registers
  at handler-body entry (the interrupt body's analogues: r6=save area? r7/r8=class/flags?
  r1 via SPRG0? r11=SRR1? r13=CR?), which KDP/ctx state it consumes (`[KDP+0x65c]` —
  currently MMCB mid-excursion; `[KDP-0x14]`; `[KDP+0x660]` bit family — R-9's sibling),
  where it saves (does it save through `[KDP+0x65c]` like the interrupt path — in which
  case the MMCB is the architecturally-correct target during the excursion, same logic
  as the W2 DEC-fence rationale — or does the syscall path skip the full save because
  args ride in registers?). **Deliverable MUST include a register → expected-value/class
  table for the handler entry; Task A's probe gate is exact conformance to it.** Also
  pin: shim-required verdict (and if required, host-side glue helper vs guest-side
  stub — recommend host-side per the DEC precedent; justify if not).
- [ ] **(Q-S3) MPLibrary's first-sc conformance vector**: `SS_PROBE_PC=0x500d638c` full
  register dump — r0 (selector), r3..r10 (args), plus the post-sc consumption: what the
  caller at SRR0=0x500d6390 does with the return (disassemble 0x500d6390ff; name the
  checked register/memory — the SS_EXC_SC=legacy 52M/s spin is the negative datum: the
  caller polls/branches on something the no-op never produced). **Deliverable: selector +
  args + the expected-return protocol (result register(s), error convention) — the
  conformance vector Task B gates on.**
- [ ] **(Q-S4) The handler exit path + environment interactions**: does the NK syscall
  service return via `rfi` (SRR0/SRR1 intact ⇒ ExcRfi resumes at +4 — verify nothing
  clobbers SRR0/1 in between) or via the scheduler restore? Verify the SRR0=+4 ownership
  matches what the NK expects (it must NOT re-increment). Record: handler MSR after
  ExcEnter (0x1040 expected — IR/DR off; does any handler leg require translation
  state?), `[0x2810]` during the handler (does the NK touch run-mode on the syscall
  path?), EE (stays 0 — SRR1=0x7072), and the **R-7 residue** (ctx SRR1 for the
  MixedMode ctx) wherever the syscall path exposes it.
- [ ] **(Q-S5) Staged-surface audit of the targeted service** (the stop-rule tripwire):
  follow the selector dispatch (static) from the handler entry to MPLibrary's specific
  service(s); enumerate every NK structure it reads/writes (KDP fields, queues,
  per-task blocks, the kernel pool 0x68FF7000+). **Verdict required in writing: the
  service runs on staged state (+ at most seed-class fixes) — or it requires unstaged
  Trampoline/kernel-init surfaces, in which case the STOP-RULE fires here** (the
  rung-5 L-class question), after the addendum.
- [ ] **Probe pack** (within the boot budget): [PROBE-S1] live `[0xC00]` + candidate
  publication slots at the sc wall; [PROBE-S2] the 0x500d638c register dump (Q-S3);
  [PROBE-S3] one candidate-entry boot `SS_EXC_ENTRY=0x50412b1c,0xCAND` — classify the
  outcome against the Q-S2 static expectation (handler-entry probe + where it walls);
  this boot is DIAGNOSTIC (no shim yet — a missing-shim divergence is signal, not
  failure).
- [ ] **Gate (the blocking-answer table):** the addendum exists; every answer tagged;
  the BLOCKING answers pinned (not residues): **Task A blocks on Q-S1 + Q-S2 (address +
  ABI/shim verdict); Task B blocks on Q-S3 + Q-S4 (conformance vector + exit path);
  Task C blocks on Q-S5's verdict.** A residue on a blocking answer invokes the
  stop-rule — no improvisation. Commit the addendum.

### Task A: resolve the entry + the pre-entry surface (env-gated `SS_NW_SC_SURFACE`, default OFF)

- [ ] Land `NW_SYSCALL_ENTRY_DEFAULT` (glue, next to the interrupt default) **active only
  under `SS_NW_SC_SURFACE=1`** during bring-up (gated off ⇒ table stays `{intr, 0}` ⇒
  the abort-with-capture baseline byte-identical). `SS_EXC_ENTRY` override precedence
  unchanged (documented).
- [ ] Implement the pre-entry surface per Q-S2's pinned ABI: the sc-side shim (host-side
  glue helper called from `execute_syscall`'s newworld arm, transcribed from the pinned
  vector-stub postconditions — the DEC-shim transcription discipline: exact offsets,
  exact order, honest upgrades documented) and/or KDP/ctx seeds (guest-side only if
  Q-S2 demands a post-NK-init assert — then: occupancy map first, verified-zero region
  at/above 0x429da0, PatchROM-time only). If Q-S2's verdict is "no shim needed —
  bare transition correct", record that explicitly and land only the entry default.
- [ ] Misuse hardening carried: unresolved-entry abort stays for the gated-off path;
  any new seed site logs one loud `[NW-SC]` line under the gate.
- [ ] **Probe sub-contract (PASS/FAIL):** with `SS_NW_SC_SURFACE=1`, the handler-entry
  probe (`SS_PROBE_PC=<entry>`) shows ≥1 visit with the register dump conforming to
  **Q-S2's expected-register table** (exact/class per row — no "sane registers").
  What the handler does AFTERWARD is diagnostic, recorded.
- [ ] **Gated-off A/B:** one boot without the env var reproduces the captured `[EXC]
  FATAL` baseline byte-identically (same line, same HB signature class).
- [ ] Gates: full gates + both sub-contracts. Commit.

### Task B: the first-sc round trip (conformance per Q-S3/Q-S4)

- [ ] **Round-trip sub-contract (PASS/FAIL), env-on:** (a) no `[EXC] FATAL` sc line;
  (b) handler-entry probe conforms (Task A's gate re-asserted); (c) **post-sc resume
  observed**: `SS_PROBE_PC=0x500d6390` ≥1 visit with r1=0x103ffb50 (the caller's stack —
  continuity) — LR/other registers recorded as diagnostic (the sc ABI may legitimately
  clobber them; gate only on the rows Q-S3/Q-S4 pinned as preserved); (d) **syscall
  result conformance**: the Q-S3-pinned return register(s)/memory hold a value of the
  pinned class at the resume (and the legacy-spin's polled condition is satisfied —
  the negative datum closed).
- [ ] **Invariant carry-over (PASS/FAIL):** rung-2 invariants still hold on this config —
  table[0] cold exactly once, guest[0]/[4] stable (WATCH), slot-15 stop unvisited,
  no reset transitions in the ring; `exc=` 4th field (deferred_native) recorded —
  any DEC delivered INSIDE the syscall handler is a falsification (EE=0 + fence both
  say impossible; if seen, stop and re-pin).
- [ ] **Diagnostic (recorded, not gates):** how many sc's MPLibrary issues, their
  selectors (bounded probe at the handler with r0 dumps), and where the boot stands
  after each — the honest map of "more syscalls or the next parcel's walls".
- [ ] If a pinned contract is falsified live: the one-iteration mechanics (stop-rule
  section) — dated addendum falsification entry, ONE bounded re-pin boot, resume;
  second falsification of the same contract escalates.
- [ ] Gates: full gates + the sub-contracts. Commit.

### Task C: acceptance + default flip (flip LAST, revert-on-red)

- [ ] **PASS/FAIL gates first, env-on (`SS_NW_SC_SURFACE=1`):** (a) full gates;
  (b) Task A handler-entry conformance; (c) Task B round-trip + result conformance;
  (d) rung-2 invariant carry-over (cold-once, guest[0]/[4], slot-15, switch round trip
  itself still green: TVector visit + completion resume per the rung-2 Task Y recipe);
  (e) gated-off boot reproduces the sc-wall baseline byte-identically.
- [ ] **Fix budget:** telemetry/capture commits freely; at most ONE small in-scope fix
  iteration per falsified contract, full gates re-run after any fix.
- [ ] **THEN flip** `SS_NW_SC_SURFACE` to the newworld profile default (opt-out `=0`
  kept, polarity mirroring SS_NW_MM_SWITCH) and re-run (a)–(e) with NO env vars.
  **Any gate failure after the flip ⇒ the flip is REVERTED in the same task (machinery
  stays env-gated), the failure recorded — the milestone does not ship default-on with
  red gates.**
- [ ] **DIAGNOSTIC OUTCOMES (recorded, NOT gates):** does MPLibrary's init RETURN
  (ring: the excursion's 68k resume at the completion-written `[saveblk+0x3c]` +
  continued CFM caller-region execution past it)? Does the parcel chain reach the
  CodeFragmentMgr parcel? Capture the new frontier with the full HB/CUDA/ring baseline,
  named honestly: the NEXT milestone's opening evidence — likely more syscalls
  (new selectors), the next parcel's walls, or the EE-enable/interrupt chain. Stop-rule
  trigger 2 applies: capture and stop, no staging beyond abort-capture for new classes.
- [ ] Record results: M3A-ENTRY-TABLE.md (the syscall_entry row RESOLVED + results
  section — the M3a descope formally closed), M6A-WAVE2-SHIM-RECON.md (frontier
  update). Commit.

### Task Z: docs

- [ ] DIAGNOSTICS.md: `SS_NW_SC_SURFACE` (default + opt-out + interaction with
  `SS_EXC_ENTRY`/`SS_EXC_SC=legacy` — legacy's diagnostic disposition re-stated);
  CHANGELOG (the first resolved guest syscall — acceptance numbers); MACHINE-LAYER-PLAN
  header + M6 row (+ M3a carry-forward closure note); ROADMAP cross-check; LEARNINGS
  (whatever the recon taught — at minimum the copy-ambiguity resolution and the
  shim-asymmetry verdict); cross-tracker grep for stale "syscall_entry unresolved"/
  "descope active"/"vector 0xC00 unstaged" claims (historical sections stay per the
  rule). Commit.

## Stop-rule (triggers per MACHINE-LAYER-PLAN §9)

1. If Task 0 (esp. Q-S5) shows the syscall path requires **any NK surface beyond entry
   resolution + the pinned pre-entry shim/seeds** (an unstaged service table, per-task
   kernel structures only the real Trampoline/kernel-init builds, a vector-page-code
   requirement, or an M-class service body that is genuinely absent from the staged
   image), STOP after the addendum and re-scope — the recon itself is the tripwire
   (the rung-5 L-class question, named in advance).
2. If Task C's diagnostic shows MPLibrary returns but the chain dies on the NEXT
   surface (another selector, the CFM parcel, the EE chain), that is the NEXT
   milestone's named frontier — capture and stop; no staging beyond the existing
   abort-with-capture for new exception/service classes.
3. A residue on a BLOCKING Task-0 answer (the blocking-answer table) invokes trigger
   1's re-scope, not improvisation.

Within tasks — the one-iteration rule, operationalized: if a pinned contract is
falsified live, (a) reopen the addendum with a dated falsification entry; (b) ONE
additional bounded probe boot to re-pin; (c) resume the falsified task with the
corrected contract. A SECOND falsification of the same contract escalates to the
stop-rule (re-plan, not patch-on-patch).

## Self-review record

Spec coverage: all coordinator inputs consumed (M3A entry-table contract + carry-forward,
HANDOFF §2.8 as superseded background, the rung-2 design doc's syscall-adjacent facts
(R-7, [KDP-0x14], [0x65c]-during-excursion, [0x2810] fence, EE/MSR), WAVE2 frontier
capture, exc_core/glue/ppc-execute live sites, MACHINE-LAYER-PLAN §2d placement). The
architectural shape follows the machine-layer philosophy: real NK code handles the sc;
our obligation is entry resolution + the vector-stub surface, exactly the M3a KDP-shim
idiom one vector over. Every open design question is a Task-0 recon item with a
blocking-answer table; gates are falsifiable in advance (expected-register tables, the
conformance vector, byte-identical off-boots) and separated from boot-advancement
diagnostics. Env-gated bring-up with flip-last + revert-on-red mirrors rung 2. Known
tensions flagged for the red team: (1) the 0x503xxxxx/0x504xxxxx copy ambiguity
(M3A's +0x100000 staged-copy claim vs rung-2's live NK publications — Q-S1); (2) M3A's
vector-page-empty evidence never covered 0xC00; (3) the sc path's no-shim asymmetry vs
the DEC path (Q-S2 decides; §2d constrains the fix site); (4) HANDOFF §2.8's sc framing
is pre-M3a and must not be treated as spec; (5) the legacy-spin datum is load-bearing
for Q-S3's return-protocol pin. Sequencing: strictly one task at a time (shared
ppc-execute/glue/rom_patches sites). Wave-2 backlog stays parallel.

## Red-team record

*(empty — a red-team round follows this draft; findings to be folded as rev 2 markers)*
