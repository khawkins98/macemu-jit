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
# A-form FP op (fadd/fsub/fmul/fdiv/fmadd/...): opcode, frD, frA, frB, frC, XO, Rc.
def aform(op, fD, fA, fB, fC, xo, rc=0):
    return (op << 26) | (fD << 21) | (fA << 16) | (fB << 11) | (fC << 6) | (xo << 1) | rc
# X-form FP op (fsqrt/frsp/fctiwz/fneg/fmr/...): opcode, frD, frB, XO, Rc (frA=0).
def xform(op, fD, fA, fB, xo, rc=0):
    return (op << 26) | (fD << 21) | (fA << 16) | (fB << 11) | (xo << 1) | rc
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
add("fp_fmadd_real",  A() + B() + C() + [aform(63, 1, 1, 3, 2, 29)] + grabd(0x130), "fmadd f1,f1,f3,f2: 2*4+3=11.0")
add("fp_fmsub_real",  A() + B() + C() + [aform(63, 1, 1, 3, 2, 28)] + grabd(0x130), "fmsub f1,f1,f3,f2: 2*4-3=5.0")
add("fp_fnmadd_real", A() + B() + C() + [aform(63, 1, 1, 3, 2, 31)] + grabd(0x130), "fnmadd: -(2*4+3)=-11.0")
add("fp_fnmsub_real", A() + B() + C() + [aform(63, 1, 1, 3, 2, 30)] + grabd(0x130), "fnmsub: -(2*4-3)=-5.0")
add("fp_frsp_real",   B() + [xform(63, 1, 0, 2, 12)] + grabd(0x130), "frsp f1,f2: round 3.0 to single")
add("fp_fctiwz_real", B() + [xform(63, 1, 0, 2, 15)] + grabd(0x130), "fctiwz f1,f2: 3.0 -> int 3")
add("fp_fneg_real",   A() + [xform(63, 1, 0, 1, 40)] + grabd(0x130), "fneg f1,f1: -(2.0)")
add("fp_fabs_real",   A() + [aform(63, 1, 0, 1, 0, 264)] + grabd(0x130), "fabs f1,f1: |2.0|=2.0")
add("fp_fmr_real",    B() + [xform(63, 1, 0, 2, 72)] + grabd(0x130), "fmr f1,f2: copy 3.0")

# ---- single-precision (opcode 59) ----
As = lambda: sets(1, 0x4000, 0x100)  # 2.0f
Bs = lambda: sets(2, 0x4040, 0x110)  # 3.0f
Cs = lambda: sets(3, 0x4080, 0x120)  # 4.0f
add("fp_fadds_real",  As() + Bs() + [aform(59, 1, 1, 2, 0, 21)] + grabs(0x130), "fadds: 2.0f+3.0f=5.0f")
add("fp_fsubs_real",  As() + Bs() + [aform(59, 1, 1, 2, 0, 20)] + grabs(0x130), "fsubs: 2.0f-3.0f=-1.0f")
add("fp_fmuls_real",  As() + Bs() + [aform(59, 1, 1, 0, 2, 25)] + grabs(0x130), "fmuls: 2.0f*3.0f=6.0f")
add("fp_fdivs_real",  As() + Cs() + [aform(59, 1, 1, 3, 0, 18)] + grabs(0x130), "fdivs: 2.0f/4.0f=0.5f")
add("fp_fmadds_real", As() + Bs() + Cs() + [aform(59, 1, 1, 3, 2, 29)] + grabs(0x130), "fmadds: 2*4+3=11.0f")

if __name__ == "__main__":
    print("# ==== FP arithmetic coverage (generated by gen-fp-vectors.py) ================")
    print("# Each vector loads its FP result back into r4(/r5) because REGDUMP captures")
    print("# GPRs, not FPRs (without this the test would be vacuous). See the script header.")
    for name, hexwords, comment in VECTORS:
        print("# %s" % comment)
        print('T_%s="%s"' % (name, hexwords))
        print("TEST_ORDER+=(%s)" % name)
