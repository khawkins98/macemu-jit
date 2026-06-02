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

**Working hypothesis**: the 68k code that builds/validates CD-ROM boot reads (Disk Manager
/ boot-blocks validation, running in the JIT-compiled DR emulator) computes something
wrong — i.e., another latent codegen bug in a 68k-region instruction, like crorc was.

**Next steps**:
1. Extend the 'E'/'R' EMUL_OP records for CDROM_PRIME to also capture the IOParam block
   contents (guest memory at the param-block pointer): ioPosOffset, ioReqCount, ioBuffer.
2. Capture CDROM_PRIME sequences from an interpreter boot (working) and the stuck JIT
   boot; compare request parameters at the same boot phase (anchor on the Nth PRIME call
   after first CDROM_OPEN, not on addresses).  Diverging request parameters → trace back
   which 68k computation produced them → isolate that ROM block → harness vector → fix.
3. Audit candidates with zero harness coverage that the DR emulator + Disk Manager use:
   remaining case-19 XO ops, mcrxr, lhbrx/lwbrx (byte-reversed loads — used in disk I/O!),
   lha/lhau update forms, mulhwu/divwu edge cases.

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
