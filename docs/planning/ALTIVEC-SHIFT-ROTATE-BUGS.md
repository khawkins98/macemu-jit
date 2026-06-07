# AltiVec / FP JIT codegen bug hunt (2026-06-06)

*(Filename kept as `ALTIVEC-SHIFT-ROTATE-BUGS.md` for stable links; scope is now the whole
2026-06-06 AltiVec/FP differential sweep — shift/rotate **and** saturating arith, averages, FP
`fctiw`, and the still-open pack/pixel/sum families. The reusable sweep tool is
`SheepShaver/tools/jit-diff-sweep.py`.)*

**Status:** 27 codegen bugs found + fixed this session (26 AltiVec + 1 FP), each oracle-validated
against the **real emulator** interpreter (`SS_TEST_HEX … SS_TEST_JIT=0` vs `=1`); the pack/pixel/
sum families + the `fctiw`/`fctid` non-default-RN gap remain (designs below). Found via the
test-jit/`SS_TEST_HEX` hunt, ROADMAP A1 "AltiVec/FP operand coverage".

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

---

# Broad-sweep results (2026-06-06) — second bug cluster + remaining worklist

After the shift/rotate family, a broad differential sweep ran **every untested VX-form AltiVec op**
(JIT vs real interpreter, distinct operands) + a sub-agent audit (canonical XO from the interpreter
decode table, NEON disassembled with capstone). Outcome:

**✅ FIXED — saturating add/sub + signed averages (14 ops):**
- `vaddubs/uhs/uws` (512/576/640), `vaddsbs/shs/sws` (768/832/896): were **SABA/UABA** (abs-diff) →
  now **UQADD/SQADD**.
- `vsububs/uhs` (1536/1600), `vsubsbs/shs/sws` (1792/1856/1920): **signed/unsigned swapped** → now
  correct **UQSUB/SQSUB**.
- `vavgsb/sh/sw` (1282/1346/1410): were **SMAXP** → now **SRHADD** (signed rounding average).
- All capstone-verified, validated mid+boundary+overflow (42/42), 14 committed vectors, test-jit 296.

**✅ Confirmed CORRECT (no fix needed):** integer compares `vcmpequb/uh`, `vcmpgt{u,s}{b,h,w}`
(all pass); unsigned averages `vavgu*`.

**🟡 REMAINING (structural — overlap ROADMAP A2, deferred):**
- **Pack-saturate** `vpkshss/swss` (398/462 emit FP-narrow `fcvtn` — wrong), `vpkshus/swus`
  (270/334), `vpkuhus/uwus` (142/206 wrong signed/unsigned narrow), `vpkuwum` (78, known-tracked):
  all 2-source narrows needing ev_mixed lane order — the A2 class.
- **Pixel** `vpkpx` (782), `vupkhpx` (846 — XO routed to a float-convert case!), `vupklpx` (974):
  need the 1-5-5-5 pixel field expansion, not a width narrow/widen.
- **Sum-across** `vsumsws` (1928), `vsum2sws` (1672), `vsum4sbs` (1800): the `case` labels are
  **rotated** (each emits a neighbour's reduction) and none apply PPC saturation.
- **FP conversions** `vctsxs` (970), `vctuxs` (906): right base instruction but ignore the UIMM
  2^scale factor — incomplete, not a wrong-opcode bug.
- Note: sweep XOs 910/354/418/1038/1356/1420/1932 are **not real AltiVec opcodes** (the matching
  JIT `case` labels are dead code at the wrong XO) — test artifacts, not bugs.

> **✅ ALL 6 SATURATING PACKS FIXED 2026-06-07** (`ppc-jit.cpp` `emit_vpk_h2b` cases
> 398/270/142 halfword→byte, `emit_vpk_w2h` cases 462/334/206 word→halfword). Derived
> empirically against the interpreter REGDUMP per the methodology below, with **saturation-
> crossing operands** (negatives + over-range) so the three signednesses are DISTINCT — the
> non-saturating-operand false-PASS trap (point 1 in the attempt log) is closed. 6 committed
> vectors (`av_vpksh{ss,us}`, `av_vpkuhus`, `av_vpksw{ss,us}`, `av_vpkuwus`), `make test-jit`
> 324/324. Key findings: (a) the pre-fix "UQXTN" `0x2E212800` was actually **SQXTUN** (opcode
> 10010 not 10100) → unsigned packs clamped negatives-as-signed to 0; (b) halfword→byte needs
> REV32+REV16 input normalize (.8H values) + REV32 byte-output normalize; (c) word→halfword
> needs **NO** input normalize (raw .4S already holds correct word values — NEON LE read cancels
> ev_mixed) + REV32+REV16 halfword-output normalize; (d) REV16/REV32 commute. Pixel + sum-across
> remain.

## Pack-saturate fix design (2026-06-06, derived — ✅ SHIPPED 2026-06-07, see above)

Canonical XOs (decode table, authoritative): `vpkshss=398` (signed→signed sat),
`vpkshus=270` (signed→unsigned sat), `vpkswss=462`, `vpkswus=334`, `vpkuhus=142`
(unsigned→unsigned sat), `vpkuwus=206`. The JIT `case` comments are swapped, and **270/398 have
the saturation signedness swapped** (270 emits `SQXTN`, should be `SQXTUN`; 398 emits `SQXTUN`,
should be `SQXTN`). **But that's not the whole bug:** `vpkuhus` (142) emits the *correct* `UQXTN`
yet still fails — because none of these do the ev_mixed normalize or the 2-source narrow.

**Interp semantics** (`execute_vector_pack`, ppc-execute.cpp:1521): result element `i` =
`saturate(vA.element[i])` for `i < n/2`, else `saturate(vB.element[i-n/2])`. So **vA fills the
high-order half, vB the low-order half**, in PPC element order (element 0 = most significant),
using the ev_mixed element accessors.

**Correct NEON recipe** (per op, halfword→byte shown; word→half is `.4S`→`.4H`):
1. `REV32.16B` normalize vA and vB into the natural element order (as `emit_vmrg` does).
2. Narrow with the **signedness-correct** saturating op into the two halves of vD:
   - `vpkshss`: `SQXTN  Vd.8B, vA.8H` + `SQXTN2  Vd.16B, vB.8H` (signed→signed)
   - `vpkshus`: `SQXTUN Vd.8B, vA.8H` + `SQXTUN2 Vd.16B, vB.8H` (signed→unsigned)
   - `vpkuhus`: `UQXTN  Vd.8B, vA.8H` + `UQXTN2  Vd.16B, vB.8H` (unsigned→unsigned)
   - …matching word variants (`vpkswss/swus/uwus`).
3. Place vA in the **high-order** half and vB in the low — mind that PPC element-0-is-high is the
   reverse of NEON lane order, so the half assignment + a final `REV32` (or REV64) normalize must
   be derived empirically against the interp (distinct operands per half + saturation boundaries).

**Validation:** distinct, sign-crossing operands in *both* halves (so a half-swap or lane-order
bug can't pass), values that saturate at the high and low clamps, signed vs unsigned sources.
Reuse `gen-altivec-vectors.py` `load_bytes` for crafted operands. This is the same intricacy class
as `emit_vmrg` (which took several rounds) — **do it as a focused pass, not inline**, validating
each op against the real interp before moving on.

(Pixel `vpkpx`/`vupkhpx`/`vupklpx` and sum-across `vsumsws`/`vsum2sws`/`vsum4sbs` remain separately
— pixel needs 1-5-5-5 field expansion; sum-across has rotated case labels + missing saturation.)

### Attempt log (2026-06-06) — what NOT to repeat

A first implementation (`emit_vpk`: `REV` normalize → `[SU]QXTN`/`QXTN2` 2-source narrow → `REV`
back) was written, validated against the interp, and **reverted** because the byte-order model was
wrong. Hard-won findings for the next attempt:
1. **The saturating-operand trap (cost me a false "PASS").** Operands like vA=`0x00..0x0F` make
   every narrowed element saturate to `0x7f`/`0xff`, so any lane permutation is invisible and a
   wrong impl looks correct. **Always test with small, distinct, non-saturating operands** (e.g. vA
   halfwords 1..8, vB 0x11..0x18) so the full lane map is observable.
2. **The byte-order premise was wrong.** I reasoned "VR raw bytes = the memory bytes `lvx` loaded",
   but `emit_load_vr` is a raw `LDR Q` of the **interpreter's ev_mixed-stored VR** — and `lvx`
   itself applies the ev_mixed transform at *load* time. So the JIT's `v0` is NOT the natural memory
   layout; reasoning from the `SS_TEST_HEX` operand bytes to the NEON lane values is invalid.
   Symptom: every byte-pack narrowed to `0x7f` (NEON read each small halfword byte-unswapped as a
   large value). Neither `REV32.16B` nor `REV16.16B` input-normalize fixed it.
3. **Do this first next time:** empirically map the ev_mixed layout the JIT actually sees — e.g. a
   tiny `lvx; stvx` round-trip vector, or dump `v0` after `emit_load_vr` for a known operand — and
   derive the normalize from *observed* lane values, not from a model. Then `emit_vmrg`'s working
   `REV32.16B`-for-permute precedent + the `vmul_byte` `REV32.8H`-output precedent are the anchors.
   Validate each of the 6 ops against the interp with the non-saturating operands from (1).
