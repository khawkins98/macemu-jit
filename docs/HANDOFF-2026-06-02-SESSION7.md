# SheepShaver ARM64 JIT — Session 7 Handoff

## Status: four bugs fixed, fifth under investigation

Boot reaches "Starting Up..." with progress bar but hangs during extension loading.
A workaround (skip list) boots to Finder desktop. Root cause narrowed to a cross-
instruction state leak in the JIT codegen.

## Read first

1. `LEARNINGS.md` session 7 entries — full investigation timeline
2. `docs/PPC-ARM64-JIT-LESSONS.md` — architectural narrative
3. `docs/BOOT-TEST-METHOD.md` — reproducible test procedure

## Bugs fixed this session

| Bug | Root cause | Impact | Fix |
|-----|-----------|--------|-----|
| subfe/adde carry | ADDS reads partial carry, not full 3-operand sum | DR emulator SCSI hang | 64-bit arithmetic |
| mftb TBU/TBL | CNTVCT_EL0 stored as-is for both halves | Time base garbage values | LSR #32 for TBU |
| DR entry-poll | Block-entry spcflags poll fires mid-dispatch-cycle | Premature CR2.LT injection | Skip poll for DR blocks |
| icbi NOP | icbi compiled as NOP, stale JIT blocks survive code rewrite | Potential stale code | Fall through to interpreter |

## Current blocker: extension-loading hang

### Symptom

Boot reaches "Starting Up..." at ~10% progress and hangs. jNK freezes, jRAM grows
at ~85M/s forever. Pre-existing bug (present at commit 77d47baa before session 7).

### Workaround

```bash
SS_JIT_SKIP_OPC=21,28,29,30,31,32,33,43,44,45,46,47,48,49,50,51,57,61,62,63 ./SheepShaver
```

This forces ~20 opcode types to the inline interpreter call while letting the rest
compile natively. Boots to Finder desktop from 8.6 ISO in ~30 seconds.

### What the skip list means

When `compile_one()` returns false for an instruction, the JIT emits an inline
interpreter call (BLR to a C handler) instead of native ARM64 code. Crucially,
the **block ends immediately** — all state (registers, flags) is flushed to memory.
The next block starts fresh.

When `compile_one()` succeeds, the block **continues** — the next instruction shares
the same ARM64 NZCV flags, RTMP temporary registers, and lazy CR0 state with the
previous instruction. No flush between them.

### Diagnosis: cross-instruction state leak

| Test | Result | Implication |
|------|--------|-------------|
| Skip ALL opcodes | PASS | Every instruction via interpreter = correct |
| MAX_INSNS=1 (all native) | FAIL | Even 1 native instruction per block fails |
| Remove any single opcode from skip list | PASS | No one opcode is solely responsible |
| Multiple native opcodes together | FAIL | Bug requires ≥2 native instruction types |

The bug is an interaction between native instructions sharing dirty state within
the JIT block framework. The inline interpreter call resets this state (via
lazy_flush_cr0 + ra_flush_all + block termination).

### Top suspects

1. **Lazy CR0**: an Rc=1 instruction stores CR0 in NZCV flags (deferred write).
   A subsequent non-Rc instruction clobbers NZCV without flushing CR0 first.
   When CR0 is eventually read, it gets the wrong value.

2. **NZCV leak into block epilogue**: the spcflags poll or epilogue code reads
   NZCV flags that were left dirty by the last native instruction in the block.

3. **RTMP register collision**: one instruction's address computation in RTMP0
   overwrites a value that the previous instruction expected to persist.

### Next step

Add a defensive `lazy_flush_cr0()` at the START of every instruction in
`compile_one()`. If this fixes the boot, the bug is lazy CR0 corruption.
If not, instrument the spcflags poll to detect when it reads dirty NZCV.

## Diagnostic tools added this session

| Tool | Purpose |
|------|---------|
| `SS_JIT_ROM_SIZE=<hex>` | Override ROM JIT range (ROM binary search) |
| `SS_JIT_INTERP_RANGE=lo-hi` | Force interpreter for a PC range (RAM binary search) |
| `SS_JIT_SKIP_OPC=n,n,...` | Force interpreter for specific primary opcodes |
| `SS_JIT_SKIP_XO=n,n,...` | Force interpreter for specific XO31 sub-opcodes |
| `SS_JIT_MAX_INSNS=n` | Cap JIT block length |
| `SS_JIT_CHAIN_LOG=1` | Log dispatch call chain at stuck-loop entry |
| `SS_EMULOP_TRACE=1` | Log SCSI EMUL_OP return values |
| Heartbeat: `M/s comp=N` | Block rate and unique-blocks-compiled count |

## Current configuration

- Branch: macos-arm64
- ROM range: 0x500000 (full, DR emulator JIT-compiled)
- JIT_BLOCK_CHAINING: 1 (enabled)
- Harness: 235/235, score=100
- Boot: hangs at "Starting Up..." without skip list; Finder desktop WITH skip list
- Test config: 8.6 ISO boot (`bootdriver -62`), VNC on port 5999
- Assets: /Users/Shared/macemu/
