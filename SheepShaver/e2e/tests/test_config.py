from sse2e import config


def test_resolve_assets_from_env(monkeypatch):
    monkeypatch.setenv("SS_E2E_ROM", "/ci/rom.rom")
    monkeypatch.setenv("SS_E2E_ISO", "/ci/system.iso")
    monkeypatch.setenv("SS_E2E_DISK", "/ci/master.dsk")
    a = config.resolve_assets()
    assert a.rom == "/ci/rom.rom"
    assert a.iso == "/ci/system.iso"
    assert a.disk == "/ci/master.dsk"


def test_resolve_assets_falls_back_to_defaults(monkeypatch):
    for v in ("SS_E2E_ROM", "SS_E2E_ISO", "SS_E2E_DISK"):
        monkeypatch.delenv(v, raising=False)
    a = config.resolve_assets()
    # Local-dev defaults under the shared asset dir.
    assert a.rom == config.DEFAULT_ROM
    assert a.iso == config.DEFAULT_ISO
    assert a.disk == config.DEFAULT_DISK
