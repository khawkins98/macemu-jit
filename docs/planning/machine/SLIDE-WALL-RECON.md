# The 0x505bb060 garbage-return slide — next-wall recon (2026-06-11)

> Stream-A recon for the post-DSAT frontier (DSAT-WALL-RECON.md Tasks B/C,
> commits 86b1b1f7/2024a835; frontier named in M6A-WAVE2-SHIM-RECON.md
> "Frontier update 2026-06-11"). READ-ONLY task: no source edits.
> **Boots: 1 of the ≤6 budget** (slot protocol): `slide-recon-sc-r0`
> (rundir /tmp/ss-slots/slot0/runs/20260611-233328.48571 — sc-handler r0
> sampling; reproduced the frontier signature exactly: SIGSEGV, crash
> r24=0x500050ef, delivered_sc=169/16-distinct census byte-for-byte).
> All ring analysis via `tools/ring-walk.py` against the preserved artifact
> `/tmp/desync_taskC_frontier_ring.txt` (header: records #3390052..#3652195
> of 3,652,196; ephemeral, re-derivable). Provenance:
> `tools/dump-manifest.sh --check` PASSED (raw 7b1378be…, patched e432df64…).
> Claims label: `slide-recon`.

## TL;DR — the verdicts

- **(Q-SL1) There is NO stack drain and NO garbage pop. The wall is a ROM
  PATCH MISALIGNMENT: upstream's `tm_task` NOP-out (rom_patches.cpp, NW/
  Gossamer arm, "Don't install Time Manager task for 60Hz interrupt") lands
  mid-instruction on the 9.0.1 ROM and rewrites the displacement of a
  top-level boot `bsr.l`, retargeting it to the ODD address 0x500050ed.**
  The "final rts on an empty frame" is a perfectly balanced return inside
  the ROM's top-level boot sequencer, whose resting stack level IS
  A7=0x103ffffe (installed by our own `boot_stack` patch). Invariant-poison
  class: **patch-offset poison** — a 1.1-ROM-calibrated `find_rom_data`+fixed-
  offset write clobbering different 9.0.1 bytes. [RING✓ + RAW-ROM≠PATCH +
  STATIC patch site]
- **(Q-SL2) The +0x100000 slot anomaly is BY-DESIGN DR geometry, not
  corruption**: the dispatch emitter inserts **bit 0 of the new 68k PC into
  r29 bit 0x100000** (`rlwimi r29,rPC,0x14,0xb,0xb` [STATIC mirror
  0x50467e70]) — odd PCs deliberately dispatch into a SECOND handler table
  at 0x50580000 (address-error class). That table lies beyond the staged-copy
  end 0x50500000 → unmapped zeros → the slide. Secondary symptom; with
  Q-SL1 fixed no odd PC arises. The staging gap is a named residue.
- **(Q-SL3) The negative selectors are LEGITIMATE NK fast traps**, handled
  pre-save at the sc entry itself (no dispatch table): −1 = KDP flags-bit
  poll (returns CR0), −2 = ctx-word fetch, −3 = SRR1 privilege-bit fast set
  [STATIC file 0x314ac0..0x314b38, raw==patched range]. ×103/−1 + ×17/−2 is
  a guest polling idiom in the advanced regime — not a mishandled surface,
  not related to Q-SL1. Live r0 sampled at the handler entry: 0x3f, 0x50,
  **0xffffffff** (visits 1/10/100) [PROBE✓ boot 1].

## Q-SL1 — the pinned chain (evidence-tagged)

### 1. A7=0x103ffffe is the boot sequencer's legitimate resting level

- Our `boot_stack` patch (rom_patches.cpp ~3046, CompBootStack) sets
  a0 := RAMBase+0x3ffffe = 0x103ffffe; the raw ROM's `movea.l a0,a7` applies
  it. sp=0x103ffffe recurs across the whole late boot with the top-level
  sequencer executing there and SURVIVING — at least three earlier episodes
  in the same ring: #3591041 (r24=0x50000262), #3594289 (r24=0x50000270),
  #3597437 (r24=0x50000276 → bsr.l into 0x50041e40). [RING✓]
- The ROM header region 0x230..0x28a is a REAL top-level boot sequencer
  [RAW-ROM, m68k-dis]: a straight chain of `bsr.l` boot steps —
  0x268→0x50001690, 0x26e→0x500015c0, 0x274→0x50041e40,
  0x27a→0x5000060a (raw), then `movea.l a0,a7; suba.w #$2000,a7;
  _SetApplLimit ($A02D); bsr.w 0x2560`.

### 2. The "fatal rts" is balanced

[RING✓ #3651985–93]: the routine called from 0x274 (0x50041e40 — it runs
~54k records, the bulk of late boot, #3597438→#3651990) reaches its `rts`
at 0x50041e52 (sp 0x103ffff6→fa→fe — TWO nested rts pops, both balanced)
and returns to **0x5000027a** (observed r24=0x5000027c = word+2 convention)
— exactly the bsr.l@0x274's pushed return address. Nothing drained A7;
nothing popped garbage.

### 3. The patch clobber [RAW-ROM vs PATCH, offset 0x270..0x28f]

```
RAW:      0x27a: 61ff 0000 038e   bsr.l 0x5000060a   (CompBootStack region —
                                   the boot_stack patch's own target zone)
          0x280: 2e48             movea.l a0,a7
          0x282: 90fc 2000        suba.w  #$2000,a7
          0x286: a02d             _SetApplLimit
          0x288: 6100 22d6        bsr.w   0x50002560
PATCHED:  0x27a: 61ff 0000 4e71   bsr.l 0x500050ed   ← ODD target
          0x280..0x288: 4e71 ×5   (nops)
          0x28a: 22d6             ORPHAN word (move.l (a6),(a1)+) — the
                                   beheaded bsr.w's displacement
```

The writer [STATIC rom_patches.cpp, `tm_task_dat` NW/Gossamer arm ~3282–95]:
pattern `30 3c 4e 2b a9 c9` (move.w #$4e2b,d0; _SysError), declared window
0x2a0..0x320, NOPs 6 words at **base+28**. On 9.0.1 the pattern occurs
exactly ONCE in the whole image, at **0x262** — OUTSIDE the window — so the
in-window search misses and the **lenient whole-image fallback**
(`g_rom_904_lenient`, rom_patches.cpp find_rom_data ~128–137; armed because
SS_ROM_LENIENT=1 is part of the slot-boot default env) RELOCATES the match
to 0x262. The misalignment is therefore the compound of (a) lenient
relocation of the anchor + (b) the 1.1-calibrated fixed +28 offset + (c) a
1.1-assumed following layout that 9.0.1 does not have. The six NOPs at
0x27e..0x289 cover: the bsr.l displacement low word (0x038e→0x4e71 ⇒ target
0x27c+0x4e71 = 0x500050ed, odd), the boot-stack `movea.l a0,a7`, the
`suba.w`, `_SetApplLimit`, and the first word of the bsr.w. On the 1.1 ROM,
base+28 presumably lands on the Enable60HzInts install; on 9.0.1 it lands
2 bytes inside an instruction. (Exact 1.1 layout: Task-0 question — needs a
decompressed 1.1 dump or upstream archaeology.)

### 4. The slide mechanics, end-to-end [RING✓ + PATCH + STATIC]

Return to 0x27a → patched `bsr.l 0x500050ed` pushes 0x280 and jumps odd
(#3651990–93: r24 0x5000027c → 0x500050ef) → DR fetch at odd 0x500050ed
reads word **0x760c** (bytes 76 0c at file 0x50ed; next word 0x0200 = the
ring's prefetch r27 ✓) → dispatch r29 = 0x50580000 | (0x760c<<3) =
**0x505bb060** (odd-PC bit, see Q-SL2) → beyond staged-copy end 0x50500000
→ zero slide through every 64KB region → SIGSEGV ea=0x400055590000.
Crash regs (r24=0x500050ef, lr=r29=0x505bb060, r26=0xfffffffe = DR scratch
from the dispatch block's XER fold, NOT a selector echo) all conform.

## Q-SL2 — the odd-PC dispatch geometry [STATIC]

DR dispatch emitter at mirror 0x50467e6c..0x50467e90 (file 0x367e6c,
raw==patched):

```
lha    r27, 0(r3)               ; fetch opcode at new 68k PC (r3)
rlwimi r29, r3, 0x14, 0xb, 0xb  ; r29 bit 0x100000 := PC bit 0 (ODD bit)
rlwimi r29, r27, 3, 0xd, 0x1c   ; r29 bits 0x0007fff8 := opcode<<3
mtlr   r29 / bgelr cr2 …        ; dispatch
```

Even PC → table 0x50480000 (healthy form live in the same window: block
0x504b0ff8 = slot(0x61ff) [RING✓]); odd PC → table **0x50580000** — the
DR's address-error/odd-PC handler table, which on real hardware exists in
the emulator's runtime allocation. Our staged copy ends at 0x50500000, so
the odd table is unmapped zeros. **Not corruption; a staging-coverage gap**
that only matters when guest control flow goes odd (here: caused by Q-SL1's
patch clobber). This refines M6A-DR-HANDOFF-ANALYSIS's
`(r29 & 0xFFF80007) | opcode<<3` model: there is a second rlwimi carrying
the odd bit into bit 0x100000.

## Q-SL3 — negative selectors are NK fast traps [STATIC + PROBE✓]

sc handler 0x50314ac0 (file 0x314ac0, the M3A-resolved primary copy) opens
with a fast-trap ladder BEFORE any context save (each leg ends in rfi with
SPRG1/SPRG2 restore):

- **r0 = −3** (0x314ac0): SRR1 bit test + clear of bit 17 (privilege-class)
  → rfi. Fast "drop/set privilege" service.
- **r0 = −1** (0x314aec): r1:=SPRG0(KDP); reads `[KDP-0x10]` flags word,
  tests bit 0x00200000 (`rlwinm. …,0xa,0xa` — result returned in CR0) → rfi.
  **A poll.** The ×103 census = a guest polling loop. [STATIC]
- **r0 = −2** (0x314b10): same flags test, plus r1:=`[KDP-8]`, returns
  r0:=`[ctx+0xec]` → rfi. A "get current ctx word" query; ×17.
- Positive selectors fall through to the 0x313d40 save + table dispatch
  (bounds 0x86) — negatives never reach the table, so there is nothing for
  our 0x50314ac0 entry to mishandle: the entry shim (SPRG1/SPRG2) is exactly
  what these legs consume.

Live [PROBE✓ boot 1, `SS_PROBE_PC=0x50314ac0:r0`, logarithmic visits
1/10/100 of 169]: r0 = 0x3f, 0x50, **0xffffffff** — negative selectors
arrive as ordinary in-regime sc deliveries. Verdict: legitimate boot
behavior of the advanced stage; unrelated to Q-SL1; no emulator defect.

## Milestone-shape recommendation

**The milestone is a ROM-patch correctness fix, not a JIT/DR/NK fix:**
make the `tm_task` patch safe on 9.0.1. Recommended shape, in order:

1. **Verify-EXPECTED-first guard at the tm_task NW arm** (the pack's
   standing idiom): before NOPing base+28, verify the six words match the
   1.1-ROM expectation; on mismatch (9.0.1), SKIP with a loud
   `[ROMPATCH]` banner. This alone un-clobbers the bsr.l AND restores the
   boot-stack movea / _SetApplLimit / bsr.w sequence. Acceptance gate:
   "the boot executes 0x27a as `bsr.l 0x5000060a`, returns, runs
   0x280..0x28b intact, frontier moves past the sequencer step at 0x288."
2. **Then decide the 60Hz question for 9.0.1** (Task-0 below): with the
   patch skipped, the guest's Time Manager 60Hz task installs — on the
   newworld diagnostic profile this may be correct (M3 real delivery is the
   plan of record; ROM-PATCH-AUDIT already marks tm_task RETIRE@M3) or may
   need a re-anchored 9.0.1-specific NOP of just the install call.
3. (Residue, not this milestone): stage/map the odd-PC dispatch table
   region 0x50580000+ — converts any future odd-PC excursion from a silent
   zero-slide into the ROM's own address-error raise. Diagnosability, not
   boot progress.

NOT the milestone: anything in the sc surface (Q-SL3 clean), the DR
dispatch math (Q-SL2 by-design), or stack handling (no drain exists).

## Task-0 question list for the milestone plan

1. **(T0-A) Where is Enable60HzInts on 9.0.1?** The beheaded `bsr.w 0x2560`
   is Resource Manager 'expt' code [RAW-ROM dis], NOT the TM install — so
   the 1.1 patch's victim corresponds to which 9.0.1 call (one of
   0x268→0x1690 / 0x26e→0x15c0 / 0x274→0x41e40 / a later site)? Needed only
   if step-2 chooses re-anchoring over plain skip.
2. **(T0-B) What does the 1.1 ROM actually have at pattern+28?** One-time
   decompressed 1.1 dump (SS_DUMP_ROM with the 1.1 prefs) or upstream
   archaeology — pins the verify-EXPECTED bytes for the guard.
3. **(T0-C) Lenient-relocation sweep**: tm_task is reached via the
   `g_rom_904_lenient` whole-image fallback (window 0x2a0..0x320 misses;
   sole match at 0x262). **Audit every other lenient-relocated patch that
   writes at a fixed offset from its anchor** — the same compound failure
   (relocated anchor + 1.1-calibrated offset) may be silently armed
   elsewhere. `SS_ROM_PATCH_TRACE=1` prints `RELOCATED @…` lines — one boot
   enumerates them.
4. **(T0-D) Post-fix consequence scan**: with 0x280..0x28b restored, the
   boot re-runs `movea.l a0,a7` (A7 re-set to 0x103ffffe — harmless),
   `_SetApplLimit`, and the 'expt' walk at 0x2560 — first new wall beyond
   is unknown; budget a frontier-capture boot.
5. **(T0-E, hygiene) Stale-claim sweep**: M6A-WAVE2-SHIM-RECON.md frontier
   update + DSAT-WALL-RECON.md closeout describe this wall as "rts pops
   garbage on an empty stack" — correct those current-state claims to the
   patch-misalignment mechanism when the fix lands.

## Residues (named, not load-bearing)

- **R-SL1**: the odd-PC dispatch table 0x50580000+ unstaged (Q-SL2) —
  recommendation step 3; only reachable via a guest bug or another patch
  defect.
- **R-SL2**: −1's polled flag bit (KDP flags 0x00200000) and −2's
  `[ctx+0xec]` semantic names — uncharted; bookkeeping only.
- **R-SL3**: why the ×103 −1 poll loop runs at this boot stage (what the
  guest is waiting on) — irrelevant to the wall (the sequencer advanced
  past it); may resurface as the post-fix frontier.
- **R-SL4**: the 1.1-ROM layout at tm_task pattern+28 (T0-B) — unverified
  here; the misalignment verdict rests on the 9.0.1 raw≠patched diff +
  the patch-site code, which is sufficient.
- **R-SL5**: probe note — `SS_PROBE_LINEAR=1` did not linearize the
  0x50314ac0 probe (3 hits at the logarithmic 1/10/100 cadence despite
  SS_PROBE_CAP=200); harmless here, but the linear-mode interaction with
  this entry path is unpinned (delivery-path entry vs JIT block-entry?).

## Instrument notes (carried forward)

- The top-level boot sequencer's stack is 0x103ffffe-resting; sp values
  103fffxx with r24 in 0x50000230..0x290 are NORMAL late-boot records,
  not anomalies.
- File-offset conventions used here: NK primary 0x503xxxxx → file
  guest−ROMBase (0x314ac0); mirror 0x5046xxxx → file guest−0x50100000
  (0x367e60). Mixing them up disassembles the wrong bytes (it produced a
  bogus `blr`-prologue read at 0x214ac0 before correction).
- `tools/ring-walk.py --window` + the preserved frontier ring remain the
  whole-story source: the surviving low-ROM episodes (#3591041, #3594289,
  #3597437) are the controls that falsified the "garbage pop" framing.
