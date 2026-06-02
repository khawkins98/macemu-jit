# C2 — Study of MAME's PowerPC DRC (PPC→UML→ARM64) for technique/code lifting

Backlog item: `IMPLEMENTATION-BACKLOG.md` §C2. Landscape entry:
`landscape-2-arm64-dynarec-projects.md` §1 (MAME PPC DRC — top lead).

**Scope:** MAME's `powerpc/ppcdrc.cpp` (PPC→UML frontend), `drcbearm64.cpp` (UML→ARM64
backend), `drccache.cpp` (W^X-aware code cache), `powerpc/ppccom.*`, `uml.*`. Compared
against our hand-written emitter in
`SheepShaver/src/kpx_cpu/src/cpu/jit/aarch64/ppc-jit.cpp`.

**License:** `ppcdrc.cpp` (© Aaron Giles) and `drcbearm64.cpp` (© windyfairy, Vas Crabb)
both carry `// license:BSD-3-Clause` SPDX headers — GPLv2-compatible, vendorable with
attribution. `drccache.cpp`/`uml.cpp` are BSD-3-Clause as well.

**Source pin:** all line numbers and quotes are from `mamedev/mame` commit
`d066f16134121f02a2fd9716582a17f8ee66f6d5` (fetched 2026-06-02). `master` URLs below will
drift; use this SHA in the URL to reproduce exactly, e.g.
`https://github.com/mamedev/mame/blob/d066f16134121f02a2fd9716582a17f8ee66f6d5/src/devices/cpu/powerpc/ppcdrc.cpp#L2543`.

---

## 1. MAME DRC architecture

MAME's recompiler is a **two-stage IR pipeline**, fundamentally different from our
direct PPC→ARM64 emission:

```
PPC bytes ──(ppcfe.cpp)──▶ opcode_desc (per-insn flag/reg read-write descriptors)
opcode_desc ──(ppcdrc.cpp generate_*)──▶ UML instruction stream (drcuml_block)
UML stream ──(drcbearm64.cpp op_* )──▶ ARM64 machine code (via asmjit a64::Assembler)
```

- **Frontend decode (`ppcfe.cpp`, 1684 lines).** `describe()` fills an `opcode_desc` per
  instruction with `regin/regout` bitmasks and, crucially, **flag liveness**: which of
  CR0..CR7, XER.CA, XER.OV are *read* and *written*. This is the dependency metadata our
  JIT has no equivalent of.
- **Frontend codegen (`ppcdrc.cpp`, 3977 lines).** `generate_sequence_instruction()` →
  `generate_opcode()` (L1945) dispatches the primary opcode; secondary groups go to
  `generate_instruction_13/1f/3b/3f`. Each handler emits a short UML sequence. A single
  shared helper, `generate_compute_flags()` (L1767), emits *all* CR0/XER updates.
- **Backend (`drcbearm64.cpp`, 5732 lines).** `generate()` walks the UML block; a big
  `switch` (L713+) dispatches each UML opcode to an `op_*` emitter. Emission goes through
  **asmjit's `a64::Assembler`** (`a.adds(...)`, `a.cset(...)`, `a.bfi(...)`), not a
  hand-rolled `emit32()`.

### How UML represents flags/carry/overflow

UML defines five condition flags (`uml.h`): `FLAG_C` (carry, bit 0), `FLAG_V` (overflow),
`FLAG_Z`, `FLAG_S`, `FLAG_U` (unordered, FP). The ARM64 backend keeps a **persistent host
register `FLAGS_REG`** holding C and U across UML ops (carry is the LSB, by deliberate
design — see `store_carry_reg`, L417). Z/S/V are *not* kept resident; they live transiently
in the host NZCV immediately after a flag-setting op and are harvested by `op_getflgs`.

The backend runs a small **carry-polarity state machine** `m_carry_state ∈
{CANONICAL, LOGICAL, POISON}` (L667) so it can skip re-materializing the carry into NZCV
when the host NZCV already holds the right polarity. PPC subtract uses *borrow* semantics
(inverted carry vs. ARM), so UML's `invertcarry` flag and the LOGICAL state track that.

Key contrast with us: **we have no IR, no flag liveness, and no flag register.** Every
record-form op unconditionally calls `lazy_update_cr0`, and every carry op reads/writes the
XER.CA byte in guest memory. MAME computes flags only when `desc->cr_required(0)` /
`xer_ca_required()` say a later instruction consumes them.

---

## 2. Per-instruction comparison (MAME vs. ours)

| Op(s) | MAME (UML) | Ours (`ppc-jit.cpp`) | Verdict |
|---|---|---|---|
| **adde** (138) | `UML_CARRY [xer],29` → `UML_ADDC rd,ra,rb` (one add-with-carry); CA harvested by `generate_compute_flags` (ppcdrc L2543) | `ADDS ra+rb`, dead `MRS NZCV`, then non-flag `ADD` of CA — **double-counts carry, drops 2nd carry-out** (L1442) | **MAME correct; we have bug A1.** Our fix (single `ADCS`) matches MAME's `ADDC`. |
| **subfe** (136) | `UML_XOR i0,[xer],XER_CA` → `UML_CARRY i0,29` → `UML_SUBB rd,rb,ra`; `invertcarry=true` (L2581) | `ADDS ~ra+rb` then non-flag `ADD` of CA — same drop (L1358) | **MAME correct; we have bug A2.** MAME's borrow-invert is the canonical model. |
| **addme** (234) | `UML_CARRY` + `UML_ADDC rd,ra,-1` (L2559) | 64-bit sum, carry = bit 32 (L1455) | Both correct; ours more verbose but right. |
| **addze** (202) | `UML_CARRY` + `UML_ADDC rd,ra,0` (L2551) | single `ADDS` (L1472) | Both correct. |
| **subfme/subfze** (232/200) | `XOR`/`CARRY`/`SUBB` (L2599/2590) | 64-bit sum / ADDS (L1482/1498) | Both correct. |
| **addco/subfco** (522/520) | `UML_ADD`/`UML_SUB` + `generate_compute_flags(..., XER_CA|XER_OV, invert)` — OV from host **V flag** harvested via `GETFLGS` (L2536/2574) | `ADDS`/`SUBS` + `emit_write_xer_ov_so_from_overflow` (`CSET VS`) (L1393) | Equivalent; both read the host V flag. Ours is fine. |
| **addo** (778) | `UML_ADD` + `generate_compute_flags(..., XER_OV)` (L2529) | `ADDS` + OV/SO write (L1413) | Equivalent. |
| **mullwo** (235+OE) | `UML_MULSLW rd,ra,rb` — dedicated UML op that sets host **V from 32-bit mul overflow**, then `generate_compute_flags(..., XER_OV)` (L2647) | Aliased onto plain `MUL`, **never sets OV/SO — silent wrong result** (bug A3) | **MAME correct.** Our fix punts to interpreter; MAME shows the inline path is `MULSLW`+OV-harvest if we ever want it. |
| **rlwinm** (21) | `UML_ROLAND ra,rs,SH,mask` — one IR op; mask = `compute_rlw_mask(mb,me)` (L2052) | ROR/EXTR then materialize 32-bit mask + reg-reg AND (L2190) | **MAME structurally better.** `ROLAND` lowers to optimal ARM64 (see §3). Our B2 item. |
| **rlwimi** (20) | `UML_ROLINS ra,rs,SH,mask` — rotate-insert-with-mask in one op (L2045) | rotate + AND mask + load rA + AND ~mask + OR (L2221, ~7 insns) | **MAME structurally better.** `ROLINS` → ARM64 BFM family. |
| **cmpw** (0) | `UML_CMP` → `UML_GETFLGS Z\|V\|C\|S` → `LOAD cmp_cr_table` → `OR cr,xerso` (L2615) | inline signed compare + CSEL nibble build | MAME uses a **256-entry lookup table** (`m_cmp_cr_table`) to map NZCV→CR nibble; elegant but needs a near-cache table. Ours is fine. |
| **cmplw** (32) | same shape, `GETFLGS Z\|C`, `m_cmpl_cr_table` (L2622) | inline unsigned | Equivalent. |
| **CR0 record form** | `generate_compute_flags` → `GETFLGS S\|Z` → `LOAD m_sz_cr_table` → `OR cr0,xerso` (L1786). **Skipped entirely if `!cr_required(0)`** (L1774) | `emit_update_cr0`: CMP #0 + 3×CSEL + OR xerso + shift + load/and/or (~18 insns), **always emitted** (L566) | **MAME far better via liveness gating + table.** Our B1 rewrite (CSET+BFI) closes part of the gap; the liveness gate is the bigger win. |
| **mfcr** (19) | 8× `ROLAND` rotate-mask-merge of CR0..7 into one word (L3401) | (our case 19, L1106) | Equivalent idiom; MAME's leans on `ROLAND`. |
| **mtcrf** (144) | per-CR-field `UML_ROLAND cr(n),rs,shift,0xf` guarded by CRM bit (L3466) | (our case 144, L1111) | Equivalent. |
| **mcrxr** (512) | `ROLAND i0,[xer],4,0x0f` + `SHL xerso,3` + `OR crd` + clear XER + zero xerso (L3525) | LDRB SO/OV/CA, shift, OR, insert into CR, clear (L1806) | Equivalent. MAME is terser because XER OV/CA are packed in the XER word (we use separate bytes). |
| **lwarx** (20) | `ADD` EA + `CALLH read32align`; **no reservation address tracked** (L3166) | treated as `lwzx`, no reservation (L1834) | **Both simplified identically — not a better oracle.** |
| **stwcx.** (150) | `CALLH write32align` then `CMP i0,i0` (always Z) → CR0.EQ — **always "succeeds"** (L3313) | always succeeds, sets CR0.EQ (L1841) | **Both identical simplification.** No lift available. |
| **FP add/sub/mul** (3F) | `UML_FDADD/FDSUB/FDMUL` (double) + `generate_fp_flags` (L3744) | native FP via VR/FP regs | Comparable. |
| **FPSCR / FP record** | FPRF computed by **C callback** `cfunc_ppccom_update_fprf` (L1854); FPSCR exception bits largely *not* maintained inline | inline `emit_sync_fpscr_rounding` syncs RN→FPCR (L642); mffs/mtfsf inline (L3287) | **Roughly even / ours arguably ahead on rounding-mode.** MAME has FPRF (we don't); MAME punts the rest to C. No clear win either direction. |

**Bottom line for §2:** MAME confirms our three known bugs (A1 adde, A2 subfe, A3 mullwo)
are real, and its carry model is the textbook fix. The genuine *structural* advantages are
**rlwinm/rlwimi** (single rotate-mask UML op) and **CR0/flag handling** (liveness-gated +
table-driven). lwarx/stwcx and FPSCR are *not* better in MAME — do not look there for lifts.

---

## 3. ARM64 backend lowering (verified code quotes)

All from `drcbearm64.cpp` @ SHA `d066f16`. Emission is via asmjit `a64::Assembler`.

### Add-with-carry (UML_ADDC → ARM64), `op_add<CarryIn=true>` L3848

Before the `adcs`, the saved carry bit is materialized into host NZCV only if the carry
state isn't already canonical (L3865-3871):

```cpp
if (CarryIn && (m_carry_state != carry_state::CANONICAL))
{
    m_carry_state = carry_state::CANONICAL;
    a.sbfx(TEMP_REG1, FLAGS_REG, uml::FLAG_BIT_C, 1);   // sign-extend carry bit: 0 or -1
    a.cmn(TEMP_REG1, 1);                                 // CMN -1,#1 sets C=1 (else C=0)
}
...
const a64::Inst::Id opcode = CarryIn
    ? (inst.flags() ? a64::Inst::kIdAdcs : a64::Inst::kIdAdc)
    : (inst.flags() ? a64::Inst::kIdAdds : a64::Inst::kIdAdd);
```

So `adde` lowers to (roughly) `sbfx; cmn; adcs` — one true add-with-carry, identical in
spirit to our designed A1 fix (`CMP Wn,#1` to set C, then `ADCS`). MAME's `cmn reg,#1`
where `reg ∈ {0,-1}` is the trick that turns a 0/1 carry bit into the ARM C flag.

### Carry capture (store), `store_carry` L1456

```cpp
void drcbe_arm64::store_carry(a64::Assembler &a, bool inverted)
{
    m_carry_state = inverted ? carry_state::LOGICAL : carry_state::CANONICAL;
    if (inverted) a.cset(SCRATCH_REG1, a64::CondCode::kCC);   // CSET cc (carry clear)
    else          a.cset(SCRATCH_REG1, a64::CondCode::kCS);   // CSET cs (carry set)
    store_carry_reg(a, SCRATCH_REG1);                          // bfxil FLAGS_REG[0] = carry
}
```

Our `emit_write_xer_ca_from_carry()` (`CSET CS` → STRB) is the same `CSET CS`, but we
store to the **guest XER.CA byte in memory**, while MAME stores to bit 0 of its resident
`FLAGS_REG`. That memory-vs-register difference is exactly why our carry chains can't be
deferred and MAME's can.

### Carry restore into NZCV, `load_carry` L1468

```cpp
a.mrs(SCRATCH_REG1, kNZCV);
a.bfi(SCRATCH_REG1, FLAGS_REG, 29, 1);     // insert carry bit into NZCV.C (bit 29)
if (inverted) a.eor(SCRATCH_REG1, SCRATCH_REG1, 1 << 29);
a.msr(kNZCV, SCRATCH_REG1);
```

### Overflow (UML_FLAG_V) capture, `op_getflgs` / `set_flags` L1485

V is read straight out of NZCV (`getflgs` masks the requested flags out of NZCV after a
flag-setting op); `set_flags` reconstructs NZCV from a saved flag word via
`bfxil ... FLAG_BIT_V` + `bfi ...,28,4`. Our `emit_write_xer_ov_so_from_overflow()`
(`CSET VS` + sticky-OR into SO) is the equivalent harvest; we just commit to memory
immediately rather than keeping V live.

### rotate-mask lowerings (the real structural win)

`UML_ROLAND` (rlwinm) → `op_roland` L3554 and `UML_ROLINS` (rlwimi) → `op_rolins` L3663
lower contiguous-bit masks to ARM64 `BFM`/`UBFM`/`AND #imm` using asmjit's logical-immediate
encoder — no mask materialization, no reg-reg AND. This is what backlog B2 wants us to do
by hand.

### W^X / writable code cache (the macOS-relevant part)

The ARM64 backend does **no** W^X management itself. It is all in `drc_cache`
(`drccache.cpp`), which is **page-granular and codegen-batched**, not per-instruction:

```cpp
// drccache.cpp L177 allocate_cache
m_cache.emplace({NEAR_CACHE_SIZE, m_size-NEAR_CACHE_SIZE},
                osd::virtual_memory_allocation::READ_WRITE_EXECUTE);
...
else if (rwx && m_cache->set_access(..., READ_WRITE_EXECUTE)) {
    osd_printf_verbose("drc_cache: RWX pages supported\n");  m_rwx = true;
} else {
    osd_printf_verbose("drc_cache: Using W^X mode\n");        m_rwx = false;
}
```

```cpp
// L63 — flip pages to RW just before writing
inline void drc_cache::ensure_writable(drccodeptr ptr) {
    if (!m_rwx && (ptr < m_rwbase)) {
        drccodeptr top = align_ptr_down(ptr, page_size());
        m_cache->set_access(top-m_near, m_rwbase-top, READ_WRITE);
        m_rwbase = top;
    }
}
// L75 — flip the just-written pages to RX at codegen end
void drc_cache::make_executable() {
    drccodeptr top = align_ptr_up(m_top, page_size());
    m_cache->set_access(m_rwbase-m_near, top-m_rwbase, READ_EXECUTE);
    m_rwbase = top;
}
```

`osd::virtual_memory_allocation::set_access` on macOS arm64 is what actually issues
`mmap(MAP_JIT)` + `pthread_jit_write_protect_np` (inside MAME's OSD layer, not these files).
**The architectural lesson for us:** MAME toggles W^X **once per codegen batch over a page
range** (`ensure_writable` … emit many instructions … `make_executable`), whereas
`jit-target-cache.hpp` brackets writes more finely and our `patch_chain_sites` currently
toggles per word (backlog B3). MAME's model is the proof-of-shape for B3's "hoist the toggle
out of the loop." It does **not** use dual RW/RX mapping (that's oaknut/C4), so it doesn't
answer the C4 spike.

---

## 4. Liftable sections

Ranked by value/effort. URLs use the pinned SHA.

1. **`compute_rlw_mask` + the ROLAND/ROLINS *idea*** (effort: low; high value).
   - Source: `ppcdrc.cpp` L180 (`compute_rlw_mask`), L2045-2060 (rlwimi/rlwinm/rlwnm);
     lowering in `drcbearm64.cpp` `op_roland` L3554 / `op_rolins` L3663.
   - Replaces: our `case 21`/`case 20` (ppc-jit.cpp L2190-2252) — the materialize-mask +
     reg-reg AND / 5-op insert sequences.
   - Adaptation: we can't lift `op_roland` verbatim (it needs asmjit's logical-immediate
     encoder). The liftable piece is `compute_rlw_mask` (trivial, identical to our inline
     loop) **plus** a standalone ARM64 logical-immediate encoder (backlog B2 already plans
     to crib one from Dolphin/oaknut). With that encoder, rlwinm becomes `ROR` + `AND #imm`
     and rlwimi becomes a `BFM`/`BFI`. Effort is in the encoder, which B2 already scopes.

2. **The carry model for adde/subfe** (effort: low; correctness-critical).
   - Source: `ppcdrc.cpp` L2543-2606 (ADDE/SUBFE/…); lowering `drcbearm64.cpp`
     `op_add<true>` L3848-3871, `store_carry` L1456, `load_carry` L1468.
   - Replaces: our buggy `case 138`/`case 136` (ppc-jit.cpp L1442/L1358).
   - Adaptation: we don't need MAME's `FLAGS_REG`. We adopt only the *shape* — read XER.CA,
     turn it into the host C flag (`sbfx;cmn` per MAME, or `CMP Wn,#1` per our A1 design),
     then a single `ADCS`/`SBCS`. This is precisely the A1/A2 fix already designed in the
     backlog; MAME is the independent confirmation. Effort: minutes (already specified).

3. **`generate_compute_flags` liveness-gated CR0/XER policy** (effort: medium-high; high value).
   - Source: `ppcdrc.cpp` L1767-1830, plus `ppcfe.cpp` `describe()` flag descriptors.
   - Replaces: the unconditional `lazy_update_cr0` / `emit_write_xer_ca_from_carry` calls
     scattered through every record-form and carry op.
   - Adaptation: **not a code lift — a design lift.** It requires the `ppcfe.cpp`-style
     per-instruction read/write descriptors (which CR fields / XER bits a later in-block
     insn consumes). We'd add a lightweight pre-pass over each block recording flag liveness,
     then gate emission. This is the single biggest structural improvement available and the
     prerequisite the Dolphin CR/carry leads were blocked on. Pairs with backlog B1.

4. **CR0/compare via NZCV→CR lookup table** (effort: low-medium; medium value).
   - Source: `ppcdrc.cpp` L1786-1788 (`m_sz_cr_table`), L2615-2627 (`m_cmp_cr_table`,
     `m_cmpl_cr_table`); table build in `ppccom.cpp`.
   - Replaces: our `emit_update_cr0` CSEL chain (ppc-jit.cpp L566) and the inline compare
     nibble builds.
   - Adaptation: allocate three small const tables (256/16 entries) in a data area
     reachable by the JIT, then `GETFLGS`(=`MRS NZCV`+mask) → `LDRB table[idx]` → `ORR
     xerso`. Competes with B1's CSET+BFI rewrite; the table is faster but needs a data
     region. Lift the table-construction code, not the x86-flavored UML.

5. **`mullwo` inline path** (effort: low; optional).
   - Source: `ppcdrc.cpp` L2647 (`UML_MULSLW`); UML def in `uml.cpp`.
   - Replaces: our A3 punt (delete `case 715`). Only worth it if profiling shows mullwo hot;
     A3's interpreter fallback is the safe default. The lift is the recipe: 64-bit `SMULL`,
     compare bits [63:32] against sign-extension of bit 31, `CSET VS`-equivalent for OV.

**Not liftable (verified):** lwarx/stwcx (MAME equally simplified), FPSCR exception bits
(MAME punts to C callback), the asmjit-based emitters (dependency, see §5).

---

## 5. Wholesale-backend feasibility (UML + drcbearm64 for SheepShaver)

**Verdict: not viable as a drop-in; viable only as a "adopt the whole UML subsystem"
project, which is large and probably not worth it given our JIT already boots to desktop.**

The headline blocker is that `drcbearm64.cpp` is **not a standalone ARM64 emitter**. It is
the bottom of a tightly-coupled stack:

```
drcbearm64.cpp  →  asmjit (a64::Assembler, external lib)         [new third-party dep]
                →  drcuml  (UML opcode set, drcuml_block, code_handle, parameters)
                →  drc_cache (near-cache tables, page W^X, codegen batching)
                →  osd::virtual_memory_allocation (MAME OSD memory layer)
```

To use `drcbearm64` you must produce a `drcuml_block` of UML instructions — i.e. you must
also adopt `drcuml` and write a SheepShaver frontend that emits UML (essentially porting
the parts of `ppcdrc.cpp` we need). You'd also pull in asmjit, `drc_cache`, and either
MAME's OSD memory layer or a reimplementation of `set_access`. "Vendor one BSD file" is
**off the table**; "vendor the UML backend subsystem" is the real shape.

**Additional integration frictions specific to MAME:**

- **Device/scheduler model.** `ppcdrc.cpp` is woven into MAME's `device_t` / cycle-counted
  scheduler: `generate_update_cycles`, `m_core->icount`, `compiler->checkints`, exception
  vectors via `m_exception[]`, `CALLC`/`CALLH` into MAME device methods. None of that maps
  onto SheepShaver's execution model (EMUL_OP traps, our own interrupt/spcflags handling).
  We'd take `generate_opcode`'s per-instruction *semantics* but rewrite all the
  block-exit / cycle / interrupt scaffolding.
- **Memory system.** MAME PPC memory goes through `static_generate_memory_accessor` (TLB,
  address spaces, `read32align` handlers). Our DIRECT_ADDRESSING (`NATMEM_OFFSET + (guest &
  0xFFFFFFFF)`, single `LDR [RMEMBASE + addr]`) is far simpler and faster for our use. We
  would *not* want MAME's memory path; UML's `LOAD`/`STORE` would have to be retargeted to
  our flat mapping.
- **State layout.** MAME's `m_core` (`ppccom.h`) packs XER (OV/CA in the word, SO separate
  in `xerso`), CR as 8 separate 32-bit fields, FPSCR, MSR, SPRs. Our `powerpc_registers`
  uses byte-wise XER (SO=1028, CA=1030) and a packed CR word. A wholesale adoption means
  re-pointing every UML `mem(&...)` at our struct.

**What a realistic "next-gen backend" path would actually be** (if ever pursued): adopt
**UML as our IR** (it is the cleanest BSD IR with a maintained ARM64 backend), write a
SheepShaver-specific frontend (`ss_ppcfe` + `ss_ppcdrc`) that emits UML against *our* state
struct and *our* DIRECT memory model, reuse `drcbearm64` + `drc_cache` + asmjit unchanged.
That is a multi-week rewrite competing against a JIT that already works. The flag-liveness
and rotate-mask wins (§4 items 1, 3) capture most of the benefit at a fraction of the cost.

---

## 6. Recommendation

1. **Confirm and ship the carry fixes (A1/A2) and the mullwo fix (A3)** — MAME independently
   validates all three. The A1/A2 designs in the backlog match MAME's `ADDC`/`SUBB` model
   exactly; no further design work needed. *(These belong to the agent currently editing
   ppc-jit.cpp — this study is the second oracle that says "yes, those are bugs, here is the
   reference fix.")*

2. **Do backlog B2 (rotate-mask via logical-immediate encoder)** using `compute_rlw_mask`
   (lift verbatim, trivial) + a cribbed logical-immediate encoder. MAME's `op_roland`/
   `op_rolins` confirm rlwinm→`AND #imm` and rlwimi→`BFI` are the right targets.

3. **Treat `generate_compute_flags` + `ppcfe` descriptors as the design target for the
   eventual flag-liveness pass** (pairs with B1). This is the highest-value structural lift,
   but it is a *design* adoption (needs a per-block flag-liveness pre-pass), not a snippet
   copy. Sequence it after the JIT-residency work (C1).

4. **Adopt MAME's page-batched W^X shape for B3** — toggle once per write-batch over a page
   range (`ensure_writable`/`make_executable`), not per word. MAME is the existence proof.

5. **Do NOT pursue MAME as a wholesale backend.** The asmjit + drcuml + drc_cache + OSD
   dependency tail and MAME's device/scheduler/memory coupling make it a multi-week rewrite
   that competes with a working JIT. If we ever want an IR, UML is the right one to adopt —
   but that decision should wait until the hand-written emitter demonstrably hits a ceiling.

6. **Do NOT look to MAME for lwarx/stwcx or FPSCR exception semantics** — verified equally
   simplified (atomics) or punted to C (FPSCR). No oracle value there.
