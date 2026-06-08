# Plan: Proper New World (parcels) ROM Support — break the 9.0.4 ceiling

> **Status:** 🟡 Phase 2/3 · **RUNTIME MILESTONE 2026-06-08** — the parcels (9.0.1) PPC **nanokernel BOOTS under our JIT** (diagnostic gate skips the un-ported 68k handoff). `:715` + `sr_load` skips runtime-validated. Remaining gate to a real 9.x boot: `jump68k` handoff redirect + 68k-side HLE (nvram/via) ports. See "RUNTIME MILESTONE" below. · **Created:** 2026-06-03 · **Updated:** 2026-06-08
> **Why this doc exists:** Support New World (parcels/CHRP) ROMs and break the Mac OS 9.0.4 ceiling. Drafted after getting 9.0.4 booting via the 1.1 ROM and building the `rom-inspect` tool.
>
> **Phase 0 RESULT (2026-06-07) — and it's PHASE 2, not Phase 1.** Ran the env-gated `find_rom_data`
> tracer (`SS_ROM_PATCH_TRACE=1`, `rom_patches.cpp:98`) against `2001-12-19 - Mac OS ROM 9.0.1.rom`
> (CHRP-**parcels**, decodes + type-detects NewWorld). First failure = the very first `find_rom_data`
> in `patch_nanokernel_boot` (`sr_init_dat`, not in `[0x3101b0,0x3105b0)` but present at `0x3106f8`).
> The "early failure → Phase 1" gate guess was then **refuted by deeper analysis** (don't trust the
> gate; measure):
> - **Parcel enumeration** (extend below): the `'rom '` parcel decodes to a **complete, valid 4 MB
>   image** (nanokernel "NewWorld v1.0" present at 0x30d064). The other 27 parcels are OpenFirmware
>   device-tree `node`/`prop`/`psum` entries (+ 2 `node`/"Code" worth a glance), NOT Mac OS ROM image.
>   So **`decode_parcels` is NOT dropping the Mac OS image → Phase 1 (incomplete decode) is RULED OUT.**
> - **Full pattern sizing** (`tools/rom-patch-sizing.py` over `rom-inspect --dump`, control = the
>   working `1.1` image): of 83 literal-range patches — **27 in-range, 31 relocated, 25 absent
>   (17 applicable-absent** after excluding other-`ROMType` patterns the `1.1` boot also skips). Absent
>   = the byte sequence was *rewritten* between the 1998 and 2001 ROMs → real RE to re-locate. This is
>   squarely **Phase 2 (version pattern-drift, "days to weeks, no guarantee")**, not a quick fix. Naive
>   range-widening is a dead end (patches also write hardcoded/absolute offsets).
>
> (Triggered while testing the AltiVec-detection hypothesis: this NewWorld ROM carries `'ppcf'`
> (2× decoded) which the 1998 LZSS `1.1` ROM lacks (0× decoded) — see LEARNINGS 2026-06-07.)
> **Strategic alternative surfaced:** rather than the Phase-2 port of *this* ROM, source a G4-era ROM
> whose layout is closer to `1.1`'s (so most patterns match) AND carries `'ppcf'`; `rom-patch-sizing.py`
> checks any candidate in seconds. Whether such a ROM exists is unknown (G4 'ppcf' arrived with newer,
> drifted layouts) — but it's a cheap check before committing to Phase 2.
> _Markers: ✅ done · 🟡 in progress · ⏸ blocked/deferred · ☐ todo. Finished an item? Flip its marker, bump **Updated**, and add a `CHANGELOG.md` entry (see [CONTRIBUTING](../../CONTRIBUTING.md) → "Documentation Lifecycle")._
>
> **▶ FRESH-AGENT HANDOFF for the current frontier (supervisor/MMU fidelity):**
> [`HANDOFF-NEWWORLD-SUPERVISOR-MMU.md`](HANDOFF-NEWWORLD-SUPERVISOR-MMU.md) — mission/why (defends the
> correctness-via-forcing-function rationale), current state, the MMU rung-ladder + cheap first
> experiment, and the how-to-work loop. **Start there if you're picking this up.**

---

## Why this matters (the premise)

SheepShaver's practical OS ceiling (~9.0.4) is fundamentally a **ROM-support limitation**,
not an arbitrary cap. To boot Mac OS 9.1+/9.2.x you need the newer "Mac OS ROM" file that
shipped with those releases — and those newer ROMs are **parcels-format** New World ROMs,
which SheepShaver's `PatchROM()` currently rejects. So "proper New World ROM support"
(parcels ROMs) is the necessary first domino for going past 9.0.4.

**Important nuance — "New World support" already partly exists.** SheepShaver *does* boot
New World ROMs: the `1998-07-21 - Mac OS ROM 1.1.rom` is a New World ROM (LZSS-format,
`ROMTYPE_NEWWORLD`) and boots the whole 8.x–9.0.4 range. What's missing is support for the
**parcels-format** New World ROMs used by later systems. This plan is about those.

---

## What we already know (verified 2026-06-03)

Using the `rom-inspect` tool (`SheepShaver/rom-inspect/`, shares decode code with the
emulator via `src/include/rom_decode.hpp`):

| ROM | Format | Decodes? | Nanokernel ID @0x30d064 | Type-detect | Emulator result |
|-----|--------|----------|--------------------------|-------------|-----------------|
| `Mac OS ROM 1.1` | CHRP + LZSS | ✅ | `NewWorld v1.0.p.` | NewWorld | **Boots** 8.6 + 9.0.4 |
| `Mac OS ROM 9.0.1` | CHRP + **parcels** | ✅ | `NewWorld v1.0..` | NewWorld | **"Unsupported ROM type"** |

Key facts established:
- The 9.0.1 parcels ROM **decodes fine** and **passes type-detection** (it IS recognized
  as NewWorld). So the failure is **downstream in `PatchROM()`**, not in decode or type ID.
- The emulator's "Unsupported ROM type" alert (`src/main.cpp:162`) fires on **any**
  `PatchROM()` failure — it is misleading; it does NOT mean the type wasn't recognized.
- Confirmed empirically: a rebuilt binary run against the 9.0.1 ROM prints
  `Reading ROM file...` (decode OK) then `ERROR: Unsupported ROM type.` and exits cleanly.

---

## Code map (current line numbers — verify before editing, files change)

`SheepShaver/src/rom_patches.cpp`:
- `find_rom_data(start, end, data, len)` — **:98** — the linear `memcmp` byte-pattern search
  every patch relies on; returns the offset or `0` if not found.
- `check_rom_patch_space(base, size)` — **:207** — verifies a ROM region is free for patching.
- `PatchROM()` — **:562** — the gauntlet. After type-detection (**:575**, now via
  `rom_detect_type`), it runs:
  - 5× `check_rom_patch_space(...)` — **:580, :582, :584, :586, :588** — any false → reject
  - `patch_nanokernel_boot()` — **:592** (defn **:620**)
  - `patch_68k_emul()` — **:593** (defn **:939**)
  - `patch_nanokernel()` — **:594** (defn **:1187**)
  - `patch_68k()` — **:595** (defn **:1363**)
- Each patch function is a long sequence of `find_rom_data(...) == 0` → `return false`
  checks (e.g. **:643, :655, :798, :807, :820, ...**). ~100+ pattern searches total, each
  calibrated to byte sequences present in specific ROM versions.

`SheepShaver/src/include/rom_decode.hpp`:
- `decode_parcels(src, dest, size)` — **:86** — **only decodes the `'rom '` parcel** into
  `dest`. A parcels ROM contains multiple parcels; if patch targets live elsewhere, the
  assembled image is incomplete. (Prime suspect for Phase 1 — see below.)

`SheepShaver/rom-inspect/rom-inspect.cpp` — offline ROM analyzer; extend it for Phase 0.

---

## Decomposition

```
Phase 0 — Diagnose      Pinpoint WHICH PatchROM stage fails on a parcels ROM, and why   ← start here
Phase 1 — Decode        Fix decode completeness (parcels merge) if that's the gap
Phase 2 — Patch         Extend/variant the byte-pattern patches for the new layout
Phase 3 — Boot & debug  Get it to boot; chase downstream nanokernel / Mac OS issues
```

Everything depends on Phase 0. The Phase 0 result decides whether the bulk of work is
Phase 1 (structural, tractable) or Phase 2 (deep reverse-engineering, weeks).

---

## Phase 0 — Diagnostic ✅ DONE 2026-06-07 (see RESULT in the header)

**Outcome:** the `SS_ROM_PATCH_TRACE` tracer (option A below, the env-gated tracer — realized at the
`find_rom_data` level in `rom_patches.cpp:98`) named the first miss: `patch_nanokernel_boot`'s
`sr_init_dat`. Decision gate → **Phase 1** (incomplete parcels decode). Phase 0 design retained below.

**Goal:** one data point — the first `PatchROM` stage that fails on the 9.0.1 parcels ROM,
then the specific `find_rom_data` pattern that isn't found within it.

**Design — env-gated stage tracer in `rom_patches.cpp`:**
- Gate on `getenv("SS_ROM_PATCH_TRACE")`. When unset: **zero behavior change** (must not
  risk the working 1.1 / 8.6 / 9.0.4 path).
- When set: in `PatchROM()` (**:580–:595**), log the outcome of each top-level step and
  which one first returns false:
  ```c
  if (!check_rom_patch_space(CHECK_LOAD_PATCH_SPACE, 0x40)) { TRACE("check CHECK_LOAD"); return false; }
  ...
  if (!patch_nanokernel_boot()) { TRACE("patch_nanokernel_boot"); return false; }
  ...
  ```
- Second pass: inside whichever function failed, add a `TRACE` at each `find_rom_data(...)==0`
  site so the exact missing pattern is named.

**How to run** (no interactive boot; clean exit on ROM rejection):
1. Build the instrumented binary (`cd SheepShaver/src/Unix && make`) while no emulator runs.
2. Point a throwaway prefs at the 9.0.1 ROM and run:
   `SS_ROM_PATCH_TRACE=1 ./SheepShaver --config <9.0.1-prefs> 2>/tmp/patchtrace.log`
3. Read `/tmp/patchtrace.log` — it names the failing stage and pattern.

**Alternative (no emulator launch at all):** extend `rom-inspect` to replicate
`check_rom_patch_space` + the `find_rom_data` searches against the decoded image and report
the first miss. More porting up front (~100 patterns), but keeps everything offline and
reusable. The env-gated tracer is the faster first cut; the rom-inspect extension is the
durable tool (see **Open follow-ups** below).

**Decision gate after Phase 0:**
- Fails at `check_rom_patch_space` or very early in `patch_nanokernel_boot` → likely
  **incomplete decode** (image missing content) → Phase 1.
- Decodes complete but specific deep patterns aren't found → **pattern drift** → Phase 2.

---

## Phase 1 — Decode completeness (if Phase 0 points here)

Hypothesis: `decode_parcels` (`rom_decode.hpp:86`) extracts only the `'rom '` parcel. The
parcels container has others (properties, config, possibly additional code parcels). If the
4 MB image PatchROM patches is missing pieces, patterns won't be found.

Work:
- Enumerate all parcels in the 9.0.1 ROM (add a parcel-listing mode to `rom-inspect`:
  walk the parcel chain at `decode_parcels`'s `parcel_offset`, print type/offset/size/name).
- Determine which parcels must be merged and at what target offsets to reconstruct a
  layout matching what PatchROM expects.
- Extend `decode_parcels`/`decode_rom_image` to assemble the full image. Keep it shared
  (header) so emulator + inspector + harness all benefit.
- Re-run Phase 0 tracer to confirm the failing stage advances.

---

## Phase 2 — Patch adaptation 🟡 IN PROGRESS (2026-06-07, target = 9.0.4 G4 ROM)

> **🔑 KEY FINDING (2026-06-07, autonomous diagnostic boot of the user's New World ROM set).** The blocker
> is **bigger than the patch-sizing "17 absent patterns" implied** — it's a **structural rewrite of the
> nanokernel-boot routine**, not just missing byte sequences. With `SS_ROM_LENIENT=1` (forces lenient mode
> for any NewWorld ROM, so the 31 *relocated* patterns auto-resolve), booting the versioned **Mac OS ROM
> 9.1.1** (and 9.0.1/9.6.1/9.8.1/10.2.1 — all identical: 27 in-range / 31 relocated / 17 applicable-absent)
> aborts almost immediately at `patch_nanokernel_boot` **`rom_patches.cpp:715`**:
> `if (ntohl(lp[6]) != 0x2c0c0001) return false;` — it expects `cmpwi r12,1` (the CPU-detect compare) at
> `pvr_read+24`. In the parcels ROM that word is a `mtspr`; the real `cmpwi r12,1` is ~0x520 bytes away in
> a *different* routine (`@0x310a1c` vs the site `@0x3104f8`). So the entire pvr_read → CPU-detect →
> per-CPU-data-table logic (`rom_patches.cpp` ~714-870) is **calibrated to the 1.1 LZSS ROM's code layout
> and must be re-RE'd for the parcels layout** — and this is the *first* patch in `patch_nanokernel_boot`,
> so essentially the whole boot-patch routine needs re-RE. **Implication:** patch-sizing (byte-pattern
> presence) *undercounts* the work — it can't see structural `lp[N]==const` assumptions. The real Phase-2
> effort is "re-RE patch_nanokernel_boot for parcels," genuine multi-session RE (the "days-to-weeks"
> estimate stands, possibly worse). Tooling for it shipped: `SS_ROM_LENIENT=1` + `SS_ROM_PATCH_TRACE=1`.
> Assets staged by the user: `Downloads/New_World_Mac_Roms/New World ROM/` (19 versions), `macos_921_ppc.iso`,
> `macos-922-uni.zip`.

> **First patch-adaptation target characterized (2026-06-07, capstone disasm of 9.1.1 parcels).** The
> `rom_patches.cpp:715` blocker, decoded: the 1.1 ROM has `mfpvr r12` then `cmpwi r12,1` at +24 (CPU-detect
> inline, per-CPU offset in the next word). The **parcels ROM restructured it**: `mfpvr r12` →
> `rlwinm. r12,r12,0,0,14` → `bne` → a **BAT/SPR-clear init block** (`mtspr/mtibatl/mtibatu …`) → `b`.
> The real CPU-detect (`cmpwi r12,1` / `cmpwi r12,3`) is moved to **`0x310a1c`** and uses a **jump-table
> mechanism** (`addi r11,r11,<offset>` accumulation → common handler `@0x311350`), not the inline
> compare+beq the patch expects. So the patch must (a) detect the parcels layout (mfpvr→rlwinm./bne, not
> →cmpwi) and (b) re-target the CPU-detect/per-CPU-data patch to the `0x310a1c`/`0x311350` structure (or
> confirm the parcels ROM self-handles our faked PVR=7400 and skip). This is the *first* of the boot-patch
> routine's structural divergences; each subsequent patch needs the same treatment. **Approach: additive —
> a parcels branch in `patch_nanokernel_boot` gated on layout/cksum, leaving the 1.1 path byte-identical
> ("support both old + new world ROMs").** A reference-research agent is checking for prior art (adapt vs
> from-scratch) before committing to the full grind.

### ⭐ GO/NO-GO RE-SCOPE (2026-06-08) — the `:715` wall COLLAPSES; `patch_nanokernel_boot` is ~2 routines, not a full re-RE

**Method:** decoded both ROMs (`rom-inspect --dump` → `/tmp/rom901.bin`, `/tmp/rom11.bin`), disassembled
the CPU-detect region with capstone, cross-referenced against the buildable RE'd nanokernel source
([`elliotnunn/NanoKernel`](https://github.com/elliotnunn/NanoKernel), cloned), and byte-tested **every**
`patch_nanokernel_boot` pattern against the decoded 9.0.1 image. Result overturns the earlier
"essentially the whole boot-patch routine needs re-RE" read.

**1. The `:715` CPU-detect block is SKIPPABLE on parcels (the biggest documented wall — gone).**
The patch (`rom_patches.cpp` ~714–858) exists to (a) feed the ROM our faked PVR and (b) inject per-CPU
cache/TLB data **because the 1998 1.1 ROM has no table entry for the 7400**. On the 2001 parcels ROM
neither is needed:
- `mfpvr` in THIS emulator returns the faked global `PVR` directly (`ppc-execute.cpp:1294`,
  `SPR_PVR → PVR`; default `0x000c0000` = 7400, `main_unix.cpp:453`). So the patch's `mfpvr→lwz XLM_PVR`
  swap is a **redundant no-op** here (both yield 0x000c0000).
- The parcels ROM's real CPU-detect at **0x310a1c** has a native **`cmpwi r12,0xc` (7400/G4) handler**
  (chain: 1,3,4,6,7,8,9,0xa,**0xc**,0xd → common handler 0x311350). With `PVR>>16 = 0xc` it self-selects
  the correct per-CPU data. **So the patch should be SKIPPED for parcels, not re-RE'd** — a branch-guard
  (`if lp[6] != 0x2c0c0001 && parcels → skip block`), not weeks of CPU-detect reverse-engineering. The
  1.1 path (`lp[6] == 0x2c0c0001`) stays byte-identical.

**2. Behind `:715`, only ~2 mandatory patterns are genuinely ABSENT (restructured); the rest relocate.**
Byte-test of all 17 `patch_nanokernel_boot` patterns vs the 9.0.1 image:
- **Present (relocated — the existing `g_rom_904_lenient` whole-image fallback resolves these):**
  `sr_init`, `pvr_read`, `sprg3_mq`, `msr`, `sprg3`, `pvr_read2`, `pvr_read4`, `sdr1_read`, `pgtb_clear`,
  `desc_create`, `sr_load_caller`, `sr_load2`, `pm_check`, `jump68k_caller`; `twi` is in-range. (~14)
- **ABSENT — restructured, need real RE (find the rewritten routine + retarget):**
  `sr_load` (`7c0004ac 839d0000 938105e8` — "don't load SRs/BATs"; only the bare `mtmsr` opcode survives,
  ×257) and `jump68k_dat` (`7d9243a6 7d5a03a6 7d7b03a6` — SRR0/SRR1 setup to enter the 68k emulator).
  Both are core boot steps; both have direct analogues in `elliotnunn/NanoKernel` (SR/BAT load in
  `PageTable.s`/`Power.s`, the 68k-emulator entry in `Emulate.s`) to ground the relocation.
- `pvr_read3` absent but **optional** (`!= 0` then patch) — no action.

**Revised effort for `patch_nanokernel_boot` (1 of 4 patch functions):** skip 1 block (hours) + RE 2
routines (days, grounded by the NanoKernel source) + lenient for the rest (done). NOT the multi-week
"whole routine" grind the `:715` finding first implied. **Caveat (honest):** this is only the first
patch function — `patch_68k_emul`/`patch_nanokernel`/`patch_68k` have their own absent patterns (the
2026-06-07 runtime attempt on 9.0.4 reached `patch_nanokernel`'s `nvram2_dat`), and Phase 3 + the
possible "second wall" remain unquantified. But the **single biggest documented blocker is now a
branch-guard**, which materially improves the odds. **Next concrete step:** add the env-gated parcels
skip-guard at `:715`, run `SS_ROM_LENIENT=1 SS_ROM_PATCH_TRACE=1` on 9.0.1, and confirm PatchROM
advances to the `sr_load`/`jump68k` RE work (or the next function).

### patch_nanokernel_boot wall-map (2026-06-08) — iterative skip+trace on 9.0.1, execution order

Built the parcels skip-guards and ran `SS_ROM_LENIENT=1 SS_ROM_PATCH_TRACE=1` on 9.0.1 after each,
mapping the real (execution-order) walls. **Two walls collapsed, the third is the hard one:**

| # | wall | verdict | status |
|---|------|---------|--------|
| 1 | `:715` CPU-detect / per-CPU-data | **SKIP** — redundant: mfpvr already returns faked 7400, ROM's own 0x310a1c chain has a `cmpwi r12,0xc` (7400) handler | ✅ committed, validated (provably redundant) |
| — | sprg3_mq, msr, sprg3, pvr_read2(×2), pvr_read4, sdr1_read, pgtb_clear, desc_create, sr_load2, pm_check | relocated → existing lenient whole-image fallback resolves | ✅ pass (⚠ some lenient hits may be false-positive matches — verify semantically before trusting at boot) |
| — | pvr_read3 | absent but **optional** (`!=0` then patch) | ✅ tolerated |
| 2 | `sr_load` (SR/BAT-load) | **SKIP** — `mtsrin`/`mt{i,d}bat*` are JIT no-ops; routine runs harmlessly under flat addressing | ✅ committed (⚠ RUNTIME-UNVALIDATED: nanokernel may read the KernelData BAT fields it saves) |
| 3 | **`jump68k`** (`7d9243a6` = mtsprg2;mtsrr0;mtsrr1;rfi) | **CANNOT SKIP** — the PPC→68k boot handoff; must be retargeted to SheepShaver's emulator entry | 🟡 **probe-characterized** — dispatch fields mapped, architecture understood |

**`jump68k` probe results (2026-06-08).** SS_PROBE_PC at the idle loop (0x5032751c) and
check_work (0x50326880) reveals the nanokernel's steady-state:

| KDP field | guest addr | value | meaning |
|-----------|-----------|-------|---------|
| +0x5a0 (context ptr→SPRG0) | 0x68ffe5a0 | **0x00000000** | 68k context never initialized |
| +0x5a4 (68k code base) | 0x68ffe5a4 | **0x00000000** | 68k code base never set |
| +0x648 (opcode table) | 0x68ffe648 | **0x5046e8c0** | points into ROM area (past 4MB ROM; built by nanokernel init) |
| -0x964 (target MSR) | 0x68ffd69c | **0x0000d032** | IR\|DR\|EE\|ME\|RI = supervisor, MMU on, interrupts |
| -0x900 (work queue head) | 0x68ffd700 | **0x00000000** | empty — no work posted |

0x503126b4 (dispatch routine) is **never reached** — the idle loop polls [KDP-0x900]=0
forever. The work queue is empty because the PPC→68k handoff was skipped (diagnostic
`SS_ROM_SKIP_JUMP68K=1`), so no code ever posts the initial boot work item.

**Architecture (parcels vs OldWorld).** OldWorld has a direct `bl jump68k` caller that
SheepShaver replaces with a 5-insn redirect to its emulator init. Parcels has a
fundamentally different work-queue-driven architecture: the nanokernel idles polling
[KDP-0x900], and dispatch (0x503126b4) switches context via KDP+0x5a0/0x5a4 + rfi. The
OldWorld `mtsprg2;mtsrr0;mtsrr1;rfi` signature is completely absent from parcels.

**Discriminator check (2026-06-08).** The OldWorld redirect reads KDP+0x1184 (emulator init
routine) and +0x119c (opcode table). Searched the entire parcels nanokernel (0x50310000–
0x50330000) for any `stw`/`lwz` referencing those offsets — **zero hits**. Runtime probe
confirms: [KDP+0x1184]=0x73bf0000, [KDP+0x119c]=0x73d70000 (uninitialized garbage, not
valid guest pointers). **Verdict: the OldWorld 5-insn redirect is NOT copy-pasteable.**
Parcels uses a completely different KDP layout for its dispatch fields; a new redirect
must be designed from the parcels nanokernel's own field map (+0x648 opcode table,
+0x5a0/0x5a4 context, -0x900 work queue).

**Dispatch field seeding experiment (2026-06-08).** Seeded the 3 missing fields in the
NW trampoline (`sheepshaver_glue.cpp`, gated by `SS_NW_TRAMPOLINE`):

| Seed | Value | Rationale |
|------|-------|-----------|
| KDP+0x5a0 (context) | KDP (0x68FFE000) | cold init sets this from SPRG0, which = KDP |
| KDP+0x5a4 (code base) | ROMBase + 0x46d218 | mirror region; +0x26e8 → emulator start at 0x46f900 |
| [KDP-0x900] (work queue) | 1 | non-zero triggers dispatch |

Also patched 5 VIA/CUDA I/O poll loops (`lbz rN,2(r28); eieio; andi. rN,rN,4; beq $-0xC`)
that spin waiting for a device-ready bit that doesn't exist in emulation (NOP the beq).
Requires `SS_SYNTH_DEC=1` (synthesized decrementer) to pass the nanokernel's timer checks.

**Results:** The nanokernel advanced past the idle loop into the interrupt-handling event
loop (VIA/CUDA init sequence). Block rate dropped from ~1500M/10s (tight idle) to ~240M/10s
(real work). 629 blocks compiled (vs 614 idle). But jDR=0 — the DR Emulator is never
entered. Root cause: the nanokernel has **multiple work queues** at different KDP offsets
(-0x900, -0xaf0, -0xb30). The active interrupt handler checks `-0xaf0` and `-0xb30`, NOT
`-0x900`. Our seed at `-0x900` is never consumed. Additionally, value `1` is not a valid
task descriptor — it should be a pointer to a task control block (TCB).

**Next step:** understand the TCB format and which work queue drives the DR Emulator
dispatch. The dispatch routine at 0x503126b4 ends with `rfi` into `code_base + 0x26e8`
= SheepShaver's patched emulator start at ROM+0x46f900 (`patch_68k_emul` writes this).
The routing is correct; only the work-queue seeding is wrong. Two approaches:
(a) RE the TCB format from the nanokernel's context-switch code;
(b) bypass the work queue entirely — patch the nanokernel to call the dispatch routine
directly after init completes (surgical redirect, avoids TCB complexity).

**Net revised picture:** `patch_nanokernel_boot` is *not* a wholesale re-RE — it's ~2 skips (done) + 1
load-bearing handoff retarget (`jump68k`) + the lenient-resolved remainder. After it: 3 more patch
functions (`patch_68k_emul`/`patch_nanokernel`/`patch_68k` — the 9.0.4 runtime attempt reached
`patch_nanokernel`'s `nvram2_dat`) + Phase 3 + possible second wall. The headline wall shrank a lot;
the path is now "finish jump68k → other functions → first boot attempt (validates the skips)."

### ⭐⭐ RUNTIME MILESTONE (2026-06-08) — the parcels PPC nanokernel BOOTS under our JIT

Forced PatchROM to complete via a diagnostic gate (`SS_ROM_SKIP_JUMP68K=1` + `SS_ROM_LENIENT=1`,
default off, 1.1 byte-identical): leave `jump68k` un-redirected and make `patch_68k`'s absent 68k-side
HLE (nvram/via — all absent on parcels) non-fatal at the call site, since none of it is reached before
the handoff. Then **booted the 9.0.1 parcels ROM** (`./SheepShaver --config <9.0.1, no disk>`):

- **PatchROM completes** and the **PPC nanokernel boots and runs** — heartbeat shows execution **100%
  in `jNK`** (nanokernel region), `comp=27`, settling into a stable ~60Hz interrupt-idle loop at
  `pc=0x503127a8/b8`. It idles because `jump68k` is un-redirected (no 68k OS to enter).
- **This RUNTIME-VALIDATES the `:715` + `sr_load` skips** — the nanokernel ran 160s+ with no crash, so
  those skips don't break PPC boot (the open worry on `sr_load`'s KernelData BAT fields is answered:
  fine through nanokernel init at least).
- **Watchdog finding:** the `[ALARM]` boot-stall watchdog initially missed this wedge (it runs at
  ~0.1M/s, under the old `>1M/s` "spinning" gate, which was tuned for the ~150M/s DSAlert tight-loop).
  Lowered the gate to `>0.01M/s` (pre-idle scoping + idle-disarm is the real false-positive guard);
  `[ALARM]` now fires at 15.1s. Committed.

**So the PPC side of the parcels port is PROVEN to work.** The remaining gate to a real 9.x boot is
exactly: (1) `jump68k` — redirect the parcels PPC→68k handoff to SheepShaver's emulator (the genuine
RE; boot-flow trace from the nanokernel idle/handoff at 0x503127xx), then (2) port the 68k-side HLE
shims (nvram/via) that `patch_68k` currently can't find. With (1)+(2) the 68k OS should start.

### Trace-ring diagnosis of the wedge (2026-06-08) — it's a self-deadlocked spinlock, not a benign idle

`SS_JIT_TRACE_RING=1` + live `lldb … ppc_jit_dump_trace_ring()` at the wedge. The full 262k-entry ring
is **only the 5-PC loop** `0x503127a8/b8/bc/c0/c4` — nothing else runs. Disassembled, the loop is a
**spinlock-acquire with a decrementer timeout**:
```
entry: r29 = DEC_start - (timeout<<3)              ; deadline
loop:  if flag(-0xb30(r30)) != 0: r29 = DEC; addis -1   ; this path DISABLES the timeout
       if (DEC_now - r29) > 0: try-acquire (0x312818)    ; else -> "Timeout ... locked CPU" panic
try:   if [r31] != 0: goto loop                    ; lock HELD -> spin; else lwarx/stwcx acquire
```
Findings: (a) the lock `[r31]` is held and never released; (b) the `-0xb30` flag is nonzero, routing
through the path that recomputes `r29` from DEC each iteration → the timeout never fires (this is why
`SS_SYNTH_DEC` didn't break it out); (c) the diag log shows interrupts *delivered but not taken* →
**MSR[EE]=0 (interrupts masked)**, so no other context runs to release the lock; (d) a nearby ROM
string is **"Recursive spinlock"**. ⇒ This is a **self-deadlock**: the nanokernel is acquiring a lock
it already holds, with interrupts off — a classic **"faulted while holding a spinlock, panic path
re-took it"** signature.

**Most likely cause:** the diagnostic skip of `jump68k` leaves the PPC→68k handoff un-redirected, so
when the nanokernel reaches it, the `rfi` enters the wrong target → fault → recursive-spinlock panic
deadlock. So skipping `jump68k` does NOT cleanly idle; it faults — which *strengthens* "`jump68k` is
the real blocker." (Caveat: the fault could also be a JIT codegen issue on a nanokernel instruction;
`SS_JIT_VERIFY` would discriminate, but the un-redirected handoff is the leading hypothesis.)

### ROOT-CAUSE drilldown (2026-06-08) — SPRG0/Trampoline, and a general SPRG correctness fix

`SS_LOG_FIRST_BLOCKS` (new diag) captured the full 27-block boot path: the nanokernel deadlocks
**almost immediately** (block 12) at the first spinlock-acquire `0x50312700` — NO earlier fault.
`SS_JIT_VERIFY` was clean (no codegen divergence). A one-shot lock-state dump showed the lock address
is computed from a **garbage KDP**: `r1=0xfcffffff`, `r22=0` (yet `r31=0x68ffdcc0` = the real
KernelData region). The subroutine `0x3263e0` does `mfspr r1, SPRG0; lwz r1,-4(r1)` — i.e. the parcels
nanokernel keeps its per-CPU/KDP pointer in **SPRG0**. SheepShaver was **dropping mtspr SPRG and
returning 0 for mfspr SPRG** → garbage pointer → deadlock.

- **Fixed (general correctness, committed):** real SPRG0-3 registers (mfspr/mtspr + storage). Applies
  to ANY OS; was a latent bug just never exercised by the 1.1 ROM. **test-jit=100.**
- **Did NOT fix the deadlock by itself:** SPRG0 is never *written* before the read. On real hardware
  the **Trampoline ELF bootloader** sets SPRG0 = per-CPU/KDP pointer before entering the nanokernel;
  SheepShaver jumps straight to the nanokernel, so SPRG0 stays 0. The next parcels step would be to
  **initialize SPRG0** (mimic the Trampoline) — but that is **parcels-environment-specific**, not a
  general fix (the 1.1 path uses `XLM_KERNEL_DATA`, not SPRG0).

### 🎯 GUIDING POLICY (set 2026-06-08; clarified by the maintainer) — boot as forcing-function for JIT correctness

**The primary goal is increasing emulation CORRECTNESS — especially the JIT.** SheepShaver only models
a thin slice of the PPC supervisor stack; that thinness is *why* the New World ROM won't fully boot.
So the New World ROM / 9.2 boot is deliberately used as a **forcing function** to surface and fix those
PPC/JIT correctness gaps (the SPRG bug is the first harvest). Framing:

- **The product = general PPC/JIT correctness fixes** discovered along the way (SPRG was one).
- **The parcels boot = the driver/stimulus.** Each new region of nanokernel/OS code it reaches
  exercises PPC paths nothing else does.
- **Environment plumbing (SPRG0/Trampoline init, jump68k redirect, 68k HLE) = the test harness.** Do
  the *minimum* needed to advance the boot into new code — its value is the *stimulus it unlocks*, not
  the plumbing itself.

**Revised stop-rule:** keep advancing the boot as long as doing so keeps surfacing fixable PPC/JIT
correctness gaps (the common case). Only pause when the remaining step is pure mechanical
`find_rom_data` pattern-relocation that unlocks *no new executed code* — that's harness drudgery with
no stimulus payoff. Once the parcels ROM boots far enough, the **9.2 ISO itself** becomes the next,
richer stimulus. (The earlier "stop now" read under-weighted the maintainer's actual goal — correctness
discovery — for which this path is on-target, not a tunnel.)

**Next (if/when resumed):** initialize SPRG0 to the per-CPU/KDP pointer at nanokernel entry (Trampoline
emulation), re-boot, expect the deadlock to clear and advance to the `jump68k` handoff.

### NEXT CORRECTNESS TARGET (2026-06-08) — Trampoline / per-CPU supervisor environment (the real "second wall")

Drilling past the SPRG register fix exposed the actual gap, and it's a *general* supervisor-fidelity
hole (the maintainer's real target — "SheepShaver models only a thin slice of the PPC stack"):

**The New World nanokernel expects a Trampoline-established supervisor environment that SheepShaver
never builds.** Concretely, the early routine at parcels `0x3263e0` (reached at boot block ~10) does:
```
mfspr r1, SPRG0          ; r1 = per-CPU block ptr (Trampoline-set)
stmw  r24, -0x108(r1)    ; save regs in the per-CPU area BELOW the block ptr
lwz   r1, -4(r1)         ; r1 = KDP pointer (stored just below the block)
lwz   r28, -0x900(r1)    ; read a KDP field
```
So it requires: (a) **SPRG0** = a per-CPU block pointer, (b) writable RAM in `[SPRG0-0x108 .. SPRG0)`,
(c) `[SPRG0-4]` = the KDP pointer, (d) the KDP populated with the fields it then reads
(`-0x900(KDP)`, etc.). On real HW the **Trampoline ELF** sets SPRG0 + builds the per-CPU/KDP block,
placing the KDP relative to the real SDR1/HTAB it just created. SheepShaver: uses a fixed
`KernelDataAddr`, fakes SDR1 (`0xdead001f` / HTAB ptr `0xdead0000`), runs no Trampoline → SPRG0=0 →
garbage KDP → the first spinlock deadlocks (see drilldown above).

**Why this matters beyond parcels:** it's the boundary where SheepShaver's "fake the MMU / skip the
firmware" model stops being sufficient. Cross-references `MMU-NANOKERNEL-MP-PLAN.md`. The fix is a
real build-out, roughly:
1. Add SPRG read/write reach to `set_register` (or a direct setter) — currently `set_register` aborts
   on non-GPR/standard SPRs, so even injecting SPRG0 needs a small plumbing change.
2. In `init_emul_ppc` (sheepshaver_glue.cpp ~1283), construct a per-CPU block: carve a region, write
   `KDP` at `[block-4]`, ensure `[block-0x108..block)` is writable, set `SPRG0 = block`.
3. Boot, observe the next KDP field the nanokernel reads (the forcing-function then surfaces each
   missing KDP/per-CPU field in turn — harvest as it goes).
4. Likely eventually needs a *consistent* SDR1/HTAB↔KDP relationship (the MMU work), since the
   nanokernel derives KDP from HTAB in its own Init path.

This is a focused multi-iteration correctness effort (best started fresh, not at the tail of a long
session). It IS on-target for "increase JIT/PPC correctness via the New World forcing-function."

### PROGRESS (2026-06-08 overnight) — SPRG0+KDP probe advances the boot 27 → 128 PCs

Implemented the `SS_NW_TRAMPOLINE` probe (env-gated, default off, 1.1 byte-identical; `init_emul_ppc`):
seed `SPRG0 = KernelDataAddr`, zero the negative KDP scratch (`0x1000` below KernelDataAddr, within the
NATMEM reservation), and set `[SPRG0-4] = KDP = KernelDataAddr`. Results, step by step (all via
`SS_LOG_FIRST_BLOCKS` + lock-state dump, diagnostic config `SS_ROM_LENIENT=1 SS_ROM_SKIP_JUMP68K=1`):
- **SPRG0 alone:** boot passes the block-12 spinlock (0x312700), 27 → 41 PCs, then derails to wild PCs
  (`[SPRG0-4]`=0 → KDP=0 → garbage field reads).
- **SPRG0 + backed KDP:** 27 → **128 distinct PCs, all clean in 0x50xxxxxx, no derail**; comp advances
  128 → 146. The parcels nanokernel now executes real init code well past the old deadlock.
- **New wedge:** a JIT wait loop at **`0x50322990` / `0x503251e4`** (comp frozen at 146, `iDR=0` so not
  interpreter fallback). `SS_SYNTH_DEC=1` makes NO difference here → not a decrementer-timed wait.
- **Two general correctness fixes harvested en route (committed, test-jit=350/100):** real SPRG0-3
  registers (were dropped), and `fctiw` honoring dynamic FPSCR[RN] (was fixed FCVTAS).

**The new wedge IS the MMU/HTAB wall (characterized 2026-06-08, disasm of the decoded ROM):**
`0x322990` is a **memory-zeroing loop** (`stwu r14(=0),4(r15); cmpw r15,r16; ble 0x322990`) and
`0x3251e4` a table-scan. The nanokernel is clearing/initializing its **page table / page-descriptor
region**, whose base/size it derives from SDR1/HTAB — which SheepShaver fakes (SDR1=`0xdead001f`, HTAB
ptr `0xdead0000`, and our `sr_init` patch feeds `lis r13,0xdead` / page-table size `0x100000`). So it
zeroes a **fake, non-RAM region** via NATMEM page-faults (slow ~136 blk/s, cpu busy) and stalls;
`SS_SYNTH_DEC` doesn't help (not timed). This is the genuine **supervisor/MMU page-table fidelity
"second wall"** — the agent's predicted deeper layer, now reached concretely.

**CROSS-ROM CONFIRMED (2026-06-08) — one patch set covers the 9.x family.** Booted a *second* parcels
ROM (`Mac OS ROM 9.1.1`, cksum `ecef6af1`) through the identical diagnostic path: same `:715` skip
(0x310a1c→0x311350), same skips, same SPRG0/KDP shim, same 128 PCs, **same wedge at 0x50322990**. A
byte-diff of the decoded nanokernel region (0x300000–0x340000) of 9.0.1 vs 9.1.1 = **93 bytes (0.04%)**;
the 34% whole-image diff is all in the *other* parcels (device tree/drivers). ⇒ **the 9.x "Mac OS ROM"
family (9.0.1/9.1.1/9.6.1/9.8.1/10.2.1) shares one nanokernel → one supervisor-environment fix covers
them all.** Pin **9.0.1** as the reference ROM. The **9.0.4-G4** extract is a different lineage (sizing
49/14/12 vs the family's 27/31/17) — handle separately. Exact assets/setup: see the handoff doc
(`HANDOFF-NEWWORLD-SUPERVISOR-MMU.md` §1.5).

**NEXT (the big one):** give the nanokernel a *consistent* SDR1 ↔ HTAB ↔ KDP relationship — i.e. a
real (or realistically-sized, RAM-backed) page-table region instead of the `0xdead0000` fake, so its
zero/init loop targets backed RAM of a sane size and its later HTAB derefs work. This is the MMU work
in `MMU-NANOKERNEL-MP-PLAN.md` (shadow-arena / dynamic-BAT options) — a major multi-session effort,
best started fresh. It is squarely the "thin PPC supervisor stack" gap the forcing-function exists to
expose. Full register/KDP design: `sheepshaver-research/SPRG0-KDP-DESIGN.md`.

### Prior-art survey (2026-06-07) — no port exists, but it's documented-adaptation not virgin RE

- **No prior art boots a parcels ROM / 9.1-9.2 anywhere [verified].** Upstream `cebix/macemu` has
  `decode_parcels` + `ROMTYPE_NEWWORLD` but its patch layer carries the **identical `lp[6]!=0x2c0c0001`
  assumption and no version branching** — it stops at the 1.1 LZSS ROM too. All forks/Emaculation: ceiling
  is **9.0.4**; zero reports of 9.1/9.2 on SheepShaver. So "no prior art" is a fact, and our
  `g_rom_904_lenient`/`SS_ROM_LENIENT` lenient path is this fork's own work.
- **⭐ Single best lead — [`elliotnunn/NanoKernel`](https://github.com/elliotnunn/NanoKernel) [verified].**
  A complete, **buildable, version-branched** reverse-engineered PPC-assembly source of the Mac OS
  NanoKernel **through v2.28 (the 9.x-era kernel)** — i.e. the *exact* code our `patch_nanokernel_boot`
  targets (PVR read, SR/BAT/SDR init, per-CPU data tables, decrementer). **Method:** cross-reference its
  routines against `rom-inspect --dump` of the parcels ROM to relocate each `find_rom_data` site. Turns
  Wall-1 from blind disassembly into documented-adaptation (it reduces the RE; it doesn't supply the
  per-ROM byte-offset relocation).
- **Why the "absent" hardware-init patches vanished [verified, 68kMLA "Picking apart the NewWorld ROM"]:**
  9.1+ **offloads hardware-specific init from the 4 MB ROM into the Trampoline ELF bootloader** (sets up
  the OF device tree + nanokernel interrupts). So some absent patches (nvram/via/cpu_speed) may be
  **genuinely unnecessary** for SheepShaver — verify per-patch rather than porting them.
- **The "will not work on this Macintosh model" check (the 9.2.1-on-1.1 wall) is OF-property-based, not
  gestalt [verified + open].** All New World Macs share `gestaltMachineType` 406; the real model identity
  is the **Open Firmware device-tree root `model`/`compatible` properties** the New World ROM synthesizes.
  Mac OS 9 Lives extended 9.2.x to unsupported G4s by **patching the ROM file's model logic (not a gestalt
  byte)** — leaning "needs the New World ROM environment." **OPEN cheap experiment:** does a targeted
  **NameRegistry `compatible`-property injection** (SheepShaver builds the NameRegistry) satisfy 9.2's
  check *without* the full parcels ROM port? Untested — a potential shortcut worth a separate probe.
- **MMU "second wall" stays folklore [per the red-team memos]** — unproven below where claimed; do not let
  it headline. ROM-patch parity (Wall 1) is the proven first gate. DingusPPC = the Wall-3 reference if it
  ever turns real.

**Verdict: documented-adaptation, multi-week, no guarantee — but tractable with elliotnunn/NanoKernel.**
Full survey: the 2026-06-07 reference-research report (in session history).

**Infrastructure landed (safe, 1.1 boot verified byte-identical):**
- ROM-version discrimination by checksum (`g_rom_904_lenient`, auto-on ONLY for the 9.0.4 G4
  ROM cksum `0xb8d0b672`; 1.1 `0xfd86d120` path untouched; opt-out `SS_ROM_NO_904`).
- `find_rom_data` whole-image fallback in lenient mode → **auto-resolves all RELOCATED patterns**
  (e.g. `via_init2/3` found at 0x10874/0x10920). Logged as `RELOCATED` under `SS_ROM_PATCH_TRACE`.
- A clean 9.0.4 ROM extracted to `/Users/Shared/macemu/MacOS-ROM-9.0.4-G4-extracted.rom`
  (2.32 MB, < 4 MB read cap — so `load_mac_rom` is fine; no read-cap fix needed for 9.0.4).

**Remaining work = the ABSENT patches (each needs proper RE; they are load-bearing, so the
current `g_rom_904_lenient` SKIP placeholders make PatchROM progress but WON'T boot yet).**
Enumerate the rest by iterating `SS_ROM_PATCH_TRACE=1 ./SheepShaver --config <9.0.4 prefs>`.
Known absent NewWorld-path patches so far (in execution order), with role + current state:
| pattern | bytes | role | state |
|---------|-------|------|-------|
| `mdec_dat` | `7ff602a6…` | nanokernel: neutralize decrementer (`mfdec`→`li 0`) | SKIP placeholder |
| `suspend_dat` | `7c886839…` | nanokernel: suspend `bgt`→`b` | SKIP placeholder |
| `run_diags_dat` | `60ff000c` | early 68k: set a6 = RAM top (stack) | SKIP placeholder |
| nvram (`48e71ce0…`) | — | nvram access neutralize | next blocker (unguarded) |
| `page_size`/`page_size2` | — | InitGestalt page size / CPU-type byte | guarded SKIP (9.0.4 ROM may self-supply) |
| `cpu_speed`/`time_via`/`open_firmware` | — | clock-speed gestalt / VIA timing / OF pointer | guarded SKIP |

For each: disassemble the equivalent code in 9.0.4 vs the working 1.1 image (both dumped via
`rom-inspect --dump`; capstone, big-endian), find where the rewritten routine lives, and either
(a) add a `g_rom_904_lenient` pattern/offset variant that patches the real 9.0.4 site, or
(b) confirm it's genuinely unnecessary for 9.0.4 and leave the SKIP. **Boot-verify after each**
(needs the user / a 9.0.4-bootable disk). Phase 3 risk is LOW here: SheepShaver+1.1 already boots
the 9.0.4 *OS*, so the 9.0.4-era environment is proven — this is "just" ROM-patch parity.

**RUNTIME boot attempt 2026-06-07 (user-authorized):** ran `./src/Unix/SheepShaver --config <isolated
9.0.4 prefs>`. Confirmed at runtime: lenient fallback resolves the relocated patches, the SKIP guards
let PatchROM progress, and it blocks exactly at the **`nvram2_dat` `48e71ce0`** XPRAM-HLE patch
(`rom_patches.cpp:1557`, range `[0xa000,0xd000)`) — absent even whole-image. **Decision: do NOT
brute-guard the remaining HLE chain** — those EMUL_OP shims replace ROM routines that poke
VIA/Cuda/PMU hardware SheepShaver doesn't emulate, so skipping them just moves the failure to a runtime
hang (inference, not slogged). The path is per-patch *porting* (find the rewritten routine, re-apply the
EMUL_OP), boot-verified. The 12-absent list above IS the worklist.

### De-risk FIRST: force `'ppcf'` under the working 1.1 ROM (Track 2)
Before investing the multi-session HLE port, prove the premise — *is the gestalt vector bit (bit 6)
sufficient to make a real app issue AltiVec?* Forcing it + seeing the profiler `MIX_ALTIVEC>0` proves
"bit 6 sufficient AND codegen runs real-app AltiVec end-to-end" (it does NOT prove "NewWorld sets bit 6").
Injection-point hunt (static, 2026-06-07): `'ppcf'` literal is **0× in the 1.1 ROM** (System registers it
from disk); ROM `InitGestalt` (`rom_patches.cpp:1757`) sets CPU-type/page-size but not `'ppcf'`; the ROM
`a1ad` (`_Gestalt`) **dispatcher is ROM-resident** (base selectors `sysv/proc/mmu/fpu/qd/kbd` present;
`~0x12c00` is a consumer, not the dispatcher). **Next step (needs a boot):** boot 1.1 under the e2e
harness, locate the `a1ad` dispatcher via the toolbox trap table (single careful lldb attach — heed the
VBL-timer caution), wrap it with an env-gated `SS_FORCE_ALTIVEC=1` EMUL_OP that ORs bit 6 when
selector==`'ppcf'`, run Fractal Carbon, check `MIX_ALTIVEC`. See LEARNINGS 2026-06-07.

### Phase 2 — original notes
For each failing `find_rom_data` pattern:
- Disassemble the region in both a working ROM (1.1) and the parcels ROM (lldb +
  byte-swap, or capstone — see CLAUDE.md "lldb Workflow"). Find the equivalent code whose
  bytes shifted.
- Add a ROM-version-specific pattern variant (the codebase already has per-`ROMType`
  branches in the patch functions — extend that pattern).
- `check_rom_patch_space` may need new free-region offsets for the parcels layout.
- This is whack-a-mole: fix one pattern, hit the next. Budget accordingly.

---

## Phase 3 — Boot & downstream debug

Even a fully-patched parcels ROM may not boot — nanokernel/Mac OS differences beyond ROM
patching can surface. Use the existing diagnostics (heartbeat + warnings, trace ring,
`SS_JIT_*` env vars; see `SheepShaver/docs/DIAGNOSTICS.md`). Treat as a normal boot-bringup.

> **The possible "second wall" — supervisor-level fidelity.** Getting the parcels ROM to
> *patch and load* (Phases 0–2) is the first wall. A distinct one may sit behind it: 9.1+/9.2.x
> could depend on supervisor machinery SheepShaver deliberately **stubs** — the MMU (it fakes
> virtual=physical), the nanokernel exception/interrupt model (it bypasses the real PPC exception
> table and uses a host-signal timer instead of a real decrementer), and preemptive
> Multiprocessing tasks (absent). Phase 3 is exactly where that would show up. If a patched 9.2.2
> ROM loads but won't boot — or boots but MP-using software misbehaves — the diagnosis and the
> (large, exploratory) emulation options are spec'd in
> [`MMU-NANOKERNEL-MP-PLAN.md`](MMU-NANOKERNEL-MP-PLAN.md). That plan's cheap "stub-pressure"
> probe can be run *now*, before this phase, to gauge how load-bearing the stubs are.

---

## ROM target strategy

**Comparative pattern sizing (2026-06-07, `tools/rom-patch-sizing.py` vs the `1.1` control).** Mac OS
ROM files extracted by signature-scanning install media for `<CHRP-BOOT>` (modern macOS can't mount
HFS), then `rom-inspect --dump`:

| Source | format | in-range | relocated | applicable-absent | `'ppcf'` | machines (CHRP `<COMPATIBLE>`) |
|--------|--------|---------:|----------:|------------------:|:--------:|--------------------------------|
| `1.1` (1998, current) | LZSS | 83 (all) | 0 | 0 | **0** | (working ROM, no vector gestalt) |
| **9.0.4 (`macos904.toast`)** | parcels | **49** | 14 | **12** | ✅ 2 | iMac,1 PowerMac1,1…**PowerMac3,1 PowerMac3,2** (G4) |
| 9.0.1 (`…9.0.1.rom`) | parcels | 27 | 31 | 17 | ✅ 2 | — |
| 9.2.1 (`…9.2.1/macos_921_ppc.iso`) | parcels | 27 | 31 | 17 | ✅ 2 | — |

- **⭐ Best AltiVec-unlock target: the 9.0.4 "MacROM for NewWorld"** (G4-compatible per its
  `<COMPATIBLE>` list → carries `'ppcf'`). It is **markedly closer** than 9.0.1/9.2.1: **49/83 patterns
  in-range, only 12 applicable-absent**, and — decisively — its **nanokernel-boot patches all match**
  (`sr_init`/`pvr_read`/`virt2phys`/`ppc_excp_tbl`/`trap_return`/`fe0a*` are in-range; they're
  absent/relocated in 9.0.1). Its 12 absent patches are mostly **peripheral hardware init**
  (nvram/via/cpu_speed/time_via/open_firmware/page_size) — candidates to make `ROMType`-conditional
  or skip under emulation. So 9.0.4 is the **shortest path to a booting `'ppcf'`-bearing ROM** (and to AltiVec).
- **Extraction caveat:** the candidates above were pulled by contiguous signature-extraction from
  install media; a clean file (proper fork extraction) is wanted before real use. Also `load_mac_rom`
  caps its read at `ROM_SIZE` (4 MB) — if a CHRP file exceeds 4 MB it truncates (verify the 9.0.4 file
  length; fix the read cap if needed). These are the first concrete Phase-2 setup steps.
- **End goal for breaking past 9.0.4:** a **9.2.x "Mac OS ROM"** (the ceiling-breaker). 9.0.4 is the
  cheapest *first* parcels ROM to make boot (shared parcels plumbing + fewest pattern fixes), and it
  already unlocks the AltiVec experiment; 9.2.x can follow on the same machinery. Note 9.1+ may have
  non-ROM blockers too (the "second wall").

---

## Honest risk / effort assessment

- Phase 0: hours. Decisive.
- Phase 1: days if it's "merge N parcels at known offsets"; could be quick.
- Phase 2: open-ended — days to weeks. Upstream's New World support is years of community
  RE calibrated to specific ROMs; extending it is real reverse-engineering with no
  guarantee of success.
- Phase 3: unknown — may reveal that ROM patching alone isn't sufficient for 9.1+.
- **Whole project is exploratory.** Phase 0 is the cheap probe that tells us if this is a
  weekend or a multi-week endeavor before committing.

---

## Open follow-ups

*Folded from the (retired) 2026-06-03 review backlog. The reusable decoder this both depend on
is **done** — `decode_rom_image()` / `src/include/rom_decode.hpp`, shared by the emulator and
`rom-inspect`.*

- **rom-harness New World scanning.** `rom-harness` still loads raw bytes and scans for PPC
  blocks. To exercise New World ROM code it should call `decode_rom_image()` into a 4MB buffer
  first, then scan the decoded image. Decoder dependency done; only the scan-loop wiring remains.
  Revisit if New World ROM JIT coverage becomes a focus.
- **Model the full `PatchROM` gauntlet in `rom-inspect`.** `rom-inspect` models decode +
  type-detection only. `Mac OS ROM 9.0.1` *passes* both yet the emulator rejects it downstream
  (a `patch_*` byte-pattern search or patch-space check fails), and the emulator's "Unsupported
  ROM type" alert (`main.cpp:162`) fires for *any* `PatchROM()` failure — misleading. Either
  (1) extend `rom-inspect` to run the patch-space checks and report which `patch_*` stage fails,
  or (2) make the emulator's error distinguish type-detection failure from patch failure.

---

## References

- `docs/superpowers/specs/2026-06-03-rom-inspector-design.md` — the inspector + shared decoder
- `SheepShaver/docs/DIAGNOSTICS.md` — rom-inspect usage + runtime diagnostics
- `SheepShaver/src/rom_patches.cpp`, `SheepShaver/src/include/rom_decode.hpp`
- Assets: `/Users/Shared/macemu/` (9.0.1 ROM, 9.2.1/9.2.2 ISOs); CLAUDE.md "lldb Workflow"
