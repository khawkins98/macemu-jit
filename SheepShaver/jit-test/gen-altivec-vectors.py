#!/usr/bin/env python3
# gen-altivec-vectors.py — generate AltiVec test-jit vectors for jit-test/run.sh.
#
# TWO TRAPS THIS FILE EXISTS TO AVOID (both hit real shipped vectors, 2026-06-04):
#
# 1. VX-FORM XO IS UNSHIFTED. Unlike X-form/A-form (where XO sits at bits 21-30
#    and is emitted as xo<<1), VX-form AltiVec ops put an 11-bit XO at bits 21-31
#    with NO shift. An earlier batch shifted it (xo<<1) → every op decoded to an
#    illegal/no-op, left vD untouched, and "passed" the harness vacuously because
#    the result GPR stayed 0 in both interpreter and JIT (0==0).
#
# 2. RESULT MUST REACH A GPR, AND OPERANDS MUST EXERCISE THE OP. REGDUMP captures
#    GPRs only. So: op into v2 -> stvx v2 to memory -> lwz a result word into r5.
#    AND for splats, the source vector needs DISTINCT per-lane bytes (load via lvx
#    from a 00,01,..,0F memory pattern) — splat-immediate (vspltisb) makes all
#    lanes identical, so the splat INDEX would not be tested.
#
# Verify every generated vector with `make test-jit` (it diffs JIT vs the
# interpreter). A failure is either a construction bug OR a real JIT divergence.

def lis(r,i):  return 0x3C000000|(r<<21)|(i&0xFFFF)
def ori(r,i):  return 0x60000000|(r<<21)|(r<<16)|(i&0xFFFF)        # ori r,r,imm
def li(r,i):   return 0x38000000|(r<<21)|(i&0xFFFF)
def stw(r,d,a):return 0x90000000|(r<<21)|(a<<16)|(d&0xFFFF)
def lwz(r,d,a):return 0x80000000|(r<<21)|(a<<16)|(d&0xFFFF)
def lvx(v,a,b):return 0x7C000000|(v<<21)|(a<<16)|(b<<11)|(103<<1)  # X-form: XO<<1
def stvx(v,a,b):return 0x7C000000|(v<<21)|(a<<16)|(b<<11)|(231<<1)
def vx(vD,fA,fB,xo): return (4<<26)|(vD<<21)|(fA<<16)|(fB<<11)|xo  # VX-form: XO UNSHIFTED
def va(vD,vA,vB,vC,xo): return (4<<26)|(vD<<21)|(vA<<16)|(vB<<11)|(vC<<6)|xo  # VA-form
def H(w): return "%08X"%(w&0xFFFFFFFF)

OFF=0x600  # 16-byte aligned scratch slot under the (seeded) r1
def load_distinct(vT):  # vT.bytes = 00,01,02,...,0F (so splat index is testable)
    return [lis(3,0x0001),ori(3,0x0203),stw(3,OFF,1),
            lis(3,0x0405),ori(3,0x0607),stw(3,OFF+4,1),
            lis(3,0x0809),ori(3,0x0A0B),stw(3,OFF+8,1),
            lis(3,0x0C0D),ori(3,0x0E0F),stw(3,OFF+12,1),
            li(3,OFF), lvx(vT,1,3)]
def vspltisb(vT,simm): return vx(vT, simm&0x1F, 0, 780)
def grab(): return [li(3,OFF), stvx(2,1,3), lwz(5,OFF,1)]  # set r3=OFF, store v2, result word -> r5
                                                            # (li here makes grab self-contained — do NOT
                                                            # rely on a prior li, or arith kernels stvx to
                                                            # the wrong address and go vacuous)

PASS=[]   # vectors that pass make test-jit (committed to run.sh)
BUG=[]    # vectors that expose a CONFIRMED JIT divergence (NOT committed; kept as repro)
def p(n,w,c): PASS.append((n," ".join(H(x) for x in w),c))
def b(n,w,c): BUG.append((n," ".join(H(x) for x in w),c))

# --- passing: word splat + integer multiplies ---
p("av_vspltw_0", load_distinct(1)+[vx(2,0,1,652)]+grab(), "vspltw v2,v1,0 -> 0x00010203")
p("av_vspltw_2", load_distinct(1)+[vx(2,2,1,652)]+grab(), "vspltw v2,v1,2 -> 0x08090A0B")
p("av_vspltb_0",  load_distinct(1)+[vx(2,0,1,524)] +grab(), "vspltb v2,v1,0  -> 0x00000000 (ev_mixed remap)")
p("av_vspltb_3",  load_distinct(1)+[vx(2,3,1,524)] +grab(), "vspltb v2,v1,3  -> 0x03030303")
p("av_vspltb_15", load_distinct(1)+[vx(2,15,1,524)]+grab(), "vspltb v2,v1,15 -> 0x0F0F0F0F")
p("av_vsplth_0",  load_distinct(1)+[vx(2,0,1,588)] +grab(), "vsplth v2,v1,0  -> 0x00010001")
p("av_vsplth_3",  load_distinct(1)+[vx(2,3,1,588)] +grab(), "vsplth v2,v1,3  -> 0x06070607")
p("av_vsplth_7",  load_distinct(1)+[vx(2,7,1,588)] +grab(), "vsplth v2,v1,7  -> 0x0E0F0E0F")

# --- arithmetic / logical / compare: two splat-immediate operands (v0=0x05.., v1=0x03..).
# These REPLACE 12 pre-existing vec_* vectors that were confirmed VACUOUS (r5=r6=0,
# their setup ops were doubled-XO no-ops). VXO is the unshifted 11-bit XO. ---
def two(xo): return [vspltisb(0,5),vspltisb(1,3),vx(2,0,1,xo)]+grab()
p("av_vadduwm", two(128),  "vadduwm: 0x05050505+0x03030303=0x08080808")
p("av_vsubuwm", two(1152), "vsubuwm: 0x05..-0x03..=0x02020202")
p("av_vand",    two(1028), "vand: 0x05&0x03=0x01010101")
p("av_vor",     two(1156), "vor: 0x05|0x03=0x07070707")
p("av_vxor",    two(1220), "vxor: 0x05^0x03=0x06060606")
p("av_vnor",    two(1284), "vnor: ~(0x05|0x03)=0xF8F8F8F8")
p("av_vmaxsw",  two(386),  "vmaxsw: signed-word max=0x05050505")
p("av_vminsw",  two(898),  "vminsw: signed-word min=0x03030303")
p("av_vcmpequw",[vspltisb(0,5),vx(2,0,0,134)]+grab(), "vcmpequw v2,v0,v0: equal -> 0xFFFFFFFF")

# --- CONFIRMED JIT BUG (2026-06-04): even/odd byte MULTIPLIES select the wrong
# elements (same ev_mixed root cause; vspltb/vsplth were FIXED, multiplies pending).
# Correct encodings (verified), distinct-lane source. The JIT diverges from the
# interpreter (the reference): e.g. vspltb idx 3 -> interp 0x03030303, JIT
# 0x00000000 (byte 0); vsplth idx 3 -> interp 0x06070607, JIT 0x04050405 (hw 2).
# vspltw is correct. Codegen: ppc-jit.cpp:3146-3148 (DUP element). imm5 looks
# right, so suspect an element-order/endianness interaction. Re-add once fixed.
b("av_vmuloub",  [vspltisb(0,5),vspltisb(1,3),vx(2,0,1,8)]+grab(), "vmuloub: odd-byte select diverges (same element-order class)")
b("av_vmuleub",  load_distinct(1)+[vx(2,1,1,520)]+grab(), "vmuleub: even-byte select diverges with DISTINCT operands (interp 0x00000004, JIT 0x00040009) — the uniform-operand version was a MASKING pass")

if __name__ == "__main__":
    print("# ==== AltiVec coverage (generated by gen-altivec-vectors.py) =================")
    for n,h,c in PASS:
        print("# %s"%c); print('T_%s="%s"'%(n,h)); print("TEST_ORDER+=(%s)"%n)
    print("# --- repro for the confirmed vspltb/vsplth JIT bug (NOT in TEST_ORDER) ---")
    for n,h,c in BUG:
        print("# BUG %s: %s"%(n,c)); print('# T_%s="%s"'%(n,h))
