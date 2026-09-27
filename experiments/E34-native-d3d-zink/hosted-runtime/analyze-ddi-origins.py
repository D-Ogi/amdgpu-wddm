"""Correlate complete DDI scopes with instrumented Zink maps in one DLL/process."""
import argparse
import importlib.util
import json
from pathlib import Path

_spec = importlib.util.spec_from_file_location("lifetimes", Path(__file__).with_name("analyze-map-lifetimes.py"))
lifetimes = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(lifetimes)
require = lifetimes.require
DDI = "BC250 audit ddi "
MAP = "BC250 audit lifetime "
BASE = set("event id tid time_ns".split())
DETAIL = set("origin ctx resource level usage x y z box_width box_height box_depth".split())
MATCH = "ctx resource level usage x y z box_width box_height box_depth".split()


def fields(line, prefix):
    pairs = [x.split("=", 1) for x in line[len(prefix):].split()]
    row = dict(pairs)
    require(len(row) == len(pairs), "duplicate fields")
    for k,v in list(row.items()):
        if k not in ("event", "origin"):
            row[k] = int(v, 16 if k in ("ctx", "resource", "usage", "bind") else 10)
    return row


def analyze(text):
    lines = text.splitlines()
    lifetime = lifetimes.analyze(lines, allow_live=True)
    scopes = {}; stacks = {}; maps = {}; unknown = []
    for line in lines:
        if line.startswith(DDI):
            r = fields(line, DDI); event = r["event"]
            require(event in ("begin", "copy_complete", "end"), "unknown DDI event")
            require(set(r) == BASE | (DETAIL if event == "begin" else set()), "DDI schema mismatch")
            require(r["id"] > 0 and r["tid"] > 0 and r["time_ns"] > 0, "invalid DDI identity/time")
            stack = stacks.setdefault(r["tid"], [])
            if event == "begin":
                require(r["id"] not in scopes, "reused DDI identity")
                require(r["origin"] in ("initial_data", "resource_map", "update_subresource"), "unknown DDI origin")
                scopes[r["id"]] = dict(begin=r, maps=[], copied=False)
                stack.append(r["id"])
            else:
                require(stack and stack[-1] == r["id"], "orphan or nonnested DDI event")
                scope = scopes[r["id"]]
                require(r["time_ns"] >= scope["begin"]["time_ns"], "DDI time reversed")
                require(len(scope["maps"]) == 1, "DDI scope needs one direct map")
                mid = scope["maps"][0]; mapping = maps[mid]
                require("result" in mapping, "DDI scope missing map result")
                require(r["time_ns"] >= mapping["result"]["time_ns"], "DDI event precedes map result")
                if event == "copy_complete":
                    require(not scope["copied"] and scope["begin"]["origin"] != "resource_map", "invalid copy witness")
                    require(mapping["result"]["success"] == 1, "copy after failed map")
                    scope["copied"] = True
                    scope["copy_time_ns"] = r["time_ns"]
                else:
                    if scope["begin"]["origin"] != "resource_map":
                        require(scope["copied"] == bool(mapping["result"]["success"]), "missing or false copy witness")
                    require(r["time_ns"] >= scope.get("copy_time_ns",0), "end before copy")
                    scope["end_time_ns"] = r["time_ns"]
                    stack.pop()
        elif line.startswith(MAP):
            r = fields(line, MAP)
            if r["event"] == "begin":
                require(r.get("tid",0) > 0, "threaded map required")
                stack = stacks.get(r["tid"], [])
                sid = stack[-1] if stack else None
                if sid is not None and all(r[k] == scopes[sid]["begin"][k] for k in MATCH):
                    require(r["time_ns"] >= scopes[sid]["begin"]["time_ns"], "map before DDI")
                    scopes[sid]["maps"].append(r["map"])
                else:
                    sid = None; unknown.append(r["map"])
                maps[r["map"]] = dict(begin=r, scope=sid)
            elif r["event"] == "result":
                maps[r["map"]]["result"] = r
    require(scopes and all(not s for s in stacks.values()), "missing or unfinished DDI scopes")
    require(sorted(scopes) == list(range(1,len(scopes)+1)), "missing DDI sequence")
    return dict(lifetime=lifetime, scopes=list(scopes.values()),
        unmatched_map_ids=unknown, matched_map_count=sum(m["scope"] is not None for m in maps.values()),
        copy_complete_count=sum(s["copied"] for s in scopes.values()),
        scope="Direct same-thread map attribution and completed frontend copy calls only; unmatched paths, external writes through ResourceMap and persistent-pointer stores remain unproven.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    args = parser.parse_args()
    print(json.dumps(analyze(args.log.read_text()), indent=2))
