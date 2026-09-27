"""Check one process/DLL lifetime trace; this does not measure CPU stores."""
import argparse
import json
from pathlib import Path

PREFIX = "BC250 audit lifetime "
FIELDS = {
    "begin": "event map time_ns ctx resource resource_id object_id target width height depth format bind level usage user_ptr runtime x y z box_width box_height box_depth",
    "result": "event map time_ns success resource resource_id object_id usage staging stride layer_stride",
    "end": "event map time_ns",
    "checkpoint": "event seq time_ns marker requests successful failed ended pending live",
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def analyze(lines, allow_live=False, end_marker=None, start_marker=None):
    maps = {}
    events = 0
    sequenced = None
    last_time = 0
    checkpoints = []
    counts = dict(requests=0, successful=0, failed=0, ended=0)
    start = None
    stopped = False
    if start_marker is not None:
        require(end_marker is not None and 0 < start_marker < end_marker, "invalid marker interval")
    for number, line in enumerate(lines, 1):
        if PREFIX not in line:
            continue
        items = line.split(PREFIX, 1)[1].split()
        try:
            pairs = [item.split("=", 1) for item in items]
            row = dict(pairs)
            event = row["event"]
            require(len(row) == len(pairs), "duplicate fields")
            require(event in FIELDS, "unknown/invalid event")
            schema = set(FIELDS[event].split())
            has_sequence = "seq" in row
            if has_sequence:
                schema.add("seq")
            require(set(row) == schema, "missing/unknown fields")
            if sequenced is None:
                sequenced = has_sequence
            require(has_sequence == sequenced, "mixed sequenced/legacy events")
            for key, value in list(row.items()):
                if key == "event":
                    continue
                base = 16 if key in {"ctx", "resource", "usage", "bind"} else 10
                row[key] = int(value, base)
                require(row[key] >= 0 or key in {"x", "y", "z"}, "negative value")
            require(row["time_ns"] > 0, "zero time")
            if sequenced:
                require(row["seq"] == events + 1, "missing/reordered event sequence")
                require(row["time_ns"] >= last_time, "global time reversed")
                last_time = row["time_ns"]
            if event == "checkpoint":
                for key, value in counts.items():
                    require(row[key] == value, f"checkpoint {key} mismatch")
                require(row["pending"] == counts["requests"] - counts["successful"] - counts["failed"], "checkpoint pending mismatch")
                require(row["live"] == counts["successful"] - counts["ended"], "checkpoint live mismatch")
                if row["marker"]:
                    require(row["marker"] > max((cp["marker"] for cp in checkpoints), default=0), "reused/reversed marker")
                checkpoints.append(row)
                events += 1
                if row["marker"] == start_marker:
                    start = row
                if row["marker"] == end_marker:
                    stopped = True
                    break
                continue
            mid = row["map"]
            require(mid > 0 and row["time_ns"] > 0, "zero identity/time")
            if event == "begin":
                require(mid not in maps, "duplicate begin")
                require(row["resource_id"] and row["object_id"] and row["resource"], "missing resource identity")
                require(row["user_ptr"] in (0, 1) and row["runtime"] in (0, 1), "invalid boolean")
                maps[mid] = {"begin": row}
                counts["requests"] += 1
            else:
                require(mid in maps, "orphan result/end")
                record = maps[mid]
                require(event not in record, "duplicate result/end")
                previous = record.get("result", record["begin"])
                require(row["time_ns"] >= previous["time_ns"], "time reversed within map")
                if event == "result":
                    require(row["success"] in (0, 1) and row["staging"] in (0, 1), "invalid boolean")
                    if row["success"]:
                        require(row["resource_id"] and row["object_id"] and row["resource"], "missing mapped identity")
                    else:
                        require(not (row["resource_id"] or row["object_id"] or row["resource"]), "failed map has mapped identity")
                else:
                    require("result" in record and record["result"]["success"], "end without successful result")
                record[event] = row
                if event == "result":
                    counts["successful" if row["success"] else "failed"] += 1
                else:
                    counts["ended"] += 1
            events += 1
        except (AssertionError, ValueError, KeyError) as error:
            raise ValueError(f"line {number}: {error}") from error
    if end_marker is not None:
        require(end_marker > 0 and stopped, "requested end marker not observed")
    if start_marker is not None:
        require(start is not None, "requested start marker not observed")
    if not maps:
        raise ValueError("no map lifetime events")
    pending = [mid for mid, row in maps.items() if "result" not in row]
    live = [mid for mid, row in maps.items() if row.get("result", {}).get("success") and "end" not in row]
    if pending or (live and not allow_live):
        raise ValueError(f"incomplete trace: pending={pending}, live={live}")
    return {
        "events": events, "requests": len(maps),
        "successful": sum(row["result"]["success"] for row in maps.values()),
        "failed": sum(not row["result"]["success"] for row in maps.values()),
        "ended": sum("end" in row for row in maps.values()), "live_map_ids": live,
        "object_replacements": sum(row["result"]["success"] and row["begin"]["resource_id"] == row["result"]["resource_id"] and row["begin"]["object_id"] != row["result"]["object_id"] for row in maps.values()),
        "staging_successes": sum(row["result"]["success"] and row["result"]["staging"] for row in maps.values()),
        "sequenced": sequenced, "checkpoint_count": len(checkpoints),
        "last_checkpoint": checkpoints[-1] if checkpoints else None,
        "interval_delta": {key: checkpoints[-1][key] - start[key] for key in counts} if start else None,
        "scope": "sequenced event and checkpoint consistency; CPU stores and uninstrumented paths are not measured" if checkpoints else "observed event consistency only; missing whole maps and CPU stores are not detected",
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--allow-live", action="store_true")
    parser.add_argument("--end-marker", type=int)
    parser.add_argument("--start-marker", type=int)
    args = parser.parse_args()
    print(json.dumps(analyze(args.log.read_text().splitlines(), args.allow_live, args.end_marker, args.start_marker), indent=2))
