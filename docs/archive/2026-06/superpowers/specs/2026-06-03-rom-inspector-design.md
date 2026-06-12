> **ARCHIVED 2026-06-12** — Moved to archive during the Reku pass.
> May contain outdated assumptions, resolved questions, or superseded plans.
> Current state: `docs/HANDOFF.md` · `docs/planning/ROADMAP.md`
> **Reason:** milestone shipped
>

# ROM Inspector + Reusable ROM Decoder (Approach A)

**Date:** 2026-06-03
**Status:** Approved design
**Component:** SheepShaver ROM handling + new standalone tool

## Problem

SheepShaver rejected a New World `Mac OS ROM 9.0.1` file with "unsupported ROM type."
Diagnosing why required reasoning about `DecodeROM`/`PatchROM` internals, and there's no
way to vet a candidate ROM without launching the full emulator. Separately, the ROM-decode
logic is trapped inside `rom_patches.cpp` (writing to the `ROMBaseHost` global), so neither
the test harness nor any tool can reuse it (deferred item P3.5).

## Design

### 1. Reusable decoder header — `src/include/rom_decode.hpp`

Header-only, self-contained (only `<stdint.h>` + `<arpa/inet.h>`; defines its own `FOURCC`
guard). No SheepShaver runtime dependency, so standalone tools can include it.

- `decode_lzss(src, dest, size)` — moved verbatim from `rom_patches.cpp` (`static inline`)
- `decode_parcels(src, dest, size)` — moved verbatim (`static inline`, debug prints dropped)
- `decode_rom_image(data, size, dest, dest_size) -> bool` — the generalized `DecodeROM`
  body, writing to an explicit `dest` of `dest_size` instead of the `ROMBaseHost` global
  and `ROM_SIZE`. Same logic: raw 4 MB copy, CHRP+LZSS, CHRP+parcels.
- `rom_detect_type(decoded) -> int` — the six-string nanokernel-ID check at offset
  `0x30d064`; returns `-1` (unknown) or `0..5` **matching the existing `ROMTYPE_*`
  ordering** in `rom_patches.h` (TNT=0 … NEWWORLD=5).
- `rom_type_name(int) -> const char *` — display helper ("TNT", …, "NewWorld", "UNKNOWN").

### 2. Refactor `rom_patches.cpp` (behavior-preserving)

- `#include "rom_decode.hpp"`; delete the moved function bodies.
- `DecodeROM(data, size)` → `return decode_rom_image(data, size, ROMBaseHost, ROM_SIZE);`
- `PatchROM` type detection (current lines ~690-704) →
  `ROMType = rom_detect_type(ROMBaseHost); if (ROMType < 0) return false;`
- The `ROMTYPE_*` enum stays in `rom_patches.h`; the returned int maps directly because the
  ordering is identical. No emulator behavior change.

### 3. New tool — `rom-inspect/` (mirrors `rom-harness/`)

`rom-inspect.cpp` + `Makefile`. Reads a ROM file **in full** (stat + read-all — not capped
at 4 MB the way `main_unix.cpp` reads it; that cap is a latent truncation bug for CHRP
files > 4 MB, noted but out of scope), decodes into a 4 MB buffer via `decode_rom_image`,
and reports:

- file size and detected on-disk format (raw / CHRP-LZSS / CHRP-parcels)
- decode success/failure
- the nanokernel ID bytes at decoded `0x30d064` (printable rendering)
- detected type via `rom_detect_type` + name, or "UNSUPPORTED — PatchROM would reject"

Exit code: `0` if `rom_detect_type >= 0` (PatchROM would accept), non-zero otherwise —
scriptable for vetting ROMs.

### 4. Build + docs wiring

- `SheepShaver/Makefile`: `build-rom-inspect` target + `inspect-rom ROM=<file>` convenience
  target (platform-neutral; the tool is standalone g++).
- Document in `SheepShaver/docs/DIAGNOSTICS.md` (ROM inspection section) and update the
  P3.5 backlog entry: the reusable decoder is the foundation; full rom-harness New-World
  scanning can now build on `decode_rom_image`.

## Build sequencing (emulator may be running)

The standalone inspector builds and runs without touching the live `SheepShaver` binary, so
it is built and verified immediately. The `rom_patches.cpp` refactor is committed and
**syntax-checked** now (`-fsyntax-only`, no object output), but the **full SheepShaver
rebuild + harness regression is deferred** until the user is at a stopping point. The
inspector exercises the exact `decode_rom_image`/`rom_detect_type` code the refactored
`rom_patches.cpp` calls, so a correct inspector run on real ROMs validates the shared
header before the emulator is ever relinked.

## Testing / verification

1. Build inspector; run against the two ROMs on disk:
   - `Mac OS ROM 1.1.rom` (boots 8.6 and 9.0.4) → expect **accepted**, prints its type.
   - `Mac OS ROM 9.0.1.rom` (rejected by emulator) → expect **UNSUPPORTED**, and reveals
     what is actually at `0x30d064` (the root-cause datum).
2. `rom_patches.cpp` passes `-fsyntax-only`.
3. **Deferred:** full rebuild + `make test-opcodes` 235/235 confirms behavior unchanged.

## Not doing

- Full rom-harness New-World *scanning* (decode → JIT block exercise) — still P3.5; this
  spec delivers the reusable decoder it would build on, not the scan loop itself.
- Fixing `main_unix.cpp`'s 4 MB ROM read cap — noted as latent, separate change.
