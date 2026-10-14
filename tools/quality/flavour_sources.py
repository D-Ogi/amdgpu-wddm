"""Every read of a lab-only setting in the control application's sources is inside a flavour guard.

The release flavour of the control application reads none of the four diagnostic environment variables
(BD-167). `build.ps1` proves that on the built image, by looking for their names in its user string heap, and
that check is what caught `Crash.cs` reading AMDGPU_WDDM_CONTROL_STATE in every flavour. This rule says the
same thing about the sources, which costs no build: a reader of one of those names must sit inside
`#if AMDGPU_WDDM_LAB_FLAVOUR`, and the `#else` arm must do without it.

A source rule and an image rule are not the same check and neither replaces the other: the image rule is the
one the release depends on, and this one fails in a second, names the file and the line, and can be given
planted controls (test_flavour_sources.py).

    python -I tools/quality/flavour_sources.py --root <repo> [--out <dir>]
"""
import argparse
import io
import os
import re
import sys

# The four names a lab build reads and a release build must not (build.ps1, BD-167).
LAB_NAMES = [
    "AMDGPU_WDDM_CONTROL_STATE",
    "AMDGPU_WDDM_CONTROL_INSTALLER",
    "AMDGPU_WDDM_CONTROL_NO_DLL",
    "AMDGPU_WDDM_CONTROL_STRINGS",
]

GUARD = "AMDGPU_WDDM_LAB_FLAVOUR"
SOURCES = os.path.join("tools", "win", "amdgpu_wddm_control", "src")

_IF = re.compile(r"^\s*#\s*if\b(.*)$")
_ELIF = re.compile(r"^\s*#\s*elif\b(.*)$")
_ELSE = re.compile(r"^\s*#\s*else\b")
_ENDIF = re.compile(r"^\s*#\s*endif\b")


def unguarded_lab_reads(text, names=None):
    """Lines that name a lab-only setting while not inside a lab-flavour arm.

    Returns a list of (line number, name, line). The preprocessor state is a stack of "this arm is the lab
    arm" flags, so a read inside a nested #if under the guard still counts as guarded, and the #else arm of
    the guard does not.
    """
    names = names or LAB_NAMES
    stack = []
    out = []
    for n, line in enumerate(text.splitlines(), 1):
        m = _IF.match(line)
        if m:
            stack.append(GUARD in m.group(1))
            continue
        m = _ELIF.match(line)
        if m:
            if stack:
                stack[-1] = GUARD in m.group(1)
            continue
        if _ELSE.match(line):
            if stack:
                stack[-1] = False
            continue
        if _ENDIF.match(line):
            if stack:
                stack.pop()
            continue
        if any(stack):
            continue
        # A comment is not a read: the name may be explained in prose next to the guarded code.
        bare = line.split("//", 1)[0]
        for name in names:
            if name in bare:
                out.append((n, name, line.strip()))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", required=True)
    ap.add_argument("--out")
    args = ap.parse_args(argv)

    src = os.path.join(args.root, SOURCES)
    if not os.path.isdir(src):
        print("flavour-sources: no %s" % src)
        return 2

    findings = []
    files = 0
    for name in sorted(os.listdir(src)):
        if not name.endswith(".cs"):
            continue
        files += 1
        path = os.path.join(src, name)
        text = io.open(path, encoding="utf-8-sig", newline="").read()
        for n, var, line in unguarded_lab_reads(text):
            findings.append((name, n, var, line))

    if args.out:
        os.makedirs(args.out, exist_ok=True)
        with io.open(os.path.join(args.out, "flavour-sources.txt"), "w", encoding="utf-8") as fh:
            fh.write("%d source file(s) read\n" % files)
            for name, n, var, line in findings:
                fh.write("%s:%d %s | %s\n" % (name, n, var, line))

    for name, n, var, line in findings:
        print("%s:%d reads %s outside #if %s: %s" % (name, n, var, GUARD, line[:100]))
    print("flavour-sources: %d file(s), %d unguarded read(s) of a lab-only setting" % (files, len(findings)))
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
