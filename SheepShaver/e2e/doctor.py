#!/usr/bin/env python3
"""Guided E2E setup / health check — `make e2e-setup`.

Verifies everything a run needs (emulator binary + its Homebrew libs, the Python venv, and the three
assets) and prints a ✓/✗ report with the exact command to fix each gap. When run in a terminal it
also OFFERS to symlink ROM/ISO/disk files it finds (e.g. under /Users/Shared/macemu or ~/Downloads)
into the in-project `assets/` dir — so you don't have to hand-place them. Safe in CI: with no TTY it
just reports. Exits 0 only when everything is ready.
"""
from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
EMULATOR = HERE.parent / "src" / "Unix" / "SheepShaver"

sys.path.insert(0, str(HERE))
from sse2e import bench_export, config, runner  # noqa: E402

OK, BAD = "[\033[32m✓\033[0m]", "[\033[31m✗\033[0m]"
if not sys.stdout.isatty():
    OK, BAD = "[OK]", "[ -]"


def _line(ok: bool, label: str, detail: str) -> None:
    print(f"  {OK if ok else BAD} {label:18} {detail}")


def check_venv() -> bool:
    ok = (HERE / ".venv" / "bin" / "python").exists()
    _line(ok, "Python venv", ".venv ready" if ok else "missing — run `make e2e-setup` (auto-creates it)")
    return ok


def check_binary() -> bool:
    ok = EMULATOR.exists()
    if ok:
        _line(True, "Emulator binary", runner.binary_build_info(str(EMULATOR)).replace("emulator: ", ""))
    elif not runner.is_configured(str(EMULATOR)):
        # `make build-ss` FAILS on an un-configured tree — point at the one-time configure step.
        _line(False, "Emulator binary", "not built and the tree is NOT configured — run the one-time "
              "configure first (see e2e README 'Building the emulator'), then `make build-ss`")
    else:
        _line(False, "Emulator binary", "not built — run `make build-ss`")
    return ok


def check_gui_session() -> None:
    """The emulator opens a real SDL window; without an Aqua (GUI) session a run hangs/fails. Optional
    (informational) — not part of the readiness gate, since the check is conservative."""
    if runner.gui_session_ok():
        _line(True, "GUI session", "Aqua session present (the SDL window can open)")
    else:
        _line(False, "GUI session", "no Aqua session — `make e2e`/`e2e-bench` need a logged-in desktop, "
              "not a bare SSH session (Prerequisite 1)")


def check_brew_libs() -> bool:
    """The binary dynamically links Homebrew dylibs; verify they exist on disk (it won't run without
    them). If the binary isn't built yet, just name the brew formulae to install."""
    if not EMULATOR.exists():
        _line(False, "Homebrew libs", "build the binary first; then this checks sdl3 / vde / libvncserver")
        return False
    try:
        out = subprocess.run(["otool", "-L", str(EMULATOR)], capture_output=True, text=True).stdout
    except FileNotFoundError:
        _line(True, "Homebrew libs", "(otool unavailable — skipped)")
        return True
    missing = [ln.split()[0] for ln in out.splitlines()
               if "/opt/homebrew" in ln and not Path(ln.split()[0]).exists()]
    if missing:
        _line(False, "Homebrew libs", f"missing: {missing} — `brew install sdl3 vde libvncserver`")
        return False
    _line(True, "Homebrew libs", "sdl3, vde, libvncserver present")
    return True


def check_hfsutils() -> None:
    """Optional: hfsutils enables benchmark-history export (Cmd-T report extraction). Its absence
    does NOT block readiness — the smoke and benchmark still run, just without saved history."""
    if bench_export.hfsutils_available():
        _line(True, "hfsutils (opt)", "present — benchmark history export enabled")
    else:
        _line(False, "hfsutils (opt)", "missing — benchmark history export disabled. `brew install hfsutils`")


def _scan_candidates(glob: str) -> list[Path]:
    """Find asset files matching `glob` (e.g. *.iso) in the usual places."""
    dirs = [Path("/Users/Shared/macemu"), Path.home() / "Downloads", Path.home() / "Documents"]
    found: list[Path] = []
    for d in dirs:
        if d.is_dir():
            found += sorted(d.glob(glob))
    # de-dup, keep order
    seen, uniq = set(), []
    for f in found:
        if f not in seen:
            seen.add(f); uniq.append(f)
    return uniq


def check_assets() -> bool:
    """Resolve each asset; if missing, offer to symlink a discovered candidate into assets/."""
    all_ok = True
    for kind, (env, name, glob, _legacy) in config._SPECS.items():
        path = config._resolve_one(kind)
        if Path(path).exists():
            shown = path if not Path(path).is_symlink() else f"{path} -> {os.readlink(path)}"
            _line(True, f"{kind} asset", shown)
            continue
        all_ok = False
        _line(False, f"{kind} asset", f"not found (looked at ${env}, assets/{name}, legacy default)")
        cands = _scan_candidates(glob)
        if not cands:
            print(f"        → place one in {config.ASSETS_DIR}/{name} (symlink or copy), or set {env}.")
            print(f"          See {config.ASSETS_DIR}/README.md.")
            continue
        print("        Found candidate(s):")
        for i, c in enumerate(cands):
            print(f"          [{i}] {c}")
        if not sys.stdin.isatty():
            print(f"        → symlink one into {config.ASSETS_DIR}/{name} (non-interactive; skipping prompt).")
            continue
        ans = input(f"        Symlink which into assets/{name}? [0-{len(cands)-1}, or Enter to skip]: ").strip()
        if ans.isdigit() and int(ans) < len(cands):
            dst = config.ASSETS_DIR / name
            dst.parent.mkdir(parents=True, exist_ok=True)
            if dst.exists() or dst.is_symlink():
                dst.unlink()
            dst.symlink_to(cands[int(ans)])
            _line(True, f"{kind} asset", f"linked assets/{name} -> {cands[int(ans)]}")
            all_ok = all_ok and True
    return all_ok


def main() -> int:
    print("SheepShaver E2E — setup check\n")
    results = [check_venv(), check_binary(), check_brew_libs(), check_assets()]
    check_gui_session()  # informational (conservative check) — not part of the readiness gate
    check_hfsutils()  # optional — informational only, not part of the readiness gate
    print()
    if all(results):
        print("Ready — run `make e2e` (smoke) or `make e2e-bench` (Speedometer benchmark).")
        return 0
    print("Not ready — address the [✗] items above, then re-run `make e2e-setup`.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
