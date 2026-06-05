"""Resolve the harness's large, non-redistributable assets (ROM + boot media).

The *prefs config* is checked into the repo (`config/test.prefs.template`). The *assets* are large
and not redistributable, so they live in `SheepShaver/e2e/assets/` (gitignored — drop a symlink or
copy there; see `assets/README.md`). Each asset is resolved in this order:

  1. the env var (`SS_E2E_ROM` / `SS_E2E_ISO` / `SS_E2E_DISK`) — CI / explicit override;
  2. the in-project `assets/` dir — `rom.rom` / `smoke.iso` / `bench.dsk`, or the first `*.rom` /
     `*.iso` / `*.dsk` found there;
  3. a legacy shared default under `/Users/Shared/macemu/` (back-compat for existing setups).

If an asset is needed but missing everywhere, `require_asset()` raises a clear message pointing at
`assets/README.md` — so a missing file is an obvious DX error, not a cryptic "?" boot.
"""
from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path

# In-project asset dir: SheepShaver/e2e/assets/  (this file is SheepShaver/e2e/sse2e/config.py)
ASSETS_DIR = Path(__file__).resolve().parents[1] / "assets"

# kind -> (env var, conventional in-project filename, glob fallback, legacy shared default)
_SPECS = {
    "rom":  ("SS_E2E_ROM",  "rom.rom",   "*.rom",
             "/Users/Shared/macemu/1998-07-21 - Mac OS ROM 1.1.rom"),
    "iso":  ("SS_E2E_ISO",  "smoke.iso", "*.iso",
             "/Users/Shared/macemu/Mac OS 8.6 Internal Edition.iso"),
    "disk": ("SS_E2E_DISK", "bench.dsk", "*.dsk",
             "/Users/Shared/macemu/macos9_mini.dsk"),
}


@dataclass(frozen=True)
class Assets:
    rom: str
    iso: str
    disk: str


def _resolve_one(kind: str) -> str:
    env, name, glob, legacy = _SPECS[kind]
    # 1. explicit env override (CI)
    p = os.environ.get(env)
    if p:
        return p
    # 2. in-project assets/<name>, else the first matching glob
    cand = ASSETS_DIR / name
    if cand.exists():           # follows symlinks
        return str(cand)
    matches = sorted(ASSETS_DIR.glob(glob))
    if matches:
        return str(matches[0])
    # 3. legacy shared default (back-compat)
    if Path(legacy).exists():
        return legacy
    # 4. unresolved — return the conventional in-project path so the not-found error points there
    return str(cand)


def resolve_assets() -> Assets:
    """Resolve all three asset paths (env -> assets/ -> legacy default). Paths are NOT existence-
    checked here (the smoke needs only the ISO, the benchmark only the disk) — call require_asset()
    on the one(s) a scenario actually boots."""
    return Assets(rom=_resolve_one("rom"), iso=_resolve_one("iso"), disk=_resolve_one("disk"))


def require_asset(path: str, kind: str) -> None:
    """Raise a clear, DX-friendly FileNotFoundError if `path` doesn't exist, pointing at the assets
    README and the env override — so a missing asset fails fast with guidance, not a cryptic boot."""
    if Path(path).exists():
        return
    env = _SPECS[kind][0]
    raise FileNotFoundError(
        f"E2E {kind} asset not found: {path}\n"
        f"  Drop it in {ASSETS_DIR}/ (a symlink or copy is fine) or set {env}.\n"
        f"  See {ASSETS_DIR}/README.md for what this file is and where to get it."
    )
