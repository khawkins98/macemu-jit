from pathlib import Path

from sse2e import disk


def test_render_prefs_substitutes_fields(tmp_path):
    out = tmp_path / "test.prefs"
    disk.render_prefs(
        template=Path("config/test.prefs.template"),
        out_path=out,
        rom="/roms/x.rom",
        disk="/tmp/run.dsk",
        vncport=5950,
    )
    text = out.read_text()
    assert "rom /roms/x.rom" in text
    assert "disk /tmp/run.dsk" in text
    assert "vncport 5950" in text
    assert "idlewait true" in text


def test_render_iso_prefs_substitutes_cdrom(tmp_path):
    out = tmp_path / "iso.prefs"
    disk.render_prefs(
        template=Path("config/test.prefs.iso.template"),
        out_path=out,
        rom="/roms/x.rom",
        cdrom="/isos/system.iso",
        vncport=5950,
    )
    text = out.read_text()
    assert "cdrom /isos/system.iso" in text
    assert "bootdriver -62" in text       # CD boot
    assert "nocdrom false" in text
    assert "idlewait true" in text
    assert "\ndisk " not in text          # ISO-only, no hard disk


def test_pristine_copy_makes_independent_file(tmp_path):
    master = tmp_path / "master.dsk"
    master.write_bytes(b"PRISTINE")
    run_copy = disk.copy_pristine(master, tmp_path / "work")
    assert run_copy.read_bytes() == b"PRISTINE"
    run_copy.write_bytes(b"DIRTIED")
    assert master.read_bytes() == b"PRISTINE"  # master untouched
