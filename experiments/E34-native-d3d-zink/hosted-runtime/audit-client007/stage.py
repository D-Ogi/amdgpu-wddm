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
out = root / "scratch/g0-hosted/audit-client007"
if out.exists():
    raise SystemExit("Stage exists; inspect the original attempt")

def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest().upper()

artifacts = {
    "bc250d3d_zink.dll": (root/"scratch/g0-hosted/present-count001/bc250d3d_zink.dll", "92697AE5E531BE654BB4A4C97CA4F981D0DF53BDE63F570E2F0DD0039820455B"),
    "vulkan_radeon.dll": (root/"scratch/g0-hosted/hosted-fence001/vulkan_radeon.dll", "C0CE5DCDEB3B8D399FDA0D7548D78C9E93CAAC87C201285549B441DAC59107A0"),
    "runtime-audit-control.exe": (root/"scratch/g0-hosted/runtime-audit-texture/runtime-audit-control.exe", "546CC76ADA83BE042A2A979C0378C94230167C3BF76269056F2EBB3BC09E04B3"),
}
for path, expected in artifacts.values():
    if sha(path) != expected:
        raise SystemExit(f"Input hash mismatch: {path.name}")
router = root/"scratch/g0-hosted/audit-client007-build/router.dll"
if not router.is_file():
    raise SystemExit("Build the router first")
if sha(router) != "92895C64A620FAB1DAFF2BB2DFA2CF4879328C1258C806E13B90AD1FAC526B77":
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
