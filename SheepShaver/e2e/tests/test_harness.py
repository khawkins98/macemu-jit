"""Offline tests for the entry-point scaffolding (harness.py) + the host-check primitives."""
import subprocess

from sse2e import harness, runner


# --- runner.is_configured ------------------------------------------------------------------

def test_is_configured_detects_config_status(tmp_path):
    emu = tmp_path / "src" / "Unix" / "SheepShaver"
    emu.parent.mkdir(parents=True)
    assert runner.is_configured(str(emu)) is False      # no config.status next to the binary
    (emu.parent / "config.status").write_text("x")
    assert runner.is_configured(str(emu)) is True


# --- runner.gui_session_ok -----------------------------------------------------------------

def _fake_managername(value):
    return lambda *a, **k: subprocess.CompletedProcess(a, 0, stdout=value, stderr="")


def test_gui_session_ok_true_for_aqua(monkeypatch):
    monkeypatch.setattr(runner.subprocess, "run", _fake_managername("Aqua\n"))
    assert runner.gui_session_ok() is True


def test_gui_session_ok_false_for_non_aqua(monkeypatch):
    monkeypatch.setattr(runner.subprocess, "run", _fake_managername("Background\n"))
    assert runner.gui_session_ok() is False


def test_gui_session_ok_conservative_on_error(monkeypatch):
    def boom(*a, **k):
        raise FileNotFoundError
    monkeypatch.setattr(runner.subprocess, "run", boom)
    assert runner.gui_session_ok() is True               # never false-blocks a legit run


# --- harness.check_preconditions -----------------------------------------------------------

class _Assets:
    rom, iso, disk = "/x/rom", "/x/iso", "/x/disk"


def _stub_env(monkeypatch, tmp_path, *, gui=True, missing=()):
    """Point harness at a tmp 'binary' and stub the GUI + asset checks. `missing` = asset kinds
    whose require_asset should raise."""
    emu = tmp_path / "SheepShaver"
    emu.write_text("x")
    monkeypatch.setattr(harness, "EMULATOR", emu)
    monkeypatch.setattr(harness.runner, "gui_session_ok", lambda: gui)
    monkeypatch.setattr(harness.config, "resolve_assets", lambda: _Assets())

    def require(path, kind):
        if kind in missing:
            raise FileNotFoundError(f"no {kind}")
    monkeypatch.setattr(harness.config, "require_asset", require)


def test_check_preconditions_ok_returns_assets(monkeypatch, tmp_path):
    _stub_env(monkeypatch, tmp_path)
    assert harness.check_preconditions(need_disk=True) is not None


def test_check_preconditions_fails_without_gui(monkeypatch, tmp_path, capsys):
    _stub_env(monkeypatch, tmp_path, gui=False)
    assert harness.check_preconditions() is None
    assert "GUI" in capsys.readouterr().out


def test_check_preconditions_fails_on_missing_asset(monkeypatch, tmp_path, capsys):
    _stub_env(monkeypatch, tmp_path, missing=("disk",))
    assert harness.check_preconditions(need_disk=True) is None
    assert "no disk" in capsys.readouterr().out


def test_check_preconditions_does_not_require_unrequested_media(monkeypatch, tmp_path):
    # disk is "missing", but a smoke run that only needs the iso must still pass.
    _stub_env(monkeypatch, tmp_path, missing=("disk",))
    assert harness.check_preconditions(need_iso=True) is not None
