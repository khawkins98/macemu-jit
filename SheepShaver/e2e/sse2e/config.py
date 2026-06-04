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
# Read-only ISO is the PREFERRED boot medium (can't get dirty -> stable, reproducible, no
# pristine-copy-per-run). The disk default is kept for disk-boot scenarios (e.g. P2 app-launch).
DEFAULT_ISO = "/Users/Shared/macemu/Mac OS 8.6 Internal Edition.iso"
# Small purpose-built test/benchmark disk: stripped Mac OS 9.0.4 + Speedometer (~142 MB sparse).
# Writable, so it's used via copy-per-run (instant APFS clonefile). The benchmark-automation
# scenario boots this and runs Speedometer (Cmd+A).
DEFAULT_DISK = "/Users/Shared/macemu/macos9_mini.dsk"


@dataclass(frozen=True)
class Assets:
    rom: str
    iso: str
    disk: str


def resolve_assets() -> Assets:
    """Asset paths from env (SS_E2E_ROM / SS_E2E_ISO / SS_E2E_DISK) or local-dev defaults."""
    return Assets(
        rom=os.environ.get("SS_E2E_ROM", DEFAULT_ROM),
        iso=os.environ.get("SS_E2E_ISO", DEFAULT_ISO),
        disk=os.environ.get("SS_E2E_DISK", DEFAULT_DISK),
    )
