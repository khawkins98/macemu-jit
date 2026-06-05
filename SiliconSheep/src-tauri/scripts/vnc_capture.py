#!/usr/bin/env python3
"""Capture a screenshot from a VNC server, fixing BGR→RGB channel swap."""
import sys
import subprocess
import tempfile
from pathlib import Path

def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <host:port> <output.png>", file=sys.stderr)
        sys.exit(1)

    server = sys.argv[1]
    output = sys.argv[2]

    with tempfile.NamedTemporaryFile(suffix=".png", delete=False) as tmp:
        tmp_path = tmp.name

    try:
        result = subprocess.run(
            [sys.executable.replace("python3", "vncdotool").replace("python", "vncdotool"),
             "-s", server, "capture", tmp_path],
            capture_output=True, text=True, timeout=10
        )
        # Fallback: find vncdotool next to this python
        if result.returncode != 0:
            venv_bin = Path(sys.executable).parent
            vncdotool = venv_bin / "vncdotool"
            if vncdotool.exists():
                result = subprocess.run(
                    [str(vncdotool), "-s", server, "capture", tmp_path],
                    capture_output=True, text=True, timeout=10
                )

        if result.returncode != 0:
            print(f"vncdotool failed: {result.stderr}", file=sys.stderr)
            sys.exit(1)

        from PIL import Image
        img = Image.open(tmp_path)
        if img.mode == "RGB":
            r, g, b = img.split()
            img = Image.merge("RGB", (b, g, r))
        elif img.mode == "RGBA":
            r, g, b, a = img.split()
            img = Image.merge("RGBA", (b, g, r, a))
        img.save(output, "PNG")

    finally:
        Path(tmp_path).unlink(missing_ok=True)

if __name__ == "__main__":
    main()
