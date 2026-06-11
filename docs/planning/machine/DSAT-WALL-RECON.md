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
