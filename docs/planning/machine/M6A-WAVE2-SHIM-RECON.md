# M6a Wave 2 — Shim Recon: the 0x50466ee0 / ea=0xffffaad0 crash, root-caused statically

> **Status:** static-RE memo (read-only recon, 2026-06-11). No code changed, nothing committed.
> Inputs: `/tmp/m6a-crash-context.txt` (Wave-1 Boot A crash dump), `/tmp/rom901_decompressed.bin`
> (stale in one region — see §6 residue), `rom_patches.cpp`, `PATCH-68K-SHIM-INVENTORY.md`,
> `HANDOFF-NEWWORLD-SUPERVISOR-MMU.md` §2.8, `NEW-WORLD-ROM-SUPPORT-PLAN.md` (Case B),
> `machine/M6A-DR-HANDOFF-ANALYSIS.md`.

## TL;DR

The Wave-1 wall is **not** an instruction at 0x5000aa3c and **not** a JIT bug. It is the
**machine-detect module dispatch at ROM+0xAD7C** jumping into **data**: the existing
paravirtual `universal_info` patch (applied on the live boot via lenient whole-image
fallback) redirects the UniversalInfo record's DecoderInfo offset at **0xE184** to the
synthetic AddrMap at **`ADDR_MAP_PATCH_SPACE = 0x2FD140`** — but the dispatcher treats
DecoderInfo as a *code* target (`jmp (a0,a2.l)` with CheckFor-offset `[a0-0xc] = 0`, zeroed
by the AddrMap constructor's memset). The first AddrMap word **0xFFC0 executes as an F-line
trap** → vector **[VBR+0x2C] = [0x2C] = 0** (unseeded) → PC=0 → instruction slide through
the reset vector → garbage `ori.b #0,-$552F(a2)` with a2=0 → operand read at
**ea=0xffffaad0** → host SIGSEGV. Every register in the crash dump confirms the chain
(five independent clues, §2.3).

**First shim (Wave-2 target #1):** give the machine-detect a real answer — preferred:
**seed the NK hardware-info record (`'Hnfo'`) behind `[KDP+0xfd0]` (= 0x68ffefd0)** so the
ROM's data-driven path runs and the probe dispatcher is never reached; tactical fallback:
a 10-byte CheckFor stub in ROM patch space + `[0x2FD134] = stub offset`.
**Second shim (do it regardless, hours):** seed the 68k exception vectors (0x10/0x28/0x2C…)
so future shim-desert hits stop cleanly instead of cascading through address 0.

---

## 1. Q1 — What raised the exception (and which vector)

### 1.1 It is NOT at 0x5000aa3c

`r22 = A6 = 0x5000aa3c` is the boot code's **continuation pointer** (this ROM's early 68k
code uses `lea ret(pc),a6 / bra.l target / ... / jmp (a6)` calling convention — no usable
stack yet). The DR-emulator 68k register map in the dump: `r8–r15 = D0–D7`,
`r16–r22 = A0–A6`, `r1 = A7/SP`, `r24 = 68k PC`, `r25 = SR high byte` (0x27 ✓ from
`move.w #$2700,sr`).

### 1.2 The actual boot flow to the fault (disassembly, /tmp dump + live values)

Reset: `guest[4] = 0x5000002a` → `0x5000002a: jmp $500000b6(pc)`:

```
0x500000b6  move.w  #$2700, sr
0x500000bc  lea.l   $500000c6(pc), a6        ; A4 saved = 0x500000c6 ✓ (= r20 in dump)
0x500000c0  bra.l   $5000aa10                ; vector-catcher install
0x500000c6  movea.w #$2600, a7               ; (re-seeded; also done inside aa10)
0x500000ce  move.w  #$11, d7
0x500000d6  bra.l   $500a8d60                ; RunDiags — patched by run_diags shim (@0xd6)
0x500000e2  lea.l   $500000ec(pc), a6
0x500000e6  bra.l   $5000ab4e                ; ← machine detect. Crash is inside this call.
```

`0x5000aa10` (vector-catcher install — self-contained, works):

```
0x5000aa10  movea.w #$2600, a7
0x5000aa14  movea.l a6, a4
0x5000aa16  lea.l   $5000aad0(pc), a0
0x5000aa1a  move.l  a0, d0                   ; d0 = 0x5000aad0
0x5000aa1c  cmp.l   -(a0), d0                ; scan known-ROMbase table at aac0..aacc
0x5000aa1e  beq.b   $5000aa2a                ;   (4000aad0/4080aad0/0000aad0/ffc0aad0 —
0x5000aa20  tst.l   (a0)                     ;    0x5000xxxx NOT in table → write path)
0x5000aa22  bne.b   $5000aa1c
0x5000aa24  lea.l   $8.w, a0
0x5000aa28  move.l  d0, (a0)                 ; ★ guest[8] = 0x5000AAD0 (bus-error catcher)
0x5000aa2a  subq.w  #$8, a0
0x5000aa2c  movec   a0, vbr                  ; VBR = 0 — vectors live in zeroed low RAM
0x5000aa32  lea.l   $5000aa3c(pc), a6        ; ← the r22 value. Continuation, not the raiser.
0x5000aa36  bra.l   $5000ab4e
```

The catcher at `0x5000aad0` (`btst.l #27,d7; movea.l a5,sp; jmp (a6)`) is the classic
probe-with-bus-error-recovery handler — **only vector 2 (bus error, guest[8]) is seeded by
the ROM itself. All other vectors are 0.**

`0x5000ab4e` (machine detect):

```
0x5000ab4e  bset.b  #$1b, d7                 ; "probing" flag (r15=D7=0x08000000 ✓)
0x5000ab52  movea.l a7, a5                   ;  a5 = recovery SP for the aad0 catcher
0x5000ab5a  bra.l   $5000afb4                ; → NK hardware-info check:
   0x5000afb4  move.l ([$68ffefd0],$70), d0  ;   [KDP+0xfd0] -> +0x70
   0x5000afbe  cmpi.l #$486e666f, d0         ;   'Hnfo' signature
   0x5000afc6  move.w ([$68ffefd0],$76), d0  ;   if found: d0 = machine id, EQ
0x5000ab60  movea.l a1, a6                   ; a6 = 0x5000aa3c again
0x5000ab62  beq.b   $5000ab68                ; found → table-match path (ab86: list@0xE15C)
0x5000ab64  bra.w   $5000abe8                ; NOT found (our boot: KDP+0xfd0 = 0)
0x5000abe8  tst.b   d2                       ; d2 = 0
0x5000abea  beq.w   $5000ad7c                ; → raw machine-probe dispatcher
```

`0x5000ad7c` — **the module/probe dispatcher (the gate)**:

```
0x5000ad7c  move.l  a6, d0                   ; d0 = 0x5000AA3C  ★ (see clue 2 below)
0x5000ad7e  lea.l   $3406.l, a1
0x5000ad84  lea.l   $5000ad7e(pc,a1.l), a1   ; a1 = 0x5000E184 = UniversalInfo record
0x5000ad88  movea.l a1, a0
0x5000ad8a  adda.l  (a1)+, a0                ; a0 = 0xE184 + [0xE184]; a1 → 0xE188 (= r17 ✓)
0x5000ad8c  movea.l -$c(a0), a2              ; a2 = CheckFor routine offset
0x5000ad90  jmp     (a0, a2.l)               ; ★★ call the machine CheckFor probe
```

On the **live** boot, `[0xE184] = 0x2EFFBC` — written by the existing `universal_info`
patch (`rom_patches.cpp:1956`: `lp[0x00>>2] = ADDR_MAP_PATCH_SPACE - (base-0x14)`, with the
pattern `3f ff 04 00` found at 0xE198 → base−0x14 = 0xE184). So:

- `a0 = 0x502FD140` (= `ADDR_MAP_PATCH_SPACE`) — **confirmed by the crash dump: r16 = A0 = 0x502fd140**
- `a2 = [0x502FD134] = lp[-3] = 0` — zeroed by the AddrMap constructor's `memset(lp-10, 0, 0x128)` — **confirmed: r18 = A2 = 0**
- `jmp 0x502FD140` lands on the synthetic AddrMap **data**: `lp[0] = 0xFFC00000`

### 1.3 Answer: **F-line trap (opcode 0xFFC0) at 0x502FD140, vector 11, offset [0x2C]**

`0xFFC0` executes as an F-line trap → 4-word format-0 frame pushed (8 bytes:
**sp 0x2600 → 0x25f8 ✓ = r1**) → new PC = `[VBR + 0x2C] = [0x2C] = 0` (unseeded) → slide:

| 68k PC | guest bytes | decodes as | effect |
|---|---|---|---|
| 0 | `0000 0000` | `ori.b #0,d0` | d0 unchanged |
| 4 | `5000` | `addq.b #8,d0` | **d0: 0x5000aa3c → 0x5000aa44 ✓ (= r8)** |
| 6 | `002a 5000 aad0` | `ori.b #$00,-$552F(a2)` | ea = a2 + sext(0xAAD0) = **0xffffaad0** |

The DR emulator's operand read for that `ori.b` is the host SIGSEGV
(`ea 0x4000ffffaad0`). Five independent confirmations from the dump:

1. **ea = 0xffffaad0** = 0 (a2/r18) + sign-extended d16 `0xAAD0` (= low half of guest[8]
   = `0x5000AAD0` written at aa28) — exactly an `lhau` d16 fetch at guest[0xA].
2. **r8 (D0) = 0x5000aa44** = `0x5000aa3c` (set at ad7c) + the slide's `addq.b #8` at PC=4.
3. **r29 = 0x50480150** = mirror dispatch table + `0x002A << 3` — last dispatched opcode
   was the garbage `ori.b` (0x002A) at PC=6.
4. **r24 = 8** — 68k PC mid-decode of that instruction (after imm-word fetch).
5. **r1 = 0x25f8** = 0x2600 − 8 = exactly one format-0 exception frame.

The crash block `0x50466ee0` (`lhau r7,2(r24); lhau r5,2(r24); lbzux r4,r3,r7 …`) is the
mirror DR emulator's extension-word decode path doing that fetch — faithful emulation of
garbage, not a codegen bug. This also matches and *refines* the earlier Path-A "Case B"
finding (`NEW-WORLD-ROM-SUPPORT-PLAN.md` ~line 700): same site, now with the F-line raise
and slide pinned to specific register values.

## 2. Q2 — What paravirtual seeds that fidelity lacks (minimal set for THIS crash)

What protects the **paravirtual + ROM 1.1** boot at this exact site:

1. **`powermac_id_dat` patch** (`rom_patches.cpp:1932`): on ROM 1.1 the CheckFor probe
   routine is real code (`lea $5FFFFFFC,a2; move.l (a2),d0` — reads the hardware machine-ID
   register); paravirtual replaces its prologue with
   `move.l #$3020,d0; cmp.w d0,d0; jmp (a6)` — fake id, EQ, return. **Absent in the 9.0.1
   parcels ROM** (inventory row: pattern eliminated — parcels has no probe routine at all;
   it *relies on the NK 'Hnfo' record existing*).
2. **`universal_info` patch** + **AddrMap constructor**: synthesizes a PowerMac-9500-flavored
   UniversalInfo and an AddrMap whose device slots are already **our M1 MMIO bus
   addresses** (`lp[2]=0xf3016000` VIA ✓ via6522 region, `lp[3]=0xf3012000` SCC ✓).
   On 9.0.1 it half-applies: the record redirect lands (0xE184 → 0x2FD140) but the
   *CheckFor slot it implies* (`[0x2FD140-0xc]`) is left 0 → **this patch is what converts
   "no probe routine" into "jump into data."**
3. **`Mac_memset(0, 0, 0x3000)` + XLM seeding** (`main.cpp:229ff`): paravirtual zeroes low
   memory but **never seeds 68k exception vectors either** — paravirtual survives only
   because shims (1)+(2) prevent any exception from being raised here.
4. `[KDP+0xfd0]` (0x68ffefd0): **nobody writes it** — not `main.cpp` (NewWorld branch writes
   0xb80–0xf6c only), not the patched-out NK cold-init (the "Skip SR/BAT/SDR init" patch
   skips the architectural KDP writers — `HANDOFF` §2.2 caveat). On real hardware the
   **nanokernel** builds this hardware-info record from Trampoline data. Note the sibling
   PPC-side check `[[KDP-0x20]+0xf70] == 'Hnfo'` (`M6A-DR-HANDOFF-ANALYSIS.md` §2.1) —
   plausibly the same record (+0x70 vs +0xf70 ⇒ `[KDP+0xfd0] ≈ [KDP-0x20]+0xf00`); seeding
   one structure may satisfy both. (Unverified — residue.)

**Minimal seed/shim set for THIS crash — two options:**

- **Option A (fidelity-preferred): seed the 'Hnfo' record.** Build a small hardware-info
  block (host-side, trampoline/glue or `PatchROM` tail) and point `[0x68ffefd0]` at it:
  `+0x70 = 'Hnfo'` (0x486e666f), `+0x76 = machine id (word)`, `+0x8 = pointer to a writable
  scratch record` (the copy-out at 0xAC16 writes `[[0x68ffefd0]+8]` fields +0x10..+0x16).
  Then `afb4` succeeds → the table-match path (0xAB86) scans the record list at **0xE15C**
  (`[0xE160]=0x24` → first record 0xE184, id word at `+0x58` = `[0xE1DC]` = **0x3035** ✓
  present in parcels) → matched record drives the data-driven init. **The probe dispatcher
  at ad7c is never reached.** Choose id **0x3035** to match the record the `universal_info`
  patch already populates (or the SS_NW_MODEL variant).
- **Option B (tactical, paravirtual parity): CheckFor stub.** Place the 10-byte 1.1-style
  stub `203C 0000 3035 B040 4ED6` (`move.l #$3035,d0; cmp.w d0,d0; jmp (a6)`) in ROM patch
  space and write `[0x2FD134] = (stub - 0x2FD140)`. Mirrors the Universal CheckFor contract
  (`[DecoderInfo-0xc]` = CheckFor offset). Caveat: the post-probe continuation on 9.0.1
  (jmp (a6) → 0xAA3C `btst #0,d0` thunk vs. the ROM's own probes' `bra 0x5000AD94` tail)
  needs one live verification run — id bit0 selects the aa3c branch (0x3035 is odd → aa42
  relocation-thunk leg; 0x3020-style even id → aa5c second-pass leg, matching the 1.1
  patch's deliberate id choice).

**Plus, in either case: the exception-vector quick-win** (Case B's recommendation, still
unimplemented): seed `guest[0x10]`, `guest[0x28]`, `guest[0x2C]` (+ ideally 0xC, 0x20, 0x24)
with a safe handler so the next shim-desert hit faults *diagnosably* instead of sliding
through address 0. Cheapest correct stub today: point them at the ROM's own catcher
`0x5000AAD0` (restores a5→sp, jmp (a6)) or a patch-space `halt` loop with a probe on it.

## 3. Q3 — Wave-2 shim queue (boot-flow order from reset)

Boot order on the parcels ROM (verified by disassembly): `0x2a → 0xb6 → aa10 (vector
catcher) → 0xd6 RunDiags call → 0xe6 machine detect (ab4e) ★crash → 0xf4 ae82 (record
geometry reads) → 0x10c aad8 (VIA-init leg) → 0x112 bsr 0x81f8 → 0x11a/0x11e/0x126
init bsrs (NVRAM/clock territory) → BootGlobs consumption (movem to -0x5a(a6))`.

| # | Shim | Inventory row (PATCH-68K-SHIM-INVENTORY.md) | 9.0.1 status | Wave-2 action | Cost |
|---|---|---|---|---|---|
| 1 | **Machine-detect gate** (THIS crash) | `univ_info_dat` (relocated @0xe198, half-effective) + `powermac_id_dat` (**absent**) | univ applied via lenient fallback; probe routine doesn't exist in parcels | Option A ('Hnfo' seed at `[KDP+0xfd0]`, id 0x3035) preferred; Option B (CheckFor stub @patch space, `[0x2FD134]`) fallback | A: S–M (1–2d, incl. record-copy contract at 0xAC16); B: S (0.5–1d + 1 probe run) |
| 2 | **68k exception-vector seeding** | not in inventory (Case B quick-win) | n/a | Seed [0x10]/[0x28]/[0x2C] (+0xC/0x20/0x24) → safe catcher; do unconditionally on newworld profile | S (hours) |
| 3 | **run_diags / BootGlobs** | `run_diags_dat (NW)` (relocated @0xde, lenient alt-pattern, patch @0xd6) | applied — but `0xdc movea.l (a7),a6` may clobber the patched `lea RAMTop-0x1c,a6`; BootGlobs then 0 → `movem.l …,-$5a(a6)` at 0xfe wild-writes | Verify live (probe [0x25fc] + the 0xfa..0xfe leg); fix patch placement if clobbered | S (0.5–1d) |
| 4 | **VIA-init legs** | `via_init_dat` (@0xaad8), `via_init2_dat` (@0x9584), `via_init3_dat` (@0x9630) | found/applied (skip VIA) | M1-spirit decision: **retire on newworld profile** — record VIA base 0xf3016000, register offsets used (0x000/0x400/0x600/0x1e00) all inside the via6522 region (+0x2000) ✓; keep skip-patch on paravirtual | S (0.5d + M1 conformance run) |
| 5 | **NVRAM/XPRAM cluster** | `nvram1_dat` (in_range) … `nvram2–7_dat` (**absent**) | 1 of 7 portable | Tactical: minimal XPRAM-read stub (EMUL_OP) so clock/PRAM-dependent init proceeds; strategic: M3b Cuda/ADB owns RTC+PRAM on fidelity | M (5–10d full; S for read-stub) |

Items 1+2 unblock the current wall; 3 is the *next* predicted wall on this exact flow
(BootGlobs is consumed ~30 instructions after the machine detect returns); 4 is the first
fidelity dividend (real device init against the M1 bus); 5 is the first EMUL_OP-class HLE
need.

## 4. Q4 — Probes for the orchestrator

**Probe A — confirm the whole chain in one run** (crash block fires once at block entry;
use NO_CHAIN so the entry isn't skipped via a chained edge):

```bash
SS_JIT_NO_CHAIN=1 SS_NW_TRAMPOLINE=1 SS_ROM_LENIENT=1 SS_ROM_SKIP_JUMP68K=1 \
SS_PROBE_PC='0x50466ee0:r1,r8,r16,r18,r22,r24,[r1:0x10],[0x8],[0x68ffefd0],[0x5000e184],[0x502fd134],[0x502fd140],[0x2c],[0x10]' \
./SheepShaver --config /tmp/trace901.prefs
```

Expected (chain confirmed if all hold):

| Field | Expect | Meaning |
|---|---|---|
| `[r1:0x10]` | `25f8: SR(2)=27xx, PC(4)=0x502FD140, fmt/vec(2)=0x002C` | **F-line frame** — the smoking gun (vector offset 0x2C) |
| `[0x8]` | `0x5000AAD0` | ROM seeded only the bus-error vector |
| `[0x2c]`, `[0x10]` | `0x00000000` | F-line + illegal vectors unseeded |
| `[0x68ffefd0]` | `0x00000000` | no NK 'Hnfo' record → fallback probe path taken |
| `[0x5000e184]` | `0x002EFFBC` | universal_info patch applied (→ 0x502FD140) |
| `[0x502fd134]` | `0x00000000` | CheckFor offset zeroed by AddrMap memset |
| `[0x502fd140]` | `0xFFC00000` | the F-line word executed as code |
| `r16/r18/r22/r24/r8/r1` | `502fd140 / 0 / 5000aa3c / 8 / 5000aa44 / 25f8` | as in the crash dump |

If `fmt/vec` reads `0x0010` instead of `0x002C`, the raiser was an illegal decode of the
same data word — same fix, update the memo.

**Probe B — identify the DR exception-delivery code** (which mirror block pushes the
frame), useful for the vector-seeding shim and any future exception-path work:

```bash
SS_JIT_WATCH_ADDR=9722 SS_JIT_WATCH_DUMPS=0 ...   # 9722 = 0x25FA = pushed-PC word (decimal!)
# grep '\[WATCH\]' — the writing block PC = the DR emulator's exception-dispatch path
```

**Probe C — post-fix acceptance (after shim #1):**
`SS_PROBE_PC='0x50466ee0:r24;0x50466ec0:r24'` → expect **0 visits** at both; plus
`[0x68ffefd0]` non-zero (Option A) and boot advancing to the run_diags/BootGlobs leg
(predict next fault, if any, in the `0xfa–0xfe` movem with a6 from `[0x25fc]` — probe
`[0x25fc]` to pre-check shim #3).

## 5. Costs / ordering recommendation

Wave-2 first PR: **#2 (vector seeding, hours) + #1 Option B (CheckFor stub, ~1d)** gets the
boot moving and is fully reversible; **#1 Option A ('Hnfo')** is the architecture-true
investment (also likely feeds the PPC-side IRP signature check at 0x503109f4) and should
supersede B once the record layout is confirmed live — B's stub remains useful as the
paravirtual-parity fallback. Then #3 (already-predicted next wall), #4 (first real-device
dividend), #5 (first EMUL_OP port).

## 6. Honest residue

1. **The static dump `/tmp/rom901_decompressed.bin` is stale for patched regions**: it shows
   `[0xE184]=0`, `0x2FD140='kckc…'` filler — i.e. dumped from a run *without* the
   universal_info/AddrMap patches. The live values (0x2EFFBC / AddrMap data) are inferred
   from `rom_patches.cpp` + the Case-B live measurement + crash registers (r16=0x502fd140).
   **Re-dump with the current binary/profile (`SS_DUMP_ROM`) before Wave-2 byte-surgery.**
2. **Vector identity (F-line vs illegal)** is statically certain only if the live `lp[0]` at
   0x2FD140 is `0xFFC00000` as the constructor writes; Probe A's frame word settles it.
3. **Option B's continuation contract** (where `jmp (a6)` lands post-probe, and the id-bit0
   branch at 0xAA3C) — one probe run needed; the 1.1 patch chose an even id (0x3020),
   suggesting bit0=0 is the safe leg.
4. **'Hnfo' record full layout** beyond +0x8/+0x70/+0x76 (the copy-out at 0xAC16 writes
   +0x10..+0x16 of `[[0x68ffefd0]+8]`; later consumers unknown). RE before Option A ships.
5. **`[KDP+0xfd0]` vs `[KDP-0x20]+0xf70` same-record hypothesis** — unverified.
6. The `[VCLK]/[EXC]` lines in the crash dump (`deferred_ee=1`, `pending=1`) are *not*
   implicated in this crash (it's a synchronous fault), but note the 68k world runs with a
   deferred EE pending — relevant once shims let the boot reach interrupt-driven stages.
7. Capstone M68K mis-aligns on inline `bra.l`-thunk data; all excerpts above were
   re-verified by hand at the byte level where it mattered (aa10 entry, ab68, afb4, ad7c).

---

## Wave 2 night-run results (2026-06-11, commits 4c978a50..HEAD)

Executed queue: io_poll retirement (#5-adjacent, 4c978a50) -> 'Hnfo' + vector stubs (#1+#2,
2f71ac2a) -> run_diags a6-clobber NOP (#3) + the MacIO absent-device fence policy (PENDING
RATIFICATION) -> backpatch-thunk non-MMIO fallback (M1 debt) -> via_init/time_via retirement
(#4) + the VIA read histogram. Plus M6a rung 1 (r1-independent UserModeMSR): **the first
EE-enabled DEC delivery on the boot path fired and the NK round-trip RESUMED the guest**
(srr1=0xd032 through the real machinery).

Boot-frontier ladder crossed tonight: console-printer landing -> machine-detect F-line ->
BootGlobs a6 clobber -> IDE probe (absent-device) -> backpatch non-MMIO abort -> the VIA
ORB poll. **Final frontier: the 68k boot polls the Cuda TREQ handshake (histogram:
ORB=18338, SR=1) against the M1 loud stub, gives up, and parks in a non-device spin
(comp=841, jDR climbing, MMIO counters frozen).** The audit's adb_init_dat row and the
donor study's §7.2 predicted precisely this: **Cuda is now the named, live, on-critical-path
consumer — M3b (Cuda protocol + minimal ADB stub + OpenPIC) is the next milestone**, with
the M6a remaining items (the post-Cuda spin diagnosis, ongoing-entry rung 2 polish, shim
queue continuation) interleaved as its acceptance reveals them.

---

## M3b Wave 1 acceptance — symptom record (2026-06-11, dev_cuda live, commits 143f66e7..8c7f6796)

**The 18338-read sync frontier is CROSSED; the next wall is a bounded ~15000-iteration IFR
poll that expires with no transfer ever attempted.**

Acceptance boot (90s, /tmp/m2accept.prefs, log /tmp/m3b_accept.log):

```
[VIA] reads: ORB=3340 SR=2 IFR=15002
[VIA] orb: ddrb=30 writes=4 trace=38,28,30
[CUDA] packets=0 responses=0 syncs=1 bytes_in=0 bytes_out=0 adb=0 ... unknown=0
[HB] comp frozen at 849, mmio frozen at V:18344 (the boot gave up and parked)
```

Symptom anatomy (all ROM references = file offsets in the SS_DUMP_ROM 9.0.1 image,
guest = +0x50000000):

1. **Sync now completes.** The startup sync at 0xd0e0–0xd134 was matched
   instruction-by-instruction to the telemetry: ACR shift-in setup; SR read #1 (IFR.2
   clear); `bclr #4,(a1)` TACK assert (trace 38→28); wait-TREQ-assert at 0xd0ae
   (`btst #3,(a1); dbeq` — THE old 18338-budget loop, now exits after ~3.3k reads);
   `bset #4,(a1)` TACK negate (RMW of the derived ORB → writes 0x30); wait-TREQ-negate
   at 0xd0c8 (`dbne`); SR read #2 = the sync byte; success; ACR := 0x1C. All 4 ORB
   writes + both SR reads accounted. dev_cuda's TREQ-mirrors-TACK sync semantics
   (QEMU cuda_update lines 150–159) satisfied the real ROM choreography on first contact.

2. **The new wall: IFR=15002 polls, then give-up.** ~15000+2 reads = a bounded timeout
   loop (same `dbeq`-budget shape as the old 18338 ≈ 18336+2). CRITICAL DATUM: the
   pre-model boot showed the SAME IFR=15002 — this phase always existed behind the sync
   wall and its budget expires regardless of the model. The boot never enters any send
   engine (no TIP assert in the trace, packets=0): it is waiting for an EVENT, not a
   response — most plausibly an SR-int (IFR.2) or CB1 (IFR.4) the real machine would
   generate after sync that our model does not, OR a non-Cuda IFR bit entirely
   (timer — the Ticks-starvation hypothesis from the M3b plan rev 2 M1 stays live).

3. **Polled-dispatch architecture surfaced** (Wave-2-relevant): trampolines at 0x6d58
   (`btst #2,IFR` → push handler [$19a]) and 0x6e90 (`btst #4,IFR` = CB1 → push [$1a2]),
   plus an SCC poller at 0x6ea0 (VIA base − 0x16000) — the 68k boot world dispatches
   device service handlers by POLLING the VIA IFR, not via real interrupt delivery.
   The send engines: Cuda-polarity at 0x9068 (TIP=bclr#5, per-byte IFR.2 waits at
   0x90d0/0x911e, commit `ori.b #$30` at 0x9112); Egret-polarity at 0x9c28 (TIP
   active-HIGH via bset#5). Full IFR.2 poll-site catalog: 0x90d0 0x911e 0x915c 0x9190
   0x92be 0x964c 0x96f2 0x9730 0x9788 0x97bc 0x97e0 0x980c 0x9832 0x984e 0x9c84
   0x9c9c 0x9d2e (+ the 0x96xx–0x98xx third engine family).

Disposition: root-cause diagnosis dispatched (which loop, which IFR bit, which oracle
behavior is missing); per the plan's stop-rule, if the awaited event is NOT
Cuda-model-side (timer/interrupt delivery), it is Wave-2/M6 territory — no tunneling.
_Root-cause results to be appended below by the diagnostician._

### Root cause (2026-06-11, diagnostician): deferred SR-int delivery — FIXED (commit d3e60d88)

**One paragraph:** the 68k boot was not waiting for a timer, CB1, or any unmodeled event —
it was waiting for the Cuda's **post-sync SR interrupt, which our seam had already delivered
and the ROM had already (unknowingly) cleared**. The executed startup sync is NOT the
0xd0e0 routine from the symptom record but the routine at **file 0x9584** (guest
0x50009584; symptom-record item 1's instruction-matching of 0xd0e0 was a coincidence of
similar choreography — 0xd0e0 does RMW `ori.b` on the ACR, and the read histogram shows
**ACR reads = 0**, which excludes it). 0x9584's sequence reconciles every counter EXACTLY:
`ori.b #$30,ORB` (write #1 = 0x38, 1 RMW read) → 3334-read `tst.b ORB` settle delay
(`dbra #$d05`) → TREQ check (1 read, negated) → SR read #1 → `bclr #4` TACK assert
(write #2 = 0x28, 1 RMW read; model mirrors TREQ + raises SR int) → TREQ-assert poll
(1 read) → IFR.2 poll #1 (1 read, satisfied) → `bset #4` TACK negate (write #3 = 0x30,
1 RMW read; model raises SR int again) → TREQ-negate poll (1 read) → **SR read #2 (the
sync byte — this read CLEARS IFR.2, M4)** → **IFR.2 wait with `move.l #$3a98,d4` =
15000-budget `dbra` at 0x960e → 15001 reads → EXPIRES** → `bra.l 0x974a` → park at
0x9754 `bra.b *`. Totals: ORB = 1+3334+1+1+1+1+1 = **3340** ✓, IFR = 1+15001 = **15002** ✓,
SR = **2** ✓, ORB writes = 4 ✓ (incl. the wrapper's), syncs = **1** ✓. The parked 68k PC
was confirmed LIVE: `SS_PROBE_PC=0x50467ed4:r24` → r24 = 0x50009756 = PC+2 of the
0x9754 self-branch, constant from visit 10 through 1e9.

**The awaited event, per the oracle:** QEMU cuda.c @ de5d8bfd delays EVERY Cuda-raised
SR int through `cuda_delay_set_sr_int` (`sr_delay_ns` = **20 µs**, timer-fired) — so on
QEMU/hardware the TACK-negate edge's int lands AFTER the host's immediate sync-byte SR
read, and the 15000-budget wait at 0x960e sees it. Our seam delivered the raise
synchronously inside `CudaORBWritten`, so the ROM's SR read at 0x9602 consumed it before
the wait began. The dev_cuda.h m10 note ("QEMU's 20 µs delay exists for an
interrupt-delivery race that cannot occur on the IFR-poll-only newworld profile") is
exactly the falsified assumption — the race occurs on the POLL path too, because the
poll loop starts after an SR read.

**Fix (model-side, oracle-backed, commit d3e60d88):** deterministic lazy equivalent of
QEMU's delay — (a) `CudaORBWritten` never returns RAISE (pending stays latched);
(b) `CudaSettle`, called from the VIA's **R_IFR read path only**, is the single delivery
surface (ORB reads no longer settle; TREQ derivation stays synchronous); (c) SR
read/write clear the LATCHED IFR.2 only, never an undelivered pending raise (QEMU does
not cancel the sr_delay timer on SR access either). TDD: new **CV-10** conformance
vector scripts the exact 0x9584 sequence (fails red against eager delivery).

**Gates:** machine suite 11/11 ALL PASS; `build-ss`; `SS_HARNESS_BATCH=1 make test-jit`
AND plain `make test-jit` both 353/353; paravirtual untouched (unbound loud-stub paths
byte-identical).

**Acceptance boot after the fix** (90 s, same prefs, log /tmp/m3b_accept2.log) — the park
is GONE and the machine is running:

```
[CUDA] packets=13156 responses=13156 syncs=1013 bytes_in=48576 bytes_out=55660
       pram_rd=3036 pram_wr=1012 unknown=9108(last=1:22)
[VIA]  reads: ORB=3636421 SR=146743 ACR=145729 IFR=249966 IER=66325468
[MMIO] scc8530 reads=66344657 writes=2026 idle_sleeps=259143
[HB]   comp 849 -> 4912+ still climbing at 60s; j2i transitions live; no STALL park
```

**New frontier (next wall, not tunneled):** pseudo command **0x22 = Cuda I2C
(DingusPPC `READ_WRITE_I2C`)**, rejected as unknown 9108 times (the boot's PMU/clock
probing retries through it but keeps progressing). Also still zero: `get_time`,
`autopoll`, `adb` — not yet reached or command-set gaps to revisit when the boot's
device-probe phase is mapped. The 66M IER/SCC reads are the polled interrupt
architecture (symptom-record item 3) running hot — Wave-2 idle/interrupt-delivery
territory as planned.

### I2C commands implemented (2026-06-11): 0x22 + 0x25 retired from unknown — commit 4e544aff

The rev-2 m2 counter spoke, twice. **READ_WRITE_I2C (0x22)** implemented per DingusPPC
`i2c_simple_transaction` @ 92bb6d1 (packet `[01 22 addr data...]`, addr = 7-bit dev
addr << 1 | RW); no I2C devices are modeled (stop-rule), so every transaction takes the
oracle's start_transaction-failed path: `error_response(CUDA_ERR_I2C = 5)` →
`[02 05 01 22]`. QEMU @ de5d8bfd does NOT implement 0x22/0x25 (frames error 2 "unknown
command" — what the boot had been retrying against). The first diagnostic boot with 0x22
live immediately surfaced **COMB_FMT_I2C (0x25)**, issued once per probe cycle (686x) —
boot-demanded, so implemented the same way (`i2c_comb_transaction`: dev_addr/dev_addr1
bits-7:1 match check before the bus lookup, then the same absent-device error).

**What the boot probes over I2C** (capture-only probe map, raw addr bytes):
`41,4F,B5,91,80,C1,71,9D,28` — 7-bit addresses 0x20,0x27,0x5A,0x48,0x40,0x60,0x38,0x4E,
0x14; all reads except 0x80/0x28 (writes). A broad device-discovery sweep, not a single
SPD/clock fetch — no probe loops on one address demanding data, so no device model is
warranted yet (stop-rule holds). DingusPPC's Cuda-machine I2C population for reference:
SPD DIMMs 0x50–0x53 (Yosemite) / 0x55–0x57 (Gossamer), Athens clock 0x28, Perch EEPROM
0x53 — none of which match the swept addresses, consistent with "probe and move on".

Conformance vectors: test_dev_cuda 3715 → 3924 checks (red-first on both commands).
Gates: machine suite 11/11 ALL PASS; build-ss; batch + plain `make test-jit` 353/353.
Build gotcha (repo memory validated again): the CudaDevice struct grew mid-struct and
the Unix build's `main_unix.o`/`dev_via6522.o`/`sheepshaver_glue.o` did NOT rebuild on
the header change — the first boot showed garbage `powerdowns=6114308096`; forced .o
removal fixed it. Treat any absurd [CUDA] counter as a stale-object symptom first.

**Acceptance boot after (60 s, same prefs, /tmp/i2c_diag2.log):**

```
[CUDA] packets=9529 responses=9529 syncs=734 bytes_in=35184 bytes_out=40315
       pram_rd=2199 pram_wr=733 i2c=6597 i2c_absent=6597(addrs=41,4F,B5,91,80,C1,28,71)
       unknown=0
[HB]   comp 849 -> 4786+ climbing at 50s; no STALL park; j2i live
```

**Frontier, honestly:** `unknown=0` — the Cuda pseudo-command set is now sufficient for
everything this boot phase sends. But `adb`/`get_time`/`autopoll` remain zero: the boot
is cycling a Cuda probe sequence (sync + PRAM rd/wr + ~9 I2C probes per ~80 ms cycle,
734 cycles in 60 s) without handing off to ADB/RTC. The next wall is whatever ends that
probe loop — likely NOT Cuda-command-side (the 48M IER/SCC polls point at Wave-2
interrupt-delivery/timer territory, per the plan's stop-rule).
