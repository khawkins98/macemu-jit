from sse2e import config


def test_resolve_assets_from_env(monkeypatch):
    monkeypatch.setenv("SS_E2E_ROM", "/ci/rom.rom")
    monkeypatch.setenv("SS_E2E_ISO", "/ci/system.iso")
    monkeypatch.setenv("SS_E2E_DISK", "/ci/master.dsk")
    a = config.resolve_assets()
    assert a.rom == "/ci/rom.rom"
    assert a.iso == "/ci/system.iso"
    assert a.disk == "/ci/master.dsk"


def test_resolve_prefers_in_project_assets_dir(monkeypatch, tmp_path):
    for v in ("SS_E2E_ROM", "SS_E2E_ISO", "SS_E2E_DISK"):
        monkeypatch.delenv(v, raising=False)
    monkeypatch.setattr(config, "ASSETS_DIR", tmp_path)
    (tmp_path / "rom.rom").write_text("x")
    (tmp_path / "smoke.iso").write_text("x")
    (tmp_path / "bench.dsk").write_text("x")
    a = config.resolve_assets()
    assert a.rom == str(tmp_path / "rom.rom")
    assert a.iso == str(tmp_path / "smoke.iso")
    assert a.disk == str(tmp_path / "bench.dsk")


def test_resolve_glob_fallback_when_no_conventional_name(monkeypatch, tmp_path):
    for v in ("SS_E2E_ROM", "SS_E2E_ISO", "SS_E2E_DISK"):
        monkeypatch.delenv(v, raising=False)
    monkeypatch.setattr(config, "ASSETS_DIR", tmp_path)
    (tmp_path / "some-oldworld-dump.rom").write_text("x")  # not rom.rom, but matches *.rom
    assert config.resolve_assets().rom == str(tmp_path / "some-oldworld-dump.rom")


def test_require_asset_raises_with_guidance(tmp_path):
    import pytest
    with pytest.raises(FileNotFoundError) as ei:
        config.require_asset(str(tmp_path / "nope.rom"), "rom")
    msg = str(ei.value)
    assert "not found" in msg and "README.md" in msg and "SS_E2E_ROM" in msg


def test_require_asset_ok_when_present(tmp_path):
    p = tmp_path / "rom.rom"
    p.write_text("x")
    config.require_asset(str(p), "rom")  # must not raise


# ---------------------------------------------------------------------------
# appsdisk — optional attached apps disk
# ---------------------------------------------------------------------------

def test_appsdisk_absent_by_default(monkeypatch, tmp_path):
    """No SS_E2E_APPSDISK, no assets/apps.dsk -> appsdisk resolves to "" (clean "no apps disk")."""
    monkeypatch.delenv("SS_E2E_APPSDISK", raising=False)
    # Point ASSETS_DIR at an empty tmp dir so no apps.dsk can be found by glob either.
    monkeypatch.setattr(config, "ASSETS_DIR", tmp_path)
    assets = config.resolve_assets()
    assert assets.appsdisk == "" or not assets.appsdisk  # empty when unconfigured


def test_appsdisk_env_override(monkeypatch, tmp_path):
    """SS_E2E_APPSDISK env var is returned verbatim (path need not exist — CI may pre-create it)."""
    p = tmp_path / "apps.dsk"
    p.write_text("x")
    monkeypatch.setenv("SS_E2E_APPSDISK", str(p))
    assert config.resolve_assets().appsdisk == str(p)


def test_appsdisk_in_project_assets_dir(monkeypatch, tmp_path):
    """assets/apps.dsk found in project assets dir -> resolved to that path."""
    monkeypatch.delenv("SS_E2E_APPSDISK", raising=False)
    monkeypatch.setattr(config, "ASSETS_DIR", tmp_path)
    p = tmp_path / "apps.dsk"
    p.write_text("x")
    assert config.resolve_assets().appsdisk == str(p)
