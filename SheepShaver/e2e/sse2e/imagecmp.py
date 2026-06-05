"""Masked perceptual-hash image comparison for visual (video) regression — smoke P2.

A raw screenshot of the Mac desktop isn't pixel-stable (the menu-bar clock ticks, the cursor
blinks), so exact comparison is hopelessly flaky. Instead: black out the volatile regions, then
compare with a *perceptual* hash (pHash) + a Hamming-distance threshold. That tolerates tiny
rendering noise while still catching a real video regression (wrong colors, garbled layout).

Usage: capture a known-good "golden" desktop once, then on each run compare the boot screenshot
against it with the clock region masked. Distance 0 = identical structure; a few bits = noise; a
large distance = a real visual change.
"""
from __future__ import annotations

from pathlib import Path

import imagehash
from PIL import Image

# Volatile regions to mask at 800x600 (x0, y0, x1, y1). The menu-bar clock sits top-right.
DEFAULT_MASKS = ((690, 0, 800, 18),)  # menu-bar clock + frontmost-app label


def phash_masked(path: str | Path, masks=DEFAULT_MASKS) -> imagehash.ImageHash:
    """Perceptual hash of an image with the given rectangles blacked out."""
    img = Image.open(path).convert("RGB")
    for box in masks:
        img.paste((0, 0, 0), box)
    return imagehash.phash(img)


def compare(path_a: str | Path, path_b: str | Path, masks=DEFAULT_MASKS, threshold: int = 8):
    """Return (within_threshold, hamming_distance) for two images under the mask."""
    dist = phash_masked(path_a, masks) - phash_masked(path_b, masks)
    return (dist <= threshold, dist)
