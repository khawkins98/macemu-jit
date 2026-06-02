# B2 prep — ARM64 logical-immediate encoder for rlwinm/rlwimi

Implementation-prep for backlog item **B2** (`IMPLEMENTATION-BACKLOG.md`, Tier B):
port a dependency-free ARM64 bitmask-immediate encoder and use it to emit
`AND Wd,Wn,#imm` in the PPC rotate-mask paths instead of materialize-mask +
reg-reg AND. This doc + two standalone files are everything the JIT agent
needs to integrate with a tiny diff.

**Deliverables (already written, standalone, no edits to the agent's files):**
- `SheepShaver/src/kpx_cpu/src/cpu/jit/aarch64/ppc-logical-imm.hpp` — the encoder
- `SheepShaver/spikes/logical-imm/test_logical_imm.c` + `Makefile` — exhaustive test
- this doc

---

## 1. Encoding background

ARM64 logical instructions (`AND/ORR/EOR/ANDS` immediate forms, and `TST`/
`ORN`-via-alias) do not take an arbitrary immediate. They take a 13-bit
**bitmask immediate** packed as three fields: `N` (bit 22), `immr` (bits
21:16), `imms` (bits 15:10). Hardware reconstructs the actual constant from
these via the ARM ARM `DecodeBitMasks()` pseudocode:

- `len` = position of the highest set bit of `(N:NOT(imms))` (7-bit value).
  `esize = 1 << len` is the element size (2, 4, 8, 16, 32, or 64). `N=1`
  selects `esize=64`; for 32-bit instructions `N` must be 0.
- Within each `esize`-bit element the immediate is a single **contiguous run
  of ones**, `(imms_low + 1)` bits long, **rotated right by `immr`**.
- That element is **replicated** to fill the 32- or 64-bit datapath.

So the set of encodable values is exactly: *take a contiguous run of 1..(esize-1)
ones in an esize-bit field, rotate it arbitrarily, replicate it.* All-zeros and
all-ones are **not** encodable (a run is never empty or full).

**Why this matters for PPC:** `rlwinm`/`rlwimi`/`rlwnm` masks are generated from
`MB..ME` — by construction a contiguous (possibly wrap-around) run of ones in a
32-bit word. That is precisely a rotated contiguous run, so essentially every
PPC mask lands in the bitmask-immediate domain and collapses to one instruction.

The encoder here is the inverse of `DecodeBitMasks`: it finds the element period,
normalizes the run to bottom-justified, verifies it really is a single run, and
packs `N:immr:imms`. This is the published ARM algorithm — the same routine
implemented in Dolphin's `Arm64Emitter.h` `LogicalImm` (GPL-2.0-or-later) and in
oaknut (MIT). It was written independently from the ARM pseudocode and
cross-referenced against those (the Dolphin source could not be fetched at
authoring time; correctness rests on the assembler oracle below, not on copying).

---

## 2. Header API + design notes

`ppc-logical-imm.hpp` — header-only, deps only `<stdint.h>`/`<stdbool.h>`,
`static inline`, matches ppc-jit.cpp comment/naming style, GPL-2.0-or-later.

```c
uint32_t ppc_mask(int mb, int me);
bool a64_encode_logical_imm(uint64_t value, bool is_64bit,
                            uint32_t *out_n_immr_imms);
```

- **`ppc_mask(mb, me)`** — the exact MB..ME mask generator (contiguous and
  wrap-around) currently open-coded inline in the rlwinm/rlwimi/rlwnm cases. PPC
  bit 0 = `0x80000000`. Lets the JIT replace four copies of that loop with one
  call (optional cleanup; not required for B2).

- **`a64_encode_logical_imm(value, is_64bit, out)`** — returns `false` if `value`
  is not a logical immediate. On success writes, **pre-positioned for OR-ing
  into the instruction word**, the field
  `(N << 22) | (immr << 16) | (imms << 10)` into `*out`. Design choice: returning
  it pre-shifted (rather than three separate fields) means the caller just ORs it
  into the opcode base — no field assembly at the call site, no ambiguity about
  bit positions.

  `is_64bit=false` is the rlwinm/rlwimi case: only the low 32 bits are
  considered, the value is internally replicated into both 64-bit halves to find
  the true period, and `N` is forced 0 (returns false if a 32-bit value somehow
  needed `N=1`, which cannot happen for true 32-bit masks).

**Caller opcode bases (X-forms set bit 31; for 32-bit W-forms N is 0):**
```
AND  Wd,Wn,#imm : 0x12000000 | enc | (Rn << 5) | Rd
ORR  Wd,Wn,#imm : 0x32000000 | enc | (Rn << 5) | Rd
EOR  Wd,Wn,#imm : 0x52000000 | enc | (Rn << 5) | Rd
ANDS Wd,Wn,#imm : 0x72000000 | enc | (Rn << 5) | Rd   (sets NZCV)
```
> **NOTE for the integrator:** the immediate AND base is **`0x12000000`**, NOT
> the reg-reg `0x0A000000` currently used in these cases. Copying the old base
> with the new operand layout is the obvious foot-gun. ORR-imm is `0x32000000`
> (vs reg-reg `0x2A000000`), EOR-imm `0x52000000` (vs `0x4A000000`).

---

## 3. Test results (verbatim, all passing)

Build/run: `cd SheepShaver/spikes/logical-imm && make run`
(Apple clang 17.0.0; clang is used both to build the test and as a hardware
assembler oracle.)

The test uses **three independent oracles** so a bug in the encoder cannot hide:
1. **Oracle 1 — round-trip via an independent `DecodeBitMasks` reimplementation**
   (reconstructs the mask *from* `N:immr:imms`, written from the ARM pseudocode,
   structurally distinct from the encoder — not encoder⁻¹).
2. **Oracle 2 — the clang integrated assembler.** Every encodable mask is
   assembled as `and w0,w1,#<mask>`; bits [22:10] of the resulting word are
   compared to the encoder output. Unencodable values make clang error, which
   also confirms rejection. This is the authoritative hardware oracle.
3. **Oracle 3 — known-value rejection** (0, all-ones) + breadth spot-checks.

```
== a64_encode_logical_imm exhaustive PPC-mask test ==
PPC masks encodable: 992 / 1024
Round-trip (oracle1) failures: 0
OK: 992 encodable as predicted (32 all-ones masks rejected)
  [oracle2] clang assembler agrees on all 992 encodable masks
OK: zero rejected by both encoder and clang
OK: all-ones-32 rejected by both encoder and clang
OK: 0xF (4-bit run) encodable=1 (clang agrees)
OK: 0xFFFFFFF8 (rotated run) encodable=1 (clang agrees)
OK: 0x80000001 (wrap run) encodable=1 (clang agrees)
OK: 0x55555555 (period-2) encodable=1 (clang agrees)
OK: 0x00010001 (period-16) encodable=1 (clang agrees)
OK: 0x7 encodable=1 (clang agrees)
OK: 0xF0 encodable=1 (clang agrees)
OK: 0x12345678 (arbitrary) encodable=0 (clang agrees)

ALL TESTS PASSED (0 failure group(s))
```

**Coverage result: 992 / 1024 PPC `(mb,me)` masks encode in one instruction.**
The 32 that do not are exactly the all-ones masks — `mb=0,me=31` plus the 31
wrap pairs where `me == mb-1` — all of which produce `0xFFFFFFFF`, which is not a
logical immediate. The current JIT already special-cases `mask == 0xFFFFFFFF`
(skips the AND entirely in rlwinm/rlwnm), so these never need an encoded AND.
`ppc_mask` never produces 0, so 0 does not occur among the 1024.

A separate 64-bit sanity run (`is_64bit=true`) was cross-checked against clang
`and x0,x1,#imm` and matches on `0xFFFFFFFFFFFF0000` (N=1), `0x5555...5555`,
`0xF`, `0x8000000000000001`, and correctly rejects `0`, `~0`, and an arbitrary
value — so the 64-bit path is also good, though B2 only uses the 32-bit form.

---

## 4. Integration plan for ppc-jit.cpp

> **Line numbers below are from the working tree on 2026-06-02 and WILL drift**
> (another agent is actively editing this file). Locate the cases by their
> `case NN:` labels and the literal `emit_load_imm32(RTMP1, (int32_t)mask)` /
> `emit32(0x0A000000 | ...)` pattern, not by line number.

**Step 0 — include the header.** Near the top of `ppc-jit.cpp`, with the other
includes (after `#include "ppc-codegen-aarch64.h"`):
```c
#include "ppc-logical-imm.hpp"
```

**The change is always conditional — never a replacement.** The reg-reg
materialize+AND fallback MUST remain for the unencodable case (all-ones, which is
a legal rlwimi mask). This keeps the harness at 100 regardless of encoder
coverage. Pattern in every site:
```c
uint32_t enc;
if (mask != 0xFFFFFFFF && a64_encode_logical_imm(mask, /*is_64bit=*/false, &enc)) {
    /* AND Wd,Wn,#imm — 1 instruction, no scratch clobber */
    emit32(0x12000000 | enc | (RTMP0 << 5) | RTMP0);
} else {
    /* existing fallback: materialize mask, reg-reg AND */
    emit_load_imm32(RTMP1, (int32_t)mask);
    emit32(0x0A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
}
```

### 4a. `case 21: /* rlwinm */` (was ~line 2190; mask AND ~line 2212)

Current (after the rotate/EXTR, mask already computed into `uint32_t mask`):
```c
		if (mask != 0xFFFFFFFF) {
			emit_load_imm32(RTMP1, (int32_t)mask);
			emit32(0x0A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* AND */
		}
```
Replace the body of the `if (mask != 0xFFFFFFFF)` with the conditional-encode
pattern above (the `mask != 0xFFFFFFFF` guard stays; inside it, try the encoder,
fall back to load+reg-AND). Net: 992/1024 masks drop from 2-3 insns to 1 and stop
clobbering RTMP1. No change to the rotate, store, or `lazy_update_cr0` lines.

### 4b. `case 23: /* rlwnm */` (was ~line 896; mask AND ~line 912)

Identical shape to rlwinm (same `if (mask != 0xFFFFFFFF) { emit_load_imm32; AND }`
block). Apply the same conditional-encode replacement. `RTMP0` holds the rotated
value here too.

### 4c. `case 20: /* rlwimi */` (was ~line 2221; the double-mask ~line 2243)

Current:
```c
		emit_load_imm32(RTMP1, (int32_t)mask);
		emit32(0x0A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* rotated & mask */
		emit_load_gpr(RTMP2, ra);
		emit_load_imm32(RTMP1, (int32_t)~mask);
		emit32(0x0A000000 | (RTMP1 << 16) | (RTMP2 << 5) | RTMP2); /* rA & ~mask */
		emit32(0x2A000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0); /* OR (reg-reg) */
```
Two masks here: `mask` (on the rotated value in RTMP0) and `~mask` (on the
original rA in RTMP2). **The complement of a rotated contiguous run is also a
rotated contiguous run**, so when `mask` is encodable `~mask` is too — both
collapse to one insn. Replace each `emit_load_imm32 + AND` with the conditional
pattern (encode `mask` for the RTMP0 AND, encode `~mask` for the RTMP2 AND). The
final `OR (reg-reg, 0x2A000000)` stays unchanged — it is a register-register OR
of two computed values, not an immediate.
> Edge case the fallback covers: if `mask == 0xFFFFFFFF` (legal rlwimi),
> `mask` is unencodable → fall back; `~mask == 0` → `rA & 0 = 0`, and the
> existing reg-reg path with `emit_load_imm32(RTMP1, 0)` handles it correctly.
> Do not add a `mask != 0xFFFFFFFF` guard in rlwimi (unlike rlwinm) — rlwimi
> must always produce all three operations.

### 4d. (Optional, not required for B2) andi./andis./ori/oris/xori/xoris

`case 28/29` (andi./andis.) and `case 24..27` (ori/oris/xori/xoris) also do
`emit_load_imm32 + reg-reg AND/ORR/EOR`. These immediates are *not* always
contiguous runs (UIMM is arbitrary 16-bit), so the conditional-encode pattern
applies (with bases `0x12000000`/`0x32000000`/`0x52000000`, and ANDS=`0x72000000`
for the record-form CR0 cases) but will fall back more often. Lower priority;
mention only — B2's verified target is the rotate-mask paths.

### Verification after integrating

1. `cd SheepShaver && make test-opcodes` → must stay at score 100 (rlwinm/rlwimi
   vectors exist, e.g. `rlwimi_insert`).
2. `make run-jit` → boot to desktop.
3. Optional: disassemble a compiled rlwinm block and confirm a single
   `and w,w,#imm` where the old code emitted movz/movk+and.

---

## 5. License note

`ppc-logical-imm.hpp` carries an **SPDX-License-Identifier: GPL-2.0-or-later**
header, consistent with the macemu/SheepShaver tree.

The algorithm is the **public ARM Architecture Reference Manual** routine (the
inverse of the AArch64 `DecodeBitMasks` pseudocode), implemented independently
and re-typed to this JIT's plain-`uint32_t`-register convention. It was
**cross-referenced** against the Dolphin Emulator Project's `LogicalImm`
(`Source/Core/Common/Arm64Emitter.h`, GPL-2.0-or-later) and oaknut (MIT), both of
which implement the same well-known routine. **No third-party source was copied
verbatim** (the Dolphin file could not be fetched during authoring; the
implementation's correctness is established by the clang-assembler oracle, not by
derivation). The header credits ARM as the algorithm source and notes the
Dolphin/oaknut cross-reference. GPL-2.0-or-later is compatible with the GPLv2
tree either way.
