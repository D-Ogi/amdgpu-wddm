"""Validate current status-map inputs without requiring a package to rewrite its source commit."""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import sys


def evaluate_outputs(profile: str, repo: Path, outputs: dict[Path, str], out: Path) -> int:
    if profile not in ("Full", "KmdPackage"):
        raise ValueError("unknown quality profile")
    stale = [path.relative_to(repo).as_posix() for path, text in outputs.items()
             if not path.exists() or path.read_text(encoding="utf-8").replace("\r\n", "\n") != text]
    snapshots = []
    if profile == "KmdPackage":
        for path, text in outputs.items():
            relative = path.relative_to(repo)
            target = out / "generated" / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            data = text.encode("utf-8")
            target.write_bytes(data)
            snapshots.append({"path": relative.as_posix(), "sha256": hashlib.sha256(data).hexdigest()})
    code = 1 if profile == "Full" and stale else 0
    receipt = {
        "profile": profile,
        "status": "FAIL" if code else "PASS",
        "tracked_output_freshness": "required" if profile == "Full" else "profile-exclusion",
        "reason": "Package checks current source and headers; tracked presentation files are not build inputs.",
        "different_tracked_outputs": stale,
        "snapshots": snapshots,
    }
    out.mkdir(parents=True, exist_ok=True)
    (out / "result.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    for name in stale:
        print(f"status-map {profile}: tracked output differs: {name}")
    print(f"status-map {profile}: {'FAIL' if code else 'PASS'}, {len(outputs)} outputs regenerated")
    return code


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=("Full", "KmdPackage"), default="Full")
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--kits", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    repo = args.repo.resolve()
    spec = importlib.util.spec_from_file_location("kmd_status_generator", repo / "tools/docs/status_map.py")
    generator = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = generator
    spec.loader.exec_module(generator)
    # This is the production parser and renderer. Header/source errors remain fatal in both profiles.
    outputs = generator.build(args.kits.resolve())
    return evaluate_outputs(args.profile, repo, outputs, args.out.resolve())


if __name__ == "__main__":
    sys.exit(main())
