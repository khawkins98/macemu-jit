from PIL import Image, ImageDraw

from sse2e import imagecmp


def _solid(path, color):
    Image.new("RGB", (800, 600), color).save(path)


def test_identical_images_zero_distance(tmp_path):
    a, b = tmp_path / "a.png", tmp_path / "b.png"
    _solid(a, (100, 100, 150))
    _solid(b, (100, 100, 150))
    within, dist = imagecmp.compare(a, b, masks=())
    assert within and dist == 0


def test_masked_volatile_region_ignored(tmp_path):
    # b differs from a ONLY inside the masked clock region -> still "same" under the mask.
    a, b = tmp_path / "a.png", tmp_path / "b.png"
    _solid(a, (100, 100, 150))
    img = Image.new("RGB", (800, 600), (100, 100, 150))
    ImageDraw.Draw(img).rectangle((700, 2, 790, 16), fill=(255, 255, 255))  # inside default mask
    img.save(b)
    within, _ = imagecmp.compare(a, b)  # DEFAULT_MASKS covers (690,0,800,18)
    assert within


def test_different_images_exceed_threshold(tmp_path):
    a, b = tmp_path / "a.png", tmp_path / "b.png"
    _solid(a, (20, 20, 20))
    img = Image.new("RGB", (800, 600), (20, 20, 20))
    ImageDraw.Draw(img).rectangle((50, 50, 750, 550), fill=(240, 240, 240))  # big structural change
    img.save(b)
    within, dist = imagecmp.compare(a, b, masks=(), threshold=8)
    assert not within and dist > 8
