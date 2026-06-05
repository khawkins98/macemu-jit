"""Thin wrapper over vncdotool's synchronous API."""
from __future__ import annotations

import time

from vncdotool import api


class Vnc:
    def __init__(self, host: str = "127.0.0.1", port: int = 5950):
        # vncdotool address form is "host::port" (note the double colon).
        self._client = api.connect(f"{host}::{port}")

    def click(self, x: int, y: int) -> None:
        # HOLD the button: move -> settle -> down -> brief hold -> up. vncdotool's mousePress
        # fires button-down and -up with no gap; the guest drains both in a single ADB poll and
        # never sees the button held, so no click registers (the cursor moves but nothing is
        # selected). Holding ~0.2s spans several 60Hz ADB interrupts -> a real click. Verified by
        # a held click visibly opening the Apple menu over VNC. See LEARNINGS (2026-06-05).
        self._client.mouseMove(x, y)
        time.sleep(0.2)
        self._client.mouseDown(1)
        time.sleep(0.2)
        self._client.mouseUp(1)

    def key(self, name: str) -> None:
        self._client.keyPress(name)

    def type_text(self, text: str) -> None:
        """Send each character as a discrete key press (alphanumeric names map 1:1)."""
        for ch in text:
            self._client.keyPress(ch)

    def capture(self, path: str) -> None:
        self._client.captureScreen(path)

    def close(self) -> None:
        self._client.disconnect()
        # Stop vncdotool's Twisted reactor — without this the non-daemon reactor thread keeps the
        # process alive after disconnect (every capture/drive command would hang until timeout).
        api.shutdown()
