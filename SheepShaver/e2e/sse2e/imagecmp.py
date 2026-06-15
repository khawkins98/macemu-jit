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

# The menu-bar TITLE strip (top-left), excluding the clock/app label. Classic Mac OS gives the
# menu bar to the frontmost application, so a perceptual hash of this strip is a robust "who is
# frontmost" signal: it changes when an app launches (its menus replace the Finder's) and stays put
# for a Finder-level dialog. Measured FC menus vs Finder = 30; a Finder error dialog vs Finder = 4.
MENUBAR_BOX = (0, 0, 690, 20)


def phash_masked(path: str | Path, masks=DEFAULT_MASKS) -> imagehash.ImageHash:
    """Perceptual hash of an image with the given rectangles blacked out."""
    img = Image.open(path).convert("RGB")
    for box in masks:
        img.paste((0, 0, 0), box)
    return imagehash.phash(img)


def region_phash(path: str | Path, box=MENUBAR_BOX) -> imagehash.ImageHash:
    """Perceptual hash of a cropped region (e.g. the menu-bar title strip)."""
    return imagehash.phash(Image.open(path).convert("RGB").crop(box))


def compare(path_a: str | Path, path_b: str | Path, masks=DEFAULT_MASKS, threshold: int = 8):
    """Return (within_threshold, hamming_distance) for two images under the mask."""
    dist = phash_masked(path_a, masks) - phash_masked(path_b, masks)
    return (dist <= threshold, dist)
