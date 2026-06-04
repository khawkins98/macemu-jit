"""Resolve system-specific asset paths (ROM, disk) for the harness.

The *prefs config* is checked into the repo (config/test.prefs.template). The *asset
paths* are system-specific and large, so they are resolved here from environment
variables (CI sets them) with local-dev defaults. This keeps the harness portable: CI
points SS_E2E_ROM / SS_E2E_DISK at its own assets without editing any tracked file.
"""
from __future__ import annotations

import os
from dataclasses import dataclass

# Local-dev defaults (the shared asset dir documented in CLAUDE.md). CI overrides via env.
DEFAULT_ROM = "/Users/Shared/macemu/1998-07-21 - Mac OS ROM 1.1.rom"
DEFAULT_DISK = "/Users/Shared/macemu/e2e_master.dsk"


@dataclass(frozen=True)
class Assets:
    rom: str
    disk: str


def resolve_assets() -> Assets:
    """ROM/disk paths from env (SS_E2E_ROM / SS_E2E_DISK) or local-dev defaults."""
    return Assets(
        rom=os.environ.get("SS_E2E_ROM", DEFAULT_ROM),
        disk=os.environ.get("SS_E2E_DISK", DEFAULT_DISK),
    )
