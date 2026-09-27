"""Package exact DWM039 candidates locally; no connection or task launch."""
import hashlib
import json
import shutil
from pathlib import Path

source = Path(__file__).resolve().parent
root = source.parents[4]
build = root / "scratch/g0-hosted/dwm039"
out = root / "scratch/g0-hosted/dwm039-package"

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
        "bc250d3d_zink.dll": (root/"scratch/g0-hosted/map-checkpoint004/bc250d3d_zink.dll", "CC82A2D9BAD5534D54B1460B1FF49A1F83ED40CADF8694A5F7C1F87CB02386FD"),
        "vulkan_radeon.dll": (root/"scratch/g0-hosted/package053/vulkan_radeon.dll", "3508416F7FB6367BC7345970C22603E03DA963A01F4B5C0A1DF1AAAC2D90CF71"),
        "router.dll": (build/"router.dll", "465F0219BC0F427F0F4A9C9A538662A3D8E72630227AF1A20A2B78C7AD98A9AD"),
        "composition-control.exe": (build/"composition-control.exe", "6B1884247E9F5DAFF8D81558B1DCD44CAA6AB0F257FB3B512C9797D270210202"),
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
