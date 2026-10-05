"""The exact image of d3d11bench's draws scene (scene revision 2), and a comparison with a dump.

    python draws_model.py DIR\\draws.pam --draws N

Recomputes every draw as DrawsScene::Init sets it up (Hash(d + 1): corner, 0/1 channel mask; texture d % 4
created with seed d % 4 + 1) and rasterizes it in exact rational arithmetic. No triangle edge passes through a
pixel centre by construction, which the model asserts rather than assuming a fill rule, so every conforming
implementation must produce this image bit for bit. Exit 0 when the dump equals the model, 1 otherwise, and
prints the first differing pixels.
"""
import argparse
import os
import sys
from fractions import Fraction

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import imgdiff  # noqa: E402


def mix(x):
    """d3d11bench's Hash."""
    x &= 0xFFFFFFFF
    x ^= x >> 16
    x = (x * 0x7FEB352D) & 0xFFFFFFFF
    x ^= x >> 15
    x = (x * 0x846CA68B) & 0xFFFFFFFF
    x ^= x >> 16
    return x


def texel(seed, x, y):
    """PatternTexture's level-0 texel as (R, G, B, A)."""
    v = mix(seed * 7919 + (x << 16 | y)) | 0xFF000000
    return [(v >> (8 * c)) & 255 for c in range(4)]


def legs(width, height):
    dx = max(2, (5 * width + 64) // 128)
    dy = max(2, (6 * height + 64) // 128)
    if (dy - dx) % 2 == 0:
        dy += 1
    return dx, dy


def expected(draws, width, height):
    """Rows of (R, G, B, A) tuples: the clear colour, then each draw in order (no blending)."""
    img = [[(0, 0, 0, 255)] * width for _ in range(height)]
    dx, dy = legs(width, height)
    half = Fraction(1, 2)
    for d in range(draws):
        h = mix(d + 1)
        x0 = (h & 255) * width // 256 + Fraction(1, 4)
        y0 = ((h >> 8) & 255) * height // 256 + Fraction(1, 4)
        mask = 1 + (h >> 16) % 7
        # Pixel space, y down: right angle at (x0, y0), then (x0 + dx, y0) and (x0, y0 - dy).
        for py in range(max(0, int(y0) - dy - 1), min(height, int(y0) + 2)):
            cy = py + half
            for px in range(max(0, int(x0) - 1), min(width, int(x0) + dx + 2)):
                cx = px + half
                edge = (cx - x0) / dx + (y0 - cy) / dy
                assert cx != x0 and cy != y0 and edge != 1, "an edge passes through a pixel centre"
                if cx > x0 and cy < y0 and edge < 1:
                    t = texel(d % 4 + 1, px & 63, py & 63)
                    img[py][px] = tuple(t[c] if (mask >> c) & 1 else 0 for c in range(3)) + (255,)
    return img


def compare(draws, image):
    """Pixels (x, y, expected, actual) where a parsed PAM image differs from the model."""
    width, height, pixels = image
    model = expected(draws, width, height)
    bad = []
    for py in range(height):
        for px in range(width):
            i = (py * width + px) * 4
            got = tuple(pixels[i:i + 4])
            if got != model[py][px]:
                bad.append((px, py, model[py][px], got))
    return bad


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("image")
    p.add_argument("--draws", type=int, required=True, help="the run's --draws")
    a = p.parse_args(argv)
    try:
        with open(a.image, "rb") as f:
            image = imgdiff.parse_pam(f.read())
    except (OSError, imgdiff.ImageError) as e:
        print("UNUSABLE %s" % e)
        return 2
    bad = compare(a.draws, image)
    print("%s %s: %d of %d pixels differ from the model%s" % (
        "EXACT" if not bad else "DIFFERS", a.image, len(bad), image[0] * image[1],
        (" first " + ", ".join("(%d,%d) want %s got %s" % b for b in bad[:5])) if bad else ""))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
