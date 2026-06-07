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
def load_pattern(vT, base):  # vT.bytes = base, base+1, ..., base+15 (distinct lanes)
    out=[]
    for k in range(0,16,4):
        out += [lis(3, ((base+k)<<8)|(base+k+1)),
                ori(3, ((base+k+2)<<8)|(base+k+3)),
                stw(3, OFF+k, 1)]
    return out + [li(3,OFF), lvx(vT,1,3)]
def load_distinct(vT):  # vT.bytes = 00,01,02,...,0F (so splat index is testable)
    return load_pattern(vT, 0x00)
def load_bytes(vT, bs):  # vT.bytes = bs[0..15] (arbitrary; for non-linear operands)
    out=[]
    for k in range(0,16,4):
        out += [lis(3,(bs[k]<<8)|bs[k+1]), ori(3,(bs[k+2]<<8)|bs[k+3]), stw(3,OFF+k,1)]
    return out + [li(3,OFF), lvx(vT,1,3)]
# Boundary-crossing operand pair for add/sub/avg saturation tests: per-lane values straddle
# signed (0x7F/0x80) and unsigned (0x00/0xFF) limits with VARYING differences, so subtraction
# is not a constant (load_pattern's linear slope makes a-b uniform -> vacuous).
_SAT_A=[0x7F,0x80,0x01,0xFF,0x40,0xC0,0x10,0x90, 0x7E,0x81,0x02,0xFE,0x41,0xC1,0x11,0x91]
_SAT_B=[0x01,0x80,0xFF,0x02,0xC0,0x40,0x90,0x10, 0x02,0x7F,0xFE,0x03,0xC1,0x41,0x91,0x11]
def satop(xo): return load_bytes(1,_SAT_A)+load_bytes(3,_SAT_B)+[vx(2,1,3,xo)]+grab()
def vspltisb(vT,simm): return vx(vT, simm&0x1F, 0, 780)
def grab(): return [li(3,OFF), stvx(2,1,3), lwz(5,OFF,1)]  # set r3=OFF, store v2, result word -> r5
                                                            # (li here makes grab self-contained — do NOT
                                                            # rely on a prior li, or arith kernels stvx to
                                                            # the wrong address and go vacuous)
# Two-distinct-operand vector op: vA=v1=00..0F, vB=v3=10..1F, vD=v2 (grab stores v2).
# Distinct lanes so a wrong element-select / A<->B swap can't pass coincidentally.
def merge2(xo): return load_pattern(1,0x00)+load_pattern(3,0x10)+[vx(2,1,3,xo)]+grab()
# Variable shift/rotate: vA=v1=data, vB=v3=per-element shift amount, vD=v2.
# Strong operands per the 2026-06-06 hunt: high-bit data (distinguishes logical from
# arithmetic right shift) + per-lane amounts 0..15 (>=8 exercises AltiVec's mod-width
# masking, which a raw NEON shift lacks). Byte ops are byte-order-safe (each lane
# independent); halfword/word variants need ev_mixed-aware operands — not added yet.
def shift2(xo, dbase, abase): return load_pattern(1,dbase)+load_pattern(3,abase)+[vx(2,1,3,xo)]+grab()

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

# --- even/odd unsigned BYTE multiplies (vmuloub/vmuleub) FIXED 2026-06-04: the old
# codegen emitted a non-widening MUL.8B and ignored ev_mixed even/odd selection.
# Fix (ppc-jit.cpp emit_vmul_byte): REV32.16B normalize -> UZP1/UZP2.16B select even/
# odd byte -> [U]MULL.8H widen -> REV32.8H output. DISTINCT operands (vA=00..0F,
# vB=10..1F) test BOTH the selection AND the widening: even-lane products like
# 0x0A*0x1A=260 (>255) would truncate under the old MUL.8B, so a non-widening op
# diverges visibly. (Signed vmulosb/vmulesb share the helper but have no signed test
# vector yet; halfword vmul*h still broken — ROADMAP A2.)
p("av_vmuloub", merge2(8),   "vmuloub v2,v1,v3: odd  unsigned byte multiply -> halfword products")
p("av_vmuleub", merge2(520), "vmuleub v2,v1,v3: even unsigned byte multiply -> halfword products")

# --- ev_mixed element-order class: byte/halfword/word MERGES and the PACK.
# DISTINCT operands (vA=v1=00..0F, vB=v3=10..1F) so every output byte uniquely
# identifies its source element AND vA/vB ordering is tested — a self-operand vector
# (v1,v1) can rubber-stamp a wrong ZIP1<->ZIP2 / A<->B swap. The full 128-bit result
# (v2) is compared via the REGDUMP VR dump, so one all-distinct pattern is complete
# positional coverage for a permute.
# Byte/halfword merges FIXED 2026-06-04: ev_mixed-aware codegen (REV32.16B normalize ->
# ZIP1/ZIP2.{16B,8H} -> REV32.16B back; ppc-jit.cpp emit_vmrg). Verified xpass with
# distinct operands; promoted from quarantine to the scored gate.
p("av_vmrghb", merge2(12),  "vmrghb v2,v1,v3: high-byte merge (ev_mixed-normalized)")
p("av_vmrglb", merge2(268), "vmrglb v2,v1,v3: low-byte merge (ev_mixed-normalized)")
p("av_vmrghh", merge2(76),  "vmrghh v2,v1,v3: high-halfword merge (ev_mixed-normalized)")
p("av_vmrglh", merge2(332), "vmrglh v2,v1,v3: low-halfword merge (ev_mixed-normalized)")
# vmrghw/vmrglw FIXED 2026-06-04 (correct ZIP1.4S/ZIP2.4S; word_element is identity so
# no rev) — now on DISTINCT operands too, proving the word fix is not coincidental.
p("av_vmrghw", merge2(140), "vmrghw v2,v1,v3 -> high-word merge [A0,B0,A1,B1]")
p("av_vmrglw", merge2(396), "vmrglw v2,v1,v3 -> low-word merge [A2,B2,A3,B3]")
p("av_vpkuhum",merge2(14),  "vpkuhum v2,v1,v3: halfword->byte pack (ev_mixed-normalized UZP2.16B)")
# --- saturating HALFWORD->byte packs FIXED 2026-06-07 (ppc-jit.cpp emit_vpk_h2b): REV32+REV16
# normalize -> [SU]QXTN/QXTN2 (vA low, vB high) -> REV32 back. Operands CROSS the saturation
# boundaries (negatives AND >255) so the three signednesses are DISTINCT (a non-saturating
# operand set makes SQXTUN and UQXTN identical -> false PASS, the trap the earlier attempt hit).
# big-endian halfwords: vA={0001,0100,7FFF,8000,FF00,00FF,0080,017F}, vB={1234,FFFF,0010,8001,7F00,007F,ABCD,0005}.
_PK_A=[0x00,0x01, 0x01,0x00, 0x7F,0xFF, 0x80,0x00, 0xFF,0x00, 0x00,0xFF, 0x00,0x80, 0x01,0x7F]
_PK_B=[0x12,0x34, 0xFF,0xFF, 0x00,0x10, 0x80,0x01, 0x7F,0x00, 0x00,0x7F, 0xAB,0xCD, 0x00,0x05]
def packop(xo): return load_bytes(1,_PK_A)+load_bytes(3,_PK_B)+[vx(2,1,3,xo)]+grab()
p("av_vpkshss", packop(398), "vpkshss: halfword->byte signed source, signed-saturate (SQXTN)")
p("av_vpkshus", packop(270), "vpkshus: halfword->byte signed source, unsigned-saturate (SQXTUN)")
p("av_vpkuhus", packop(142), "vpkuhus: halfword->byte unsigned source, unsigned-saturate (UQXTN)")
# --- variable byte shifts: FIXED 2026-06-06 (ppc-jit.cpp case 260/516/772). Were
# emitting unmasked, signed, rounding NEON shifts; vsrb shifted the wrong direction.
# data=0x80..0x8F (high bit -> logical vs arith fill), amounts=0x00..0x0F (>=8 -> mask mod 8). ---
p("av_vslb",  shift2(260, 0x80, 0x00), "vslb v2,v1,v3: byte shift-left, amount masked mod 8")
p("av_vsrb",  shift2(516, 0x80, 0x00), "vsrb v2,v1,v3: byte LOGICAL shift-right (zero-fill), mask mod 8")
p("av_vsrab", shift2(772, 0x80, 0x00), "vsrab v2,v1,v3: byte ARITHMETIC shift-right (sign-fill), mask mod 8")
# Halfword/word shifts: FIXED 2026-06-06 (same fix, .8H/.4S element sizes; validated against
# byte-ASYMMETRIC lvx operands, so the ev_mixed byte order is covered). abase chosen so each
# element's amount EXCEEDS its width (halfword: 0x10 base -> low bytes >=16; word: 0x20 -> >=32),
# exercising the mod-width mask. Strong + non-vacuous (distinct high-bit lanes).
p("av_vslh",  shift2(324, 0x80, 0x10), "vslh: halfword shift-left, amount masked mod 16")
p("av_vsrh",  shift2(580, 0x80, 0x10), "vsrh: halfword LOGICAL shift-right (zero-fill), mask mod 16")
p("av_vsrah", shift2(836, 0x80, 0x10), "vsrah: halfword ARITHMETIC shift-right (sign-fill), mask mod 16")
p("av_vslw",  shift2(388, 0x80, 0x20), "vslw: word shift-left, amount masked mod 32")
p("av_vsrw",  shift2(644, 0x80, 0x20), "vsrw: word LOGICAL shift-right (zero-fill), mask mod 32")
p("av_vsraw", shift2(900, 0x80, 0x20), "vsraw: word ARITHMETIC shift-right (sign-fill), mask mod 32")
# Rotates: FIXED 2026-06-06 — NEON has no vector rotate, synthesized as
# (x<<k)|(x>>>(w-k)), k=amt&(w-1). data=0x80.. so wrapped bits are observable (a plain
# shift would drop them); amounts exercise the mod-width mask. Validated byte-asymmetric.
p("av_vrlb",  shift2(4,   0x80, 0x00), "vrlb: rotate-left byte (mod 8), wrap-around")
p("av_vrlh",  shift2(68,  0x80, 0x10), "vrlh: rotate-left halfword (mod 16), wrap-around")
p("av_vrlw",  shift2(132, 0x80, 0x20), "vrlw: rotate-left word (mod 32), wrap-around")
# Saturating add/sub + signed averages: FIXED 2026-06-06. Were emitting the wrong NEON op
# (SABA/UABA abs-diff for adds, SMAXP for signed avg) and/or swapped signedness (sat-sub).
# Now SQADD/UQADD/SQSUB/UQSUB/SRHADD (capstone-verified). Boundary operands exercise the
# saturation clamp + signedness; results are lane-asymmetric (non-vacuous).
p("av_vaddubs", satop(512),  "vaddubs: unsigned saturating add, byte")
p("av_vadduhs", satop(576),  "vadduhs: unsigned saturating add, halfword")
p("av_vadduws", satop(640),  "vadduws: unsigned saturating add, word")
p("av_vaddsbs", satop(768),  "vaddsbs: signed saturating add, byte")
p("av_vaddshs", satop(832),  "vaddshs: signed saturating add, halfword")
p("av_vaddsws", satop(896),  "vaddsws: signed saturating add, word")
p("av_vsububs", satop(1536), "vsububs: unsigned saturating sub, byte")
p("av_vsubuhs", satop(1600), "vsubuhs: unsigned saturating sub, halfword")
p("av_vsubuws", satop(1664), "vsubuws: unsigned saturating sub, word (newly JIT-accelerated)")
p("av_vsubsbs", satop(1792), "vsubsbs: signed saturating sub, byte")
p("av_vsubshs", satop(1856), "vsubshs: signed saturating sub, halfword")
p("av_vsubsws", satop(1920), "vsubsws: signed saturating sub, word")
p("av_vavgsb",  satop(1282), "vavgsb: signed rounding average, byte")
p("av_vavgsh",  satop(1346), "vavgsh: signed rounding average, halfword")
p("av_vavgsw",  satop(1410), "vavgsw: signed rounding average, word")

if __name__ == "__main__":
    print("# ==== AltiVec coverage (generated by gen-altivec-vectors.py) =================")
    for n,h,c in PASS:
        print("# %s"%c); print('T_%s="%s"'%(n,h)); print("TEST_ORDER+=(%s)"%n)
    print("# --- repro for the confirmed vspltb/vsplth JIT bug (NOT in TEST_ORDER) ---")
    for n,h,c in BUG:
        print("# BUG %s: %s"%(n,c)); print('# T_%s="%s"'%(n,h))
