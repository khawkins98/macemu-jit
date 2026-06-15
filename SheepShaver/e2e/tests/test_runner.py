import sys
import time

from sse2e import runner as runner_mod
from sse2e.runner import Runner


def test_binary_build_info_missing(tmp_path):
    msg = runner_mod.binary_build_info(str(tmp_path / "SheepShaver-nope"))
    assert "not found" in msg and "make build-ss" in msg


def test_binary_build_info_present_reports_build_time(tmp_path):
    p = tmp_path / "SheepShaver"
    p.write_text("x")  # stand-in binary; its mtime is "now"
    msg = runner_mod.binary_build_info(str(p))
    assert msg.startswith("emulator: built ") and "ago)" in msg
    # No real source tree next to a tmp file -> no false staleness warning.
    assert "WARNING" not in msg


def test_runner_captures_stderr_and_exit_code():
    # A tiny python program stands in for the emulator.
    prog = "import sys; sys.stderr.write('hello\\n'); sys.stderr.flush(); sys.exit(0)"
    r = Runner(argv=[sys.executable, "-c", prog])
    r.start()
    code = r.wait(timeout=10)
    assert code == 0
    assert "hello" in r.log_text()


def test_runner_request_shutdown_sends_sigusr1():
    # Stand-in for the emulator: exit 0 when SIGUSR1 arrives (the harness's clean-shutdown signal).
    prog = (
        "import signal, sys, time\n"
        "signal.signal(signal.SIGUSR1, lambda *a: sys.exit(0))\n"
        "time.sleep(30)\n"
    )
    r = Runner(argv=[sys.executable, "-c", prog])
    r.start()
    time.sleep(0.5)  # let the handler install
    r.request_shutdown()
    assert r.wait(timeout=5) == 0


def test_runner_force_kill_on_timeout_is_failure():
    prog = "import time; time.sleep(30)"
    r = Runner(argv=[sys.executable, "-c", prog])
    r.start()
    code = r.wait(timeout=1)  # should time out
    assert code is None  # None == timed out / had to be killed
    r.terminate()
    assert r.was_killed is True
