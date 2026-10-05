"""Inspect ETW lifetimes for a measured Present VA pair; never infer residency from mapping."""
import argparse
import csv
import hashlib
import json
import re
from pathlib import Path

ap = argparse.ArgumentParser()
ap.add_argument("trace", type=Path)
ap.add_argument("out", type=Path)
ap.add_argument("--pid", type=int, required=True)
ap.add_argument("--source-va", type=lambda s: int(s, 0), required=True)
ap.add_argument("--destination-va", type=lambda s: int(s, 0), required=True)
ap.add_argument("--bytes", type=int, required=True)
ap.add_argument("--at-us", type=int, required=True)
ap.add_argument("--until-us", type=int, required=True)
a = ap.parse_args()
assert not a.out.exists(), "Preserve previous evidence"
assert 0 < a.at_us < a.until_us and a.bytes > 0
names = {"GpuVirtualAddressRangeMapping", "GpuVirtualAddressAllocator", "DeviceAllocation",
         "AdapterAllocation", "VidMmMakeResident", "EvictAllocation", "Context"}
headers = {}
events = []
inside = False
loss = None
with a.trace.open(encoding="utf-8-sig", newline="") as f:
    for line, raw in enumerate(csv.reader(f), 1):
        if not raw:
            continue
        row = [x.strip() for x in raw]
        name = row[0]
        if name == "BeginHeader":
            inside = True
            continue
        if name == "EndHeader":
            inside = False
            continue
        if inside:
            headers.setdefault(name, []).append(row)
            continue
        m = re.search(r"Events Lost: (\d+).*Buffers lost: (\d+)", ",".join(row))
        if m:
            loss = [int(m[1]), int(m[2])]
        bits = name.split("/")
        if len(bits) != 3 or bits[1] not in names or not row[1].isdigit():
            continue
        candidates = [h for h in headers[name] if len(h) == len(row)]
        assert len(candidates) == 1, (line, name, "ambiguous schema")
        e = dict(zip(candidates[0][1:], row[1:]))
        e.update(kind=bits[1], op=bits[2], time=int(row[1]), line=line)
        events.append(e)
assert loss == [0, 0], loss
events.sort(key=lambda e: (e["time"], e["line"]))

def number(v):
    return int(v, 0)

def lifetime(kind, key):
    """Chronological generations; pre-trace stops do not invent starts."""
    live = {}
    generations = []
    for e in events:
        if e["kind"] != kind:
            continue
        k = tuple(e[x] for x in key)
        if e["op"] == "win:Start":
            assert k not in live, (kind, k, "overlapping generations")
            live[k] = e
        elif e["op"] == "win:Stop":
            start = live.pop(k, None)
            if start:
                generations.append((start, e))
    generations.extend((e, None) for e in live.values())
    return generations

def active(g, t):
    return g[0]["time"] <= t and (g[1] is None or t < g[1]["time"])

maps = lifetime("GpuVirtualAddressRangeMapping", ["pVaAllocator", "pOwner", "StartAddress", "EndAddress"])
allocators = lifetime("GpuVirtualAddressAllocator", ["pVaAllocator"])
devices = lifetime("DeviceAllocation", ["hDevice", "hVidMmAlloc"])
globals_ = lifetime("AdapterAllocation", ["hVidMmGlobalAlloc"])
contexts = lifetime("Context", ["hContext"])
result = {"trace_sha256": hashlib.sha256(a.trace.read_bytes()).hexdigest(), "trace_loss": loss,
          "interval_us": [a.at_us, a.until_us], "sides": [],
          "scope": "Mapping/lifetime witnesses and explicit residency events only. No mapping-to-residency implication; no proof of future GPU retirement. KMD-private handles are not ETW handles."}
for role, va in [("source", a.source_va), ("destination", a.destination_va)]:
    hits = []
    for g in maps:
        e = g[0]
        if not active(g, a.at_us) or number(e["StartAddress"]) != va or number(e["EndAddress"]) < va + a.bytes:
            continue
        owners = [x for x in allocators if active(x, e["time"]) and x[0]["pVaAllocator"] == e["pVaAllocator"]]
        if len(owners) == 1 and number(owners[0][0]["hProcessId"]) == a.pid:
            hits.append((g, owners[0]))
    assert len(hits) == 1, (role, "mapping ambiguity", len(hits))
    mapping, allocator = hits[0]
    ds = [g for g in devices if active(g, a.at_us) and g[0]["hVidMmAlloc"] == mapping[0]["pOwner"]]
    assert len(ds) == 1, (role, "device allocation ambiguity", len(ds))
    device = ds[0]
    gs = [g for g in globals_ if active(g, a.at_us) and g[0]["hVidMmGlobalAlloc"] == device[0]["hVidMmGlobalAlloc"]]
    assert len(gs) == 1, (role, "global allocation ambiguity", len(gs))
    global_ = gs[0]
    end = device[1]["time"] if device[1] else float("inf")
    residence = [e for e in events if e["kind"] == "VidMmMakeResident" and e["pVidMmAlloc"] == device[0]["hVidMmAlloc"] and device[0]["time"] <= e["time"] < end]
    evictions = [e for e in events if e["kind"] == "EvictAllocation" and e["hGlobalAllocationHandle"] == global_[0]["hVidMmGlobalAlloc"] and global_[0]["time"] <= e["time"] < (global_[1]["time"] if global_[1] else float("inf"))]
    ctx = [g for g in contexts if active(g, a.at_us) and g[0]["hDevice"] == device[0]["hDevice"]]
    result["sides"].append(dict(role=role, va=va, mapping=mapping, allocator=allocator,
        device_allocation=device, global_allocation=global_, contexts=ctx,
        residency_events=residence, eviction_events=evictions,
        mapping_and_allocations_cover_interval=all(active(g, a.until_us) for g in [mapping, device, global_]),
        positive_residency_event_before_interval=any(e["time"] <= a.at_us and number(e["ResidencyCount"]) > 0 for e in residence)))
result["same_device"] = result["sides"][0]["device_allocation"][0]["hDevice"] == result["sides"][1]["device_allocation"][0]["hDevice"]
a.out.parent.mkdir(parents=True, exist_ok=True)
a.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
print(json.dumps({"same_device": result["same_device"], "sides": [{k:s[k] for k in ["role", "va", "mapping_and_allocations_cover_interval", "positive_residency_event_before_interval"]} for s in result["sides"]]}))
