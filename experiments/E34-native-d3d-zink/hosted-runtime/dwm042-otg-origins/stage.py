"""Package exact DWM042 candidates locally; no connection or task launch."""
import hashlib
import json
import shutil
from pathlib import Path

source = Path(__file__).resolve().parent
root = source.parents[4]
build = root / "scratch/g0-hosted/dwm042"
out = root / "scratch/g0-hosted/dwm042-package"

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
        "bc250d3d_zink.dll": (root/"scratch/g0-hosted/ddi-origin001/bc250d3d_zink.dll", "49A44067D2EEC4992456A9F0FC1565A193F26F1024F0210791226AEF377912A5"),
        "vulkan_radeon.dll": (root/"scratch/g0-hosted/hosted-fence001/vulkan_radeon.dll", "C0CE5DCDEB3B8D399FDA0D7548D78C9E93CAAC87C201285549B441DAC59107A0"),
        "router.dll": (build/"router.dll", "47C8182545DFC096B164C472BB0AA06A1798A4988F702AE67AB6B88BB8712836"),
        "composition-control.exe": (build/"composition-control.exe", "5FBB04B8383AF264A5316B09C8A44AB3CD61B827E4138E47A5A89C6DF3BEDF90"),
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
