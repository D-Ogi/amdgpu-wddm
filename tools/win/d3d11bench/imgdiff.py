"""How far apart two d3d11bench images are (the --dump output), when their checksums differ.

    python imgdiff.py A.pam B.pam [--tolerance N] [--diff OUT.pam]
    python imgdiff.py DIR_A DIR_B [--tolerance N]     every <scene>.pam present in both directories

Per image: the pixel count, how many pixels differ at all, how many differ by more than the tolerance in any
channel, the largest difference per channel (R, G, B, A), where the first such pixel is, and each image's
d3d11bench checksum, which must equal the scene's checksum in the result JSON of its run. Exit 0 when every
difference is within the tolerance, 1 when one is not, 2 on unusable input (unreadable file, other sizes, no
common scene).

A tolerance above 0 is a judgement: implementations may round filtering weights and transcendental functions
differently (the README's "lead, not failure" across vendors). It never replaces a same-GPU checksum match.
"""

import argparse
import os
import sys


class ImageError(Exception):
    pass


def parse_pam(data):
    """(width, height, rgba bytes) from a PAM file with DEPTH 4 and MAXVAL 255, as d3d11bench writes it."""
    end = data.find(b"ENDHDR\n")
    if not data.startswith(b"P7\n") or end < 0:
        raise ImageError("not a PAM file")
    fields = {}
    for line in data[3:end].decode("ascii").splitlines():
        if line and not line.startswith("#"):
            key, _, value = line.partition(" ")
            fields[key] = value.strip()
    try:
        width, height = int(fields["WIDTH"]), int(fields["HEIGHT"])
        depth, maxval = int(fields["DEPTH"]), int(fields["MAXVAL"])
    except (KeyError, ValueError):
        raise ImageError("incomplete PAM header")
    if depth != 4 or maxval != 255 or width <= 0 or height <= 0:
        raise ImageError("expected DEPTH 4, MAXVAL 255")
    pixels = data[end + len(b"ENDHDR\n"):]
    if len(pixels) != width * height * 4:
        raise ImageError("pixel data has %d bytes, expected %d" % (len(pixels), width * height * 4))
    return width, height, pixels


def checksum(image):
    """d3d11bench's checksum of the image (FNV-1a over B, G, R, A per pixel), to tie a dump to its result JSON."""
    h = 14695981039346656037
    pixels = image[2]
    for i in range(0, len(pixels), 4):
        for c in (2, 1, 0, 3):
            h = ((h ^ pixels[i + c]) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return "%016x" % h


def compare(a, b, tolerance):
    """Differences between two parsed images; raises ImageError when their sizes differ."""
    (wa, ha, pa), (wb, hb, pb) = a, b
    if (wa, ha) != (wb, hb):
        raise ImageError("sizes differ: %dx%d and %dx%d" % (wa, ha, wb, hb))
    channel_max = [0, 0, 0, 0]
    differing = beyond = 0
    first = None
    for i in range(0, len(pa), 4):
        d = [abs(pa[i + c] - pb[i + c]) for c in range(4)]
        worst = max(d)
        if worst:
            differing += 1
            for c in range(4):
                channel_max[c] = max(channel_max[c], d[c])
            if worst > tolerance:
                beyond += 1
                if first is None:
                    first = ((i // 4) % wa, (i // 4) // wa)
    return {"pixels": wa * ha, "differing": differing, "beyond_tolerance": beyond, "max_rgba": channel_max,
            "first_beyond": first, "checksums": (checksum(a), checksum(b))}


def diff_image(a, b):
    """A PAM image of the absolute differences, scaled so that one step shows, alpha opaque."""
    width, height, pa = a
    pb = b[2]
    out = bytearray()
    for i in range(0, len(pa), 4):
        out += bytes(min(255, 16 * abs(pa[i + c] - pb[i + c])) for c in range(3)) + b"\xff"
    header = "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n" % (width, height)
    return header.encode("ascii") + bytes(out)


def report(name, result, tolerance):
    verdict = "WITHIN" if not result["beyond_tolerance"] else "BEYOND"
    first = result["first_beyond"]
    return "%s %s tolerance=%d pixels=%d differing=%d beyond=%d max_rgba=%s%s checksums=%s,%s" % (
        verdict, name, tolerance, result["pixels"], result["differing"], result["beyond_tolerance"],
        ",".join(str(v) for v in result["max_rgba"]), " first_beyond=%d,%d" % first if first else "",
        *result["checksums"])


def pairs(a, b):
    if os.path.isdir(a) and os.path.isdir(b):
        names = sorted(n for n in os.listdir(a) if n.endswith(".pam") and os.path.isfile(os.path.join(b, n)))
        return [(n[:-4], os.path.join(a, n), os.path.join(b, n)) for n in names]
    return [(os.path.basename(a), a, b)]


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("a")
    p.add_argument("b")
    p.add_argument("--tolerance", type=int, default=0, help="largest accepted difference per channel (0-255)")
    p.add_argument("--diff", help="write the difference image (single-file mode only)")
    args = p.parse_args(argv)
    if not 0 <= args.tolerance <= 255:
        print("tolerance must be 0-255")
        return 2
    try:
        work = pairs(args.a, args.b)
        if not work:
            raise ImageError("no <scene>.pam present in both directories")
        if args.diff and len(work) != 1:
            raise ImageError("--diff takes two files")
        status = 0
        for name, path_a, path_b in work:
            with open(path_a, "rb") as fa, open(path_b, "rb") as fb:
                a, b = parse_pam(fa.read()), parse_pam(fb.read())
            result = compare(a, b, args.tolerance)
            print(report(name, result, args.tolerance))
            if result["beyond_tolerance"]:
                status = 1
            if args.diff:
                with open(args.diff, "wb") as f:
                    f.write(diff_image(a, b))
        return status
    except (OSError, ImageError, UnicodeDecodeError) as e:
        print("UNUSABLE %s" % e)
        return 2


if __name__ == "__main__":
    sys.exit(main())
