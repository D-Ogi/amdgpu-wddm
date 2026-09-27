"""Summarize map coverage without claiming an exhaustive CPU-copy proof."""
import argparse
import json
import re
from pathlib import Path


def classify(text):
    rows = []
    totals = {}
    overflow = {}
    for line in text.splitlines():
        if not line.startswith(("BC250 audit maps ", "BC250 audit bucket_summary ", "BC250 audit bucket ")):
            continue
        fields = dict(re.findall(r"(\w+)=([^ ]+)", line))
        key = (fields["ctx"], int(fields["sample"]))
        if line.startswith("BC250 audit maps "):
            totals[key] = {k: int(fields[k]) for k in ("image_calls", "buffer_calls", "persistent_calls")}
        elif line.startswith("BC250 audit bucket_summary "):
            overflow[key] = int(fields["overflow"])
        else:
            row = {k: int(fields[k]) for k in ("id", "target", "runtime", "user_ptr", "calls", "bytes")}
            row.update(ctx=key[0], sample=key[1], usage=int(fields["usage"], 16), bind=int(fields["bind"], 16), size=fields["size"])
            rows.append(row)
    if not totals or set(totals) != set(overflow):
        raise ValueError("Missing map totals or overflow witness")
    latest = {}
    runtime_observed = False
    userptr_observed = False
    persistent_image_observed = False
    for key, total in totals.items():
        buckets = [r for r in rows if (r["ctx"], r["sample"]) == key]
        if len({r["id"] for r in buckets}) != len(buckets):
            raise ValueError("Duplicate bucket")
        if overflow[key] or sum(r["calls"] for r in buckets if r["target"] != 0) != total["image_calls"] or sum(r["calls"] for r in buckets if r["target"] == 0) != total["buffer_calls"]:
            raise ValueError("Incomplete map-call coverage")
        persistent = [r for r in buckets if r["usage"] & 256 or r["user_ptr"]]
        if sum(r["calls"] for r in persistent) != total["persistent_calls"]:
            raise ValueError("Incomplete persistent-map coverage")
        runtime_observed |= any(r["runtime"] for r in buckets)
        userptr_observed |= any(r["user_ptr"] for r in buckets)
        persistent_image_observed |= any(r["target"] != 0 for r in persistent)
        if key[0] not in latest or key[1] > latest[key[0]]["sample"]:
            latest[key[0]] = dict(sample=key[1], persistent=persistent)
    return dict(scope="Recorded map requests only; signatures do not prove allocation identity or CPU-store absence.", snapshots=len(totals), runtime_resource_map_observed=runtime_observed, user_pointer_map_observed=userptr_observed, persistent_image_map_observed=persistent_image_observed, contexts=latest)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    args = parser.parse_args()
    print(json.dumps(classify(args.log.read_text(encoding="utf-8")), indent=2))
