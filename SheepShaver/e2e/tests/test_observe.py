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


def test_is_app_dialog_true_on_modal():
    assert observe.is_app_dialog("[APP] frontApp='Speedometer 4.02' modal=1 ticks=9001") is True


def test_is_app_dialog_false_when_no_modal():
    assert observe.is_app_dialog("[APP] frontApp='Finder' modal=0 ticks=900") is False


def test_is_app_dialog_false_on_non_app_line():
    # A [BOOT] line can also carry modal=1, but it's not an [APP] dialog signal.
    assert observe.is_app_dialog("[BOOT] idle frontApp='' modal=1 ticks=300 (5.0s)") is False


def test_parse_app_full_signal():
    ev = observe.parse_app(
        "[APP] frontApp='Speedometer 4.02' modal=1 win=0x1edcf8c0 title='All Done!' ticks=5245"
    )
    assert ev is not None
    assert ev.app == "Speedometer 4.02" and ev.modal is True
    assert ev.win == "0x1edcf8c0" and ev.title == "All Done!" and ev.ticks == 5245


def test_parse_app_empty_title_nonmodal():
    ev = observe.parse_app("[APP] frontApp='Finder' modal=0 win=0x10b24110 title='Desktop' ticks=369")
    assert ev is not None and ev.app == "Finder" and ev.modal is False and ev.title == "Desktop"


def test_parse_app_back_compat_no_win_title():
    # Older signal form (no win=/title=) must still parse.
    ev = observe.parse_app("[APP] frontApp='Finder' modal=0 ticks=900")
    assert ev is not None and ev.app == "Finder" and ev.win == "" and ev.title == "" and ev.ticks == 900


def test_parse_app_ignores_boot_line():
    assert observe.parse_app("[BOOT] idle frontApp='Finder' modal=0 ticks=369 (6.2s)") is None


def test_parse_boot_ready_tolerates_win_title_fields():
    line = "[BOOT] idle frontApp='Finder' modal=0 win=0x10b24110 title='Desktop' ticks=369 (6.2s)"
    ev = observe.parse_boot_ready(line)
    assert ev is not None and ev.front_app == "Finder" and ev.modal is False and ev.secs == 6.2
