"""The adapter directory a game arm names must hold this package's D3D12 shell.

The native harness resolves a session's shell as `scratch/m15/<adapter>/amdgpu_wddm_d3d12.dll` and its default
adapter is the one the train before this one promoted. A session that takes that default on a package whose
shell moved runs the wrong shell and measures nothing: train b27 lost session 526 that way. This check is the
pre step of every game arm, so the mistake costs one second instead of a game session.

Usage: adapter-check.py <package dir> <adapter dir>
"""
import hashlib
import sys
from pathlib import Path

SHELL = "amdgpu_wddm_d3d12.dll"


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    package, adapter = Path(sys.argv[1]), Path(sys.argv[2])
    want = package / "payload" / "d3d12" / SHELL
    have = adapter / SHELL
    for path in (want, have):
        if not path.is_file():
            print(f"adapter check: {path} is absent")
            return 1
    want_sha, have_sha = sha(want), sha(have)
    print(f"package shell {want_sha[:8]} {want}")
    print(f"adapter shell {have_sha[:8]} {have}")
    if want_sha != have_sha:
        print(f"adapter check: {adapter.name} holds another shell than this package: a session on it would "
              f"swap that file into the install and measure the wrong driver")
        return 1
    print(f"adapter check: {adapter.name} is this package's shell")
    return 0


if __name__ == "__main__":
    sys.exit(main())
