# SheepShaver ARM64 JIT — Session Handoff (2026-06-02, dyngen-parity session)

## Where things stand

The goal of this workstream: JIT-compile the ROM's built-in 68k (DR) emulator region
(ROMBase+0x460000..+0x500000) — the region where 94-97% of boot-time dispatches execute —
so the JIT stops paying a dispatcher round-trip per 68k instruction.

### What is COMMITTED and verified

1. **crorc miscompilation fix** — the root cause of the deterministic 68k-region boot
   crash. One missing `AND #1` after ARM64 `ORN` made crorc smear 0xFFFFFFFF across the
   whole CR. Full causal chain documented in LEARNINGS.md (2026-06-02 evening entry).
   Verified: isolated vector + 227/227 harness both modes.

2. **CR-logical harness coverage** — 9 new vectors (crorc all 4 input combos, plus
   crand/cror/crxor/crnor/crandc/creqv/crnand/mcrf). This family had ZERO coverage when
   crorc shipped broken.

3. **Diagnostic infrastructure** (all env-gated, zero cost when off):
   - `SS_JIT_TRACE_RING=1` — in-memory 256K-record execution trace ring, dumped to
     /tmp/ss_jit_ring.txt by the SIGSEGV handler. Zero I/O overhead during execution.
     Records: 'J' JIT block, 'I' interp block, 'C' inline call, 'E'/'R' EMUL_OP entry/return.
   - `SS_JIT_NO_CHAIN=1` — runtime chaining kill-switch (bisect without rebuild).
   - `SS_JIT_WATCH_STUB=1` — software watchpoint on Mixed Mode switch-back stubs.
   - `SS_JIT_RING_DUMP_TRIGGER=1` — dump ring when DR emulator executes stack-region code.
   - **lldb live-dump trick**: `lldb -p <pid> -o "expression -- (void)ppc_jit_dump_trace_ring()"`
     dumps the ring from a RUNNING (hung) process — no crash needed.

4. **Committed configuration**: `JIT_BLOCK_CHAINING 0`, ROM range `0x460000` (toolbox only).
   This is the proven-booting configuration plus the fix and instrumentation.
   **Working tree**: ROM range flipped to `0x500000` (uncommitted — needed for Bug #2 hunt).
   Harness verified 227/227 both modes with this working-tree state.

### What is UNCOMMITTED / in progress (the working-tree flips)

To continue the dyngen-parity work, flip these two values:
- `ppc-jit.cpp`: `#define JIT_BLOCK_CHAINING 1`
- `ppc-cpu.cpp`: ROM range `0x460000` → `0x500000`

With both flipped + the crorc fix: **no more crash** (previously deterministic SIGSEGV at
3-16s), harnesses 227/227, but boot hangs in an **infinite SCSI scan loop** (bug #2, below).

## Bug #2 (open): infinite SCSI scan loop with full-ROM compilation

**Symptom**: with the 68k region compiled, boot reaches the SCSI phase and loops forever
(SCSISelect 0-6 + SCSIGet, 171K+ iterations). No crash. The interpreter and the
toolbox-only JIT config complete this phase normally and boot to desktop.

**Evidence so far** (from EMUL_OP entry/return records + execution traces in the ring):
- Interrupt delivery WORKS: CR bit 8 gets set, the nanokernel runs, and the 68k interrupt
  handler executes (including Mixed Mode calls) at ~60Hz intervals.  (An earlier "zero
  OP_IRQ records" observation was an artifact of the ring window being only ~0.1s.)
- SCSI_DISPATCH and CDROM_PRIME EMUL_OPs execute and return.  SheepShaver uses the DUMMY
  SCSI driver (scsi_dummy.o), so "no SCSI devices" is the correct answer; the OS should
  accept it and boot from the CD-ROM driver — the interpreter does exactly that.
- The endless SCSI rescan is Mac OS's normal "looking for a bootable disk" behavior
  (blinking-?-floppy state).  **The real failure is therefore upstream: the CD-ROM boot
  path is failing validation in JIT mode**, so the OS falls back to scanning forever.

**ROOT CAUSE LOCALIZED (2026-06-02, end of session)** — IOParam comparison between
interpreter and JIT boots (E/R records now capture ioBuffer/ioReqCount/ioPosOffset/ioResult):

| | Interpreter (working) | JIT (stuck) |
|---|---|---|
| CDROM_PRIME requests | varied (System file loading) | identical forever: 1 KB at offset 0 (boot blocks) |
| Result | d0=0 (noErr) | **d0 = -65 (offLinErr, "drive offline")** |
| OP_IRQ / Time Mgr / CDROM_CONTROL EMUL_OPs | present at 60 Hz | **completely absent** |

Chain: the CD-ROM "disk inserted" state is set by the driver's accRun periodic action →
called from the 68k interrupt handler's work → which requires reaching the **OP_IRQ**
EMUL_OP.  In JIT mode interrupts ARE delivered (CR bit 8 set, nanokernel runs, the 68k
interrupt handler starts at ~60 Hz), but the handler **never reaches OP_IRQ** → the CD
never mounts → all boot-block reads return offLinErr → infinite boot-device rescan.

**SUPERSEDED — the hypothesis above was killed by the EMUL_OP counter test**
(SS_EMULOP_COUNTS=1): OP_IRQ fires at 60 Hz in BOTH modes.  Interrupts work fully.

**THE ACTUAL MECHANISM (found via CDROM-DBG prints + SS_JIT_WATCH_ADDR watchpoint)**:

1. CDROMOpen runs correctly: DrvSts at guest 0x100a1cc0 (deterministic address),
   dsDiskInPlace (offset +3) set to 1.  ✓
2. The CD is read successfully for a while — many 512-byte CDROM_PRIME calls return
   noErr with full ioActCount.  ✓
3. Then a **spurious CDROM_CONTROL call with a GARBAGE param block** arrives:
   A0 = 0x103ffffe (2 bytes below the top of the stack region!), param block contents
   are junk (ioBuffer=c889b35c etc.).  The garbage csCode lands in the EJECT path
   (cdrom.cpp case 7) → native code clears dsDiskInPlace.  Watch event:
   `[100a1cc0] 00008001 -> 00008000  block 504ff2b8 (CDROM_CONTROL EMUL_OP) r24=500e1586`
4. After the eject every Prime returns offLinErr → Mac OS rescans for boot devices
   forever (the SCSI loop).

**Where the garbage came from**: the 68k caller (Device Manager code at ROM 0x5007ac36-
0x5007ac4c) loaded A0 from a stack slot (`MOVE.L d16(A7),A0`, opcode 0x206f) — the slot
contained 0x103ffffe.  So bug #2 is, like bug #1, **corrupted data on the 68k stack** —
but from a different source (crorc is fixed and verified).

**Next steps (the corrupted-stack-slot hunt)**:
1. DONE — the caller is the Device Manager's driver-call glue at ROM 0x5007ac30:
   ```
   5007ac30: MOVEM.L D1-D7/A0-A6,-(SP)    ; pushes 56 bytes
   5007ac34: MOVEA.L $3C(A7),A2           ; A2 = driver entry table
   5007ac38: MOVE.L  $48(A7),D1           ; D1 = routine offset (Control = ?)
   5007ac40: MOVEA.L $44(A7),A0           ; A0 = param block ptr  <- loads the GARBAGE
   5007ac44: MOVEA.L $40(A7),A1           ; A1 = DCE pointer
   5007ac48: JSR     0(A2,D1.W)           ; -> driver routine
   ```
   So the corrupted slot is [A7_after_movem + 0x44] = the param-block ARGUMENT pushed by
   this glue's caller before JSR'ing here.  In the captured run: sp at the Control call
   was 0x103ffec4, so the slot was ~0x103fff08-0x103fff0c, containing 0x103ffffe.
2. DONE — the dual watch (SS_JIT_WATCH_ADDR=103fff0c,100a1cc0 SS_JIT_WATCH_DUMPS=0) caught
   the writer.  52 records before the eject:
   ```
   [103fff0c] fffe103f -> 103ffffe   record #24519184   block 504613e0   r24=5007aed6
   ```
   **Block 0x504613e0 (executing 68k code at ROM 0x5007aed4) wrote the garbage value.**
   Even more diagnostic, the preceding events show the halfwords of 0x103ffffe being
   written at SWAPPED positions over thousands of records:
   ```
   #24450495: -> fffe....   (wrote halfword "fffe" at slot+0)    r24=500297b4
   #24450578: -> fffe103f   (wrote halfword "103f" at slot+2)    r24=5002968c
   #24519184: -> 103ffffe   (the correctly-ordered value)        r24=5007aed6
   ```
   This 2-byte-swapped pattern is a strong signature of a **halfword store/load pair with
   a misaligned or wrongly-ordered address** (e.g. a JIT bug in sthu/sth d(rA) with some
   operand form, or a 68k MOVE.W pair whose handler computes EA+2 vs EA wrongly).

3. DONE (partial) — decoded block 504613e0's PPC instructions (2026-06-02 session 2):
   ```
   504613e0: lhau  r27, 2(r24)          ; fetch next 68k opcode, advance PC
   504613e4: addco. r4, r4, r0          ; OE arithmetic for 68k condition codes
   504613e8: rlwimi r27,r29, 3, 13, 28  ; merge CC bits
   504613ec: mtlr  r29                  ; load dispatch handler into LR
   504613f0: lhau  r27, 2(r24)          ; fetch next-next opcode (2nd advance)
   504613f4: sthu  r4, -4(r1)           ; push halfword of r4 at [r1-4], r1 -= 4
   504613f8: bclr  5, 8                 ; dispatch to handler if no interrupt
   504613fc: b     0x5046D0D4           ; jump to interrupt handler
   ```
   The 68k instruction at 0x5007aed4 is `MOVE.L A3, -(A7)` (68k bytes 0x2F0B).
   So block 504613e0 is a dispatch variant that also PUSHES r4 (= A3) onto the 68k
   stack as a halfword.  The sthu at 504613f4 writes 16 bits of r4 to [r1-4].

   **Key open question**: A3 = 0x103ffffe (stack_top - 2) is garbage.  HOW did A3
   get this value?  The sthu/lhau JIT handlers are confirmed correct (harness 227/227
   both modes after this session's binary rebuild).  The 2-byte-swapped writes at
   records #24450495 and #24450578 are from 68k code at 0x500297b2 (BSR.W) and
   0x5002968a — likely an unrelated coincidence; the slot gets reused by different
   stack frames over many records.  Block 504613e0 writes the correctly-assembled
   0x103ffffe as the PUSH of A3 — the CORRECT ASSEMBLY of the garbage value.
   The bug is upstream: something set A3 = 0x103ffffe when it should be a valid
   param-block pointer.

4. NEXT SESSION STARTS HERE:

   **Goal**: find WHY A3 = 0x103ffffe when block 504613e0 executes MOVE.L A3, -(A7).

   **Step 1 — reproduce cleanly** (use a fresh run, NO lldb, NO heavy diagnostics):
   The previous session's emulator got stuck in an early-boot loop at 0x5031040c/
   0x50310414 due to lldb SIGSTOP operations disrupting the 60Hz VBL timer.  This
   is NOT a new JIT bug — restart cleanly without lldb interference.
   ```bash
   pkill -9 -x SheepShaver 2>/dev/null
   cd /Users/khawkins/Documents/git/macemu-jit/SheepShaver/src/Unix
   SS_JIT_WATCH_ADDR=103fff0c,100a1cc0 SS_JIT_WATCH_DUMPS=0 \
     SS_JIT_NO_CHAIN=1 ./SheepShaver > /tmp/ss_diag.log 2>&1 &
   ```
   Wait 5-10 min, then dump the watch log (DO NOT ATTACH LLDB UNTIL SCSI PHASE
   ACTIVITY IS VISIBLE IN THE LOG).

   **Step 2 — once 52+ watch records appear in the log**, the eject is imminent.
   Then: `lldb -b -p $(pgrep -x SheepShaver) -o "expression -- (void)ppc_jit_dump_trace_ring()" -o detach -o quit`
   (single attach, single dump, immediately detach — minimize SIGSTOP time).

   **Step 3 — find the A3 writer**.  Add a THIRD watch address: the 68k A3 register.
   A3 in the emulator = PPC register that maps to 68k A3.  From the DR emulator
   register mapping, A3 likely lives at offset PPCR_GPR(N) in the state struct.
   Find N by reading the ROM's 68k emulator setup code, or by watching register
   writes near the "2-byte-swapped" records.  Alternatively, use:
   `SS_JIT_WATCH_ADDR=<addr_of_A3_in_state_struct>,103fff0c,100a1cc0`
   to catch WHEN A3 gets the garbage value.

   **Alternative/parallel approach** (strongly recommended — do in parallel):
   Opcode-histogram the 0x460000-0x500000 ROM region; find every uncovered opcode;
   write SS_TEST_HEX vectors for each.  This finds crorc-class bugs as isolated
   reproductions instead of boot-trace hunts.  Strong candidates not yet covered:
   mcrxr, lhbrx/lwbrx, rlwnm, divw/divwu edge cases, mtcrf with FXM≠0xFF.
   Run with `SS_HARNESS_MODE=jit ./jit-test/run.sh` to ensure JIT-mode coverage.

   **Working tree state**: ppc-cpu.cpp has ROM range 0x500000 (needed for Bug #2).
   JIT_BLOCK_CHAINING is 0.  DO NOT COMMIT until full-ROM boot is verified.

**Diagnostic tools added for this hunt** (all committed):
- `SS_EMULOP_COUNTS=1` — per-EMUL_OP execution counters dumped to stderr every 5s.
- `SS_JIT_WATCH_ADDR=<hex>` — software watchpoint on any guest word; reports every change
  with the responsible block and dumps the ring on the first changes.
- CDROM-DBG prints in cdrom.cpp were TEMPORARY and have been reverted; re-add as needed.

## How to verify any JIT change (the bar)

1. Both harness modes green: `cd SheepShaver && ./jit-test/run.sh` and
   `SS_HARNESS_MODE=jit ./jit-test/run.sh` (must be 227/227)
2. **Boot to desktop** in the committed configuration. The harness is a proven-incomplete
   oracle: block 5010bb90 passes the harness while its in-context execution exposed crorc.
   Never commit a config flip on harness-green alone.

## How to run

```bash
cd SheepShaver/src/Unix && make -j8        # build
./SheepShaver                              # JIT on by default (toolbox-only ROM range)
SS_USE_JIT=0 ./SheepShaver                 # interpreter only
SS_USE_JIT=1 SS_JIT_TRACE_RING=1 ./SheepShaver   # with crash-diagnosis ring

# Dump the ring from a hung process:
lldb -b -p $(pgrep -x SheepShaver) -o "expression -- (void)ppc_jit_dump_trace_ring()" -o detach -o quit
```

## Test assets (moved out of TCC-protected ~/Downloads — see LEARNINGS.md)

- ROM: `/Users/Shared/macemu/1998-07-21 - Mac OS ROM 1.1.rom`
- Mac OS 8.6 ISO: `/Users/Shared/macemu/Mac OS 8.6 Internal Edition.iso`
- Prefs: `~/.sheepshaver_prefs` (backup of pre-move prefs: `~/.sheepshaver_prefs.bak`)

## Performance context (unchanged from previous session)

- Pure interpreter boot: ~10 s. JIT boot (toolbox-only): 160 s – 7 min.
- Boot is the JIT's worst case; the value proposition is steady-state performance
  (unmeasured — MacBench remains a future task).
- Future levers: register allocation (2-3×), chaining (~10-30%), 68k-region compilation
  (the current workstream — removes the dominant dispatcher overhead).
