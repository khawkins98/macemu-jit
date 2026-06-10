# M1 Task 9 — Verified MMIO backpatch thunk encodings

Pre-work for Machine Layer M1 Task 9 (the JIT MMIO backpatch thunk — the most
encoding-sensitive code in the milestone). Everything here was produced and
verified by the standalone spike at `spikes/m1-thunk-prework/` on a macOS arm64
host (clang, ad-hoc signed, MAP_JIT). The spike does **not** touch ppc-jit.cpp or
any emulator source.

- `spikes/m1-thunk-prework/thunk_ref.c` — encoder helpers + thunk emitter + end-to-end test + clang cross-check table.
- `spikes/m1-thunk-prework/clang_ref.s` — reference mnemonics assembled by clang.
- `spikes/m1-thunk-prework/extract_xcheck.sh` — disassembles `clang_ref.o`, diffs words against the helpers.
- `spikes/m1-thunk-prework/Makefile` — `make run` (end-to-end), `make xcheck` (clang diff), `make all`.

## Headline results

- **End-to-end thunk test: PASS.** A BL into the emitted MAP_JIT thunk, callback
  mutates `frame[1]`/`frame[27]` and actively poisons every other saved slot;
  after return x1==mutated, x27==mutated, x5 and v16 survived unclobbered, **SP
  balanced** (captured before/after the BL, byte-identical).
- **clang cross-check: ALL MATCH (18/18 helpers).** Every encoder helper produces
  the exact word clang's assembler emits for the same mnemonic.
- **Thunk size: 66 instructions = 264 bytes.** Frame: `sub sp,sp,#624` plus the
  pre-index fp/lr pair (16 bytes) = **640 bytes of stack** below the caller's SP.

---

## 1. Verified frame layout

The thunk runs on the CPU/emul thread (no §2g constraints). Entry contract from the
patched site: `BL thunk` sets **x30 = site + 4**; **EA is in w0 (RTMP0)** but the
thunk reads it generically from `frame[rm]`, not from a fixed register.

```
  stp x29, x30, [sp, #-16]!     ; push fp/lr  (SP -= 16)
  sub sp, sp, #624              ; allocate frame  (624 is a 16-multiple → SP stays 16-aligned)
```

Offsets are **relative to the new SP** (= the frame base passed to the callback as x0):

| bytes        | contents                          | how saved                         |
|--------------|-----------------------------------|-----------------------------------|
| 0 … 216      | x0..x27 (14 pairs, x{i} at i*8)   | `STP x{r},x{r+1},[sp,#r*8]`        |
| 224          | x28                               | `STR x28,[sp,#224]`               |
| 232          | **8-byte gap** (alignment pad)    | —                                 |
| 240 … 592    | q0–q7, q16–q31 (12 pairs, 384 B)  | `STP q{n},q{n+1},[sp,#…]` (240..624)|

Total frame = `624` (the SUB) `+ 16` (the pre-index fp/lr pair) = **640 bytes**.

Key facts:
- `frame` is a `uint64_t*`; **`frame[i] == x{i}` for i in 0..28** — so the dispatcher
  reads `frame[acc.rm]` (the guest EA register) and writes `frame[acc.rt]` (the load
  destination). Restore picks the mutation up.
- The **8-byte gap at offset 232** exists because the STP-Q signed-offset imm7 scales
  by 16: the Q region must start at a 16-byte multiple, and x28 ends at 232. Start Q
  at **240**, not 232. (The plan sketch already starts Q at 240 — its `qoff=240` and
  the "30 slots" rounding handle this gap correctly; called out here only because the
  helper `assert(imm%16==0)` makes the constraint explicit.)
- SUB immediate **must be a 16-multiple** (only hard alignment rule for the BLR). 624 is.
- x0..x17, v0–v7, v16–v31 are caller-saved and may be live (guest FPRs live in
  v16–v23). x18 is saved too (slot regularity / it is reserved on Darwin anyway).
  x19–x28 are callee-saved w.r.t. the C callback, but the guest GPR allocator parks
  guest state there, so the thunk saves/restores them through the frame regardless.

---

## 2. Verified helper encodings

Each helper returns the 32-bit instruction word. `sp == reg 31` in the load/store
base position. **All formulas below were confirmed byte-identical to clang.**

| Helper | Formula | Notes |
|---|---|---|
| `emit_stp_x(rt,rt2,imm)` | `0xA9000000 \| (imm7<<15) \| (rt2<<10) \| (31<<5) \| rt`, `imm7=imm/8` | 64-bit STP, signed imm7 (−64..63), scale 8 |
| `emit_ldp_x(rt,rt2,imm)` | `0xA9400000 \| (imm7<<15) \| (rt2<<10) \| (31<<5) \| rt`, `imm7=imm/8` | L bit set |
| `emit_str_x_imm(rt,imm)` | `0xF9000000 \| (imm12<<10) \| (31<<5) \| rt`, `imm12=imm/8` | unsigned-offset, scale 8 |
| `emit_ldr_x_imm(rt,imm)` | `0xF9400000 \| (imm12<<10) \| (31<<5) \| rt`, `imm12=imm/8` | L bit set |
| `emit_stp_q(qt,qt2,imm)` | `0xAD000000 \| (imm7<<15) \| (qt2<<10) \| (31<<5) \| qt`, `imm7=imm/16` | 128-bit SIMD STP, **scale 16** |
| `emit_ldp_q(qt,qt2,imm)` | `0xAD400000 \| (imm7<<15) \| (qt2<<10) \| (31<<5) \| qt`, `imm7=imm/16` | L bit set |
| `emit_sub_sp_imm(imm)` | `0xD1000000 \| (imm12<<10) \| (31<<5) \| 31` | 64-bit SUB imm, shift=0, imm12 raw (≤4095) |
| `emit_add_sp_imm(imm)` | `0x91000000 \| (imm12<<10) \| (31<<5) \| 31` | 64-bit ADD imm |
| `mov x0, sp` | `0x910003E0` (= `ADD x0, sp, #0`) | **must be the ADD alias**; the ORR-MOV form uses XZR → x0=0 |
| `sub x1, x30, #4` | `0xD10013C1` | site = x30 − 4 |
| `stp x29,x30,[sp,#-16]!` | `0xA9BF7BFD` | pre-index, imm7 = −2 |
| `ldp x29,x30,[sp],#16` | `0xA8C17BFD` | post-index, imm7 = +2 |
| `blr x16` | `0xD63F0000 \| (16<<5)` = `0xD63F0200` | call dispatcher |
| `ret` | `0xD65F03C0` | |
| `emit_bl(off)` | `0x94000000 \| ((off>>2) & 0x3FFFFFF)` | off = target − site in **bytes**; ±128 MB range |
| `emit_load_imm64(rd,imm)` | MOVZ + 3×MOVK | `MOVZ: 0xD2800000\|(hw<<21)\|(imm16<<5)\|rd`; `MOVK: 0xF2800000\|(hw<<21)\|(imm16<<5)\|rd`, hw=0..3 |

All helpers `assert()` their immediate alignment/range (`imm%8==0` for X forms,
`imm%16==0` for Q forms, imm12 < 4096), which is the cheap insurance the plan asked for.

### clang cross-check table (verified output)

```
MNEMONIC                         HELPER       CLANG        RESULT
stp x0, x1, [sp, #0]             0xa90007e0   0xa90007e0   MATCH
stp x2, x3, [sp, #16]            0xa9010fe2   0xa9010fe2   MATCH
stp x26, x27, [sp, #208]         0xa90d6ffa   0xa90d6ffa   MATCH
ldp x0, x1, [sp, #0]             0xa94007e0   0xa94007e0   MATCH
ldp x26, x27, [sp, #208]         0xa94d6ffa   0xa94d6ffa   MATCH
str x28, [sp, #224]              0xf90073fc   0xf90073fc   MATCH
ldr x28, [sp, #224]              0xf94073fc   0xf94073fc   MATCH
stp q0, q1, [sp, #240]           0xad0787e0   0xad0787e0   MATCH
stp q30, q31, [sp, #592]         0xad12fffe   0xad12fffe   MATCH
ldp q0, q1, [sp, #240]           0xad4787e0   0xad4787e0   MATCH
sub sp, sp, #624                 0xd109c3ff   0xd109c3ff   MATCH
add sp, sp, #624                 0x9109c3ff   0x9109c3ff   MATCH
mov x0, sp                       0x910003e0   0x910003e0   MATCH
sub x1, x30, #4                  0xd10013c1   0xd10013c1   MATCH
stp x29, x30, [sp, #-16]!        0xa9bf7bfd   0xa9bf7bfd   MATCH
ldp x29, x30, [sp], #16          0xa8c17bfd   0xa8c17bfd   MATCH
blr x16                          0xd63f0200   0xd63f0200   MATCH
ret                              0xd65f03c0   0xd65f03c0   MATCH
XCHECK: ALL MATCH
```

---

## 3. Full verified thunk instruction listing

66 instructions. The four words at indices **31–34** (the `emit_load_imm64` of the
dispatcher address) vary per run because the callback address is subject to ASLR —
that is correct (they encode a function pointer); only their MOVZ/MOVK *shape* is
fixed.

```
 [ 0] 0xa9bf7bfd   stp  x29, x30, [sp, #-16]!
 [ 1] 0xd109c3ff   sub  sp, sp, #624
 [ 2] 0xa90007e0   stp  x0, x1, [sp, #0]
 [ 3] 0xa9010fe2   stp  x2, x3, [sp, #16]
 [ 4] 0xa90217e4   stp  x4, x5, [sp, #32]
 [ 5] 0xa9031fe6   stp  x6, x7, [sp, #48]
 [ 6] 0xa90427e8   stp  x8, x9, [sp, #64]
 [ 7] 0xa9052fea   stp  x10, x11, [sp, #80]
 [ 8] 0xa90637ec   stp  x12, x13, [sp, #96]
 [ 9] 0xa9073fee   stp  x14, x15, [sp, #112]
 [10] 0xa90847f0   stp  x16, x17, [sp, #128]
 [11] 0xa9094ff2   stp  x18, x19, [sp, #144]
 [12] 0xa90a57f4   stp  x20, x21, [sp, #160]
 [13] 0xa90b5ff6   stp  x22, x23, [sp, #176]
 [14] 0xa90c67f8   stp  x24, x25, [sp, #192]
 [15] 0xa90d6ffa   stp  x26, x27, [sp, #208]
 [16] 0xf90073fc   str  x28, [sp, #224]
 [17] 0xad0787e0   stp  q0, q1, [sp, #240]
 [18] 0xad088fe2   stp  q2, q3, [sp, #272]
 [19] 0xad0997e4   stp  q4, q5, [sp, #304]
 [20] 0xad0a9fe6   stp  q6, q7, [sp, #336]
 [21] 0xad0bc7f0   stp  q16, q17, [sp, #368]
 [22] 0xad0ccff2   stp  q18, q19, [sp, #400]
 [23] 0xad0dd7f4   stp  q20, q21, [sp, #432]
 [24] 0xad0edff6   stp  q22, q23, [sp, #464]
 [25] 0xad0fe7f8   stp  q24, q25, [sp, #496]
 [26] 0xad10effa   stp  q26, q27, [sp, #528]
 [27] 0xad11f7fc   stp  q28, q29, [sp, #560]
 [28] 0xad12fffe   stp  q30, q31, [sp, #592]
 [29] 0x910003e0   mov  x0, sp                 ; x0 = frame base
 [30] 0xd10013c1   sub  x1, x30, #4            ; x1 = site
 [31] 0xd2..cb90   movz x16, #...              ; \
 [32] 0xf2..0fd0   movk x16, #..., lsl #16     ;  > &ppc_jit_mmio_thunk_dispatch (ASLR-varying)
 [33] 0xf2c00030   movk x16, #0, lsl #32       ; /
 [34] 0xf2e00010   movk x16, #0, lsl #48       ; /
 [35] 0xd63f0200   blr  x16
 [36] 0xa94007e0   ldp  x0, x1, [sp, #0]
 [37] 0xa9410fe2   ldp  x2, x3, [sp, #16]
 [38] 0xa94217e4   ldp  x4, x5, [sp, #32]
 [39] 0xa9431fe6   ldp  x6, x7, [sp, #48]
 [40] 0xa94427e8   ldp  x8, x9, [sp, #64]
 [41] 0xa9452fea   ldp  x10, x11, [sp, #80]
 [42] 0xa94637ec   ldp  x12, x13, [sp, #96]
 [43] 0xa9473fee   ldp  x14, x15, [sp, #112]
 [44] 0xa94847f0   ldp  x16, x17, [sp, #128]
 [45] 0xa9494ff2   ldp  x18, x19, [sp, #144]
 [46] 0xa94a57f4   ldp  x20, x21, [sp, #160]
 [47] 0xa94b5ff6   ldp  x22, x23, [sp, #176]
 [48] 0xa94c67f8   ldp  x24, x25, [sp, #192]
 [49] 0xa94d6ffa   ldp  x26, x27, [sp, #208]
 [50] 0xf94073fc   ldr  x28, [sp, #224]
 [51] 0xad4787e0   ldp  q0, q1, [sp, #240]
 [52] 0xad488fe2   ldp  q2, q3, [sp, #272]
 [53] 0xad4997e4   ldp  q4, q5, [sp, #304]
 [54] 0xad4a9fe6   ldp  q6, q7, [sp, #336]
 [55] 0xad4bc7f0   ldp  q16, q17, [sp, #368]
 [56] 0xad4ccff2   ldp  q18, q19, [sp, #400]
 [57] 0xad4dd7f4   ldp  q20, q21, [sp, #432]
 [58] 0xad4edff6   ldp  q22, q23, [sp, #464]
 [59] 0xad4fe7f8   ldp  q24, q25, [sp, #496]
 [60] 0xad50effa   ldp  q26, q27, [sp, #528]
 [61] 0xad51f7fc   ldp  q28, q29, [sp, #560]
 [62] 0xad52fffe   ldp  q30, q31, [sp, #592]
 [63] 0x9109c3ff   add  sp, sp, #624
 [64] 0xa8c17bfd   ldp  x29, x30, [sp], #16
 [65] 0xd65f03c0   ret
```

(The save covers x0..x27 as 14 STP pairs `[2]`..`[15]` — pair `[11]` is the
`x18,x19` pair, so x18 is banked too — plus x28 via the single STR `[16]`. That is
the plan sketch's "x0..x28 (29 slots)" realized as 14 pairs + 1 STR. The listing
above is authoritative.)

---

## 4. Corrections the Task 9 implementer must apply to the plan sketch

The plan (`docs/superpowers/plans/2026-06-10-machine-layer-m1.md` Task 9, Step 1)
explicitly distrusts its own hand-packed immediates. The spike added the sketch's
**exact literal expressions** as extra rows in the clang cross-check and reran it —
the table below is the empirical verdict (not an eyeball read), and it shows only
**one** sketch line is actually broken:

```
[sketch] stp x0,x1,[sp,#0]       0xa90007e0   0xa90007e0   MATCH
[sketch] stp x2,x3,[sp,#16]      0xa9010fe2   0xa9010fe2   MATCH
[sketch] stp x26,x27,[sp,#208]   0xa90d6ffa   0xa90d6ffa   MATCH
[sketch] str x28,[sp,#224]       0xf90070fc   0xf90073fc   *** MISMATCH ***
[sketch] stp q0,q1,[sp,#240]     0xad0787e0   0xad0787e0   MATCH
```

1. **`str x28` is wrong in the sketch — THIS IS THE ONE REAL BUG.** The sketch's
   `0xF90000FC | (28<<0) | ((28*8/8)<<10)` evaluates to **`0xf90070fc`**, which
   decodes as `STR x28, [x7, #224]` — Rt=28 ✓ and imm12→224 ✓, but **Rn=7, not sp
   (31)**. It would store the saved x28 to a wild address off x7 (the byte
   `0xfc` = bits [9:5]=7 in the Rn field). Correct word: **`0xf90073fc`**
   (`emit_str_x_imm(28, 224)`); matching load **`0xf94073fc`**. Replace with the
   helper.

2. **The STP / STP-Q loop immediates are CORRECT in the sketch — no change needed
   for correctness.** Verified: for a pair based at frame offset `r*8`, the sketch's
   `((r*8)/8)<<15` is exactly `imm7 = byteoffset/8`, byte-identical to the helper
   (e.g. r=2 → `0xa9010fe2`, qoff=240 → `0xad0787e0`). *Recommendation* (not a bug
   fix): still replace the inline packing with the `emit_stp_x` / `emit_stp_q`
   helpers from §2, purely for the built-in `assert(imm%8==0)` / `assert(imm%16==0)`
   insurance — but do not "fix" them under the belief they are mis-encoded.

3. **Frame size — tighten, not a bug.** The sketch's `SUB #640` over-allocates by 16
   bytes; it is still 16-aligned and works. Prefer **`SUB #624`** (the fp/lr pair is
   the separate pre-index push; total stack = 624 + 16 = 640). The only hard rule —
   SUB immediate a 16-multiple — holds for both. Q region correctly starts at 240.

4. **`mov x0, sp` must be the ADD alias `0x910003E0`** — the sketch has the right
   bits; do not "simplify" it to an ORR-register MOV: reg 31 there is XZR, so that
   form sets x0 = 0, silently breaking the frame-base argument.

5. **`sub x1, x30, #4` = `0xD10013C1`** and **`blr x16` = `0xD63F0200`** — both
   confirmed; keep as written.

6. **BL at the patch site** = `0x94000000 | ((off>>2)&0x3FFFFFF)`, off in bytes,
   ±128 MB range. (Reserve the thunk at cache start so every site is in range, as
   the plan notes.)

7. **Dispatcher reads the EA from `frame[acc.rm]`, not w0.** Verified that the
   thunk passes `frame` base in x0 and `frame[i]==x{i}`; the generic decode of
   rt/rm/width/direction from the side table is the right design. (The spike's
   callback exercises `frame[0]` as the EA slot for its synthetic rm=0 vector.)

### Open item for Task 9 (out of scope for this encoding spike)

- **NZCV / lazy CR0 is NOT saved by this thunk.** If lazy-CR0 can be live in NZCV
  across a load/store access site, the C callback (and the BLR itself) destroys it.
  The interpreter-call precedent (`emit_inline_interp_call`) requires the caller to
  `ra_flush_all` + flush lazy CR0 *before* the call. The backpatcher must guarantee
  the same invariant holds at every patched site, or the thunk must additionally
  save/restore NZCV (`MRS x,NZCV` / `MSR NZCV,x` around the BLR). **Verify this
  liveness during Task 9 implementation** — it is the one correctness gap the
  encoding work cannot close on its own.
- `lwarx`/`stwcx.` sites lose reservation semantics if patched (same as the cold
  fault path) — acceptable per the plan; leave the comment.
