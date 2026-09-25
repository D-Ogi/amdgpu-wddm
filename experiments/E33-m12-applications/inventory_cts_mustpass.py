"""Inventory pinned upstream CTS lists without generating or filtering cases."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

PIN = "f6a29701220f34dd1407513bfe80d74ca7b392ce"


def inventory(source):
    commit = subprocess.check_output(
        ["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    if commit != PIN:
        raise ValueError("Expected vulkan-cts-1.4.6.2 source commit")
    subprocess.run(["git", "-C", str(source), "diff", "--exit-code", "HEAD",
                    "--", "external/vulkancts/mustpass/main"],
                   check=True, stdout=subprocess.DEVNULL)
    root = source / "external/vulkancts/mustpass/main"
    files = {}

    def visit(relative, stack):
        path = (root / relative).resolve()
        path.relative_to(root.resolve())
        if path in stack:
            raise ValueError("Recursive case-list include")
        raw = path.read_bytes()
        # Git checkouts may use CRLF; identity is canonical UTF-8/LF.
        text = raw.decode("utf-8-sig").replace("\r\n", "\n")
        count = 0
        children = []
        for line in text.splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("dEQP-VK."):
                count += 1
            elif line.endswith(".txt"):
                children.append(line)
                count += visit(line, stack + [path])
            else:
                raise ValueError(f"Unrecognised list entry: {relative}: {line}")
        files[relative] = {
            "canonical_sha256": hashlib.sha256(text.encode()).hexdigest(),
            "expanded_case_count": count,
            "includes": children,
        }
        return count

    configurations = {name: visit(name, []) for name in
                      ("vk-default.txt", "vk-fraction-mandatory-tests.txt")}
    return {
        "release": "vulkan-cts-1.4.6.2", "source_commit": commit,
        "status": "inventory_only_not_execution_or_conformance",
        "configuration_case_counts": configurations,
        "note": "Configurations are separate; counts must not be summed as unique cases. No additional exclusions.",
        "files": dict(sorted(files.items())),
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    result = inventory(args.source)
    # Preserve existing inventories as evidence of their original inputs.
    with args.output.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(result, stream, indent=2)
        stream.write("\n")
    print(json.dumps(result["configuration_case_counts"]))
    print(f"{len(result['files'])} source lists hashed")
