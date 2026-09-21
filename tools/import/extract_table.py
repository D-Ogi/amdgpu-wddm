#!/usr/bin/env python3
"""Extract one named C array from a reference-only import, verbatim.

The imports in driver/amdgpu-import/ are byte-identical kernel files. Two of them, gfx_v10_0.c and
sdma_v5_0.c, are too entangled with the kernel to compile (see that directory's PROVENANCE.md), but
they still hold data tables we need. Retyping such a table would put dozens of hand-copied register
names, masks and values into our tree, which the repository rules forbid and which nobody could
review. So the table is cut out mechanically instead, here, and the result is checked in next to a
note saying where it came from.

    python tools/import/extract_table.py \
        --source driver/amdgpu-import/gfx_v10_0.c \
        --array golden_settings_gc_10_0_cyan_skillfish \
        --out driver/shim/generated/gfx10_golden_cyan_skillfish.inc

The extraction takes the lines between the array's opening brace and its closing `};`, unchanged.
Nothing is reformatted, renamed or recomputed. With --check the file is not written; the extraction
is compared with what is already there and a difference is an error, which is how the replay test
notices that an import changed under it.
"""
import argparse
import hashlib
import re
import sys


BANNER = """\
/* SPDX-License-Identifier: MIT */
/*
 * GENERATED - do not edit.
 *
 * {array}[], extracted verbatim from {source} lines {first}-{last}
 * by tools/import/extract_table.py. That file is an unmodified copy of the Linux kernel's
 * {kernel_path} at tag {tag}, commit {commit}; see
 * driver/amdgpu-import/PROVENANCE.md. Copyright and license are AMD's, as in the source file.
 *
 * sha256 of the extracted body: {sha}
 *
 * Regenerate, or verify without writing:
 *   python tools/import/extract_table.py --source {source} \\
 *          --array {array} --out {out}
 *   python tools/import/extract_table.py ... --check
 */
"""


def extract(source_path, array):
    with open(source_path, "r", encoding="utf-8", newline="") as f:
        lines = f.read().split("\n")

    # The declaration line: "<type> <array>[] = {" possibly split over two lines upstream. Every
    # table we take is on one line today; if that ever changes, this fails loudly rather than
    # guessing.
    start = None
    for i, line in enumerate(lines):
        if re.search(r"\b" + re.escape(array) + r"\s*\[\s*\]\s*=\s*\{\s*$", line):
            start = i
            break
    if start is None:
        raise SystemExit("extract_table: no array %s ending in '= {' found in %s"
                         % (array, source_path))

    end = None
    for i in range(start + 1, len(lines)):
        if lines[i].startswith("};"):
            end = i
            break
    if end is None:
        raise SystemExit("extract_table: no closing '};' after %s in %s" % (array, source_path))

    body = lines[start + 1:end]
    # 1-based line numbers of the body, for the banner.
    return body, start + 2, end


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", required=True)
    ap.add_argument("--array", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--tag", default="v6.18")
    ap.add_argument("--commit", default="7d0a66e4bb9081d75c82ec4957c50034cb0ea449")
    ap.add_argument("--kernel-path", default=None,
                    help="path of the source inside the kernel tree; by default "
                         "drivers/gpu/drm/amd/amdgpu/<basename of --source>, which is where every "
                         "file we extract from lives. A fixed default was wrong the moment a "
                         "second source was added, so it is derived instead.")
    ap.add_argument("--check", action="store_true",
                    help="do not write; fail if --out differs from the extraction")
    args = ap.parse_args()

    if args.kernel_path is None:
        args.kernel_path = ("drivers/gpu/drm/amd/amdgpu/"
                            + args.source.replace("\\", "/").rsplit("/", 1)[-1])

    body, first, last = extract(args.source, args.array)
    text = "\n".join(body) + "\n"
    sha = hashlib.sha256(text.encode("utf-8")).hexdigest()

    banner = BANNER.format(array=args.array, source=args.source.replace("\\", "/"),
                           first=first, last=last, kernel_path=args.kernel_path,
                           tag=args.tag, commit=args.commit, sha=sha,
                           out=args.out.replace("\\", "/"))
    generated = banner + text

    if args.check:
        try:
            with open(args.out, "r", encoding="utf-8", newline="") as f:
                have = f.read()
        except OSError as exc:
            print("extract_table --check: cannot read %s (%s)" % (args.out, exc))
            return 1
        # The checked-in file may have been written with CRLF by a Windows editor; compare content,
        # not line endings.
        if have.replace("\r\n", "\n") != generated:
            print("extract_table --check: %s does not match %s:%d-%d"
                  % (args.out, args.source, first, last))
            return 1
        print("extract_table --check: %s matches %s:%d-%d (%d entries, sha256 %s)"
              % (args.out, args.source, first, last, len(body), sha[:16]))
        return 0

    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(generated)
    print("extract_table: wrote %s, %d lines from %s:%d-%d (sha256 %s)"
          % (args.out, len(body), args.source, first, last, sha[:16]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
