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


# ---------------------------------------------------------------------------
# appsdisk_line helper + render_prefs multi-disk integration
# ---------------------------------------------------------------------------

def test_appsdisk_line_helper():
    assert disk.appsdisk_line(None) == ""
    assert disk.appsdisk_line("") == ""
    assert disk.appsdisk_line("/x/apps dir/apps.dsk") == "disk /x/apps dir/apps.dsk"


def test_render_prefs_appsdisk_line(tmp_path):
    """render_prefs injects a second disk line when appsdisk_line is provided."""
    tmpl = tmp_path / "t.prefs"
    tmpl.write_text("disk {disk}\n{appsdisk_line}\nnocdrom true\n")
    out = disk.render_prefs(tmpl, tmp_path / "o.prefs", disk="/x/boot.dsk",
                            appsdisk_line="disk /x/apps.dsk")
    text = out.read_text()
    assert "disk /x/boot.dsk" in text
    assert "disk /x/apps.dsk" in text


def test_render_prefs_appsdisk_empty_leaves_no_literal(tmp_path):
    """Passing appsdisk_line='' leaves a blank line but no literal '{appsdisk_line}'."""
    tmpl = tmp_path / "t.prefs"
    tmpl.write_text("disk {disk}\n{appsdisk_line}\nnocdrom true\n")
    out = disk.render_prefs(tmpl, tmp_path / "o2.prefs", disk="/x/boot.dsk", appsdisk_line="")
    text = out.read_text()
    assert "disk /x/boot.dsk" in text
    assert "{appsdisk_line}" not in text


def test_render_prefs_unfilled_appsdisk_line_stripped(tmp_path):
    """Callers that don't pass appsdisk_line at all get the placeholder silently stripped."""
    tmpl = tmp_path / "t.prefs"
    tmpl.write_text("disk {disk}\n{appsdisk_line}\nnocdrom true\n")
    out = disk.render_prefs(tmpl, tmp_path / "o3.prefs", disk="/x/boot.dsk", vncport=5950)
    text = out.read_text()
    assert "disk /x/boot.dsk" in text
    assert "{appsdisk_line}" not in text


def test_render_prefs_actual_template_single_disk(tmp_path):
    """The real test.prefs.template works cleanly for existing single-disk callers."""
    out = disk.render_prefs(
        template=Path("config/test.prefs.template"),
        out_path=tmp_path / "run.prefs",
        rom="/roms/x.rom",
        disk="/tmp/run.dsk",
        vncport=5950,
    )
    text = out.read_text()
    assert "rom /roms/x.rom" in text
    assert "disk /tmp/run.dsk" in text
    assert "vncport 5950" in text
    assert "{appsdisk_line}" not in text
