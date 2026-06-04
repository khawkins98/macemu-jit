# Plan: Proper New World (parcels) ROM Support — break the 9.0.4 ceiling

**Status:** Not started — pick-up-later plan.
**Author context:** drafted 2026-06-03 after getting Mac OS 9.0.4 booting via the 1.1 ROM
and building the `rom-inspect` tool.

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

## Phase 0 — Diagnostic (do this first; cheap and decisive)

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
durable tool (and is already noted as a P3.5 follow-up in
`docs/planning/REVIEW-RECOMMENDATIONS-2026-06-03.md`).

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

## Phase 2 — Patch adaptation (if Phase 0 points here, or after Phase 1)

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

---

## ROM target strategy

- **Phase 0 specimen:** the `Mac OS ROM 9.0.1` parcels ROM already on disk — simplest
  failing parcels ROM, closest to working. Diagnose with this.
- **End goal for breaking past 9.0.4:** a **9.2.x "Mac OS ROM"** file, extractable from the
  9.2.1 / 9.2.2 install media in `/Users/Shared/macemu/` (the `Mac OS ROM` file inside the
  installer's System Folder). Getting 9.0.1 to patch is the shared prerequisite; the 9.2
  ROM is the actual ceiling-breaker. Note 9.1+ may have non-ROM blockers too.

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

## References

- `docs/superpowers/specs/2026-06-03-rom-inspector-design.md` — the inspector + shared decoder
- `docs/planning/REVIEW-RECOMMENDATIONS-2026-06-03.md` — P3.5 (rom-harness New World scanning; full
  PatchROM-gauntlet modeling in rom-inspect)
- `SheepShaver/docs/DIAGNOSTICS.md` — rom-inspect usage + runtime diagnostics
- `SheepShaver/src/rom_patches.cpp`, `SheepShaver/src/include/rom_decode.hpp`
- Assets: `/Users/Shared/macemu/` (9.0.1 ROM, 9.2.1/9.2.2 ISOs); CLAUDE.md "lldb Workflow"
