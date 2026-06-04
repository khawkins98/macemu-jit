import sys

from sse2e.runner import Runner


def test_runner_captures_stderr_and_exit_code():
    # A tiny python program stands in for the emulator.
    prog = "import sys; sys.stderr.write('hello\\n'); sys.stderr.flush(); sys.exit(0)"
    r = Runner(argv=[sys.executable, "-c", prog])
    r.start()
    code = r.wait(timeout=10)
    assert code == 0
    assert "hello" in r.log_text()


def test_runner_force_kill_on_timeout_is_failure():
    prog = "import time; time.sleep(30)"
    r = Runner(argv=[sys.executable, "-c", prog])
    r.start()
    code = r.wait(timeout=1)  # should time out
    assert code is None  # None == timed out / had to be killed
    r.terminate()
    assert r.was_killed is True
