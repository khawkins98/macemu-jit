from pathlib import Path

from sse2e import observe

FIX = Path(__file__).parent / "fixtures"


def test_parse_boot_ready_finder():
    line = "[BOOT] idle frontApp='Finder' modal=0 ticks=812 (13.5s)"
    ev = observe.parse_boot_ready(line)
    assert ev == observe.BootReady(front_app="Finder", modal=False, secs=13.5)


def test_parse_boot_ready_ignores_non_matching():
    assert observe.parse_boot_ready("[HB 14s] blocks=620M") is None


def test_boot_ready_blocked_on_dialog():
    line = "[BOOT] idle frontApp='' modal=1 ticks=300 (5.0s)"
    ev = observe.parse_boot_ready(line)
    assert ev is not None
    assert observe.is_desktop_ready(ev) is False


def test_boot_ready_desktop_ok():
    ev = observe.BootReady(front_app="Finder", modal=False, secs=13.5)
    assert observe.is_desktop_ready(ev) is True


def test_clean_exit_detected_in_log():
    text = (FIX / "clean_boot_shutdown.log").read_text()
    assert observe.saw_clean_shutdown(text) is True


def test_clean_exit_absent_when_only_boot():
    text = (FIX / "boot_ready.log").read_text()
    assert observe.saw_clean_shutdown(text) is False
