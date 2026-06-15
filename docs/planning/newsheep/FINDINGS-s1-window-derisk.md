# S1 window de-risk recon — does the live NK PTE engine touch JIT-covered RAM/ROM?

> **⚠ SUPERSEDED 2026-06-15:** the window is DEFERRED and the live coverage question is owed to S3,
> NOT a near-term build. This doc's "WINDOW-SAFE-BY-MECHANISM" verdict was the high-water optimism the
> same night's `MMU-NANOKERNEL-INSEPARABILITY.md` finding reversed (there is no live `(SR/BAT/SDR1)` map
> to cover until the real NK install runs, which only S3 provides). Read that doc.

> **★ rev-2 (2026-06-15) — ADVERSARY fold (verdict CORRECTED; recon core upheld, one over-claim cut).**
> The independent adversary re-derived the parcel and **rejected WINDOW-UNSAFE** (no primary evidence
> any PTE/`tlbie` path writes a JIT-covered EA) **AND rejected "SAFE-BY-ANALYSIS" as an over-claim.**
> Corrected verdict: **WINDOW-SAFE-BY-MECHANISM; numeric coverage owed to G1.e.** Binding amendments:
> - **ADV-1 (the over-claim):** §"Evidence 2" *asserted* DBAT coverage of RAM/ROM but only *showed* the
>   IBAT loads. IBAT and DBAT consume **INDEPENDENT** descriptors (strides IBAT `7,0xb,0xf,0x13` vs DBAT
>   `0x17,0x1b,0x1f,3` @`0x50315370-0x503153c8`). The JIT does **data** accesses (RMEMBASE loads/stores),
>   so **DBAT** coverage of RAM **and** ROM is the load-bearing predicate — a SECOND runtime unknown, not
>   a corollary of IBAT. The PTE-engine RPN comes from the region-descriptor table (it DOES point into
>   physical RAM — expected; irrelevant because PPC checks **BAT before page table**, so the exposure is
>   purely EA-side: is the JIT EA BAT-covered?).
> - **ADV-2 (completeness — STRENGTHENS the recon):** all **7 `tlbie` sites** checked
>   (`0x50310b60,0x503138dc,0x50314f64,0x503150c0,0x503155f0,0x50319af8,0x5032253c`; `0x5032253c` = the
>   unmap sibling of the `0x50319af8` leaf). **None manipulates a BAT** — BAT writes occur EXCLUSIVELY via
>   `mtspr {I,D}BATxx`, which the window hooks **wholesale by instruction** regardless of call site → the
>   "another path could remap a JIT page" gap is **structurally closed** (modulo the BAT-coverage predicate).
> - **G1.e gate TIGHTENED (binding):** the S1 MMU-oracle / G1.e MUST verify on a live `(SR/BAT/SDR1,EA)`
>   map that **DBAT (not just IBAT) covers the full RAM aperture AND the ROM aperture**, plus BAT-priority
>   sanity (no installed PTE diverges from the BAT for a BAT-covered EA). If any JIT EA is reachable only
>   via page table → fall back to tlbie/HTAB interception or softmmu and re-band S1.
> - **Disposition:** **un-park Task B as the window — CONDITIONALLY** (the verdict is "no evidence
>   against; one runtime predicate owed to G1.e", NOT "statically proven safe").

**Date:** 2026-06-15  **Mode:** BINDING recon, read-only, static only (no `src/**`, no builds/boots).
**Question:** Our preferred "window" MMU (Dolphin shadow-arena) remaps the host NATMEM arena
at `mtspr` BAT/SDR1/SR time but does NOT intercept the NK's live HTAB-store+`tlbie` PTE
updates (`0x50319af8`/`0x50319b28`). The window is SAFE iff that PTE engine never remaps a
**JIT-fast-path** page — guest RAM `0x10000000..+RAMSize` or ROM `0x50000000..` — i.e. iff
RAM/ROM stay BAT/SR-mapped (coarse) and the PTE engine only maps high logical/pageable space.

**Provenance (verified first):** NK parcel
`/tmp/newsheep/dump-9.0.1/Parcels.src/MacROM.src/NanoKernel-v02.27`,
md5 `61c176e90b6365e84e5c660d703e56af` (verified, 105280 B). Parcel base `0x50310000`
(file off = addr - 0x50310000). All disasm capstone BE-PPC. Evidence tag `[STATIC-CAPSTONE]`.
Builds on `FINDINGS-discriminator-a.md` (COARSE verdict, BAT/SR mechanism) — this recon
narrows that to the **window-specific** PA-disjointness question.

---

## VERDICT: WINDOW-SAFE-BY-ANALYSIS (mechanism), with a numeric-disjointness residue routed to G1.e

The live HTAB PTE engine operates on a **bounds-checked, segment-region-routed *logical*
page domain** that is structurally distinct from the coarse BAT/SR apertures covering
RAM/ROM. The JIT fast-path regions (RAM `0x10000000`, ROM `0x50000000`) are reached by
BAT block translation + SR, which the window's `mtspr` hooks already cover; they are not
the EAs the PTE engine maps. Therefore the window's failure-to-intercept HTAB+`tlbie`
does **not** silently desync the JIT fast path — **G1.e is a confirmation, not a gate**.

The one thing NOT statically provable is *numeric* disjointness: the actual BAT
base/length descriptor values and the segment-region-table contents are **runtime data**
(computed from RAMSize, stored in the per-context block — not constants in the parcel),
so a byte-exact "PTE-PA set INTERSECT BAT-PA set = empty" cannot be rendered from the
binary alone. That residue is the confirmation owed to **G1.e** (the S1 MMU-oracle test),
exactly as Discriminator-A's HTAB-density residual — it is *not* a re-band trigger.

---

## Evidence 1 — the PTE engine maps a segment-routed LOGICAL page domain `[STATIC-CAPSTONE]`

The HTAB store/`tlbie` cluster (`0x50319ae0`-`0x50319b3c`) is the leaf of a map routine
whose **input page `r4`** is, immediately upstream (`0x50319a3c`+), validated and routed:

```
0x50319a3c  lwz     r9, 0x6b4(r1)        ; upper bound for the logical page domain
0x50319a40  cmplw   cr4, r4, r9          ; r4 (page being mapped) vs bound
0x50319a44  rlwinm. r9, r4, 0, 0, 0xb    ; check EA[0..11]
0x50319a48  blt     cr4, 0x503187b0      ; out-of-domain -> different path
0x50319a4c  bne     0x503187b0
0x50319a50  lwz     r15, 0x5e8(r1)       ; base of a per-SEGMENT region-descriptor table
0x50319a54  rlwinm  r9, r4, 0x13, 0x19, 0x1c  ; r4 top nibble -> segment index 0..15
0x50319a58  lwzx    r15, r15, r9         ; pick the region descriptor for r4's segment
0x50319a5c  clrlwi  r9, r4, 0x10
0x50319a60  lhz     r8, 0(r15)           ; walk the segment's region list (lhzu loop)
   ...
0x50319a9c  rlwinm  r15, r9, 0x16, 0, 0x1d   ; build PTE word0 (VSID/API) from region attrs
0x50319ab0  rlwinm  r16, r9, 0, 0, 0x13      ; PTE construction
   ...
0x50319af0  slwi    r9, r4, 0xc          ; EA = page<<12
0x50319af8  tlbie   r9                   ; invalidate that EA
0x50319b28  stw     r16, 0(r15)          ; store PTE word0 into HTAB slot (r15 = PTEG ptr)
0x50319b2c  stw     r9,  4(r14)          ; PTE word1 (RPN)
```

The PTEG slot is found by hashing the EA's SR-derived VSID (`0x50319b74`+:
`mfsrin r6,r9; xor r9,r6,r4; slwi r7,r9,6; and r8,r8,r7 (HTABMASK at r1+0x6a0);
lwzux r7,r14,r8 (HTABORG at r1+0x6a4)`).

**Reading:** `r4` is a *logical* page that must (a) pass an upper-bound gate (`r1+0x6b4`)
and (b) resolve through a **16-entry per-segment region-descriptor table** (`r1+0x5e8`,
indexed by `r4`'s top nibble) before any PTE is built. This is the Mac OS VM /
Memory-Manager *logical/pageable* address space — a managed region domain — **not** the
bare physical RAM/ROM apertures the JIT bare-accesses. A JIT fast-path load of guest RAM
`0x10000000` or ROM `0x50000000` does not enter this routine; those are resolved by
BAT/SR (Evidence 2), which is what the engine *reads* (`mfsrin`) to compute the hash,
never what it *rewrites*.

## Evidence 2 — RAM/ROM are covered by BAT block translation + SR (coarse) `[STATIC-CAPSTONE]`

Per-context BAT program (`0x503152c4`-`0x5031535c`): all 8 BATUs invalidated, then 4
IBAT pairs loaded from a descriptor table at `[r29+0x280]/[r29+0x284]`, `r29` derived from
the context pointer `r28` (`rlwimi r29,r28,7|0xb|0xf|0x13,0x19,0x1c` — 4-bit stride = the
4 BAT slots):

```
0x503152e4  rlwimi  r29, r28, 7,  0x19, 0x1c
0x503152e8  lwz     r31, 0x284(r29)      ; BATL (PA + WIMG + PP)
0x503152ec  lwz     r30, 0x280(r29)      ; BATU (EA base + BL + Vs/Vp)
0x503152f0  rlwinm  r31, r31, 0, 0x1d, 0x1b
0x503152f4  mtibatl 0, r31
0x503152f8  mtibatu 0, r30
   ... (slots 1,2,3 identically) ...
```

Initial SR/SDR1 setup (`0x503104a8`+): `SR0 = 0x20000000` (`lis r12,0x2000; mtsr 0,r12`),
`SR1..15 = 0` initially; the live per-context SRs are programmed by the `mtsrin` loop at
`0x50315290` (Discriminator-A Evidence 1, 256 MB segment stride via `addis -0x1000`).
SDR1/HTAB is sized from RAMSize (`0x503105a4`: `cntlzw + lis 0x1ff; srw; ori 0xffff;
clrlwi 0xa` -> `mtspr SDR1` @`0x50310604`). So RAM/ROM translation is **block (BAT) + segment
(SR)**, both of which the window's `mtspr {IBATx,DBATx,SR,SDR1}` hooks intercept and remap.

## Evidence 3 — reconciliation (the disjointness argument)

| Region | How the guest reaches it | Window hook covers it? | PTE engine rewrites it? |
|---|---|---|---|
| RAM `0x10000000..+RAMSize` (JIT fast path) | BAT block + SR (coarse) | **Yes** (`mtspr` BAT/SR/SDR1) | No — not in the segment-region logical domain |
| ROM `0x50000000..` (NK's own home, JIT fast path) | BAT block + SR (coarse) | **Yes** | No |
| Mac OS logical / pageable space | HTAB PTE (segment-region-routed `r4`) | n/a (not bare-accessed by JIT) | **Yes** — but JIT never bare-touches it |

The PTE engine's write domain (segment-region-routed logical pages, bounds-gated at
`r1+0x6b4`) and the JIT fast-path read domain (BAT/SR-mapped RAM+ROM) are **distinct by
construction**: the engine *reads* the SR to hash but only *writes* HTAB slots for the
managed logical domain. The window therefore stays in sync for everything the JIT bare-
accesses.

---

## Residue carried to G1.e (NOT retired here)

Static analysis bounds the PTE engine's domain by **mechanism** (segment-region table +
bound gate), but the **numeric** BAT descriptor values (`[r29+0x280/0x284]`) and the
segment-region-table contents (`r1+0x5e8`) are runtime data (RAMSize-derived, context-
block-resident) — not constants in the parcel — so byte-exact PA-set disjointness is not
statically renderable. This is the same residue class Discriminator-A routed to the S1
MMU-oracle test (HTAB density / per-page-perm diversity). **G1.e** retires it: the
shadow-arena vs reference-translator oracle, run on a real `(SR/BAT/SDR1, EA)` mapping,
must show the PTE engine never produces a PA in the BAT-covered RAM/ROM window. If it ever
does, fall back to tlbie/HTAB interception or softmmu and re-band S1. Until then, the
mechanism evidence is strong enough to **un-park Task B as the window (Dolphin shadow-arena)**.

## Kill-switch / scope honored
Three bounded disasm windows only (`0x50319980-0x50319bb0`, `0x503104a0-0x50310620`,
`0x503152a0-0x50315360`); the 105280 B parcel was NOT disassembled wholesale. Read-only,
no `src/**`, no builds/boots, no git commit. Numeric-disjointness thread bounded ->
conservative residue declared rather than over-claimed.
