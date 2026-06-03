# Lessons from PPC→ARM64 JIT: What We Actually Learned

This isn't a simple port. The upstream `rcarmo/macemu-jit` targets Linux ARM64
(Raspberry Pi / Orange Pi). This fork targets macOS ARM64 (Apple Silicon). But
the bugs we found aren't Apple-specific — they're architectural gaps between
PowerPC and ARM64 that would bite any PPC JIT on any ARM64 platform.

## The Bug Classes

### 1. Carry-Flag Semantics: PPC's Three-Operand Problem

PPC has instructions like `subfe` and `adde` that compute a three-operand sum
(`~rA + rB + CA`) and set the carry-out flag from the *full* 33-bit unsigned
result. ARM64's `ADDS` only handles two operands at a time. The naive approach:

```
ADDS  result, operandA, operandB    ; sets carry from A+B
ADD   result, result, carry_in      ; adds CA but DOESN'T update carry
CSET  ca_out, CS                    ; reads carry from the ADDS — wrong!
```

The carry from `ADDS(A, B)` doesn't account for the `+CA` term. For the PPC
idiom `subfe rX,rX,rX` (carry-to-mask), `~rX + rX = 0xFFFFFFFF` which *never*
carries on ARM64 — so `CA` was always written as 0, regardless of input.

**Fix**: compute the full sum in 64 bits: `UXTW + ADD X + ADD X`, extract
bit 32 as carry-out. This is correct for all input combinations.

**Impact**: the Mac ROM's 68k emulator uses `subfe` in address calculation
chains. Corrupted CA caused wrong 68k dispatch addresses → infinite SCSI
scanning loop during boot.

**Lesson**: any PPC instruction that reads AND writes CA in one operation
(subfe, adde, subfme, addme, subfze, addze) needs careful carry-chain
handling. Two-operand `ADDS` is only sufficient when there's no carry-in
term (addc, subfc, addic, subfic).

### 2. Register-Width Mismatch: 64-bit ARM64 vs 32-bit PPC

PPC's Time Base Register is a 64-bit counter split into two 32-bit halves:
TBL (lower, SPR 268) and TBU (upper, SPR 269). ARM64's equivalent
`CNTVCT_EL0` is a single 64-bit register. The JIT read CNTVCT_EL0 and
stored it as a 32-bit GPR — giving TBL and TBU identical values (both
the lower 32 bits).

Mac OS reads the time base using a standard polling pattern:
```
loop: mftb r3, TBU    ; read upper
      mftb r4, TBL    ; read lower
      mftb r5, TBU    ; read upper again
      cmplw r5, r3    ; upper changed?
      bne loop        ; retry if rollover during read
```

With TBU returning the same value as TBL, the consistency check always
passes — but the actual time value is garbage, causing timeouts and
scheduling logic to malfunction.

**Fix**: `LSR X, X, #32` for TBU reads; implicit W-register truncation
for TBL reads.

**Lesson**: any PPC SPR that maps to a differently-sized ARM64 system
register needs explicit width handling. Other candidates: DEC (decrementer),
DABR (data address breakpoint), and any MSR/SRR fields.

### 3. Emulator-in-Emulator: The DR Dispatch Cycle

The Mac ROM contains a PPC-coded 68k instruction emulator (the "DR emulator").
When the PPC JIT compiles this emulator, we get a three-level stack:
ARM64 native → PPC JIT → 68k emulation.

The DR emulator's interrupt gate (`bclr 5,8`) checks CR2.LT at a specific
point in its dispatch cycle. The JIT's block-boundary interrupt checks fire
at a different point — BETWEEN the dispatch head and the handler — setting
CR2.LT too early and causing the handler to be skipped.

This isn't a codegen bug — it's a **semantic boundary problem**. The JIT's
block boundaries don't align with the emulator's dispatch cycle boundaries.
The fix (suppressing the block-entry spcflags poll for DR emulator blocks)
was necessary but turned out to be insufficient on its own — the subfe
carry bug was the actual blocker, not the timing.

**Lesson**: when JIT-compiling an emulator dispatch loop, the JIT's
interrupt-check points must align with the emulated architecture's
interrupt delivery points. This is a known problem in emulator literature
(QEMU, Dolphin, and others all handle it with various strategies).

### 4. Cache Coherence: icbi/isync Deferred Invalidation

PPC's `icbi` (instruction cache block invalidate) and `isync` (instruction
synchronize) form a two-step protocol: `icbi` marks a cache line for
invalidation, `isync` commits the invalidation. When the JIT compiled both
as NOPs, self-modifying code (e.g., Mac OS extension loading) continued
executing stale JIT translations of the old code.

The naive fix — fall through to the interpreter for both — made `isync` a
block terminator. Since `isync` appears after every `mtmsr`/`mtspr` sequence
in OS code, this marked most of the ROM toolbox interpreter-only, causing a
42x slowdown. The correct approach: `icbi` records the invalidated range,
`isync` calls the JIT's range-based invalidation (which reverts chain
patches and nullifies pool entries for affected PCs). `isync` stays a NOP
in the JIT; the invalidation is triggered from the interpreter's
`execute_icbi` path.

**Lesson**: instruction cache coherence on PPC is a two-phase protocol.
JIT compilers must handle both phases — not just the invalidation request
but also the commit fence — and the performance impact of the fence
instruction determines where in the pipeline the invalidation logic lives.

### 5. UXTW Addressing Mode: 32-bit Addresses in 64-bit Registers

PPC is a 32-bit architecture (in this emulator's mode). Guest addresses are
32-bit values stored in 64-bit ARM64 registers. Register-offset load/store
encodings like `LDR Xt, [Xbase, Xoffset, LSL #0]` use option=011 (LSL),
which treats `Xoffset` as a full 64-bit value. If the upper 32 bits of the
guest address register contain garbage, the effective address is wrong.

In practice, ARM64 W-register operations zero the upper 32 bits, so this
is usually safe. But as a defensive measure, using option=010 (UXTW, 
zero-extend W to X) explicitly truncates to 32 bits. This costs zero extra
instructions (it's just a different encoding of the same load/store).

**Lesson**: when emulating a 32-bit architecture on a 64-bit host, prefer
explicit zero-extension in memory access encodings. The cost is zero and
it eliminates an entire class of potential bugs.

### 6. FP Multiply-Add Family: Sign Convention Mismatch

PPC and ARM64 both have fused multiply-add instructions, but their naming
conventions encode the negation differently:

- **PPC `fmsub`** (XO63 sub-opcode 28): computes `frA*frC - frB`
- **ARM64 `FMSUB`** (opcode 0x1F408000): computes `Da - Dn*Dm` (the OPPOSITE sign)

The natural mapping (`fmsub` -> `FMSUB`) is wrong. `PPC fmsub (a*c-b)` must
map to ARM64 `FNMSUB (n*m - a)`, and `PPC fnmsub (-(a*c-b) = b-a*c)` must
map to ARM64 `FMSUB (a - n*m)`. The swap applies identically to the
single-precision variants (fmsubs/fnmsubs).

The `fmadd`/`fnmadd` pair happened to map correctly because both architectures
agree on the addition case. Only the subtraction pair disagrees.

**Impact**: FP-heavy Mac OS extension-loading code produced wrong intermediate
values, causing the "Starting Up..." hang. This was one of two root causes
of the extension-loading hang (the other being bug class 7).

**Lesson**: when mapping FP multiply-add families between architectures,
verify the sign convention for EVERY variant independently. The names are
misleading -- `FMSUB` does not mean the same thing on PPC and ARM64.

### 7. CPU-Object State: Reservation and Time-Base Model

Some PPC instructions require state that lives in the CPU *object*, not in
the flat `powerpc_registers` struct the JIT operates on:

- **lwarx/stwcx.**: PPC's load-linked/store-conditional pair maintains a
  reservation address and flag in the CPU object. The JIT's regs struct has
  no reservation fields -- native codegen would need to emit loads/stores
  to the CPU object (via a saved pointer), adding complexity for instructions
  that appear rarely outside atomic loops.

- **mftb**: the CPU object maintains a time-base model that accounts for
  emulated clock scaling. Reading the raw ARM64 `CNTVCT_EL0` counter gives
  a value in host time units, not guest time units. The earlier fix (bug 2:
  LSR #32 for TBU) fixed the upper/lower split but still used the raw counter.

**Fix**: all three instructions return `false` from `compile_one()`, falling
through to the interpreter which has full access to the CPU object. The
performance cost is negligible -- lwarx/stwcx. are rare outside atomic CAS
loops, and mftb appears only in time-reading sequences (a handful per second).

**Lesson**: a JIT that operates on a flat register snapshot cannot natively
compile instructions that depend on non-register CPU state without either
(a) passing a CPU-object pointer into generated code, or (b) accepting the
interpreter fallback. For rare instructions, (b) is the right trade-off.

## The Investigation Method

Every bug was found using the same procedure:

1. **Binary search the code range** — use `SS_JIT_ROM_SIZE` or equivalent
   to narrow from megabytes to ~256 bytes of suspect code
2. **Decode the PPC opcodes** at the failing address using the JIT's own
   fetch-and-dump mechanism (the ROM is compressed; raw file reads don't work)
3. **Read the JIT codegen** for each decoded instruction — compare against
   the PPC architecture manual
4. **Fix and verify** — harness test (235/235) + boot test (VNC screenshot
   at 20s and 90s)

This method is mechanical and repeatable. Each bug takes 1-4 hours to find
once the binary search isolates the region. The hard part is recognizing
that a bug EXISTS (the symptoms are always "boot hangs" or "infinite loop"
with no crash or error message).

### Opcode-based binary search (session 7)

When the bug is in RAM (where addresses shift between boots due to extension
ASLR), address-range binary search fails. The opcode-based approach works:

1. **`SS_JIT_SKIP_OPC=<all 64>`**: force every primary opcode to the
   interpreter. If this passes, the bug is in native codegen.
2. **Remove opcodes from the skip list** in groups to find which opcode
   types must be native to trigger the bug.
3. **`SS_JIT_SKIP_XO=n,n,...`**: for XO31 (primary opcode 31) sub-opcodes,
   further narrow within the extended opcode space.
4. **`SS_JIT_SKIP_XO63=n,n,...`**: for XO63 (primary opcode 63) sub-opcodes,
   narrow within the FP extended opcode space.

This method found bugs 7-8 (session 7): narrowed from "all 64 primary opcodes"
to "XO63 sub-opcode 30 (fnmsub)" and "XO31 sub-opcodes 20+150+371
(lwarx/stwcx./mftb)" in a single session.

**Pitfall**: "skip ALL opcodes" is not just "interpret every instruction" —
it also **terminates every block after one instruction**, returning to the
C dispatcher between each. This means spcflags are checked between every
instruction. If the bug is spcflags-related rather than codegen-related,
SKIP_ALL passes for the wrong reason.

### Memory dump comparison (session 7)

Compare JIT and interpreter memory states at the same boot phase:

1. Run both modes to a fixed wall-clock time (e.g., t=15s)
2. Dump guest RAM via lldb: `memory read --binary --outfile /tmp/jit_ram.bin`
3. Binary diff: `cmp -l jit_ram.bin interp_ram.bin | wc -l`
4. Focus on low-memory globals first (0x00-0x1000) — these are set early
   and are downstream indicators of boot progress
5. Narrow the time window (t=1s, 2s, 5s) to find when divergence starts

This identified that 1.7M bytes differ at t=15s, with low-memory globals
NULL under JIT (boot hadn't reached the code that writes them).

## What's NOT Apple-Specific

All seven bug classes affect any ARM64 platform:
- The `ADDS` carry semantics are architectural ARM64, not Apple Silicon
- `CNTVCT_EL0` is the same on Linux ARM64
- Block-boundary interrupt timing is a JIT design issue, not a platform one
- icbi/isync coherence is a PPC semantic requirement, not platform-dependent
- UXTW vs LSL addressing is an ARM64 encoding choice, not Apple-specific
- FP multiply-add sign conventions are defined by the ARM64 ISA, not Apple Silicon
- CPU-object state access (reservation, time base) is a JIT design constraint, not platform-specific

The only Apple-specific concern is `MAP_JIT` for code cache allocation
(required on macOS, not needed on Linux) and the `__PAGEZERO` 4GB mapping
that prevents low-address `REAL_ADDRESSING`.

## Current Status

All 7 bug classes (8 individual bugs documented here, 12 total including bcctr and earlier
fixes) are FIXED. Mac OS 8.6 boots to Finder desktop with full native JIT -- no skip list,
no workarounds. Both HD boot (macos86_fresh.dsk, 4GB) and ISO boot work reliably.

- **subfe/adde carry**: FIXED — all three-operand CA instructions use 64-bit sums
- **mftb TBU/TBL**: FIXED — interpreter fallback (correct time-base model)
- **DR emulator timing**: FIXED — entry-poll suppression for DR blocks
- **icbi/isync coherence**: FIXED — fall through to interpreter for correctness
- **UXTW addressing**: FIXED — defensive zero-extension for register-offset mem access
- **fmsub/fnmsub encoding**: FIXED — swapped to correct ARM64 sign conventions
- **lwarx/stwcx./mftb fallback**: FIXED — interpreter fallback for CPU-object state
- **bcctr conditional**: FIXED — interpreter fallback (was the last fix for HD boot)
- **Configuration**: ROM=0x500000, chaining=1, harness 235/235 score=100
