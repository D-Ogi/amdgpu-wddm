"""Builds the pictures of the Smart App Control and SmartScreen guide from English screenshots of Windows.

Two outputs from one set of screenshots:
  - the setup window's help card (src/HelpCards.cs): PNG, at most 720 px wide (HelpCards.ImagePixels) and 160 KiB
    (HelpCards.ImageBytes), 256 colours or fewer, no metadata. build.ps1 embeds them from this folder (-HelpArt).
  - docs/testing/INSTALL.md, section "Smart App Control and SmartScreen": WebP at quality 82, at most 960 px wide,
    no metadata, in docs/media like the other pictures of the documentation.

Usage: python make-help-art.py <screenshots dir> <setup art dir> <docs media dir>
The screenshots dir holds the PNG files named below. A required file that is absent stops the run; an optional one
is left out, and the window and the guide then show that step as text. The setup art dir gets a PROVENANCE.md with
the bytes and the SHA-256 of every picture it holds. It needs Pillow.
"""
import hashlib
import io
import sys
from pathlib import Path

from PIL import Image

SETUP_WIDTH, SETUP_BYTES = 720, 160 * 1024
DOCS_WIDTH, DOCS_QUALITY = 960, 82

# name: (in the setup window, in the guide, required). The window opens the Smart App Control page directly
# (windowsdefender://smartapp), so it needs neither the block message nor the page App & browser control; the guide
# starts from the block message and also shows the way through the Start menu.
PICTURES = {
    'sac-1-blocked.png': (False, True, True),
    'sac-2-app-browser.png': (False, True, True),
    'sac-3-settings.png': (True, True, True),
    'sac-4-confirm.png': (True, True, True),
    'sac-5-off.png': (True, True, True),
    'smartscreen-1-protected.png': (False, True, True),
    'smartscreen-2-run-anyway.png': (False, True, True),
}


def fit(img, width):
    img = img.convert('RGB')
    if img.width > width:
        img = img.resize((width, round(img.height * width / img.width)), Image.LANCZOS)
    return img


def setup_png(img):
    """The smallest of 256, 128 and 64 colours that fits the byte limit; screenshots of flat UI keep their look."""
    for colours in (256, 128, 64):
        out = io.BytesIO()
        img.quantize(colors=colours, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE).save(out, 'PNG', optimize=True)
        if out.tell() <= SETUP_BYTES:
            return out.getvalue(), colours
    raise SystemExit(f'a picture stays above {SETUP_BYTES} bytes at 64 colours: crop the screenshot')


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    shots, setup_dir, docs_dir = (Path(a) for a in sys.argv[1:])
    missing = [n for n, (_, _, required) in PICTURES.items() if required and not (shots / n).is_file()]
    if missing:
        raise SystemExit('required screenshots missing: ' + ', '.join(missing))
    setup_dir.mkdir(parents=True, exist_ok=True)
    docs_dir.mkdir(parents=True, exist_ok=True)
    rows = []
    for name, (in_setup, in_docs, _) in PICTURES.items():
        src = shots / name
        if not src.is_file():
            print(f'  {name}: absent (optional), left out')
            continue
        with Image.open(src) as raw:
            raw.load()
            source = raw.copy()
        if in_setup:
            img = fit(source, SETUP_WIDTH)
            data, colours = setup_png(img)
            (setup_dir / name).write_bytes(data)
            rows.append((name, len(data), hashlib.sha256(data).hexdigest().upper(), f'{img.width}x{img.height}'))
            print(f'  setup {name}: {img.width}x{img.height}, {colours} colours, {len(data)} bytes')
        if in_docs:
            img = fit(source, DOCS_WIDTH)
            out = io.BytesIO()
            img.save(out, 'WEBP', quality=DOCS_QUALITY, method=6)
            webp = Path(name).with_suffix('.webp').name
            (docs_dir / webp).write_bytes(out.getvalue())
            print(f'  docs {webp}: {img.width}x{img.height}, {out.tell()} bytes')
    script = Path(__file__).read_bytes()
    table = '\n'.join(f'| `{n}` | {s} | {b} | `{h}` |' for n, b, h, s in rows)
    (setup_dir / 'PROVENANCE.md').write_text(
        'PROVENANCE: the pictures of the Smart App Control help card of the setup window (src/HelpCards.cs).\n'
        'They are screenshots of Windows Security in English Windows 11 24H2 (build 26100), taken for this project,\n'
        'and show the steps of docs/testing/INSTALL.md, section "Smart App Control and SmartScreen".\n\n'
        'This file is written by `../make-help-art.py`, the recipe: each screenshot is scaled to at most\n'
        f'{SETUP_WIDTH} px wide, reduced to 256 colours or fewer and saved as PNG without metadata. The same run writes\n'
        'the WebP pictures of the guide to docs/media.\n\n'
        '| File | Size | Bytes | SHA-256 |\n|---|---|---|---|\n' + table + '\n'
        f'| `../make-help-art.py` | - | {len(script)} | `{hashlib.sha256(script).hexdigest().upper()}` |\n\n'
        'To build them again: `python ..\\make-help-art.py <screenshots> . <repo>\\docs\\media`. It needs Pillow,\n'
        'so the bytes can differ on another Pillow version. The files in this directory are the ones the exe embeds.\n',
        encoding='utf-8', newline='\n')


if __name__ == '__main__':
    main()
