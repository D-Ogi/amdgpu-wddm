"""Build-independent local packaging. Does not connect to the lab."""
import argparse
import hashlib
import json
import shutil
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[5])
args = parser.parse_args()
root = args.root.resolve()
source = Path(__file__).resolve().parent
out = root / "scratch/g0-hosted/audit-client004"
if out.exists():
    raise SystemExit("Stage exists; inspect the original attempt")

def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest().upper()

artifacts = {
    "bc250d3d_zink.dll": (root/"scratch/g0-hosted/store-census002/bc250d3d_zink.dll", "B514FF61A0EEAD7E3F7D6225B51AE42AD01D4C9BDA571E9EC7C4CFB446B4C01D"),
    "vulkan_radeon.dll": (root/"scratch/g0-hosted/hosted-fence001/vulkan_radeon.dll", "C0CE5DCDEB3B8D399FDA0D7548D78C9E93CAAC87C201285549B441DAC59107A0"),
    "runtime-audit-control.exe": (root/"scratch/g0-hosted/runtime-audit-update/runtime-audit-control.exe", "88DC1A7328C6D1CAA46433C9746AF4545EB1F2B0D5A5E6A930167F8D25B1AB93"),
}
for path, expected in artifacts.values():
    if sha(path) != expected:
        raise SystemExit(f"Input hash mismatch: {path.name}")
router = root/"scratch/g0-hosted/audit-client004-build/router.dll"
if not router.is_file():
    raise SystemExit("Build the router first")
if sha(router) != "3A69B1DB84395DB46385F7E6AA8B4E02ADDB5A408CCCC345E97FFEF5C0584138":
    raise SystemExit("Router hash mismatch")
out.mkdir()
for path in source.glob("*.ps1"):
    if path.name != "build.ps1":
        shutil.copy2(path, out/path.name)
for name,(path,_) in artifacts.items():
    shutil.copy2(path, out/name)
shutil.copy2(router, out/router.name)
manifest = {path.name:sha(path) for path in sorted(out.iterdir())}
(out/"manifest.json").write_text(json.dumps(manifest, indent=2)+"\n")
print(json.dumps({"stage":str(out), "manifest_sha256":sha(out/"manifest.json"), "router_sha256":manifest["router.dll"], "files":len(manifest)},indent=2))
