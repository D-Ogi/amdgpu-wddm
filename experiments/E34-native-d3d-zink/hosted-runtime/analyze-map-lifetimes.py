"""Check one process/DLL map lifetimes and instrumented CPU writer spans."""
import argparse
from bisect import bisect_left, bisect_right
import json
from pathlib import Path

PREFIX = "BC250 audit lifetime "
STORE_PREFIX = "BC250 audit store "
STORE_FIELDS = {
    "begin": "event seq store map time_ns writer kind offset bytes capacity mapped_offset valid",
    "end": "event seq store time_ns",
}
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
    stores = {}
    active_stores = {}
    ended_stores = 0
    store_checkpoints = None
    events = 0
    sequenced = None
    threaded = None
    last_time = 0
    checkpoints = []
    counts = dict(requests=0, successful=0, failed=0, ended=0)
    start = None
    stopped = False
    if start_marker is not None:
        require(end_marker is not None and 0 < start_marker < end_marker, "invalid marker interval")
    for number, line in enumerate(lines, 1):
        is_store = STORE_PREFIX in line
        prefix = STORE_PREFIX if is_store else PREFIX
        if prefix not in line:
            continue
        items = line.split(prefix, 1)[1].split()
        try:
            pairs = [item.split("=", 1) for item in items]
            row = dict(pairs)
            event = row["event"]
            require(len(row) == len(pairs), "duplicate fields")
            fields = STORE_FIELDS if is_store else FIELDS
            require(event in fields, "unknown/invalid event")
            schema = set(fields[event].split())
            if not is_store and event == "checkpoint":
                has_store_counts = "stores_begun" in row or "stores_ended" in row
                if store_checkpoints is None:
                    store_checkpoints = has_store_counts
                require(store_checkpoints == has_store_counts, "mixed store checkpoint schemas")
                if has_store_counts:
                    schema.update({"stores_begun", "stores_ended"})
            has_sequence = "seq" in row
            if has_sequence:
                schema.add("seq")
            if event == "begin" and not is_store:
                has_thread = "tid" in row
                if threaded is None:
                    threaded = has_thread
                require(threaded == has_thread, "mixed thread/legacy begin events")
                if has_thread:
                    schema.add("tid")
            require(set(row) == schema, "missing/unknown fields")
            if sequenced is None:
                sequenced = has_sequence
            require(has_sequence == sequenced, "mixed sequenced/legacy events")
            for key, value in list(row.items()):
                if key == "event" or (is_store and key in {"writer", "kind"}):
                    continue
                base = 16 if key in {"ctx", "resource", "usage", "bind"} else 10
                row[key] = int(value, base)
                require(row[key] >= 0 or key in {"x", "y", "z"}, "negative value")
            require(row["time_ns"] > 0, "zero time")
            if sequenced:
                require(row["seq"] == events + 1, "missing/reordered event sequence")
                require(row["time_ns"] >= last_time, "global time reversed")
                last_time = row["time_ns"]
            if is_store:
                require(sequenced, "store requires event sequence")
                sid = row["store"]
                require(sid > 0, "zero store identity")
                if event == "begin":
                    require(sid not in stores, "duplicate store begin")
                    require(row["map"] in maps, "store has unknown map")
                    mapped = maps[row["map"]]
                    require(mapped.get("result", {}).get("success") == 1 and "end" not in mapped,
                            "store outside successful map lifetime")
                    require(mapped["begin"]["target"] == 0 and mapped["begin"]["usage"] & 2,
                            "store map is not a writable buffer")
                    require(row["valid"] == 1, "invalid store range")
                    require(row["capacity"] == mapped["begin"]["box_width"], "store capacity mismatch")
                    require(row["offset"] <= row["capacity"] and
                            row["bytes"] <= row["capacity"] - row["offset"], "store exceeds map bounds")
                    require(row["kind"] in {"get_descriptor", "descriptor_copy", "buffer_subdata"},
                            "unknown store kind")
                    require(row["writer"].isidentifier(), "invalid writer identity")
                    stores[sid] = {"begin": row}
                    active_stores.setdefault(row["map"], set()).add(sid)
                else:
                    require(sid in stores and "end" not in stores[sid], "orphan/duplicate store end")
                    stores[sid]["end"] = row
                    ended_stores += 1
                    active_stores[stores[sid]["begin"]["map"]].remove(sid)
                events += 1
                continue
            if event == "checkpoint":
                if store_checkpoints:
                    require(row["stores_begun"] == len(stores), "checkpoint stores_begun mismatch")
                    require(row["stores_ended"] == ended_stores, "checkpoint stores_ended mismatch")
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
                    require(not active_stores.get(mid),
                            "unmap while store pending")
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
    pending_stores = [sid for sid, record in stores.items() if "end" not in record]
    require(allow_live or not pending_stores, "incomplete store trace")
    store_inventory = []
    marker_checkpoints = [cp for cp in checkpoints if cp["marker"]]
    marker_sequences = [cp["seq"] for cp in marker_checkpoints]
    for sid, record in stores.items():
        begin = record["begin"]
        end = record.get("end")
        store_inventory.append(dict(store=sid, map=begin["map"], writer=begin["writer"],
            kind=begin["kind"], offset=begin["offset"], bytes=begin["bytes"],
            mapped_offset=begin["mapped_offset"],
            mapped_resource_id=maps[begin["map"]]["result"]["resource_id"],
            staging=bool(maps[begin["map"]]["result"]["staging"]),
            begin_seq=begin["seq"], end_seq=end["seq"] if end else None,
            crossing_markers=[cp["marker"] for cp in marker_checkpoints[
                bisect_right(marker_sequences, begin["seq"]):
                bisect_left(marker_sequences, end["seq"]) if end else len(marker_sequences)]]))
    interval_stores = [r for r in store_inventory if start and
        start["seq"] < r["begin_seq"] and r["end_seq"] is not None and
        r["end_seq"] < checkpoints[-1]["seq"]]
    boundary_stores = [r["store"] for r in store_inventory if start and
        (start_marker in r["crossing_markers"] or end_marker in r["crossing_markers"])]
    return {
        "stores": store_inventory, "pending_store_ids": pending_stores,
        "store_checkpoint_counters": bool(store_checkpoints),
        "store_totals_scope": "whole parsed prefix, including records before start marker",
        "interval_store_span_bytes": sum(r["bytes"] for r in interval_stores) if start else None,
        "interval_boundary_store_ids": boundary_stores if start else None,
        "completed_store_span_bytes": sum(r["begin"]["bytes"] for r in stores.values() if "end" in r),
        "events": events, "requests": len(maps),
        "successful": sum(row["result"]["success"] for row in maps.values()),
        "failed": sum(not row["result"]["success"] for row in maps.values()),
        "ended": sum("end" in row for row in maps.values()), "live_map_ids": live,
        "object_replacements": sum(row["result"]["success"] and row["begin"]["resource_id"] == row["result"]["resource_id"] and row["begin"]["object_id"] != row["result"]["object_id"] for row in maps.values()),
        "staging_successes": sum(row["result"]["success"] and row["result"]["staging"] for row in maps.values()),
        "sequenced": sequenced, "checkpoint_count": len(checkpoints),
        "last_checkpoint": checkpoints[-1] if checkpoints else None,
        "interval_delta": {key: checkpoints[-1][key] - start[key] for key in counts} if start else None,
        "scope": "sequenced map and instrumented writer spans; uninstrumented writers and application stores through DDI maps require separate evidence" if checkpoints else "observed event consistency only; missing whole maps and CPU stores are not detected",
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--allow-live", action="store_true")
    parser.add_argument("--end-marker", type=int)
    parser.add_argument("--start-marker", type=int)
    args = parser.parse_args()
    print(json.dumps(analyze(args.log.read_text().splitlines(), args.allow_live, args.end_marker, args.start_marker), indent=2))
