"""Reconcile DWM capture boundaries; report map risks, never full G0 acceptance."""
import argparse
import importlib.util
import json
import re
from pathlib import Path

spec = importlib.util.spec_from_file_location("lifetimes", Path(__file__).with_name("analyze-map-lifetimes.py"))
lifetimes = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lifetimes)
require = lifetimes.require


def analyze(text, receipts):
    require(bool(receipts), "missing boundary receipts")
    receipts = sorted(receipts, key=lambda r: int(r["marker"]))
    require(len({r["marker"] for r in receipts}) == len(receipts), "duplicate marker receipt")
    require(len({(r["pid"], r["process_start_utc"]) for r in receipts}) == 1, "mixed process identities")
    labels = [r["label"] for r in receipts]
    require(len(set(labels)) == len(labels), "duplicate boundary label")
    require(labels[0] == "render-start" and labels[-2:] == ["final-capture-start", "final-capture-end"], "missing outer boundaries")
    require("render-end" in labels, "missing render end")
    require(labels.index("render-end") == len(labels)-3, "render/final-capture order")
    markers = [int(r["marker"]) for r in receipts]
    require(markers == list(range(1, len(markers)+1)), "receipt marker gap")
    lines = text.splitlines()
    summary = lifetimes.analyze(lines, allow_live=True, start_marker=markers[0], end_marker=markers[-1])
    rows = []
    by_marker = {}
    for line in lines:
        if lifetimes.PREFIX not in line:
            continue
        row = dict(item.split("=", 1) for item in line.split(lifetimes.PREFIX, 1)[1].split())
        rows.append(row)
        if row["event"] == "checkpoint" and int(row["marker"]):
            marker = int(row["marker"])
            require(marker not in by_marker, "duplicate logged marker")
            by_marker[marker] = (row, line.strip())
            if marker == markers[-1]:
                break
    require(set(by_marker) == set(markers), "log/receipt marker inventory differs")
    sequence = {}
    for receipt in receipts:
        row, line = by_marker[int(receipt["marker"])]
        require(receipt["line"].strip() == line, "receipt differs from recorded acknowledgement")
        require(int(row["pending"]) == 0, "pending request at phase boundary")
        sequence[receipt["label"]] = int(row["seq"])
    require(list(sequence.values()) == sorted(sequence.values()), "boundary sequence reversed")
    captures = []
    opened = None
    for label in labels:
        if label.endswith("-capture-start"):
            require(opened is None, "nested capture")
            opened = label[:-6]
        elif label.endswith("-capture-end"):
            require(opened == label[:-4], "unpaired capture")
            captures.append((sequence[opened+"-start"], sequence[label], opened))
            opened = None
        else:
            require(opened is None, "render boundary inside capture")
    require(opened is None, "unclosed capture")
    require(any(name.startswith("dynamic-") for _, _, name in captures), "no dynamic capture pair")
    begins = {int(r["map"]):r for r in rows if r["event"] == "begin"}
    results = {int(r["map"]):r for r in rows if r["event"] == "result"}
    ends = {int(r["map"]):int(r["seq"]) for r in rows if r["event"] == "end"}
    render_start, render_end = sequence["render-start"], sequence["render-end"]
    def phase(seq):
        for low, high, name in captures:
            if low < seq < high:
                return name
        if render_start < seq < render_end:
            return "render"
        return "outside-render"
    inventory = []
    for mid, begin in begins.items():
        result = results.get(mid, {})
        if result.get("success") != "1":
            continue
        start = int(begin["seq"])
        finish = ends.get(mid, int(rows[-1]["seq"])+1)
        inventory.append(dict(map=mid, target=int(begin["target"]), bind=begin["bind"],
            usage=begin["usage"], write=bool(int(begin["usage"],16)&2),
            phase=phase(start), spans_boundary=any(start < s < finish for s in sequence.values()),
            live=mid in summary["live_map_ids"], resource_id=int(begin["resource_id"]),
            resource=begin["resource"], box=[int(begin[k]) for k in ("box_width","box_height","box_depth")]))
    overflow = re.findall(r"BC250 audit bucket_summary .*? overflow=(\d+)", text)
    require(overflow and all(int(n)==0 for n in overflow), "missing or overflowed aggregate audit")
    return dict(lifetime=summary, boundaries=sequence, maps=inventory,
        render_image_write_ids=[r["map"] for r in inventory if r["phase"] == "render" and r["target"] != 0 and r["write"]],
        boundary_spanning_image_ids=[r["map"] for r in inventory if r["spans_boundary"] and r["target"] != 0],
        live_buffer_ids=[r["map"] for r in inventory if r["live"] and r["target"] == 0],
        scope="map provenance only; capture labels do not attribute individual stores or excuse crossing maps; image correctness, GPU fences/ownership and persistent-pointer writers require separate evidence")


if __name__ == "__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--pid", type=int, required=True)
    args=parser.parse_args()
    receipts=[json.loads(p.read_text(encoding="utf-8-sig")) for p in args.directory.glob("boundary-*.json")]
    require(receipts and all(r["pid"] == args.pid for r in receipts), "requested PID differs from boundaries")
    print(json.dumps(analyze((args.directory/f"dwm-{args.pid}.log").read_text(), receipts), indent=2))
