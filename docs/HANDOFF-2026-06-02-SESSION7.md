# SheepShaver ARM64 JIT — Session 7 Handoff (updated 2026-06-03)

## Status: workaround boots to desktop; root cause still open

The extension-loading hang is a pre-existing JIT bug. A skip-list workaround boots
to Finder desktop. The initial "cross-instruction state leak" hypothesis has been
significantly weakened by experiments — the actual root cause is still unknown.

## Read first

1. `LEARNINGS.md` — session 7 entries (2026-06-03 dated sections)
2. `docs/PPC-ARM64-JIT-LESSONS.md` — architectural narrative
3. `docs/BOOT-TEST-METHOD.md` — reproducible test procedure

## Workaround (boots to Finder desktop)

```bash
SS_JIT_SKIP_OPC=21,28,29,30,31,32,33,43,44,45,46,47,48,49,50,51,57,61,62,63 ./SheepShaver
```

Forces ~20 opcode types to the inline interpreter call. All others compile natively.
Boots to Finder desktop from 8.6 ISO in ~30 seconds.

**What this does**: When `compile_one()` returns false, the JIT emits an inline
interpreter call (BLR to C handler) and the block ENDS. This flushes all state to
memory and returns to the C dispatcher. The interpreter handler executes the instruction
correctly via the C++ code path. The block termination means spcflags are checked
between every instruction.

## Bugs fixed this session

| # | Bug | Root cause | Fix |
|---|-----|-----------|-----|
| 1 | subfe/adde carry | ADDS reads partial carry, not full 3-operand sum | 64-bit arithmetic |
| 2 | mftb TBU/TBL | CNTVCT_EL0 stored as-is for both halves | LSR #32 for TBU |
| 3 | DR entry-poll | Block-entry spcflags poll fires mid-dispatch-cycle | Skip poll for DR blocks |
| 4 | icbi NOP | Stale JIT blocks survive code rewrite | Fall through to interpreter |
| 5 | isync NOP | Deferred icbi flush not triggered | Fall through to interpreter |
| 6 | UXTW addressing | Defensive 32-bit address extension in register-offset mem access | option=010 |

## Extension-loading hang: what we know

### The symptom

Boot reaches "Starting Up..." at ~10% progress and hangs. jNK freezes, jRAM grows
at ~85M/s forever. Pre-existing at commit 77d47baa (before all session 7 changes).
Interpreter boots to Finder desktop in ~2 min.

### The discriminating tests

| Test | Result | Implication |
|------|--------|-------------|
| SKIP_ALL (every instruction via interpreter) | PASS | Interpreter-executed instructions are correct |
| MAX_INSNS=1 (all native, 1 per block) | FAIL | At least one native codegen differs from interpreter |
| Remove any single opcode from skip list | PASS | Misleading — interpreter calls mask the bug |
| Allow {30,31} native, rest interpreted | FAIL | Multiple native opcodes needed |
| SS_JIT_VERIFY (per-block register check) | 0 divergences | No detectable per-block register corruption |

### Ruled-out causes

- Lazy CR0 flush before every instruction: still hangs
- RTMP register zeroing before every instruction: still hangs
- PC store before every instruction: still hangs
- UXTW memory access encoding: still hangs
- Block chaining: still hangs
- icbi/isync coherence: still hangs
- Block length (MAX_INSNS=1/2/4/64/512): all fail
- Any single opcode skipped: all pass (misleading)
- Per-block register verification: zero divergences

### What the data shows

- **Memory comparison**: 1.7M bytes differ between JIT and interpreter at t=15s
- **Low-memory globals** (0x10, 0x20): NULL under JIT, valid under interpreter
- These NULLs are **downstream symptoms** — boot hangs before the writes that set them
- **Trace comparison**: execution chains match; transient 20-byte SP difference self-corrects
- **The actual stall** is in RAM code at 10653b60/10695xxx (spin-wait), not in ROM
- RAM addresses shift between boots (extension ASLR), defeating address-based binary search

### Where to investigate next

The state-leak hypothesis is weakened. Three avenues remain:

1. **Side-effect audit**: SKIP_ALL passes but MAX_INSNS=1 fails. Both execute one
   instruction per dispatch. The difference is C interpreter vs ARM64 native. Some
   native codegen must differ from the interpreter in a way SS_JIT_VERIFY doesn't
   catch (e.g., memory-mapped I/O side effects, atomic semantics, or write ordering).

2. **Opcode-level differential**: run SKIP_ALL minus one opcode at a time to find
   which single native opcode makes the boot fail. Previous "remove any one from skip
   list = pass" tested removing from the workaround set, not adding to a full-skip
   baseline. The inverse search may be more discriminating.

3. **Long-horizon memory diff**: the 1.7M byte difference at t=15s may be traceable
   to a specific divergence point by comparing at t=1s, t=2s, t=5s increments.

## Diagnostic tools available

| Tool | Purpose |
|------|---------|
| `SS_JIT_SKIP_OPC=n,n,...` | Force interpreter for specific primary opcodes |
| `SS_JIT_SKIP_XO=n,n,...` | Force interpreter for specific XO31 sub-opcodes |
| `SS_JIT_MAX_INSNS=n` | Cap JIT block length |
| `SS_JIT_VERIFY=1` | Per-block register verification against interpreter |
| `SS_JIT_ROM_SIZE=<hex>` | Override ROM JIT range |
| `SS_JIT_INTERP_RANGE=lo-hi` | Force interpreter for a PC range |
| `SS_JIT_NO_CHAIN=1` | Disable block chaining |
| `SS_JIT_NO_ROM=1` | Keep ROM interpreter-only |
| `SS_JIT_CHAIN_LOG=1` | Log dispatch call chain at stuck-loop entry |
| `SS_EMULOP_TRACE=1` | Log SCSI EMUL_OP return values |
| Heartbeat: `M/s comp=N` | Block rate and unique-blocks-compiled count |

## Current configuration

- Branch: `macos-arm64`
- ROM range: 0x500000 (full, DR emulator JIT-compiled)
- JIT_BLOCK_CHAINING: 1 (enabled)
- Harness: 235/235, score=100
- Boot: hangs at "Starting Up..." without skip list; Finder desktop WITH skip list
- Test config: 8.6 ISO boot (`bootdriver -62`), VNC on port 5999
- Assets: `/Users/Shared/macemu/`
