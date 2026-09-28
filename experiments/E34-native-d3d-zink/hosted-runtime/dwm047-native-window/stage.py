"""Package exact DWM047 candidates locally; no connection or task launch."""
import hashlib
import json
import shutil
from pathlib import Path

source = Path(__file__).resolve().parent
root = source.parents[4]
build = root / "scratch/g0-hosted/dwm047"
out = root / "scratch/g0-hosted/dwm047-package"

def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest().upper()


def main():
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
        "router.dll": (build/"router.dll", "05D6DBF143AE0EFA812022A93DEE016A8F96879D0F6470918BCD4259EE6BC292"),
        "composition-control.exe": (build/"composition-control.exe", "CA44E24E8BEE2B787F23C4D68B76F02E5DE20CBC3C38556852C79F81E0E95645"),
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
