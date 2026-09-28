"""Validate opt-in uploader events; counts ranges and helper copies, not all CPU stores."""
import argparse
import json
import re
from pathlib import Path

PREFIX = "BC250 audit upload "
NUMBERS = ("created_ns", "time_ns", "generation", "capacity", "offset", "size", "allocations", "requested_bytes", "copy_bytes")


def analyze(text, allow_live=False):
    managers = {}
    events = 0
    for line_number, line in enumerate(text.splitlines(), 1):
        if PREFIX not in line:
            continue
        if not line.startswith(PREFIX):
            raise ValueError(f"Interleaved event at line {line_number}")
        pairs = re.findall(r"(\w+)=([^ ]+)", line[len(PREFIX):])
        row = dict(pairs)
        required = set(NUMBERS) | {"event", "manager", "pipe", "resource", "bind", "map_flags"}
        if set(row) != required or len(pairs) != len(required):
            raise ValueError(f"Malformed event at line {line_number}")
        for name in NUMBERS:
            row[name] = int(row[name])
            if row[name] < 0:
                raise ValueError("Negative counter")
        for name in ("manager", "pipe", "resource", "bind", "map_flags"):
            row[name] = int(row[name], 16)
        event = row["event"]
        key = (row["manager"], row["created_ns"])
        if not key[0] or not key[1] or row["time_ns"] < key[1]:
            raise ValueError("Invalid manager identity/clock")
        if event == "create":
            if key in managers or row["generation"] or row["capacity"] or row["resource"] or row["offset"] or row["size"]:
                raise ValueError("Duplicate or invalid create")
            managers[key] = dict(manager=row["manager"], created_ns=key[1], pipe=row["pipe"], bind=row["bind"], time_ns=row["time_ns"], generation=0, allocations=0, requested_bytes=0, copy_bytes=0, live_buffer=False, destroyed=False, capacity=0, resource=0, highwater=0, last_alloc=None, events=0)
        if key not in managers:
            raise ValueError("Missing manager creation")
        state = managers[key]
        if state["destroyed"] or row["time_ns"] < state["time_ns"] or row["pipe"] != state["pipe"] or row["bind"] != state["bind"]:
            raise ValueError("Invalid manager lifetime/clock/identity")
        if event == "map":
            if state["live_buffer"] or row["generation"] != state["generation"] + 1 or not row["resource"] or not row["capacity"] or row["offset"] or row["size"] != row["capacity"]:
                raise ValueError("Invalid buffer generation/map")
            state.update(generation=row["generation"], resource=row["resource"], capacity=row["capacity"], highwater=0, live_buffer=True, last_alloc=None)
        elif event in ("alloc", "copy"):
            if not state["live_buffer"] or row["resource"] != state["resource"] or row["capacity"] != state["capacity"] or not row["size"] or row["offset"] + row["size"] > state["capacity"]:
                raise ValueError("Invalid allocation range/resource")
            allocation = (row["generation"], row["offset"], row["size"])
            if event == "alloc":
                if row["offset"] < state["highwater"]:
                    raise ValueError("Reused allocation range without new map")
                state["highwater"] = row["offset"] + row["size"]
                state["allocations"] += 1
                state["requested_bytes"] += row["size"]
                state["last_alloc"] = allocation
            else:
                if allocation != state["last_alloc"]:
                    raise ValueError("Copy has no matching uncopied allocation")
                state["copy_bytes"] += row["size"]
                state["last_alloc"] = None
        elif event == "release":
            if row["size"]:
                raise ValueError("Invalid release size")
            if state["live_buffer"] and (row["resource"] != state["resource"] or row["capacity"] != state["capacity"] or row["offset"] != state["highwater"]):
                raise ValueError("Release differs from observed buffer/ranges")
            if not state["live_buffer"] and row["capacity"]:
                raise ValueError("Release of unobserved buffer")
            state.update(live_buffer=False, capacity=0, last_alloc=None)
        elif event == "destroy":
            if state["live_buffer"] or row["capacity"] or row["offset"] or row["size"]:
                raise ValueError("Destroyed manager retains a buffer")
            state["destroyed"] = True
        elif event != "create":
            raise ValueError("Unknown event")
        for name in ("generation", "allocations", "requested_bytes", "copy_bytes"):
            if row[name] != state[name]:
                raise ValueError(f"Missing/incorrect event: {name} at line {line_number}")
        state["time_ns"] = row["time_ns"]
        state["events"] += 1
        events += 1
    if not managers:
        raise ValueError("No uploader evidence")
    closed = all(s["destroyed"] for s in managers.values())
    if not closed and not allow_live:
        raise ValueError("Missing terminal manager records; use --allow-live for an explicit prefix only")
    return dict(scope=__doc__, events=events, all_observed_managers_closed=closed, lifetime_coverage="closed observed managers" if closed else "observed prefix only", allocations=sum(s["allocations"] for s in managers.values()), requested_bytes=sum(s["requested_bytes"] for s in managers.values()), helper_copy_bytes=sum(s["copy_bytes"] for s in managers.values()), managers=list(managers.values()), limitations=["Cannot detect a manager absent from the entire log.", "Does not count later writes through returned pointers or copies outside this helper."])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--allow-live", action="store_true")
    args = parser.parse_args()
    print(json.dumps(analyze(args.log.read_text(encoding="utf-8-sig"), args.allow_live), indent=2))
