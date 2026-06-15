# S1 — static NK MMU-constant derivation + oracle validation (the honest S1 ceiling)

> **Mode:** BINDING recon, **READ-ONLY**. Static disasm of the md5-verified NK parcel
> (`[STATIC-CAPSTONE]`) + the COMMITTED `paged_mmu_translate()` as a reference oracle
> (`[ORACLE]`). **This is the adversary-recommended static substitute for the deferred
> live softmmu harvest. It is NOT a live verdict — live retirement of the window predicate
> remains owed to G1.e / S3.** Stated throughout.

**Date:** 2026-06-15
**Provenance (re-verified first):** NK parcel
`/tmp/newsheep/dump-9.0.1/Parcels.src/MacROM.src/NanoKernel-v02.27`,
md5 `61c176e90b6365e84e5c660d703e56af` (re-verified this session, 105280 B).
Parcel base `0x50310000` (file off = addr - 0x50310000). All disasm capstone BE-PPC.
Builds on `FINDINGS-discriminator-a.md` (COARSE mechanism) and
`FINDINGS-s1-window-derisk.md` rev-2 (ADV-1: DBAT coverage is the load-bearing predicate,
IBAT != DBAT). Oracle = committed `SheepShaver/src/machine/paged_mmu.cpp` (unmodified).

---

## VERDICT

**DBAT-covers-RAM+ROM = RESIDUE(runtime-computed).** The NK's BAT program is structurally
real and DBAT-capable (4 independent DBAT pairs loaded from a per-context descriptor table,
separate strides from IBAT - ADV-1 upheld), but the **descriptor VALUES are runtime data**
(per-context block, RAMSize-derived), **not constants renderable from the parcel**. Static
analysis therefore confirms the predicate is *expressible and satisfiable* but **cannot
byte-confirm coverage**. The `[ORACLE]` run proves that *when* a DBAT is programmed over RAM
(`0x10000000`, 256 MB) and ROM (`0x50000000`), the committed translator resolves those EAs
to identity PAs via the DBAT (BAT-before-page-table), and that *without* such a DBAT a data
EA misses BAT and faults against the (zeroed) HTAB. **Live retirement of numeric coverage
is owed to G1.e / S3** - this static+oracle pass narrows, it does not close, the predicate.

SDR1 sub-result: **HTABMASK = CONFIRMED-STATICALLY** (closed-form from the NK formula);
**HTABORG = RESIDUE(runtime free-region placement).**

---

## 1. The BAT program - IBAT and DBAT consume INDEPENDENT runtime descriptors `[STATIC-CAPSTONE]`

Per-context BAT (re)program at `0x503152c4`-`0x503153d0`. After invalidating all 8 BATUs,
it loads **4 IBAT pairs then 4 DBAT pairs**, each from the SAME descriptor table
`[r29+0x280]`/`[r29+0x284]` but indexed by a **different rotate stride** of the context base
`r28` (`rlwimi r29, r28, STRIDE, 0x19, 0x1c` - inserts a 4-bit slot index into r29 bits
25-28, i.e. `+slot*8`):

| BAT | descriptor stride (rlwimi SH) | site |
|---|---|---|
| IBAT0..3 | `7, 0xb, 0xf, 0x13` | `0x503152e4 / 0x50315304 / 0x50315324 / 0x50315344` |
| DBAT0..3 | `0x17, 0x1b, 0x1f, 3` | `0x50315364 / 0x50315380 / 0x5031539c / 0x503153b8` |

Each slot: `lwz r30,0x280(r29)` = BATU (BEPI/BL/Vs/Vp), `lwz r31,0x284(r29)` = BATL
(BRPN/WIMG/PP), then `mt{i,d}bat{u,l}`. IBAT uses `rlwinm r31,r31,0,0x1d,0x1b` to mask BATL;
DBAT loads BATL verbatim. **IBAT and DBAT draw from independent table entries -> ADV-1
confirmed: DBAT coverage of RAM/ROM is NOT a corollary of IBAT coverage; it is its own
predicate.** The JIT fast path does **data** accesses (RMEMBASE loads/stores) -> **DBAT** is
the load-bearing descriptor.

**Why the values cannot be statically rendered:** a parcel-wide scan for writes to
`+0x280`/`+0x284` finds **only `lwz` reads at the BAT-program site** - never an integer `stw`
to those offsets with the context-block base. The 8 BAT descriptors are therefore populated
**elsewhere, at NK init, from RAMSize** (table base `r29` is `r28`-derived; `r28` is the
runtime per-context pointer). The actual BEPI/BL/BRPN/WIMG/PP words are **per-context runtime
data, not constants in the binary** -> **RESIDUE(runtime-computed)**, exactly the class
`FINDINGS-s1-window-derisk.md` rev-2 routed to G1.e. (Kill-switch: the descriptor-construction
hunt was bounded to the scan + the program window; the init-time builder was not chased
wholesale through the 105 KB parcel - conservative residue declared, not over-claimed.)

**Decoded BAT semantics (for the oracle):** block size = `(BL+1)*128 KB`; `BL=0x7FF` => 256
MB; `Vs|Vp` validity; PA = `BRPN_aligned | (EA & in-block-mask)`. A 256 MB-class DBAT with
`BEPI=BRPN=0x10000000` is an identity block over guest RAM - the layout the predicate needs
and the oracle exercises below.

---

## 2. SDR1 / HTABMASK - closed-form, HTABMASK CONFIRMED-STATICALLY `[STATIC-CAPSTONE]`

The sizing formula at `0x503105a4`-`0x50310604` (`r15 = memsize-1`):

```
0x503105a4  cntlzw r12, r15            ; clz(memsize-1)
0x503105a8  lis    r14, 0x1ff          ; r14 = 0x01ff0000
0x503105ac  srw    r14, r14, r12       ; >> clz
0x503105b0  ori    r14, r14, 0xffff
0x503105b4  clrlwi r14, r14, 0xa       ; & 0x003fffff
...
0x503105f8  add    r12, r13, r15       ; r13 = runtime free phys region base
0x503105fc  subf   r12, r14, r12
0x50310600  rlwimi r12, r14, 0x10, 0x10, 0x1f  ; SDR1 mask field = (r14>>16)
0x50310604  mtspr  SDR1(0x19), r12
```

`HTABMASK = (r14 >> 16) & 0x1FF`, closed-form in `memsize`:

| RAM | memsize-1 | clz | r14 | **HTABMASK** | HTAB size = (mask+1)*64 KB |
|---|---|---|---|---|---|
| 64 MB | 0x03FFFFFF | 6 | 0x0007FFFF | **0x07** | 512 KB |
| 128 MB | 0x07FFFFFF | 5 | 0x000FFFFF | **0x0F** | 1024 KB |
| **256 MB (NewWorld)** | **0x0FFFFFFF** | **4** | **0x001FFFFF** | **0x1F** | **2048 KB** |
| 512 MB | 0x1FFFFFFF | 3 | 0x003FFFFF | **0x3F** | 4096 KB |

**HTABMASK = 0x1F for the 256 MB NewWorld config - CONFIRMED-STATICALLY.**
**HTABORG** = `(r13 + r15 - r14)` high bits, where `r13` is a *free physical region* located
by the alignment loop at `0x503105d4`-`0x503105f4` -> **runtime placement, RESIDUE.**

---

## 3. Oracle validation - committed `paged_mmu_translate()` `[ORACLE]`

A **temporary local battery** (`/tmp/oracle_nk.cpp`, run-then-deleted; **no `src/**` change,
tree left clean**) linked the COMMITTED `paged_mmu.cpp` and fed it the **derived HTABMASK
(0x1F)** plus a **reconstructed-standard** NewWorld DBAT program (DBAT0 = RAM `0x10000000`
256 MB identity, DBAT1 = ROM `0x50000000` identity - the descriptor *values* are the section-1
residue, so these are faithful reconstructions, labelled as such, not parcel-extracted):

```
Oracle: DBAT-covers-RAM+ROM predicate (MSR[DR]=1)
  [OK] RAM base   0x10000000 -> RESOLVE pa=10000000   (DBAT0 identity)
  [OK] RAM mid    0x18ABC123 -> RESOLVE pa=18abc123
  [OK] RAM top    0x1FFFF000 -> RESOLVE pa=1ffff000
  [OK] ROM base   0x50000000 -> RESOLVE pa=50000000   (DBAT1 identity)
  [OK] ROM mid    0x50312250 -> RESOLVE pa=50312250
Counter-case: NO DBAT over RAM (IBAT-only), zeroed HTAB
  [OK] RAM no-DBAT 0x10ABC123 -> FAULT                (misses BAT; empty HTAB faults)
ORACLE PASSED (fails=0)
```

**Reading:** *given* a DBAT over RAM/ROM, the committed translator resolves to identity PAs
via BAT (BAT precedes the page-table walk - `paged_mmu.cpp:120`), confirming the predicate is
satisfiable and the translator faithful. The counter-case shows the predicate is
**load-bearing**: strip the DBAT and a data EA misses BAT and faults against the zeroed HTAB
(no silent identity fallback). This is **static + oracle** evidence - it validates the
mechanism and the translator, **not** that the live NK actually programs such a DBAT. That
numeric fact is the residue owed to **G1.e / S3**.

---

## 4. Forge-state - the live boot does NOT exercise these NK constants (confirms M16) `[from glue]`

`SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp` - the SS_NW_TRAMPOLINE boot forges MMU state
in C; it does **not** run the NK MMU-install cluster, so none of the section-1/2 constants
are exercised live today:

| State | NK (real, sec 1-2) | Live forge (glue) | Site |
|---|---|---|---|
| SDR1 | `cntlzw`-sized, HTABMASK=0x1F (256 MB) | `sdr1_reg() = htab_base`, **HTABMASK=0 (64 KB)** | `:2806` |
| HTAB | NK builds PTEs | `memset(...,0,...)` - **zeroed** | `:2805` |
| SR | `mtsr`/`mtsrin` 256 MB segments (`0x503104b4`,`0x50315290`) | reset/test path sets **SR=0**; trampoline never runs the NK loop | `:566` |
| BAT | 4 IBAT + 4 DBAT from runtime descriptors (`0x503152c4`) | `bat_reg()=0` (test helper); trampoline never runs the program | `:563` |
| MSR | NK-managed | forged **0x7072** (EE=0 until NK raises) | `:2918` |

This is the honest "why static, not live": **SS runs the forge, not the NK MMU-install**, so
there is no live `(SR/BAT/SDR1)` map to harvest until the real Trampoline/NK runs (S3). The
softmmu-first + window milestones being DEFERRED is exactly this gap. **M16 confirmed.**

---

## Residue carried to G1.e / S3 (NOT retired here)

1. **BAT descriptor VALUES** (`[r29+0x280/0x284]`, all 8 IBAT+DBAT slots) - runtime,
   RAMSize-derived, per-context. The **numeric** DBAT-covers-RAM+ROM fact (which EA ranges
   the live NK actually block-maps, and that no installed PTE diverges from the BAT for a
   BAT-covered EA) is owed to the **G1.e live MMU-oracle** on a real `(SR/BAT/SDR1, EA)` map.
2. **HTABORG** - runtime free-region placement (`r13`). HTABMASK is closed-form (sec 2); only
   the org is residue.

Both are the same residue class Discriminator-A and the window de-risk already routed to
G1.e. This static+oracle pass **strengthens** the mechanism case (DBAT program is real,
independent of IBAT, translator resolves it correctly) and **bounds** HTABMASK numerically,
but it is **not** a live verdict and does **not** un-gate G1.e/S3.

## Kill-switch / scope honored
Bounded disasm windows only (`0x503104a0-0x50310620`, `0x503152c4-0x50315400`) + one filtered
parcel-wide scan (offset `0x28[04]` accesses - match-print only, not a wholesale human read of
the 105280 B parcel). Read-only; **no committed `src/**` change** (temp oracle battery in
`/tmp`, run-then-deleted, `git status` clean); no emulator build; no boots; no git commit.
Numeric residue declared, not fabricated.
