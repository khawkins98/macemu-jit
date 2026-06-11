# DSAT stack-underflow wall — pre-milestone recon (2026-06-11)

> Next-milestone recon for the frontier captured by FE1F Task B
> (M6A-ONGOING-ENTRY-DESIGN.md "Task B results" / "NEW FRONTIER", commit 9443a568;
> re-confirmed post-flip in Task C). READ-ONLY task: no source edits.
> **Boots: 2** slot-protocol boots, both 60 s, both ending in the frontier SIGSEGV
> (the capture): `dsat-wall-recon` (/tmp/ss-slots/slot0/runs/20260611-200253.78989),
> `dsat-wall-recon2` (…/20260611-200600.79388). Static RE against the manifest-checked
> dumps (`tools/dump-manifest.sh --check` clean; raw 7b1378be…, patched e432df64…).
> The e3a0..e500 (e3e0 routine) and 31aef0..31d7c0 (NK service) regions used below are
> **raw==patched byte-identical** (verified this session).
> Preserved artifact: boot-1 crash trace ring copied to `/tmp/dsat_ring_boot1.txt`
> (262,144 records, window #4,218,315..#4,480,459 — ephemeral; re-derivable, boots are
> record-for-record deterministic).

## TL;DR — the verdicts

- **(Q-D1) Error 10 is a REAL line-1111 fetch caused by a 68k instruction-stream
  DESYNC, not a bad service return.** The DR dispatched the word at ROM 0xe412 —
  which is the *pc-relative displacement word* (`ff8e`) of `lea.l $5000e3a0(pc),a2`
  at 0xe410 — as an opcode. 0xFF8E is F-line → vector 0x2c → SysError ID 10.
  [PROBE✓] + [PATCH=RAW]. The selector-$36 round trip itself is **CLEAN end-to-end**
  (service fired, args correct, type check passed, success tail, status 0):
  Task B's "suspect = the $36 callout's return path" is **retired in its
  return-predicate form** — what remains suspect is the *resume/dispatch machinery*
  in the window after the $36 callout (see the open mechanism question).
- **(Q-D2) The load-bearing milestone is (i)': fix the post-callout-window PC
  desync** so execution never lands at 0xe412. (ii) — a graceful SysError/DSAT
  surface — is NOT the milestone: a correct boot never enters this path, and the
  DSAT machinery's own death (recursion → RAMBase underflow) is a *guest-side
  consequence of unseeded DS globals*, not an emulator fault to fix now. What (ii)
  would need is recorded below for the day a real guest error recurs.
- **(Q-D3) A7=0x100000a0 is the ENDPOINT of a ~12,000-iteration recursive stack
  descent, not a loaded fallback stack.** The first SysError runs on the normal
  boot stack (frame at 0x103ffa4c [WATCH✓]). The DS-alert path then recurses
  (each pass `suba.w #$15a,a7` + frames), grinding A7 from 0x103ffaxx through
  0x100f6xxx down to 0x100000a0 and finally below RAMBase → SIGSEGV. Task B's
  "~1.09M-record wait-spin on bset $C2C" reading is **retired**: the raise→crash
  gap ≈ 12k recursion iterations × ~90 records ≈ 1.09M records. [RING✓]
- **(Q-D4) Live confirmations: done in 2 boots** (probe on the vector-0x2c raise
  entry + the $36 NK service entry; watches on the actual frame words). Evidence
  inline below.

## The pinned chain (evidence-tagged)

### 1. The $36 round trip is clean

- **Service entry fires with correct args** [PROBE✓ boot 1+2]:
  `[PROBE 0x5031d6b4 visit=1] r3=0x00120001 r4=0x00000007` — r3 = the $31-returned
  handle (A0), r4 = D1 = index **7** (= the e3e0 table-entry id field `$4(a2)`;
  matches Task B's corrected id=2→index-7 slot recipe).
- **What $36 DOES** [STATIC, file 0x31d6b4, raw==patched]: handle lookup via
  0x50325380 (directory at [r1-0xa98]; handle = (index<<16)|generation, checks
  type byte + generation halfword, returns r8=object ptr, r9=type) →
  `cmpwi r9,9; bne 0x5031b0a4` (type must be 9 = EVNT) → bounds-check r4 vs 1..8
  (out-of-range/0 falls back to index 1) → **read-modify-write obj+0x18:
  `ori r16,r16,0x10` + insert index into bits 28..31** → success tail 0x5031af38
  (`li r3,0`) → common return 0x5031b124. So $36 = "arm/bind EVNT-object slot
  index": input handle at $c(a6)→A0→r3, index D1→r4; output = status D0 only
  (the f260 stub stores D0 to $10(a6); it has NO A0 store-through, unlike f240).
- **Failure paths all return a status, never raise** [STATIC]: every error tail
  in 0x5031af0c..0x5031b0c8 sets r3 to an MPLibrary-class error
  (-0x7273=kMPInvalidIDErr −29299 for type/lookup mismatch; the −0x726x/−0x32
  family for the others) and branches to the same 0x5031b124 return. Note
  e3e0's own fallback error 0xffff8d8e (file 0xe3fa) is the same MP error family —
  this whole cluster is MPLibrary CFM-prep talking to NK MP services.
- **Live outcome = success** [PROBE✓ boot 1]: probes armed on the type-fail tail
  (0x5031b0a4) and the in-service success block (0x5031d6fc) — b0a4 never fired;
  d6b4 fired; no error path observed; the boot's [EXC] counters show no third
  PROGRAM delivery and the post-$36 68k stream reached the e3e0 epilogue region
  (frame address math, §2). The lookup passed ⟹ the $31 handle 0x00120001 resolved
  to a live type-9 EVNT object ⟹ the $31 registration registered type 9 as needed.

### 2. The raise: a real F-line fetch at a mid-instruction PC

- **The raise entry** [PROBE✓ boot 2, the ONLY visit of the whole boot]:
  `[PROBE 0x5046d760 visit=1] r24=0x5000e414 r5=0x5046d780 r27=0xffffb292
  r1=0x103ffa54` — 0x5046d760 (static 0x36d760) is the vector-0x2c raise entry
  (T-M3). r24 = word+2 convention (calibrated against the FE1F probes) ⟹ the DR
  was executing the word at **0xe412**; the prefetched r27=0xb292 = the word at
  0xe414 (`cmp.l (a2),d1`) confirms stream alignment at e412 exactly.
- **The word at 0xe412 is `ff8e`** [RAW-ROM=PATCH]: the second word of
  `45fa ff8e` = `lea.l $5000e3a0(pc),a2` at 0xe410 — e3e0's id-table base load,
  executed once per e3e0 invocation just before the id-match dbra loop
  (0xe414/0xe416/0xe418/0xe41c). Dispatched as an opcode, 0xFF8E is line-1111.
- **The raise body mechanics** [STATIC 0x36d760..0x36d800, raw==patched]:
  entries 0x36d760 (li r6,0x2c) / 0x36d770 (li r6,0x28) first `blrl` to a
  continuation hook (r5), then the shared meat at 0x36d780: build SR image from
  r25/CR/XER, `addi r24,r24,-2` (back to the faulting word), supervisor-switch
  (user mode: save USP to ECB+0x48, load SSP from ECB+0x4c / ECB+0x50 variant;
  already-super: stay on current r1), push the 68010-style frame
  `sthu r4,-8(r1)` (SR) + `stw r24,2(r1)` (PC) + `sth r6,6(r1)` (vector offset),
  fetch the new 68k PC from the vector table (`lwzx r24,r28,r7`), resume dispatch.
- **The frame, observed** [WATCH✓ boot 2]: `[WATCH] pc=5046d7fc addr=103ffa50
  value=e412002c (record #3391679 … r24=500049c4)` — frame at A7=0x103ffa4c on
  the NORMAL boot stack: PC=0x5000e412, vector=0x002c; r24 already redirected to
  the vector stub ladder (0x49c4 = the ID-10 stub). The 68k was supervisor; no
  ECB SSP involved at this first raise.
- **SysError consumes it** [WATCH✓ both boots + STATIC 0x4960..0x4b00]: the
  ladder stub `bsr.b 0x4970`; 0x4970 = `sr:=2700`, movem all regs → $0C30
  (first entry only, gated on $BFF bit 7), ID from the bsr return address, pops
  frame SR → $C74 and frame PC → $C70, joins the SysError body at 0x49f0.
  `[WATCH] addr=c70 value=5000e448 record #3391708` (both boots, deterministic).

### 3. Corrections to the Task B frontier capture

- **"Saved faulting PC 0x5000e448" is NOT the raise PC.** The frame the raiser
  wrote says PC=0x5000e412 [WATCH✓ #3391679]; $C70 received 0x5000e448 29 records
  later [WATCH✓ #3391708]. The 0x36 delta / the exact unwind that produced e448
  in the pop is **unpinned** (residue R1 below — the watched frame-lo word still
  read e412002c at pop time, so the popped-long path did not read the watched
  word the way the static stub-walk predicts). Task B's reading of 0xe448 as
  "the $36-stub argument push site, convention-skewed" is retired — the raise
  site is 0xe412, two bytes into the lea at 0xe410.
- **The "~1.09M-record wait-spin" is a RECURSION** [RING✓ /tmp/dsat_ring_boot1.txt]:
  the DS-alert draw path re-raises (cascading vector-0x2c frames observed at
  successive PCs — e.g. frame PC=0x50004a34 at record #3391732 [WATCH✓], then a
  stream of frames pushed at 0x100000a0-region in the final records), each pass
  re-entering SysError and running `suba.w #$15a,a7` (transition instruction
  observed in-ring: r24=0x50004aca takes sp 0x1000020a → 0x100000b0). Stack walks
  0x103ffaxx → 0x100f6xxx → 0x100000a0 → below RAMBase → SIGSEGV ea=0x0fffff42
  at the DR push site 0x504661a0 (unchanged crash signature).
- **d6=0x40 / d7=0x36 residues pinned structurally** [RING✓]: the DS path reads
  them fresh each pass — `move.w $af0.w,d6` at 0x4aa4 (d6 = current DSErrCode
  word) and `move.l $2ba.w,d7` at 0x4aaa (d7 = [$2BA] = DSAlertTab-class lowmem
  long, content 0x36 = unseeded residue; its nonzero-ness is exactly what selects
  the alert-draw arm at 0x4ab6). The "selector-$36-hued" reading is a coincidence
  of values; d6's 0x40 is $AF0's then-current content during the late cascade,
  resolving the Task C "residue source unpinned" item.
- **A7=0x100000a0 provenance** [WATCH✓ boot 1 + RING✓]: 0x100000a0/0x100000a4
  are initialized as a self-referential queue/list header at record #1031247
  (r24=0x50036fba, early boot, normal init). The DS recursion's stack descent
  eventually passes THROUGH that region (first stack-use of it at #4480370,
  r24=0x5000dfa2 — the mini A-trap dispatcher pushing). No code loads
  A7:=0x100000a0; it is arrived at by subtraction. The earlier "boot ran with
  A7=0x2600-class" state is unrelated (different boot phase).

### 4. What surrounds the desync window (the open mechanism question)

Timeline (boot 2, record numbers; boots deterministic ±1):

- #3390830 — e3e0 entered (r24=0x5000e3e2 in a watch record), normal stack.
- #3391078/79 — the $31 round trip completes; ExpandMem slot := 0x00120001.
- #3391298 — ROM-PPC (TVector-world, blocks 0x500ee0c0/0x500ee13c/0x500ee5a0)
  excursions run **between the fill and the raise**, doing legal stack writes at
  0x103ffa50-region (sp=0x103ffadx, r24 repurposed =1). The armed boot's extra sc
  deliveries (0x0f ×2 extra, 0x42, 0x50, 0x4d) and the $36 PROGRAM delivery all
  live in this window — i.e. multiple 68k↔PPC world transitions.
- #~3391677/79 — the raise fires at 0xe412 with A7=0x103ffa54 (a plausible
  e3e0-epilogue/re-entry stack level; the e3e0 frame's $36-call sp was 0x103ffa34).
- #3391708 — $C70 := 0x5000e448; #3391720 — $AF0 := 0x000A; then the recursion.

For the 68k to execute at 0xe412 it must have been *re-dispatched* at e410+2:
legitimate control flow into the e41x loop only enters at e410 (fall-through,
once per invocation) or e414 (dbra/beq targets). The ~350-record gap between the
$36 service and the raise is consistent with: e3e0 success-path stores
(0xe454..0xe48c), the e490 epilogue, return to the caller, and a re-entry of
e3e0 for the NEXT id (the caller iterates the 5-entry table at 0xe3a0) — but the
JIT trace-ring window (256K records, fixed) does not cover #339xxxx in a
SIGSEGV-terminated boot, so the precise re-dispatch site is **not directly
observed**. Candidate mechanisms, for the milestone's Task 0 to discriminate:

1. **Resume-state skew after a world transition**: the glue/NK context
   save-restore around one of the window's deliveries (PROGRAM #2 or the sc
   excursions) restores the DR dispatch pipeline (r24/r27/r29/cr2) such that the
   next dispatch runs one word ahead (r24 transiently points at extension words
   *between* the per-word `lhau` fetches of a multi-word instruction; a delivery
   captured at the wrong micro-moment re-enters the dispatcher mid-instruction).
2. **DR block-resolution off-by-2** for the dbra/beq loop edges after the
   window's state churn (target e414 resolved as e412).
3. **A 68k-side control-flow value corrupted on the stack** (a return address or
   frame link clobbered by the window's PPC stack writes — the observed
   0x500eexxx writes were below-sp scratch and look legal, but the
   re-entry/return addresses for the caller live in that neighborhood).

Note against (1)-at-the-$36-twi specifically: the twi fires between
instructions (the FE1F body's blrl), and the identical $31 path resumed
perfectly — so a desync of the *delivery* path itself must explain why the
second transit (or an sc in the same window) differs.

## Milestone-shape recommendation

**(i)' — fix the desync.** The milestone is a correctness fix in the
delivery/resume machinery (or DR dispatch-state restoration) for the
post-FE1F-callout window, with the acceptance gate "the boot passes 0xe410
intact, no SysError ID 10, frontier moves past e3e0's caller". It is NOT a $36
semantics fix (the service is correct and clean) and NOT a DSAT fix.

**(ii) recorded, not scheduled**: any future genuine guest error will re-enter
the same broken DSAT path (unseeded $2BA/DSAlertTab world + the recursive
alert-draw descent + RAMBase underflow). If/when that day comes, the surface
needed is small: either seed/serve the DS-alert world far enough to draw once
and halt, or an emulator-side backstop (detect SysError re-entrancy / A7 below
RAMBase+margin and freeze with telemetry instead of SIGSEGV). The crash-path
telemetry already captures everything needed for diagnosis today; (ii) buys
diagnosability of FUTURE guest errors, not boot progress.

## Task-0 question list for the milestone plan

1. **(T0-A) Pin the re-dispatch site**: capture the 68k instruction stream
   records #3391300..#3391700. Tooling gap: JIT trace ring is fixed at 256K
   records (`JIT_RING_SIZE`, ppc-cpu.cpp:829) and the window dies at the
   SIGSEGV tail; the r24-ring atexit skips on SIGSEGV. Cheapest paths:
   (a) a watch-triggered ring freeze (stop recording at first $C70 hit), or
   (b) a one-off diagnostic bump of JIT_RING_SIZE to 4M records, or
   (c) SS_INTERP_RING-class capture if the window can be forced interpreted.
2. **(T0-B) Was e3e0 re-entered?** A counter (exc=-tuple idiom) or watch on a
   uniquely-second-invocation observable (e.g. the next id's ExpandMem slot
   word) discriminates "desync during invocation 1's epilogue" vs "desync
   during invocation 2's prologue".
3. **(T0-C) Audit the delivery/restore path** (glue 0x700 surface + NK return
   0x503242dc → 0x5031b124 tail) for dispatch-pipeline state (r24/r27/r29/cr2)
   fidelity — statically first, then a targeted probe pair at the $36 resume
   (lr=0x5046db6c block) vs the $31 resume, diffing the restored r24/r27.
4. **(T0-D) Which transition is the trigger?** The window contains PROGRAM #2
   AND sc selectors 0x50/0x4d/0x42/0x0f. A bisect boot with SS_NW_FE1F_SURFACE=0
   is NOT available (the park precedes the window); instead instrument the sc
   surface's resume identically and check whether the desync follows the $36
   delivery or a specific sc.
5. **(T0-E, small)** Close residue R1 (the e412→e448 $C70 delta): watch the
   frame words 0x103ffa4c AND 0x103ffa4e-spanning words plus $C74, with the
   T0-A ring window, to see the exact unwind. Diagnostic value only.

## Residues (named, not load-bearing for the shape)

- **R1**: the $C70-popped value 0x5000e448 vs the frame's written PC 0x5000e412
  (delta 0x36) — exact unwind mechanics unpinned (one-vs-two raises in the
  29-record gap, or a pop-offset subtlety in the 0x4970 stub path). Does not
  change the verdict: both values are inside e3e0 and both postdate the clean
  $36 round trip.
- **R2**: the precise re-dispatch mechanism (Task-0 #1-#4 above) — THE milestone
  question.
- **R3**: the cascade's later raises (frame PCs 0x50004a34, 0x5007723a, …)
  enter the raise meat without re-visiting the 0x5046d760 entry (d760 printed
  visit=1 only, all boot): the other F-word slots' entry routing is uncharted
  (fine — death-throes behavior).
- **R4**: [$120] (the SysError hook slot read at 0x4a12) content and the $BFF
  re-entrancy flag's effect on the second+ SysError passes — only relevant to
  (ii).
- **R5**: the DS-phase intermediate stack at 0x100f6xxx (the recursion passes
  through at least two stack neighborhoods before 0x100000a0) — endpoint math
  pinned, the per-iteration frame chaining not walked.

## Instrument notes (carried forward)

- `0x5046d760` (vector-0x2c raise entry) is a LOW-visit probe (fired once all
  boot) — the right hook for line-1111 raises; `0x5046d780`/`0x5046d7c8` are
  useless for this (shared with the A-line raise storm — d780 hit ≥10^4 visits).
- The first-raise frame lives at 0x103ffa50/0x103ffa54 (watch-able, HEX,
  needs SS_JIT_TRACE_RING=1); deterministic record anchors: frame #3391679,
  $C70 #3391708, $AF0 #3391720, e3e0 entry #3390830, slot fill #3391078/79.
- r24=word+2 convention re-calibrated live (FE1F f246 → r24=f248; raise e412 →
  r24=e414 with r27 = next word).
- The 68k trace-ring "sp=" column IS A7 (r1); the "a7=" column reads 0.

---

## Task 0 — the re-dispatch mechanism pinned (68k-pc-desync plan) — 2026-06-11

> **Status:** BINDING root-cause recon COMPLETE — all blocking answers pinned, no
> residue on a blocking row. **Boots: 4 of the ≤8 budget** (slot protocol, one
> big-ring at a time per F13; slot registry checked clean before each):
> B1 `desync-task0-qds1` (big-ring 0x400000 + window clip + probes + watches,
> rundir 20260611-214647.7312), B2 `desync-task0-e380-writer` (e380 watch,
> 20260611-215429.8756), B3 `desync-task0-ea-capture` (extended PROBE68K,
> 20260611-220030.10815), B4 `desync-task0-r0-at-twi` (r0 probes,
> 20260611-220653.11423). Provenance: `tools/dump-manifest.sh --check` PASSED
> (raw 7b1378be…, patched e432df64…). Ring artifact preserved:
> `/tmp/desync_t0_ring_boot1.txt` (#3390800..#3391999, ephemeral, re-derivable).
> Capture-only telemetry commit (F5/F7-compliant, ppc-cpu.cpp ONLY): PROBE68K
> dump extended with the DR pipeline temps r0/r3/r4/r5/r6/r7/r30.

### TL;DR — the verdict in one paragraph

**The desync is an r0-invariant poison, not a resume-PC skew.** The DR (the
ROM's 68k dynamic recompiler) maintains a standing invariant **r0 ≡ 0** in the
emulator world — its flag-setting writeback idiom is `addco. rX,rX,r0`
(0x7c840415 at mirror 0x50460c64 [STATIC]; add-zero sets N/Z, clears V/C). The
FE1F native callout marshalls the NK service selector **into r0 by design**
(`mr r0,r8` at raw 0x5046db48 [STATIC, raw==patched]) before `blrl` into the
entry-vector slot's `twi`. Our M6A PROGRAM delivery turns that twi into an
exception: the NK prologue 0x313d40 **saves the in-flight r0=0x36 into the 68k
world's ctx r0-slot (+0x104)**. The callout RETURN is clean (the DR's own
`li r0,0` at 0x5046db7c re-asserts the invariant — both PROGRAM resumes verified
clean [PROBE✓ B4]). But the NK's save-and-switch (0x312b0c) deliberately does
NOT save r0 (the invariant is its contract), so the ctx slot keeps the stale
0x36 — and the merged restore tail (raw rfi or patched bctr, same reloads)
**re-poisons r0=0x36 on every subsequent switch-in to the 68k world**, where no
`li r0,0` exists. The first poisoned `addco.`-class writeback after the second
post-$36 switch-in is the e388 selector-shim's jump-table load:
0x5c + 0x36 = 0x92 → `jmp (-0x14,pc,d0.w)` lands at e380+0x92 = **0xe412** →
line-1111 → SysError 10. On real hardware the slot-8 callout is a
parcels-patched direct call (no exception, no ctx save) — the placeholder-twi
delivery is what introduces the poisoned save. **The patched chain constants
(trap_return / m68k_excp_tbl / sprg3) are all EXONERATED.**

### Q-DS1 [RING✓ B1] — the pinned re-dispatch record

Ring window #3390800..#3391999 walked record-by-record (424 r24 transitions):

- Invocation 1 (id table run): trampoline → **e388 selector shim** →
  `cmpi.w #1,d0; bcc.s e396; move.w (-0x10,pc,d0.w*2),d0; jmp (-0x14,pc,d0.w)`
  [RAW==PATCH bytes `0c40 0001 6408 303b 02f0 4efb 00ec`] — table word at e380
  = 0x005c → e3dc (`link a6`) → e3e0 body → $31 callout (resume r24=f248,
  clean) → slot fill → $36 callout (resume r24=f270, clean) → success stores
  e454..e48c (clean — r0 re-zeroed at db7c) → e490/e492 epilogue → rts to
  trampoline 0x103ffa92 (#3391199) → switch-out (#3391213).
- Interlude: switch-in #1 (#3391314–19) → short 68k routine 0x49392..0x493e0
  (d0='bbox' Gestalt-class) → switch-out (#3391349). Ran with r0 already
  poisoned (0x36) but its writebacks fold flags (no addco. consumer) — survived
  by luck (residue R-DS2).
- **Switch-in #2 (#3391655–60: bounce 50312cb4 → patched tail 503244d4 → fast
  exit bctr 50324524 → slot-exit 5046e1a0) → e388 shim re-entry for the NEXT
  caller iteration (#3391661, r24=e38a) — IDENTICAL prologue records and
  prefetches to invocation 1 — and the SAME dispatch block 50467cfc that
  produced r24=e3de at #3390826 produces r24=e414 at #3391672.** Last good
  r24 = e396 (#3391669); first bad r24 = e414 (#3391672); what immediately
  precedes = **the switch-in at #3391655–60** (a world transition — suspects
  1–3 class), with PURE DR flow in between (no hook bypass: full raw-record
  walk, F10 honored). Raise at #3391677; ID-10 stub #3391678.

### Q-DS2 [PROBE✓ B1/B3] — invocation census

`SS_PROBE_68K=0x5000e412` legit-class matches by the raise: **1** (invocation 1
only; the second match is the raise body's own `addi r24,-2`, block 5046d7cc,
ctr=0 — distinguishable). The desync fires in **invocation 2's PROLOGUE — the
e388 selector shim's jmp — BEFORE e3e0 is re-entered** (refines the recon's
"epilogue vs re-entry" framing: neither; the shim never reached e3e0). The
caller's table run died at iteration 2 of the expected ≥2 (5-entry table).
**Post-fix expected class (F8): ≥2, presumably 5** legit e412 transitions
(one per table id) — with the SS_PROBE_68K chaining under-count caveat; a
row-(d) gate boot may use SS_JIT_NO_CHAIN=1 accepting renumbered anchors.

### Q-DS3 [STATIC+PROBE✓+RING✓] — patched chain-constants audit

Every exit leg the window's deliveries take was walked live in the ring
(PROGRAM #2's full delivery→resume path #3391096..#3391154; both switch-ins;
the sc family by census):

| Constant | Verdict | Evidence |
|---|---|---|
| `trap_return` (3244d4 tail + 324524 fast-exit bctr) | **EXONERATED** | The patched bctr tail reloads r0/r6–r13 from ctx exactly as the raw merged-rfi tail would (F2's one-rfi frame held); resume PCs correct on every walked leg (callout returns → db6c; switch-ins → 5046e1a0). The r0 poison rides the CTX SLOT, which raw rfi would reload identically. No MSR/EE delta involved (EE never rose; delivered_dec=0). |
| `m68k_excp_tbl` ([0x2810]:=0 fence write) | **EXONERATED** | On the walked paths only as the dispatcher's fence write; no vector-regime consumption in the window. |
| `sprg3`/`sprg3_mq` | **EXONERATED** | Vector-table stubs unconsumed in the window. |
| **(IMPLICATED) the M6A FE1F twi-callout PROGRAM delivery** (our construction) | **THE mechanism** | Prologue 0x313d40 saves live r0 (=selector, the DR's own ABI at db48) → ctx+0x104 [PROBE✓ B4: r0=0x31/0x36 at twi visits 1/2, at 0x50314700 entry, and at the db6c resumes]; save-and-switch 0x312b0c omits r0 from its save set [STATIC W2S-1]; later switch-in tail reloads re-poison r0; the DR's `li r0,0` (db7c) exists only on the callout-return leg [STATIC raw==patched]. |

**The differential mechanism (the M3A constraint satisfied):** the 8 pre-window
sc resumes and the $31/$36 callout returns all survived because (a) sc callers
own r0=selector by ABI, and (b) the callout-return leg re-asserts r0=0 at db7c.
The poison only bites on a **switch-in** that follows a twi delivery with no
intervening rewrite of ctx+0x104 — a path combination that first exists in the
FE1F window. The F4 mid-tail-delivery CTR/LR question: **no mid-tail delivery
occurred** [RING✓ — every tail traversal walked]; CTR/LR fidelity confirmed on
all legs; the corrupted state is r0, not CTR/LR/SRR.

### Q-DS4 [STATIC+RING✓+PROBE✓] — the across-delivery register contract

- **r24/r27/r29/cr2 + CTR/LR**: ride the 0x313d40 ctx save (r0,r7–r13 →
  ctx+0x104/0x13c..0x16c; r10:=SRR0, r11:=SRR1, r12:=SPRG2, r13:=CR) plus the
  save-and-switch set (r17–r31, r2–r5, XER/CTR) — verified live-correct across
  both PROGRAM round trips and both world-switch round trips (the probes and
  ring show r24=f248/f270 resumes, correct prefetch r27, correct dispatch r29,
  correct CR). **The ONE register outside every restore contract is r0** —
  protected by invariant, not by slot — and the twi delivery is what breaks
  the invariant's precondition.
- **Trigger transition: the second post-$36 world switch-in (#3391655–60
  class)** — NOT a delivery resume. "No delivery implicated" in the resume
  sense; the $36 PROGRAM delivery is implicated as the SAVE that armed the
  poison.

### Q-DS5 [RING✓] — stack neighborhood: CLEAN

The e3e0 caller's return path executed correctly: the e49e `rts` (#3391198–99)
returned to the MixedMode trampoline 0x103ffa92, sp-relative flow consistent
throughout the window; the excursions' 0x500eexxx-region stack writes touched
no live return address or frame link (the desync needed no stack value — the
corrupt jmp operand came from the poisoned register add). Suspect 3 retired.

### Q-DS6 [WATCH✓+mechanism] — residue R1 closed

`[$C70]:=0x5000e448` vs frame PC e412: **e448 = e412 + 0x36 — the same r0
poison.** The SysError stub's frame-PC pop goes through a poisoned
addco.-class writeback before the $C70 store (writer r24=0x5000499a,
#3391708). The 0x36 delta family (jmp target e412=e3dc+0x36, table word
0x92=0x5c+0x36, $C70 e448=e412+0x36) is one mechanism appearing three times.
$C74 ← 0x2700 SR pop conforms. R1 is mechanism-closed (the individual pop
instruction not separately disassembled — residue R-DS3, diagnostic only).

### The blocking-answer table (Task 0 gate)

| Blocking answer | Status |
|---|---|
| Q-DS1 pinned record | **PINNED** — #3391672 (block 50467cfc, r24 e396→e414), preceded by switch-in #3391655–60; pure DR flow between |
| Implicated mechanism | **PINNED** — DR r0≡0 invariant poisoned via ctx+0x104 (twi-delivery save + no-r0 switch save + tail reload); patched constants exonerated |
| Fix-shape decision (Task A) | **DECIDED — bctr-preserving (F1 default; NO coordinator sign-off needed: no EE/MSR/rfi semantics change).** Re-assert the DR's r0≡0 invariant on the switch-in path, NW-gated: preferred site = the DR slot-exit re-entry stub(s) (5046e1a0-family; exact stub survey + verify-EXPECTED-first at Task A start) inserting `li r0,0` (Apple's own db7c idiom), OR the equivalent ctx+0x104 pre-reload zeroing on the NW switch-in leg. Gate `SS_NW_DESYNC_FIX=1` default OFF (mechanism name candidate: `SS_NW_DR_R0_INVARIANT`). Paravirtual reach: NONE (all inside MachineProfileIsNewWorld()/PatchROM_NW gating; legacy patch bodies untouched) ⇒ inertness argument + gated-off byte-identical A/B boot substitutes for paravirtual e2e, stated per commit. |
| Falsifiable fix predicate (Task B, F11) | **PINNED, register/record form:** gate ON — (a) PROBE68K@0x5000e394 match-2 shows dr-tmp r0=0x00000000, r4=0x0000005c, d0=0x5c (baseline: 0x36/0x92/0x92); (b) the #3391672-class record shows r24=0x5000e3de (not e414); 0x5046d760 visits=0 all boot; no [$C70] write; no [$AF0]:=0x000A (canonical form per F12); (c) SS_PROBE_68K=0x5000e412 legit count ≥2 (expected class 5; F8 chaining caveat); (d) gate OFF — byte-identical DSAT baseline (anchors #3391679/#3391708/#3391719–20 class, crash sig ea=0x40000fffff42 @ 0x504661a0, counter classes delivered_sc=13/delivered_program=2). Rows (a)–(c) scoped through the formerly-failing window per F9. |
| Regression scope (Task C) | **ENUMERATED:** (1) sc resume conformance (per-delivery: handler 0x50314ac0, selector census class) — fix is DR-entry-only, sc paths untouched; (2) FE1F $31/$36 round trip (slot fill 0x00120001 class, db7c leg, return predicate); (3) W2's EE facts — trap_return untouched, the EE-parker fact and the XLM_IRQ_NEST −1/delivery drift stand unchanged; (4) warm switch-back nest ±1 balance signature; (5) paravirtual byte-identity (gated-off A/B); (6) cold-once trampoline WATCH pair (#4666 class); (7) post-switch-in guest code (interlude class) now runs r0=0 — strictly less poisoned; (8) if the ctx+0x104-zeroing variant is chosen: verify no NK consumer reads the emulator-world saved r0 slot (debugger/inspection paths) before landing. |

### New residues (named, none blocking)

- **R-DS1**: which ctx the window's excursion sc's (0x42/0x50/0x4d) save into —
  the slot held 0x36 (not 0x4d) at switch-in #2, so the excursions save
  elsewhere ([KDP-0x14] differs during excursions); bookkeeping only.
- **R-DS2**: the 'bbox' interlude ran r0-poisoned and survived (its writebacks
  fold flags / no addco. consumer) — recorded as the reason switch-in #1
  didn't already crash the boot.
- **R-DS3**: R1's exact pop instruction not individually disassembled
  (mechanism-level closure above suffices).
- **R-DS4**: the d1 entry-state difference between invocations (0xffffffff vs
  0) — not consumed by the shim; unexplained, immaterial.

## Task A — the fix landed (68k-pc-desync plan) — 2026-06-11

> **Status:** the r0-invariant fix is IN, env-gated `SS_NW_DR_R0_INVARIANT`
> (the Task-0 candidate name is hereby BOUND — one name, used consistently;
> `MachineEnvFlag` bring-up polarity, default OFF). Boots this task: ON-1
> (big-ring + e394 probe + d760 probe + af0/c70 watches, rundir
> 20260611-222707.24448), ON-2/ON-2b (e412 census, plain + SS_JIT_NO_CHAIN=1,
> 20260611-223220.24906 / 20260611-223337.25113), ON-3 (e3de witness,
> 20260611-223441.25219), ON-4 (ring-form window via SS_JIT_RING_DUMP_AT_PC=
> 500f49c8, 20260611-223616.25353), OFF-1 (gated-off A/B, 20260611-223750.25488).
> Ring artifacts: `/tmp/desync_taskA_ring_on.txt` / `_off.txt` (ephemeral).

**The site as landed** (rom_patches.cpp, inside `PatchROM_NW_trampoline`):
staged-copy word ROM+0x46e1a0 (mirror 0x5046e1a0, the slot-exit re-entry —
LR of the `bnel cr2,slot1` at 0x5046e19c, THE switch-in resume PC)
verify-EXPECTED-first (`lwz r1,0x10c(r3)` = 0x8023010c) → `b 0x50429da0`;
3-word stub at 0x429da0 (verify-zero-first, above the 0x429d9c free line):
`li r0,0; lwz r1,0x10c(r3); b 0x5046e1a4`. Family survey [PATCH]: the only
link-calls into the entry-vector table are e19c→slot1 (resume e1a0, consumed),
c9e4→slot2 / c4f0→slot4 (resumes c9e8/c4f4 — unconsumed per Q-B, NOT patched);
no other static branch targets e1a0/e1a4. Bonus confirmation: the stub's own
post-resume legs consume r0 AS the invariant zero (`oris r6,r0,0x1300` @e2bc,
`ori r6,r0,0xe05c` @e360, `stw r0,0x210(r5)` @e34c).

**Probe sub-contract (gate ON), all rows PASS** (F9-scoped through the
formerly-failing window):
- (a) `0x5046d760` (vector-0x2c raise) **0 visits the whole boot** [PROBE✓ ON-1];
  PROBE68K@0x5000e394 match-2: **r0=0x00000000 r4=0x0000005c d0=0x5c**
  (baseline 0x36/0x92/0x92) — the pinned register-form predicate verbatim.
- (b) no `[$C70]` failure-class write, no `[$AF0]:=0x000A` [WATCH✓ ON-1].
- (c) ring window [RING✓ ON-4, header `#3390800..#3391999 of 3408977`]:
  the #3391672-class record (block 50467cfc, invocation-2 d1=0 signature,
  now ≈#3391877 — gate-ON renumbering: the stub adds one ring record per
  e1a0 arrival) shows **r24=0x5000e3de**; `r24=5000e414` count in the
  window = **0**. Good flow present, bad dispatch absent.
- (d) e412 legit count = **2** (≥2 PASS), confirmed TRUE count by
  SS_JIT_NO_CHAIN=1 re-run (also 2) — the F8 "presumably 5" presumption is
  NOT borne out (not a contract falsification: the pinned class was ≥2);
  consistent with the e388 shim's selector-0-only design — table ids 2..5
  take the `bcc.s e396` early-exit and never reach the e410 lea.

**Gated-off A/B PASS** [RING✓/WATCH✓ OFF-1]: total records 4,480,458 vs
baseline 4,480,459 (the pre-authorized ±1 jitter class; window diff = the
one-record edge shift only); `[$C70]:=0x5000e448` @#3391707 (writer
r24=0x5000499a), `[$AF0]:=000a0000` @#3391719; crash SIGSEGV
ea=0x40000fffff42, guest pc=0x504661a0, r24=0x50004ad0, r1=0x0fffff46
(crash regs show r0=0x36 — the poison on display); delivered_sc=13,
delivered_program=2. Byte-identical baseline per the plan's field enumeration.

**THE WALL IS PASSED — the next frontier (P-M4 capture, named, NOT chased):
the 0x505bb060 off-ROM PC slide.** Gate ON, the boot runs ~4.8s JIT-time
(vs 0.16s to the old wall): PROGRAM #3/#4 delivered (invocation 2's $31/$36,
slot 8, conformant), sc surface grows 13 → **169 deliveries, 16 distinct
selectors** (new: 0x1b/0x1c/0x07/0x0c/0x08 ×3 each, 0xfffffffe ×17,
0xffffffff ×103), deferred_ee=5, CUDA packets=13, VIA/SCC MMIO traffic.
Death: guest control flow reaches **pc=0x505bb060** (beyond the staged-copy
end 0x50500000 — unmapped zero territory; lr=r29=0x505bb060, ctr=0,
r24=0x500050ef) and slides through zeros across every 64KB region up to
0x55590000 → SIGSEGV ea=0x400055590000. Instrument note: this crash class
RE-FAULTS the crash handler inside `dump_disassembly` (reads the unmapped
pc neighborhood) BEFORE `ppc_jit_dump_trace_ring()` — the crash ring flush
does NOT fire; use `SS_JIT_RING_DUMP_AT_PC` (this task's ON-4 idiom) for
ring capture in this regime.

### Instrument notes (carried forward)

- **PROBE68K now dumps the DR pipeline temps** (r0/r3/r4/r5/r6/r7/r30) — the
  capture extension that cracked this (r3=EA, r4=operand, r30=resolver base).
- The DR's EA machinery decoded [STATIC]: brief-extension indexed EA = helper
  family 0x50465ea0/ed0/f00 (lhau ext → extsb d8 → resolver at
  r30+((ext>>5)&0x7f8), r30=0x50460000 → scale appliers 0x50465f40+) →
  operand in r4 → per-opcode writeback (e.g. 0x50460c64 move.w-to-D0:
  `addco. r4,r4,r0` flag idiom + `rlwimi r8,r4,0,16,31`).
- `SS_JIT_WATCH_ADDR` polls vm_read_memory_4 at every ring record — a
  zero-hit watch on a guest word is strong evidence the word never changed
  (used here to exonerate memory at 0x5000e380).
- The e388 selector shim (raw==patched): only selector 0 is valid; its single
  table entry at e380 (0x005c) → e3dc. Any d0≠0 at the shim returns −50.

## Task B — advancement + invariant evidence (68k-pc-desync plan) — 2026-06-11

> **Status: ALL ROWS PASS** (advancement sub-contract + invariant carry-over,
> per the Task-0 predicate verbatim). **Boots: 2** fresh slot-protocol boots,
> gate ON (`SS_NW_DR_R0_INVARIANT=1`):
> B1 `desync-taskB-on1` (full-length to the frontier crash; probes d760/cef8c/
> d6b4/314ac0/314700/429da0 linear cap 8 + watches 0,4,c70,af0 + e412 census,
> rundir 20260611-224918.32214), B2 `desync-taskB-on2-ring` (big-ring 0x400000 +
> `SS_JIT_RING_DUMP_AT_PC=500f49c8` window dump + watches 100037c4/100037dc/2818
> + e394 predicate, rundir 20260611-225222.32692). Ring artifact:
> `/tmp/desync_taskB_ring_on.txt` (#3390800..#3391999 of 3,408,978 — Task A
> ON-4 total 3,408,977, the ±1 jitter class). No source edits this task
> (evidence-only; inner gates re-confirmed green at task start: batch test-jit
> 353/353, machine suite 13 suites ALL PASS).
> Instrument note: `SS_JIT_WATCH_ADDR` caps at **4 addresses** (parser,
> ppc-cpu.cpp:1054) — B1's 5th/6th tokens were silently dropped; the slot/nest
> watches moved to B2. Absolute-address probe fields are single-word only
> (`[0xADDR]`); the range form is register-indirect (`[rN:SIZE]`) only.

### Advancement sub-contract — PASS

- **(a) The boot passes 0xe410 intact — Task A rows re-asserted fresh:**
  `0x5046d760` (vector-0x2c raise) **0 visits the whole boot** [PROBE✓ B1,
  full-length]; no `[$C70]` failure write, no `[$AF0]:=0x000A` — the only
  c70/af0 events are the known boot-fill/clear classes (#22122/#22602 fill
  0xffffffff @r24=0x500005f6, #32193 c70 clear, #1840154 af0:=0xffff0000)
  [WATCH✓ B1]; ring window `r24=5000e414` count **0**, block 50467cfc produces
  **r24=0x5000e3de** at the #3391672-class record (now #3391878, gate-ON
  renumbering, invocation-2 d1=0 signature) [RING✓ B2]; e412 legit census = 2
  (the corrected ≥2 class) [PROBE68K✓ B1]; e394 match-2
  **dr-tmp r0=0x00000000 r4=0x0000005c d0=0x5c** — the pinned register
  predicate verbatim (baseline poison 0x36/0x92/0x92) [PROBE68K✓ B2].
- **(b) The e3e0 table run completes; ExpandMem slots fill (corrected
  early-exit reading):** invocation 1 fills `[0x100037dc]:=0x00120001`
  (#3391081, writer r24=0x5000f258 = the f240 stub tail — the Task A class);
  **invocation 2 fills `[0x100037c4]:=0x00150001` (#3392112, same writer
  class)** — id 4 → index 1 → slot base+4·1, handle 0x00150001 [WATCH✓ B2,
  PROBE✓ B1: $36 service visit=2 r3=0x00150001 r4=0x00000001; $31/$36 args
  per invocation: r4=2/7 then 4/1]. Per the corrected e388-shim reading, only
  lea-reaching invocations fill slots; both observed invocations did. Note
  the d6b4 entry-time `[0x100037c0]`=2 (index-1 slot pre-fill residue R-F2
  unchanged).
- **(c) The 68k advances past e490 + caller:** e490-family epilogue walked in
  the window (e492×2, e496×3, e498, e49a, e49e rts) [RING✓ B2]; B1 runs
  ~3.4M records past the window to the NEW frontier (crash r24=0x500050ef,
  pc slide 0x505bb060→0x55590000) — the park tail (5000f248) and the DSAT
  crash tail (50004ad0) both ABSENT [RING✓/crash regs B1].

### Invariant carry-over — PASS (same boots)

- Cold-once trampoline WATCH pair: record **#4666** (pc=50429b40, guest[0]/[4]
  exactly once); pre-NK writes #213/215 + the legit vector install
  (pc=50490e00, r24=500389fe/50038a02, #1030517/18) — known classes only ✓.
- `delivered_dec=0` (deferred_ee=5 deferred_depth=0 deferred_native=0) ✓.
- PROGRAM deliveries: **count = 4** (recorded), conformant per-delivery — ALL
  slot 8, srr0=5046e8e0, word=0fff0008, lr=5046db6c → entry=0x50314700;
  entry-state r0/r4 = $31/2, $36/7 (invocation 1), $31/4, $36/1 (invocation 2)
  [PROBE✓ B1 0x50314700 linear].
- sc surface: **169 deliveries, 16 distinct** (count recorded as diagnostic);
  per-delivery conformance sampled at handler 0x50314ac0 (first 8 linear:
  selectors 0x3f/0x19/0x14/0x19/0x0f/0x27/0x40/0x42 in census order, r3-class
  args per M3A) ✓. **⚠ RECORDED PROMINENTLY — the negative-selector oddity:
  `0xfffffffe ×17` and `0xffffffff ×103`** dominate the post-wall sc census
  (full list: 0x3f×1 0x19×6 0x14×3 0x0f×8 0x27×7 0x40×1 0x42×2 0x50×1 0x4d×1
  0xfffffffe×17 0xffffffff×103 0x1b×3 0x1c×3 0x07×3 0x0c×3 0x08×3, +4 beyond
  the 16-distinct counter cap). Negative selectors are not in the NK's
  positive service-table range — **this smells like a NEW surface class**
  (sentinel/queue-poll selectors, or a different sc consumer in the advanced
  regime; crash regs r26=0xfffffffe is suggestive). Flagged as a candidate
  recon question for the NEXT milestone — not chased here.
- TVector round trip intact: `[PROBE 0x500cef8c visit=1] r25=0x5000fcf2` ✓.
- Slot-15 unvisited (no slot-15 line; all PROGRAM deliveries slot 8) ✓.
- XLM_IRQ_NEST drift recorded (diagnostic): 118 watch events on 0x2818 through
  B2's window; the −1/delivery sawtooth class (W2S-1), one return-to-0 at
  #3388726, value 0xffffffca (−54) at the window dump. Q-DS3 found it not
  load-bearing for the desync; stands as the pre-named link-8 signature.

### Diagnostic — where the boot now stands (recorded, not gates)

The honest map: both FE1F invocations complete ($31+$36 each, 4 PROGRAM
deliveries — the f280/$34 sibling did NOT fire this regime); the sc surface
grows 13→169 with the negative-selector family above; deferred_ee=5, CUDA 13
packets, VIA/SCC MMIO traffic. The boot dies at the **0x505bb060 off-ROM PC
slide** (Task A's named frontier) — the P-M4 artifact + origin-class look is
Task C's deliverable. No new wall class beyond it.
