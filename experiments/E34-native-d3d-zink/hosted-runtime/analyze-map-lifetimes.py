"""Check one process/DLL lifetime trace; this does not measure CPU stores."""
import argparse
import json
from pathlib import Path

PREFIX = "BC250 audit lifetime "
FIELDS = {
    "begin": "event map time_ns ctx resource resource_id object_id target width height depth format bind level usage user_ptr runtime x y z box_width box_height box_depth",
    "result": "event map time_ns success resource resource_id object_id usage staging stride layer_stride",
    "end": "event map time_ns",
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def analyze(lines, allow_live=False):
    maps = {}
    events = 0
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
            require(set(row) == set(FIELDS[event].split()), "missing/unknown fields")
            for key, value in list(row.items()):
                if key == "event":
                    continue
                base = 16 if key in {"ctx", "resource", "usage", "bind"} else 10
                row[key] = int(value, base)
                require(row[key] >= 0 or key in {"x", "y", "z"}, "negative value")
            mid = row["map"]
            require(mid > 0 and row["time_ns"] > 0, "zero identity/time")
            if event == "begin":
                require(mid not in maps, "duplicate begin")
                require(row["resource_id"] and row["object_id"] and row["resource"], "missing resource identity")
                require(row["user_ptr"] in (0, 1) and row["runtime"] in (0, 1), "invalid boolean")
                maps[mid] = {"begin": row}
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
            events += 1
        except (AssertionError, ValueError, KeyError) as error:
            raise ValueError(f"line {number}: {error}") from error
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
        "scope": "observed event consistency only; missing whole maps and CPU stores are not detected",
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--allow-live", action="store_true")
    args = parser.parse_args()
    print(json.dumps(analyze(args.log.read_text().splitlines(), args.allow_live), indent=2))
