"""Partition the complete pinned vk-default list; never filter or reorder cases."""
import argparse
import hashlib
import itertools
import json
from pathlib import Path
from inventory_cts_mustpass import inventory


def prepare(source, output, batch_size):
    if batch_size < 1:
        raise ValueError("Batch size must be positive")
    identity = inventory(source)
    root = source / "external/vulkancts/mustpass/main"
    output.mkdir(parents=True, exist_ok=False)

    def cases(relative):
        with (root / relative).open(encoding="utf-8-sig") as stream:
            for line in stream:
                name = line.strip()
                if not name or name.startswith("#"):
                    continue
                if name.endswith(".txt"):
                    yield from cases(name)
                else:
                    yield name

    stream = cases("vk-default.txt")
    batches = []
    total = 0
    expected_digest = hashlib.sha256()
    while True:
        rows = list(itertools.islice(stream, batch_size))
        if not rows:
            break
        raw = ("\n".join(rows) + "\n").encode("utf-8")
        expected_digest.update(raw)
        name = f"batch-{len(batches) + 1:05d}.txt"
        (output / name).write_bytes(raw)
        batches.append({"file": name, "count": len(rows), "first": rows[0], "last": rows[-1],
                        "sha256": hashlib.sha256(raw).hexdigest()})
        total += len(rows)
    if total != identity["configuration_case_counts"]["vk-default.txt"]:
        raise ValueError("Partition lost cases")
    # Read artifacts back and compare the complete ordered byte stream.
    actual_digest = hashlib.sha256()
    for batch in batches:
        raw = (output / batch["file"]).read_bytes()
        if hashlib.sha256(raw).hexdigest() != batch["sha256"]:
            raise ValueError("Batch content mismatch")
        actual_digest.update(raw)
    if actual_digest.digest() != expected_digest.digest():
        raise ValueError("Partition order mismatch")
    manifest = {
        "status": "prepared_not_run", "source_commit": identity["source_commit"],
        "configuration": "vk-default", "case_count": total, "batch_size": batch_size,
        "ordered_cases_sha256": actual_digest.hexdigest(), "batches": batches,
        "required_arguments": ["--deqp-terminate-on-fail=enable", "--deqp-terminate-on-device-lost=enable",
                               "--deqp-watchdog=enable"],
        "note": "Complete selection, not --deqp-fraction. Runtime must also enforce owner STOP, health and per-case deadline. No runtime validation yet.",
    }
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    (output / "upstream-inventory.json").write_text(json.dumps(identity, indent=2) + "\n", encoding="utf-8")
    return total, len(batches)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--batch-size", type=int, default=1000)
    args = parser.parse_args()
    print(prepare(args.source, args.output, args.batch_size))
