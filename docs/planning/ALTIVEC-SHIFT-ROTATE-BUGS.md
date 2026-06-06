# AltiVec variable-shift / rotate codegen bugs (confirmed 2026-06-06)

**Status:** CONFIRMED real JIT codegen bugs (found by the test-jit/`SS_TEST_HEX` hunt, ROADMAP
A1 "AltiVec/FP operand coverage"). Repros below are oracle-validated against the **real emulator**
interpreter (`SS_TEST_HEX … SS_TEST_JIT=0` vs `=1`).

> **✅ ALL FIXED 2026-06-06** (`ppc-jit.cpp` case 4/68/132 rotates, 260/324/388 left, 516/580/644
> logical-right, 772/836/900 arith-right). The whole 12-op family:
> - **Shifts:** mask amount mod element width (`DUP (w-1)`→v2, `AND`), then the correct
>   **truncating** NEON op — `USHL` (left / logical-right with `NEG`), `SSHL` (arith-right with
>   `NEG`). The old code emitted *rounding* (`SRSHL`/`URSHL`), *signed* logical shifts, and `vsrb`
>   shifted the wrong direction; nothing masked the amount.
> - **Rotates:** NEON has no vector rotate — synthesized `rol(x,k)=(x<<k)|(x>>>(w-k))`, `k=amt&(w-1)`.
> - All capstone-verified encodings, validated against byte-**asymmetric** lvx operands (so the
>   ev_mixed byte order is covered — empirically a non-issue for these per-element ops). **12 strong
>   committed vectors** (`av_vsl{b,h,w}`, `av_vsr{b,h,w}`, `av_vsra{b,h,w}`, `av_vrl{b,h,w}`),
>   `make test-jit` **282/282**. Boot smoke: see CHANGELOG.
>
> The sections below are the original discovery record (repros, root cause) — kept for provenance.

> How found: whole AltiVec families had **zero** test coverage (shifts, rotates, unpacks,
> saturating, integer-compares). This is the same lane/width-sensitive category that produced the
> 9 `ev_mixed` bugs. Leading with the family where ARM64 NEON is *not* 1:1 with AltiVec (variable
> shifts/rotates) turned up real divergences within minutes.

## The oracle

REGDUMP now includes all 32 VR, so a single instruction's result is directly observable — no
store/load needed. Each repro: 3 instructions (two `vspltisb` to build operands, one op into `v2`),
read `VR2` from the dump.
```
SS_TEST_HEX="<words>" SS_TEST_DUMP=1 SS_TEST_JIT=0 ./src/Unix/SheepShaver   # interp (oracle)
SS_TEST_HEX="<words>" SS_TEST_DUMP=1 SS_TEST_JIT=1 ./src/Unix/SheepShaver   # JIT
```
`vspltisb vD,SIMM` = `vx(vD, SIMM&0x1F, 0, 780)`; `vx(vD,fA,fB,xo)=(4<<26)|(vD<<21)|(fA<<16)|(fB<<11)|xo`.

## Bug 1 — variable shifts lack mod-element-width masking  (CONFIRMED: `vslb`)

AltiVec masks each per-element shift amount mod the element width (byte→mod 8, halfword→mod 16,
word→mod 32). The JIT emits NEON `USHL`/`SSHL` with the raw amount and **no mask**, so an amount ≥
element width shifts the bits out (→0 / sign) instead of wrapping.

- Repro (`vspltisb v1,4; vspltisb v3,9; vslb v2,v1,v3`):
  `SS_TEST_HEX="1024030C 1069030C 10411904"`
  - interp `VR2 = 08·16` (0x04 << (9 mod 8) = <<1)
  - **JIT `VR2 = 00·16`** (USHL by 9 ≥ 8 → 0)
- Affected (same codegen, shift amount ≥ width): `vslb/vslh/vslw`, `vsrb/vsrh/vsrw`,
  `vsrab/vsrah/vsraw`, and `vrlb/vrlh/vrlw`. (Only `vslb` is triggerable with `vspltisb` operands —
  byte mask mod 8, amount 9..15; halfword/word need lvx-built amounts ≥16/≥32, so commit those
  repros with `load_pattern`.)
- Fix direction: AND the amount vector with `(width-1)` per element before the shift.

## Bug 2 — logical right shifts emit the wrong shift  (CONFIRMED: `vsrb`, `vsrh`, `vsrw`)

`vsr{b,h,w}` are **logical** (zero-fill) right shifts. The JIT emits arithmetic shifts, and `vsrb`
additionally shifts the **wrong direction**. (`vsra{b,h,w}` arithmetic right are correct — e.g.
`vsrab` verified below.)

- `vsrb` (xo 516), `0xF0 >> 1` (data `vspltisb v1,-16`=0xF0, amt `vspltisb v3,1`):
  `SS_TEST_HEX="1030030C 1061030C 10411A04"`
  - interp `VR2 = 78·16` (logical right: 0xF0→0x78)
  - **JIT `VR2 = e0·16`** (0xF0 shifted **left** by 1)
- `vsrh` (xo 580), `0xF0F0 >> 1`: `SS_TEST_HEX="1030030C 1061030C 10411A44"`
  - interp `VR2 = 7878·8` ; **JIT `VR2 = f878·8`** (arithmetic / sign-filled, not logical)
- `vsrw` (xo 644), `0xF0F0F0F0 >> 1`: `SS_TEST_HEX="1030030C 1061030C 10411A84"`
  - interp `VR2 = 78787878·4` ; **JIT `VR2 = f8787878·4`** (arithmetic, not logical)
- Control — `vsrab` (xo 772), `0xF0 >> 1` arithmetic: `SS_TEST_HEX="1030030C 1061030C 10411B04"`
  → interp == JIT == `f8·16` ✓ (arithmetic right shifts are correct).
- Fix direction: the logical right shifts must zero-fill (negate amount, then **unsigned** shift);
  `vsrb` is additionally missing the negate. Validate each against the repro above.

## Bug 3 — rotates implemented as shifts  (SUSPECTED by inspection — needs lvx repro)

`vrl{b,h,w}` rotate each element left by the amount; the current codegen path uses a plain NEON
shift (no wrap-around), so the bits that should rotate in from the other end are lost, and the
amount is unmasked (Bug 1). Not yet reproduced (a meaningful rotate needs a high-bit-set element,
which `vspltisb` can't build; use `load_pattern`). Fix: synthesize rotate
`(x << (n & m)) | (x >> (w - (n & m)))` with `m = width-1`.

## Why this matters / scope
Any guest code using AltiVec variable shifts or logical right shifts on values with the high bit
set, or shift amounts ≥ element width, gets wrong results. These ops are common in codecs,
blitters, and DSP. The bugs survived because the suite had **zero** shift/rotate coverage.

## Next step (fix pass — greenlight)
1. Add the repros above to `jit-test/gen-altivec-vectors.py` as tracked **xfail** vectors (the
   quarantine lane), incl. lvx-built halfword/word masking + rotate repros, so the suite *captures*
   the divergence (and the fix flips them xfail→xpass → promote to TEST_ORDER).
2. Fix the codegen op-by-op in `ppc-jit.cpp` (~lines 3320–3331, 3446–3447), validating each against
   its repro via `SS_TEST_HEX`, then `make test-jit` (270) + a boot smoke.
3. Extend the same hunt to the other zero-coverage families (saturating add/sub, integer compares,
   unpacks) — lower NEON-divergence odds but untested.

Tracked: ROADMAP A1, CHANGELOG 2026-06-06.
