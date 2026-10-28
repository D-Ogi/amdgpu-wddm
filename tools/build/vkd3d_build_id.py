"""The build id of a vkd3d-proton engine: what it must be before the build, and what the build gave.

vkd3d-proton writes `vkd3d_build` into vkd3d_build.h with meson's vcs_tag: the output of
`git describe --always --exclude=* --abbrev=15 --dirty=0` in the source tree, read as a hexadecimal number.
When that command fails, vcs_tag writes the fallback 0x12345678 and says nothing. The disk shader cache
(libs/vkd3d/cache.c) refuses a blob of another build only by this number, so an engine with the fallback
id takes the SPIR-V that any other fallback engine wrote: a converter fix in dxil-spirv then never reaches
a shader that is already in the cache. The dev engines of 2026-10-28 had the fallback because the
submodule links of their worktree pointed to a gitdir that no longer existed: git describe stopped with
"not a git repository", and the build continued.

  expect --source <tree> [--git <git.exe>] [--allow-dirty]
      Runs the vcs_tag command of the tree's own meson.build in the tree and prints a JSON record: the
      commit, the expected id, the fallback, and whether the tree is dirty. Stops (exit 1) when git fails,
      when the id is not a prefix of HEAD, or when the tree is dirty without --allow-dirty.
  check --header <build>/vkd3d_build.h --expect <file with the expect JSON> [--dll <dll>] [--require-in-dll]
      Reads the id that the build wrote and stops when it is the fallback or another id than the expected
      one. --dll counts the little-endian 64-bit id in the DLL; --require-in-dll stops when it is absent.
"""
import argparse
import json
import re
import struct
import subprocess
import sys
from pathlib import Path

MAX_HEX_DIGITS = 16  # uint64_t

_VCS_TAG = re.compile(
    r"vkd3d_build\s*=\s*vcs_tag\(\s*command\s*:\s*\[(?P<cmd>[^\]]*)\]"
    r"(?P<rest>[^)]*)\)", re.S)
_FALLBACK = re.compile(r"fallback\s*:\s*'(?P<value>[^']*)'")
_HEADER = re.compile(r"static\s+const\s+uint64_t\s+vkd3d_build\s*=\s*0x(?P<hex>[0-9A-Za-z@_]+)\s*;")


class BuildIdError(Exception):
    pass


def read_vcs_tag(meson_build: str):
    """The command and the fallback of the vkd3d_build vcs_tag in meson.build text."""
    m = _VCS_TAG.search(meson_build)
    if not m:
        raise BuildIdError("meson.build has no 'vkd3d_build = vcs_tag(command : [...])': the gate does not know "
                           "how this tree makes its build id")
    cmd = re.findall(r"'([^']*)'", m.group("cmd"))
    fb = _FALLBACK.search(m.group("rest"))
    if not cmd or cmd[0] != "git" or not fb:
        raise BuildIdError(f"vkd3d_build vcs_tag is not a git command with a fallback: {m.group(0)!r}")
    dirty = [a for a in cmd if a.startswith("--dirty=")]
    return {"command": cmd, "fallback": fb.group("value"),
            "dirty_mark": dirty[0][len("--dirty="):] if dirty else ""}


def _git(git, tree, args):
    return subprocess.run([git, "-C", str(tree)] + args, capture_output=True, text=True)


def expect(source, git="git", allow_dirty=False):
    source = Path(source)
    tag = read_vcs_tag((source / "meson.build").read_text(encoding="utf-8"))
    head = _git(git, source, ["rev-parse", "HEAD"])
    if head.returncode != 0:
        raise BuildIdError(f"git rev-parse HEAD in {source} failed: {head.stderr.strip()}")
    commit = head.stdout.strip().lower()
    # The same command as meson.build, in the same directory (vcs_tag runs it in the source root).
    run = _git(git, source, tag["command"][1:])
    if run.returncode != 0:
        raise BuildIdError(
            f"'{' '.join(tag['command'])}' in {source} exits {run.returncode}: {run.stderr.strip()}. meson would "
            f"write the fallback build id 0x{tag['fallback']} and the shader cache would accept SPIR-V of other "
            f"builds. Repair the git metadata of the tree and its submodules (a submodule .git file that names a "
            f"missing gitdir is the known cause), or build from a fresh tree of the commit")
    described = run.stdout.strip().lower()
    # The same command without --dirty gives the id of the commit alone. A dirty mark can be a hex digit ('0'),
    # so the marked id can be a prefix of HEAD too: only the second run tells the two apart.
    clean = _git(git, source, [a for a in tag["command"][1:] if not a.startswith("--dirty")])
    if clean.returncode != 0:
        raise BuildIdError(f"git describe without --dirty in {source} failed: {clean.stderr.strip()}")
    digits = clean.stdout.strip().lower()
    dirty = described != digits
    if dirty and described != digits + tag["dirty_mark"]:
        raise BuildIdError(f"git describe in {source} gave {described!r} with --dirty and {digits!r} without it")
    if not re.fullmatch(r"[0-9a-f]+", described) or not commit.startswith(digits):
        raise BuildIdError(f"git describe in {source} gave {described!r}, which is no prefix of HEAD {commit}")
    if len(described) > MAX_HEX_DIGITS:
        raise BuildIdError(f"build id {described} has more than {MAX_HEX_DIGITS} hex digits: it does not fit "
                           f"the uint64_t of vkd3d_build.h")
    if dirty and not allow_dirty:
        raise BuildIdError(
            f"{source} has changes against {commit[:12]} (git describe marks it '{described}'). Every dirty tree of "
            f"this commit gets the same build id, so the shader cache of one such build serves the next. Commit "
            f"the work (the submodule pointer pins dxil-spirv), or pass -AllowDirtySource for a dev build")
    return {"commit": commit, "build_id": described, "dirty": dirty, "fallback": tag["fallback"],
            "command": tag["command"]}


def read_header(path):
    text = Path(path).read_text(encoding="utf-8")
    m = _HEADER.search(text)
    if not m:
        raise BuildIdError(f"{path} has no 'static const uint64_t vkd3d_build = 0x...;' line")
    return m.group("hex").lower()


def check(header, expected, dll=None, require_in_dll=False):
    built = read_header(header)
    result = {"header": str(header), "build_id": built, "expected": expected["build_id"]}
    if built == expected["fallback"].lower():
        raise BuildIdError(f"{header} holds the vcs_tag fallback 0x{built}: git describe failed inside the build")
    if built != expected["build_id"]:
        raise BuildIdError(f"{header} holds build id 0x{built}, not 0x{expected['build_id']} of the source commit "
                           f"{expected['commit'][:12]}")
    if dll:
        count = Path(dll).read_bytes().count(struct.pack("<Q", int(built, 16)))
        result["dll"] = str(dll)
        result["dll_occurrences"] = count
        if require_in_dll and count == 0:
            raise BuildIdError(f"{dll} holds no 64-bit constant 0x{built}: the DLL is not the build of {header}")
    return result


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("expect")
    e.add_argument("--source", required=True)
    e.add_argument("--git", default="git")
    e.add_argument("--allow-dirty", action="store_true")
    c = sub.add_parser("check")
    c.add_argument("--header", required=True)
    c.add_argument("--expect", required=True, help="a file holding the JSON record that 'expect' printed")
    c.add_argument("--dll")
    c.add_argument("--require-in-dll", action="store_true")
    args = ap.parse_args(argv)
    try:
        if args.cmd == "expect":
            out = expect(args.source, args.git, args.allow_dirty)
        else:
            expected = json.loads(Path(args.expect).read_text(encoding="utf-8-sig"))
            out = check(args.header, expected, args.dll, args.require_in_dll)
    except BuildIdError as exc:
        print(f"vkd3d build id: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
