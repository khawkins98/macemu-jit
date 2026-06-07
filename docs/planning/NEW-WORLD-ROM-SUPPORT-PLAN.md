# Plan: Proper New World (parcels) ROM Support — break the 9.0.4 ceiling

> **Status:** 🟡 Phase 2 in progress (2026-06-07) — infra landed, ~6-8 absent patches need RE; target = 9.0.4 G4 ROM · **Created:** 2026-06-03 · **Updated:** 2026-06-07
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
