"""Package exact DWM048 candidates locally; no connection or task launch."""
import argparse
import hashlib
import json
import shutil
from pathlib import Path

source = Path(__file__).resolve().parent
root = source.parents[4]
build = root / "scratch/g0-hosted/dwm048"
default_out = root / "scratch/g0-hosted/dwm048-package"

def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest().upper()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, default=default_out)
    args = parser.parse_args()
    out = args.out.resolve()
    if not out.is_relative_to(root.resolve() / "scratch"):
        raise SystemExit("Package must stay within workspace scratch")
    if out.exists():
        raise SystemExit("Package exists; preserve it and inspect the original attempt")
    receipt = json.loads((build / "build-receipt.json").read_text())
    for name in ("router.cpp", "composition-control.cpp", "shared-log.h", "build.cmd"):
        if sha(source/name) != receipt["source_sha256"][name].upper():
            raise SystemExit("Rebuild required: " + name)
    artifacts = {
        "gpu-window-control.exe": (root/"scratch/g0-hosted/gpu-window-control002/gpu-window-control.exe", "63F8F1BC53AA1152895510F898AEFD08D93A4ACC133D15D638F46CFF7441C496"),
        "bc250d3d_zink.dll": (root/"scratch/g0-hosted/present-count001/bc250d3d_zink.dll", "92697AE5E531BE654BB4A4C97CA4F981D0DF53BDE63F570E2F0DD0039820455B"),
        "vulkan_radeon.dll": (root/"scratch/g0-hosted/hosted-fence001/vulkan_radeon.dll", "C0CE5DCDEB3B8D399FDA0D7548D78C9E93CAAC87C201285549B441DAC59107A0"),
        "router.dll": (build/"router.dll", "0D86F7669A5619D8C37190D1D7357890BA71B9D719D238C0D9CFE92FC95E2504"),
        "composition-control.exe": (build/"composition-control.exe", "63895CA7CD1606D76B13A33255130E114F10251C031542BC0122EC319EDFA244"),
    }
    for path, expected in artifacts.values():
        if sha(path) != expected:
            raise SystemExit("Artifact mismatch: " + path.name)
    out.mkdir()
    for path in source.glob("*.ps1"):
        if not path.name.startswith("test-"):
            shutil.copyfile(path, out/path.name)
    shutil.copyfile(source/"etw-providers.txt", out/"etw-providers.txt")
    for name, (path, expected) in artifacts.items():
        shutil.copyfile(path, out/name)
        if sha(out/name) != expected:
            raise SystemExit("Copied artifact mismatch: " + name)
    manifest = {p.name: sha(p) for p in sorted(out.iterdir())}
    (out/"manifest.json").write_text(json.dumps(manifest, indent=2)+"\n")
    print(json.dumps(dict(package=str(out), files=len(manifest), manifest_sha256=sha(out/"manifest.json"), lab_used=False), indent=2))


if __name__ == "__main__":
    main()
