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
out = root / "scratch/g0-hosted/audit-client001"
if out.exists():
    raise SystemExit("Stage exists; inspect the original attempt")

def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest().upper()

artifacts = {
    "bc250d3d_zink.dll": (root/"scratch/g0-hosted/map-checkpoint004/bc250d3d_zink.dll", "CC82A2D9BAD5534D54B1460B1FF49A1F83ED40CADF8694A5F7C1F87CB02386FD"),
    "vulkan_radeon.dll": (root/"scratch/g0-hosted/package053/vulkan_radeon.dll", "3508416F7FB6367BC7345970C22603E03DA963A01F4B5C0A1DF1AAAC2D90CF71"),
    "runtime-audit-control.exe": (root/"scratch/g0-hosted/runtime-audit-control/runtime-audit-control.exe", "67D231A41A125E55068E07439E5A69B5560FC10CBB78BEC7E2BFA15A56E044E2"),
}
for path, expected in artifacts.values():
    if sha(path) != expected:
        raise SystemExit(f"Input hash mismatch: {path.name}")
router = root/"scratch/g0-hosted/audit-client001-build/router.dll"
if not router.is_file():
    raise SystemExit("Build the router first")
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
