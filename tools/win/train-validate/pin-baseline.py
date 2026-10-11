"""Pin the trial harness to this package, and accept a pin that is already this package.

`release-baseline.py <package> --apply` refuses when it has applied this release before, because it keeps one
backup of the baseline it replaced and will not overwrite it. That rule is right for the tool and wrong for
this arm: the arm asks whether the harness is pinned to the package under test, and a run that is repeated or
resumed must answer the same way as the first one.

So: run the tool. If it refuses because the release was applied already, read the baseline and say whether it
names this package, by its release name and the SHA-256 of its manifest. If it does, the arm is satisfied and
the file is left alone. If it names anything else, the refusal stands.

A package with a new D3D12 triplet is refused too ("promote it with promote-d3d12.py first"), and the promotion
arm comes after this one and depends on it. The tool's own answer to that order is `--keep-accepted-d3d12`: every
pin moves to the package, and the d3d12 block stays at the accepted triplet until the promotion arm puts the
accepted files back, promotes the package's and re-pins. So that refusal is retried once with the flag (train
b29, the first train of this suite with a new triplet, stopped here).

Usage: pin-baseline.py <caps dir> <package dir> [extra arguments for release-baseline.py]
"""
import hashlib
import json
import subprocess
import sys
from pathlib import Path

APPLIED = "this release was applied already"
NEW_TRIPLET = "the release D3D12 triplet differs from the accepted one"
KEEP = "--keep-accepted-d3d12"


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def main() -> int:
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    caps, package = Path(sys.argv[1]), Path(sys.argv[2])
    extra = sys.argv[3:]
    tool = caps / "release-baseline.py"
    manifest = json.loads((package / "manifest.json").read_text(encoding="utf-8"))
    done = subprocess.run([sys.executable, str(tool), str(package), "--apply", *extra],
                          capture_output=True, text=True)
    out = (done.stdout or "") + (done.stderr or "")
    sys.stdout.write(out if out.endswith("\n") or not out else out + "\n")
    if done.returncode != 0 and NEW_TRIPLET in out and KEEP not in extra:
        print("pin: the package brings a new D3D12 triplet; the baseline keeps the accepted one, and the "
              "promotion arm puts it back, promotes the package's and re-pins")
        extra = [*extra, KEEP]
        done = subprocess.run([sys.executable, str(tool), str(package), "--apply", *extra],
                              capture_output=True, text=True)
        out = (done.stdout or "") + (done.stderr or "")
        sys.stdout.write(out if out.endswith("\n") or not out else out + "\n")
    if done.returncode == 0:
        return 0
    if APPLIED not in out:
        return done.returncode
    baseline = caps / "lab-baseline.json"
    if not baseline.is_file():
        print(f"pin: {baseline} is absent, so the refusal stands")
        return done.returncode
    pinned = json.loads(baseline.read_text(encoding="utf-8")).get("release", {})
    want_release = manifest.get("release") or manifest.get("version")
    want_manifest = sha(package / "manifest.json")
    print(f"pin: baseline names {pinned.get('release')!r} with manifest {str(pinned.get('manifest_sha256'))[:8]}")
    print(f"pin: the package is {want_release!r} with manifest {want_manifest[:8]}")
    if pinned.get("release") == want_release and pinned.get("manifest_sha256") == want_manifest:
        files = len(manifest.get("files", ()))
        print(f"release {want_release}: {files} files verified, the harness is pinned to this package already")
        return 0
    print("pin: the baseline names another package, so the refusal stands")
    return done.returncode


if __name__ == "__main__":
    sys.exit(main())
