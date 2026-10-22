"""Builds the Nagi Control App icon from Nagi's smiling bust.
A round badge: the bust on the page colour of the GUI (#101217), clipped to a circle, with a ring in the accent red
(#E2263E). Large sizes show head and shoulders; 16-32 px use a tighter crop on the face, so it stays readable.
Usage: python -I make-icon.py <bust png> <out dir>   (writes nagi.ico and preview PNGs)"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw

BACK = (16, 18, 23, 255)
RING = (226, 38, 62, 255)
SIZES = [256, 128, 64, 48, 40, 32, 24, 20, 16]
SS = 4  # supersampling for the circle edges


def badge(bust, size, crop):
    """crop = (left, top, right, bottom) fractions of the bust square."""
    w, h = bust.size
    box = (int(crop[0] * w), int(crop[1] * h), int(crop[2] * w), int(crop[3] * h))
    big = size * SS
    ring = max(SS, round(big * (0.055 if size >= 48 else 0.075)))
    canvas = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    disc = Image.new('L', (big, big), 0)
    ImageDraw.Draw(disc).ellipse((0, 0, big - 1, big - 1), fill=255)
    inner = Image.new('L', (big, big), 0)
    ImageDraw.Draw(inner).ellipse((ring, ring, big - 1 - ring, big - 1 - ring), fill=255)
    canvas.paste(Image.new('RGBA', (big, big), RING), (0, 0), disc)
    canvas.paste(Image.new('RGBA', (big, big), BACK), (0, 0), inner)
    face = bust.crop(box).resize((big - 2 * ring, big - 2 * ring), Image.LANCZOS)
    layer = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    layer.paste(face, (ring, ring), face)
    clipped = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    clipped.paste(layer, (0, 0), inner)
    canvas = Image.alpha_composite(canvas, clipped)
    return canvas.resize((size, size), Image.LANCZOS)


def main():
    bust = Image.open(sys.argv[1]).convert('RGBA')
    out = Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    wide = (0.06, 0.02, 0.94, 0.90)   # head and shoulders
    tight = (0.22, 0.12, 0.84, 0.74)  # the face
    frames = [badge(bust, s, wide if s >= 40 else tight) for s in SIZES]
    frames[0].save(out / 'nagi.ico', sizes=[(s, s) for s in SIZES], append_images=frames[1:])
    for s, f in zip(SIZES, frames):
        f.save(out / f'nagi-{s}.png')
    sheet = Image.new('RGBA', (sum(SIZES) + 10 * len(SIZES), 256), (240, 240, 240, 255))
    x = 0
    for s, f in zip(SIZES, frames):
        sheet.paste(f, (x, 256 - s), f)
        x += s + 10
    sheet.save(out / 'preview-light.png')
    dark = Image.new('RGBA', sheet.size, (32, 32, 32, 255))
    x = 0
    for s, f in zip(SIZES, frames):
        dark.paste(f, (x, 256 - s), f)
        x += s + 10
    dark.save(out / 'preview-dark.png')
    print('wrote', out / 'nagi.ico', SIZES)


main()
