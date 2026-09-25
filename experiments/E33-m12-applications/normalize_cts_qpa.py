"""Normalize QPA files with the pinned upstream parser (Apache-2.0).

Provenance: KhronosGroup/VK-GL-CTS scripts/log/log_parser.py, loaded in place.
Original QPA files remain the evidence; JSONL is a derived comparison input.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
from inventory_cts_mustpass import PIN


def normalize(source, inputs, output):
    commit = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    if commit != PIN:
        raise ValueError("Unexpected CTS revision")
    relative = "scripts/log/log_parser.py"
    subprocess.run(["git", "-C", str(source), "diff", "--exit-code", "HEAD", "--", relative], check=True)
    spec = importlib.util.spec_from_file_location("cts_upstream_parser", source / relative)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    count = 0
    with output.open("x", encoding="utf-8", newline="\n") as destination:
        for argument in inputs:
            paths = sorted(argument.rglob("*.qpa")) if argument.is_dir() else [argument]
            if not paths:
                raise ValueError(f"No QPA files in {argument}")
            for path in paths:
                parser = module.BatchResultParser()
                parser.init(str(path))
                with path.open("rb") as stream:
                    while True:
                        result = parser.getNextTestCaseResult(stream)
                        if result is None:
                            break
                        count += 1
                        destination.write(json.dumps({"case": result.name, "status": result.statusCode}) + "\n")
    if not count:
        raise ValueError("No CTS results found")
    return count


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("inputs", nargs="+", type=Path)
    args = parser.parse_args()
    print(f"{normalize(args.source, args.inputs, args.output)} normalized results")
