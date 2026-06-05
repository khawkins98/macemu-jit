# E2E assets

The harness needs three large, **non-redistributable** files (a Mac ROM + boot media). They are
**not** in git — drop them here (a **symlink or a copy** both work) and the harness finds them
automatically. This dir + this README are tracked so the project documents what it needs; everything
else here is gitignored.

| File (place here) | What it is | Used by | Default size |
|---|---|---|---|
| `rom.rom`   | An **OldWorld PowerPC Mac ROM** (e.g. a "Mac OS ROM 1.1" dump). Boots the emulated Mac. | both | ~2 MB |
| `smoke.iso` | A **read-only bootable Mac OS 8.x CD** image — boots to a Finder desktop. Read-only ⇒ can't get dirty ⇒ reproducible; this is the smoke medium. | `make e2e` | ~600 MB |
| `bench.dsk` | A **writable Mac OS 9 disk image with Speedometer 4.02** installed (small/stripped). Copied per-run (APFS clonefile) so the master stays pristine. | `make e2e-bench` | ~1 GB |

## How the harness finds an asset (resolution order)

1. The env var, if set — `SS_E2E_ROM` / `SS_E2E_ISO` / `SS_E2E_DISK` (CI / overrides).
2. **This directory** — `rom.rom` / `smoke.iso` / `bench.dsk` (a symlink to the real file is fine),
   or, failing that, the first `*.rom` / `*.iso` / `*.dsk` found here.
3. A legacy shared default under `/Users/Shared/macemu/` (back-compat for existing setups).

If none are found, `make e2e` / `make e2e-bench` fails fast with a message pointing back at this file.

## Quick setup (symlink your existing files)

If you already have the assets elsewhere (e.g. `/Users/Shared/macemu/`), symlink them in — no copy,
no duplication:

```bash
cd SheepShaver/e2e/assets
ln -sf "/Users/Shared/macemu/1998-07-21 - Mac OS ROM 1.1.rom" rom.rom
ln -sf "/Users/Shared/macemu/Mac OS 8.6 Internal Edition.iso" smoke.iso
ln -sf "/Users/Shared/macemu/macos9_mini.dsk"                 bench.dsk
```

Or just copy the files in with those names. (Both are gitignored.)

## Where to get them

These are not distributed with the project (ROM is Apple-copyrighted; the disk images are built
locally). See `SheepShaver/docs/USER-HANDBOOK.md` and the repo `CLAUDE.md` "Asset Locations" for how
the project's own images were produced, and the SheepShaver community for ROM/CD sourcing.
