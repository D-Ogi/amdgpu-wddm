#!/usr/bin/env python3
"""Show the diagnostic chunks as QR codes on the local screen, one code at a time.

Draws straight into /dev/fb0 (works on the EFI framebuffer and on amdgpu's fbdev alike), with a
text-console fallback using half-block characters. Any key: next code. Q: quit to a shell.
Without a key press the codes advance on their own, so a keyboard is optional.
"""

import glob
import os
import select
import subprocess
import sys
import textwrap

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "pylib"))
import segno  # noqa: E402  (vendored pure-Python QR encoder)

QUIET = 4           # modules of white border required by the QR spec
AUTO_ADVANCE_S = 20
TEXT_COLUMNS = 56
FONT_H = 16         # fbcon's default font height; only used to keep the text from scrolling
# The code never touches the last pixels of a scanline. This is not cosmetic: see draw_fb.
RIGHT_MARGIN_PX = 16


def make_matrix(text):
    """QR matrix (rows of 0/1) with the quiet zone included."""
    qr = segno.make(text, error="l", boost_error=False, micro=False)
    n = len(qr.matrix) + 2 * QUIET
    rows = [[0] * n for _ in range(QUIET)]
    for row in qr.matrix:
        rows.append([0] * QUIET + [1 if m else 0 for m in row] + [0] * QUIET)
    rows += [[0] * n for _ in range(QUIET)]
    return rows


def pixel_rows(matrix, scale, bytespp):
    """One bytes object per framebuffer line. Black and white are all-zeros / all-ones in every
    common pixel format (16, 24, 32 bpp), so no format handling is needed."""
    white, black = b"\xff" * (bytespp * scale), b"\x00" * (bytespp * scale)
    for row in matrix:
        line = b"".join(black if m else white for m in row)
        for _ in range(scale):
            yield line


def fb_geometry(sysfs="/sys/class/graphics/fb0"):
    with open(sysfs + "/virtual_size") as f:
        width, height = (int(x) for x in f.read().strip().split(","))
    with open(sysfs + "/bits_per_pixel") as f:
        bytespp = int(f.read()) // 8
    with open(sysfs + "/stride") as f:
        stride = int(f.read())
    return width, height, bytespp, stride


def fb_layout(width, height, bytespp, stride, modules):
    """(scale, x0, y0) for a `modules` x `modules` code, or None if the framebuffer is unusable.

    The code sits on the right-hand side, vertically centred, and keeps RIGHT_MARGIN_PX free.
    That margin is what makes the code visible at all on a DRM framebuffer (see draw_fb), so the
    result is rejected if a scanline of the code would reach the end of the line.
    """
    scale = min(height, width - RIGHT_MARGIN_PX) // modules
    if scale < 2 or bytespp < 2 or stride < width * bytespp:
        return None
    size = scale * modules
    x0, y0 = width - RIGHT_MARGIN_PX - size, (height - size) // 2
    if x0 < 0 or y0 < 0 or (x0 + size) * bytespp >= stride:
        return None
    return scale, x0, y0


def write_all(fd, data, off):
    """pwrite on a framebuffer may report a short write; push the rest out before moving on."""
    done = 0
    while done < len(data):
        n = os.pwrite(fd, data[done:], off + done)
        if n <= 0:
            raise OSError("short write to the framebuffer")
        done += n


def fb_nodes():
    """(sysfs directory, device node) of every framebuffer, fb0 first.

    Loading amdgpu replaces the framebuffer the console started on, and the new one is not
    guaranteed to be fb0 again. The code is drawn into all of them: a diagnostic run on the target
    gets one attempt, and painting a second screen costs nothing.
    """
    found = [(p, "/dev/" + os.path.basename(p)) for p in sorted(glob.glob("/sys/class/graphics/fb[0-9]*"))]
    return found or [("/sys/class/graphics/fb0", "/dev/fb0")]


def draw_fb(matrix):
    """The code on every usable framebuffer. False if none of them could take it."""
    return sum(draw_one(matrix, sysfs, node) for sysfs, node in fb_nodes()) > 0


def draw_one(matrix, sysfs, dev):
    """QR on the right-hand side of the screen, vertically centred. Returns False if no usable fb.

    One write per scanline, and never up to the end of a line. The kernel's DRM fbdev emulation
    (simpledrm, bochs-drm, amdgpu) derives the rectangle it flushes to the display from the byte
    range of each write; for a range inside a single scanline it narrows that rectangle using
    (offset + length) % line_length, which is 0 when the write ends exactly on the line boundary.
    The rectangle then comes out empty and the pixels stay in the shadow buffer, invisible - the
    write itself succeeds and reads back fine, which is what made this look like a drawing bug.
    Measured on Alpine 6.18.52-lts / bochs-drmdrmfb 1280x800x32: a right-aligned code is never
    displayed, the same code shifted a few pixels left is.
    """
    try:
        width, height, bytespp, stride = fb_geometry(sysfs)
        layout = fb_layout(width, height, bytespp, stride, len(matrix))
        if layout is None:
            return False
        scale, x0, y0 = layout
        fd = os.open(dev, os.O_RDWR)
        try:
            for y, line in enumerate(pixel_rows(matrix, scale, bytespp)):
                write_all(fd, line, (y0 + y) * stride + x0 * bytespp)
        finally:
            os.close(fd)
        return True
    except OSError:
        return False


def console_lines(matrix):
    """Half-block rendering: two QR rows per text row, black on white."""
    glyph = {(0, 0): " ", (1, 0): "▀", (0, 1): "▄", (1, 1): "█"}
    rows = matrix + ([[0] * len(matrix)] if len(matrix) % 2 else [])
    for top, bottom in zip(rows[0::2], rows[1::2]):
        yield "\033[47;30m" + "".join(glyph[pair] for pair in zip(top, bottom)) + "\033[0m"


def net_line(ip_output):
    """'NET ...' line from the output of `ip -4 -o addr show scope global`."""
    found = []
    for line in ip_output.splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[2] == "inet":
            found.append(f"root@{parts[3].split('/')[0]} ({parts[1]})")
    return ("NET ssh " + ", ".join(found)) if found else "NET no address yet"


def current_net_line(keys="/root/.ssh/authorized_keys"):
    """Live, because Wi-Fi usually comes up after the diagnostics end. None if SSH is not set up."""
    if not os.path.exists(keys):
        return None
    try:
        res = subprocess.run(["ip", "-4", "-o", "addr", "show", "scope", "global"],
                             capture_output=True, text=True, timeout=5)
        return net_line(res.stdout)
    except (OSError, subprocess.SubprocessError):
        return "NET state unknown"


def text_lines(index, total, verdict_lines, rows=None, net=None):
    """Header plus verdict, wrapped to the left of the code and short enough not to scroll.

    Scrolling would matter: fbcon moves the whole screen contents up, code included.
    """
    header = [f"BC-250 DIAGNOSTICS   QR code {index + 1} of {total}", "",
              "Scan every code with a phone and send the text of all of them.",
              f"Any key: next code (auto every {AUTO_ADVANCE_S}s).  Q: shell.",
              "Full logs are saved on the USB stick in bc250/results/.", ""]
    if net:
        header += [net, ""]
    out = []
    for line in header + list(verdict_lines):
        out.extend(textwrap.wrap(line, TEXT_COLUMNS) or [""])
    if rows:
        out = out[:rows - 1]
    return out


def wait_key(timeout):
    """'' on timeout, else the key. Works without a tty (then it only sleeps)."""
    try:
        import termios
        import tty
        fd = sys.stdin.fileno()
        old = termios.tcgetattr(fd)
    except Exception:
        select.select([], [], [], timeout)
        return ""
    try:
        tty.setcbreak(fd)
        ready, _, _ = select.select([sys.stdin], [], [], timeout)
        return os.read(fd, 8).decode(errors="replace") if ready else ""
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)


def screen_rows():
    """Text rows of the framebuffer console, or None if there is no framebuffer to measure."""
    try:
        return fb_geometry(fb_nodes()[0][0])[1] // FONT_H
    except OSError:
        return None


def show(chunks, verdict_lines):
    out = sys.stdout
    rows = screen_rows()
    i, redraw_text, shown_net = 0, True, None
    while True:
        matrix = make_matrix(chunks[i])
        net = current_net_line()
        if redraw_text or net != shown_net:
            redraw_text, shown_net = True, net
            out.write("\033[2J\033[H\033[?25l")
            out.write("\n".join(text_lines(i, len(chunks), verdict_lines, rows, net)) + "\n")
            out.flush()
        # After the text: clearing the screen wipes the code too, fbcon paints into the same buffer.
        if not draw_fb(matrix) and redraw_text:
            out.write("\n".join(console_lines(matrix)) + "\n")
            out.flush()
        key = wait_key(AUTO_ADVANCE_S)
        if key.lower() == "q":
            out.write("\033[2J\033[H\033[?25h")
            out.flush()
            return
        nxt = (i + 1) % len(chunks)
        # A single code is refreshed on the framebuffer only: no clear, so nothing flickers.
        redraw_text = nxt != i
        i = nxt


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/bc250-chunks.txt"
    try:
        with open(path) as f:
            chunks = [l.strip() for l in f if l.strip()]
    except OSError:
        chunks = []
    if not chunks:
        print(f"No QR chunks in {path}: the probe did not get that far.")
        print("Look at bc250/results/ on the stick for what it did manage to write.")
        return
    try:
        with open(sys.argv[2] if len(sys.argv) > 2 else "/tmp/bc250-verdict.txt") as f:
            verdict_lines = [l.rstrip() for l in f]
    except OSError:
        verdict_lines = []
    show(chunks, verdict_lines)


if __name__ == "__main__":
    main()
