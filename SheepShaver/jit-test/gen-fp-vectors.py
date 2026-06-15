#!/usr/bin/env python3
# gen-fp-vectors.py — generate FP test-jit vectors for jit-test/run.sh.
#
# WHY THIS EXISTS / THE LOAD-BACK TRAP
# ------------------------------------
# The harness REGDUMP (sheepshaver_glue.cpp) captures GPR0-31, CR, LR, CTR, XER —
# but NOT the FP registers. So an FP test vector that ends at `stfd f1, mem` leaves
# its result invisible: a wrong FP answer produces an identical REGDUMP, so the
# JIT-vs-interpreter diff passes trivially. (The pre-2026-06-04 fp_* vectors all
# had this bug — they were vacuous.) Every vector here therefore finishes by
# loading the FP result back into a GPR:
#     set up operands in FPRs (int bit pattern -> stw -> lfd/lfs)
#     -> the FP op into f1
#     -> stfd/stfs f1 to scratch memory
#     -> lwz the result word(s) into r4 (and r5 for doubles)
# so the result lands in a REGDUMP-visible GPR and the diff is meaningful.
#
# Stack layout under r1 (the harness seeds r1 to a valid stack):
#     operand A @ 0x100, B @ 0x110, C @ 0x120, result @ 0x130.
#
# Hand-encoding PPC FP is error-prone, so encodings come from the small functions
# below (verified against the passing run.sh vectors). To add a vector: append an
# add(...) call, run `python3 gen-fp-vectors.py`, and splice the output into run.sh
# before the "# ---- Execute all tests" line, then gate with `make test-jit`.
#
# NOTE: only emit ops the INTERPRETER also implements — it is the harness's
# reference. fsqrt/fsqrts are intentionally omitted: the JIT computes them (ARM
# FSQRT) but the emulated 603/604/750 interpreter does not (fsqrt is optional and
# illegal on those CPUs), so they diverge and cannot be differentially tested here.

# ---- PPC instruction encoders (return a 32-bit big-endian word) ----
def lis(rD, imm):   return 0x3C000000 | (rD << 21) | (imm & 0xFFFF)          # lis rD,imm (addis rD,0,imm)
def li(rD, imm):    return 0x38000000 | (rD << 21) | (imm & 0xFFFF)          # li rD,imm  (addi rD,0,imm)
def stw(rS, d, rA): return 0x90000000 | (rS << 21) | (rA << 16) | (d & 0xFFFF)
def lwz(rD, d, rA): return 0x80000000 | (rD << 21) | (rA << 16) | (d & 0xFFFF)
def lfd(fD, d, rA): return 0xC8000000 | (fD << 21) | (rA << 16) | (d & 0xFFFF)
def lfs(fD, d, rA): return 0xC0000000 | (fD << 21) | (rA << 16) | (d & 0xFFFF)
def stfd(fS, d, rA):return 0xD8000000 | (fS << 21) | (rA << 16) | (d & 0xFFFF)
def stfs(fS, d, rA):return 0xD0000000 | (fS << 21) | (rA << 16) | (d & 0xFFFF)
def addi(rD, rA, imm): return 0x38000000 | (rD << 21) | (rA << 16) | (imm & 0xFFFF)  # addi rD,rA,imm
# FP D-form UPDATE loads/stores (lfsu/lfdu/stfsu/stfdu): EA=rA+d, rA:=EA. Opcodes 49/51/53/55.
def lfsu(fD, d, rA): return 0xC4000000 | (fD << 21) | (rA << 16) | (d & 0xFFFF)
def lfdu(fD, d, rA): return 0xCC000000 | (fD << 21) | (rA << 16) | (d & 0xFFFF)
def stfsu(fS, d, rA):return 0xD4000000 | (fS << 21) | (rA << 16) | (d & 0xFFFF)
def stfdu(fS, d, rA):return 0xDC000000 | (fS << 21) | (rA << 16) | (d & 0xFFFF)
# FP INDEXED loads/stores (X-form, opcode 31): EA=(rA?rA:0)+rB; the *ux variants also rA:=EA.
def _fx(fD, rA, rB, xo): return 0x7C000000 | (fD << 21) | (rA << 16) | (rB << 11) | (xo << 1)
def lfsx(fD, rA, rB):  return _fx(fD, rA, rB, 535)
def lfsux(fD, rA, rB): return _fx(fD, rA, rB, 567)
def lfdx(fD, rA, rB):  return _fx(fD, rA, rB, 599)
def lfdux(fD, rA, rB): return _fx(fD, rA, rB, 631)
def stfsx(fS, rA, rB): return _fx(fS, rA, rB, 663)
def stfsux(fS, rA, rB):return _fx(fS, rA, rB, 695)
def stfdx(fS, rA, rB): return _fx(fS, rA, rB, 727)
def stfdux(fS, rA, rB):return _fx(fS, rA, rB, 759)
# A-form FP op (fadd/fsub/fmul/fdiv/fmadd/...): opcode, frD, frA, frB, frC, XO, Rc.
def aform(op, fD, fA, fB, fC, xo, rc=0):
    return (op << 26) | (fD << 21) | (fA << 16) | (fB << 11) | (fC << 6) | (xo << 1) | rc
# X-form FP op (fsqrt/frsp/fctiwz/fneg/fmr/...): opcode, frD, frB, XO, Rc (frA=0).
def xform(op, fD, fA, fB, xo, rc=0):
    return (op << 26) | (fD << 21) | (fA << 16) | (fB << 11) | (xo << 1) | rc
def mtfsfi(crfD, imm): return (63 << 26) | (crfD << 23) | (imm << 12) | (134 << 1)  # mtfsfi crfD,IMM (XO 134)
def H(w): return "%08X" % (w & 0xFFFFFFFF)

# ---- operand setup + result load-back helpers ----
# A double whose high 32 bits are (hi16<<16) and low 32 bits are 0 (covers small
# integers like 2.0/3.0/4.0). single uses the 32-bit pattern (hi16<<16) directly.
def setd(fT, hi16, off): return [lis(3, hi16), stw(3, off, 1), li(3, 0), stw(3, off + 4, 1), lfd(fT, off, 1)]
def sets(fT, hi16, off): return [lis(3, hi16), stw(3, off, 1), lfs(fT, off, 1)]
def grabd(off):          return [stfd(1, off, 1), lwz(4, off, 1), lwz(5, off + 4, 1)]  # double -> r4:r5
def grabs(off):          return [stfs(1, off, 1), lwz(4, off, 1)]                       # single -> r4

VECTORS = []
def add(name, words, comment): VECTORS.append((name, " ".join(H(w) for w in words), comment))

# Operand makers: A=2.0, B=3.0, C=4.0 (double) at distinct stack slots.
A = lambda: setd(1, 0x4000, 0x100)   # 2.0 -> f1
B = lambda: setd(2, 0x4008, 0x110)   # 3.0 -> f2
C = lambda: setd(3, 0x4010, 0x120)   # 4.0 -> f3

# ---- double-precision (opcode 63) ----
add("fp_fadd_real",   A() + B() + [aform(63, 1, 1, 2, 0, 21)] + grabd(0x130), "fadd f1,f1,f2: 2.0+3.0=5.0")
add("fp_fsub_real",   A() + B() + [aform(63, 1, 1, 2, 0, 20)] + grabd(0x130), "fsub f1,f1,f2: 2.0-3.0=-1.0")
add("fp_fmul_real",   A() + B() + [aform(63, 1, 1, 0, 2, 25)] + grabd(0x130), "fmul f1,f1,f2(C): 2.0*3.0=6.0")
add("fp_fdiv_real",   A() + C() + [aform(63, 1, 1, 3, 0, 18)] + grabd(0x130), "fdiv f1,f1,f3: 2.0/4.0=0.5")
# fmadd result = frA*frC + frB. In the ENCODING the frB field (bits 16-20) comes
# before the frC field (bits 21-25), but the assembler order is frD,frA,frC,frB.
# So aform(...,fB,fC,...) takes fB then fC: fB=f2(3.0), fC=f3(4.0) -> 2*4+3=11.0.
# (An earlier version swapped these and silently computed 2*3+4=10.0.)
add("fp_fmadd_real",  A() + B() + C() + [aform(63, 1, 1, 2, 3, 29)] + grabd(0x130), "fmadd f1,f1,f3,f2 = f1*f3+f2: 2.0*4.0+3.0=11.0")
add("fp_fmsub_real",  A() + B() + C() + [aform(63, 1, 1, 2, 3, 28)] + grabd(0x130), "fmsub f1,f1,f3,f2 = f1*f3-f2: 2.0*4.0-3.0=5.0")
add("fp_fnmadd_real", A() + B() + C() + [aform(63, 1, 1, 2, 3, 31)] + grabd(0x130), "fnmadd: -(2.0*4.0+3.0)=-11.0")
add("fp_fnmsub_real", A() + B() + C() + [aform(63, 1, 1, 2, 3, 30)] + grabd(0x130), "fnmsub: -(2.0*4.0-3.0)=-5.0")
add("fp_frsp_real",   B() + [xform(63, 1, 0, 2, 12)] + grabd(0x130), "frsp f1,f2: round 3.0 to single")
add("fp_fctiwz_real", B() + [xform(63, 1, 0, 2, 15)] + grabd(0x130), "fctiwz f1,f2: 3.0 -> int 3")
# fctiw/fctiwz edge cases FIXED 2026-06-06 (ppc-jit.cpp case 14/15): rounding mode,
# INT32 overflow saturation, NaN->0x80000000. frB into f2 via setd(2,hi16,off);
# 2.5=0x4004, 2^31=0x41E0, NaN=0x7FF8 (all low-word 0). fctiw rounds half-AWAY (frin):
# 2.5->3, DISTINCT from fctiwz toward-zero 2.5->2 (proves fctiw is not a fctiwz clone).
add("fp_fctiw_round",  setd(2,0x4004,0x110) + [xform(63,1,0,2,14)] + grabd(0x130), "fctiw 2.5 -> 3 (round half-away per FPSCR)")
add("fp_fctiwz_trunc", setd(2,0x4004,0x110) + [xform(63,1,0,2,15)] + grabd(0x130), "fctiwz 2.5 -> 2 (toward zero)")
add("fp_fctiw_ovf",    setd(2,0x41E0,0x110) + [xform(63,1,0,2,14)] + grabd(0x130), "fctiw 2^31 -> 0x7FFFFFFF (saturate)")
add("fp_fctiw_nan",    setd(2,0x7FF8,0x110) + [xform(63,1,0,2,14)] + grabd(0x130), "fctiw NaN -> 0x80000000")
add("fp_fctiwz_nan",   setd(2,0x7FF8,0x110) + [xform(63,1,0,2,15)] + grabd(0x130), "fctiwz NaN -> 0x80000000")
# NOTE: mtfsf/mtfsfi had their codegen bodies SWAPPED vs their XOs (case 711 ran the mtfsfi
# decode, case 134 ran the mtfsf decode); fixed 2026-06-07 by swapping the case labels. The
# obvious differential test (mtfsfi sets RN; fctiw rounds per it: `[mtfsfi(7,2)] + setd(2,0x4002,
# 0x110) + [xform(63,1,0,2,14)] + grabd(0x130)`) does NOT pass, because JIT fctiw uses a FIXED
# rounding mode (FRINTA) and ignores the dynamic FPSCR RN that mtfsfi sets — a SEPARATE limitation.
# That repro lives in run.sh QUARANTINE (fp_fctiw_dynround) as a known divergence. The swap fix
# itself is correct by inspection (each case body decodes the OTHER instruction's operand fields).
add("fp_fneg_real",   A() + [xform(63, 1, 0, 1, 40)] + grabd(0x130), "fneg f1,f1: -(2.0)")
add("fp_fabs_real",   A() + [aform(63, 1, 0, 1, 0, 264)] + grabd(0x130), "fabs f1,f1: |2.0|=2.0")
add("fp_fmr_real",    B() + [xform(63, 1, 0, 2, 72)] + grabd(0x130), "fmr f1,f2: copy 3.0")
# fsel frD,frA,frC,frB = (frA >= 0) ? frC : frB. aform packs frB(bits11-15) then frC(bits6-10),
# so aform(63,frD,frA,frB,frC,23). Two vectors exercise BOTH select arms (zero-copy FP RA 2026-06-07).
add("fp_fsel_pos", A() + B() + C() + [aform(63, 1, 1, 2, 3, 23)] + grabd(0x130), "fsel f1,f1(2.0>=0),f3,f2 -> frC=4.0")
add("fp_fsel_neg", setd(1,0xBFF0,0x100) + B() + C() + [aform(63, 1, 1, 2, 3, 23)] + grabd(0x130), "fsel f1,f1(-1.0<0),f3,f2 -> frB=3.0")

# ---- single-precision (opcode 59) ----
As = lambda: sets(1, 0x4000, 0x100)  # 2.0f
Bs = lambda: sets(2, 0x4040, 0x110)  # 3.0f
Cs = lambda: sets(3, 0x4080, 0x120)  # 4.0f
add("fp_fadds_real",  As() + Bs() + [aform(59, 1, 1, 2, 0, 21)] + grabs(0x130), "fadds: 2.0f+3.0f=5.0f")
add("fp_fsubs_real",  As() + Bs() + [aform(59, 1, 1, 2, 0, 20)] + grabs(0x130), "fsubs: 2.0f-3.0f=-1.0f")
add("fp_fmuls_real",  As() + Bs() + [aform(59, 1, 1, 0, 2, 25)] + grabs(0x130), "fmuls: 2.0f*3.0f=6.0f")
add("fp_fdivs_real",  As() + Cs() + [aform(59, 1, 1, 3, 0, 18)] + grabs(0x130), "fdivs: 2.0f/4.0f=0.5f")
add("fp_fmadds_real", As() + Bs() + Cs() + [aform(59, 1, 1, 2, 3, 29)] + grabs(0x130), "fmadds f1,f1,f3,f2 = f1*f3+f2: 2.0f*4.0f+3.0f=11.0f")

# ---- FP-RA eviction pressure (>8 live FPRs) ----
# The FP register allocator (P5b) has 8 slots (V16-V23). With <=8 live FPRs nothing
# evicts, so the other vectors never exercise eviction/dirty-writeback. Here f10=2.0,
# then write f0..f8 = f10*f10 = 4.0 (9 dirty dests > 8 slots) -> the LRU dirty FPRs
# (incl. f0) are evicted AND written back to the struct; finally read f0 back: it must
# be 4.0, which proves the eviction writeback + reload-from-struct path (the one path
# the FP-RA review 2026-06-07 found untested). A dropped-without-writeback bug -> f0
# reads 0.0 and the interp-vs-JIT diff fails.
ev = (setd(10, 0x4000, 0x100)
      + [aform(63, k, 10, 0, 10, 25) for k in range(9)]   # fmul fK,f10,f10  (K=0..8)
      + [stfd(0, 0x130, 1), lwz(4, 0x130, 1), lwz(5, 0x134, 1)])  # grab f0 (the evicted one)
add("fp_evict_writeback", ev, "9 FP dests (f0..f8) force FP-RA eviction; evicted f0 must read back 4.0")

# ---- FP INDEXED + UPDATE memory (P5b follow-up 2026-06-07) ----
# These cover the indexed (lf{s,d}x / lf{s,d}ux / stf{s,d}x / stf{s,d}ux) and D-form
# UPDATE (lf{s,d}u / stf{s,d}u) FP loads/stores, which were converted from the FMOV
# bridge to zero-copy FP-RA access. The D-form non-update lf{s,d}/stf{s,d} (already
# zero-copy) are covered by fp_lfs_stfs / fp_lfd_stfd above; these 12 were UNCOVERED.
# Construction: operand 5.0 written to [r1+0x100]; EA reached two ways so a bad EA
# diverges — indexed via base r6=r1+0x80 + index r7=0x80, update via base r6=r1 + d=0x100.
# The FP result lands in r4(/r5) via grab (REGDUMP captures GPRs, not FPRs -> non-vacuous);
# the UPDATE forms additionally writeback rA into r6, which REGDUMP captures directly, so a
# wrong EA-writeback also diverges. 5.0 double=0x40140000:0 ; 5.0 single=0x40A00000.
def wr_dbl(off, hi16): return [lis(3, hi16), stw(3, off, 1), li(3, 0), stw(3, off + 4, 1)]
def wr_sgl(off, hi16): return [lis(3, hi16), stw(3, off, 1)]
# indexed loads: r6=r1+0x80, r7=0x80 -> EA=r1+0x100
add("fp_lfdx",   wr_dbl(0x100, 0x4014) + [addi(6,1,0x80), li(7,0x80), lfdx(1,6,7)]  + grabd(0x130), "lfdx f1,r6,r7: load 5.0 double via indexed EA")
add("fp_lfdux",  wr_dbl(0x100, 0x4014) + [addi(6,1,0x80), li(7,0x80), lfdux(1,6,7)] + grabd(0x130), "lfdux f1,r6,r7: load 5.0 + rA(r6):=EA")
add("fp_lfsx",   wr_sgl(0x100, 0x40A0) + [addi(6,1,0x80), li(7,0x80), lfsx(1,6,7)]  + grabd(0x130), "lfsx f1,r6,r7: load 5.0f single via indexed EA")
add("fp_lfsux",  wr_sgl(0x100, 0x40A0) + [addi(6,1,0x80), li(7,0x80), lfsux(1,6,7)] + grabd(0x130), "lfsux f1,r6,r7: load 5.0f + rA(r6):=EA")
# indexed stores: load 5.0 into f1 (slot 0x110), store to [r1+0x100] via EA, read back
add("fp_stfdx",  setd(1,0x4014,0x110) + [addi(6,1,0x80), li(7,0x80), stfdx(1,6,7)]  + [lwz(4,0x100,1), lwz(5,0x104,1)], "stfdx f1,r6,r7: store 5.0 double via indexed EA")
add("fp_stfdux", setd(1,0x4014,0x110) + [addi(6,1,0x80), li(7,0x80), stfdux(1,6,7)] + [lwz(4,0x100,1), lwz(5,0x104,1)], "stfdux f1,r6,r7: store 5.0 + rA(r6):=EA")
add("fp_stfsx",  setd(1,0x4014,0x110) + [addi(6,1,0x80), li(7,0x80), stfsx(1,6,7)]  + [lwz(4,0x100,1)], "stfsx f1,r6,r7: store 5.0f single via indexed EA")
add("fp_stfsux", setd(1,0x4014,0x110) + [addi(6,1,0x80), li(7,0x80), stfsux(1,6,7)] + [lwz(4,0x100,1)], "stfsux f1,r6,r7: store 5.0f + rA(r6):=EA")
# D-form update: r6=r1, d=0x100 -> EA=r1+0x100, r6:=EA
add("fp_lfdu",   wr_dbl(0x100, 0x4014) + [addi(6,1,0), lfdu(1,0x100,6)]  + grabd(0x130), "lfdu f1,0x100(r6): load 5.0 double + r6:=EA")
add("fp_lfsu",   wr_sgl(0x100, 0x40A0) + [addi(6,1,0), lfsu(1,0x100,6)]  + grabd(0x130), "lfsu f1,0x100(r6): load 5.0f single + r6:=EA")
add("fp_stfdu",  setd(1,0x4014,0x110) + [addi(6,1,0), stfdu(1,0x100,6)]  + [lwz(4,0x100,1), lwz(5,0x104,1)], "stfdu f1,0x100(r6): store 5.0 double + r6:=EA")
add("fp_stfsu",  setd(1,0x4014,0x110) + [addi(6,1,0), stfsu(1,0x100,6)]  + [lwz(4,0x100,1)], "stfsu f1,0x100(r6): store 5.0f single + r6:=EA")

# ---- FP-RA eviction ACROSS an indexed/update memory op (2026-06-07 review hardening) ----
# The 12 FP indexed/update mem vectors above are LOW register pressure, so they never force
# ra_fp_store/ra_fp_load to EVICT during the mem op. Here: f10=2.0, then fmul f0..f7 = 4.0
# (fill all 8 FP-RA slots, V16-V23, dirty), THEN lfdux f8 from memory — its ra_fp_store(f8)
# must evict an LRU dirty FPR and write it back. We read back f0 (an evicted dirty reg, must be
# 4.0) and f8 (the loaded value, 5.0), plus r6 carries the update-writeback EA. The interpreter
# has NO register allocator (every FPR is architectural), so any JIT eviction-writeback bug
# diverges. Operand 5.0 at [r1+0x108]; EA via r6=r1+0x08 + r7=0x100.
evx = (setd(10, 0x4000, 0x100)                                   # f10 = 2.0
       + [aform(63, k, 10, 0, 10, 25) for k in range(8)]         # fmul f0..f7 = f10*f10 = 4.0 (fill 8 slots)
       + [lis(3, 0x4014), stw(3, 0x108, 1), li(3, 0), stw(3, 0x10c, 1)]  # 5.0 -> [r1+0x108]
       + [addi(6, 1, 0x08), li(7, 0x100), lfdux(8, 6, 7)]        # f8 = 5.0; evict LRU; r6 := r1+0x108
       + [stfd(0, 0x130, 1), lwz(4, 0x130, 1)]                   # f0 (evicted) -> r4 hi (expect 40100000)
       + [stfd(8, 0x138, 1), lwz(5, 0x138, 1)])                  # f8 -> r5 hi (expect 40140000)
add("fp_evict_across_lfdux", evx, "lfdux while FP-RA full: evicted f0 must read 4.0, f8=5.0, r6=EA")

if __name__ == "__main__":
    print("# ==== FP arithmetic coverage (generated by gen-fp-vectors.py) ================")
    print("# Each vector loads its FP result back into r4(/r5) because REGDUMP captures")
    print("# GPRs, not FPRs (without this the test would be vacuous). See the script header.")
    for name, hexwords, comment in VECTORS:
        print("# %s" % comment)
        print('T_%s="%s"' % (name, hexwords))
        print("TEST_ORDER+=(%s)" % name)
