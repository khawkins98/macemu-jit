"""Thin wrapper over vncdotool's synchronous API."""
from __future__ import annotations

from vncdotool import api


class Vnc:
    def __init__(self, host: str = "127.0.0.1", port: int = 5950):
        # vncdotool address form is "host::port" (note the double colon).
        self._client = api.connect(f"{host}::{port}")

    def click(self, x: int, y: int) -> None:
        self._client.mouseMove(x, y)
        self._client.mousePress(1)

    def key(self, name: str) -> None:
        self._client.keyPress(name)

    def capture(self, path: str) -> None:
        self._client.captureScreen(path)

    def close(self) -> None:
        self._client.disconnect()
        # Stop vncdotool's Twisted reactor — without this the non-daemon reactor thread keeps the
        # process alive after disconnect (every capture/drive command would hang until timeout).
        api.shutdown()
