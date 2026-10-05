"""Validate the recorded four-thread checkpoint control and fail-closed mutations."""
import importlib.util
import json
import re
import sys
from pathlib import Path

spec = importlib.util.spec_from_file_location("lifetimes", Path(__file__).with_name("analyze-map-lifetimes.py"))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
require = module.require
lines = Path(sys.argv[1]).read_text().splitlines()
result = module.analyze(lines, end_marker=12, start_marker=11)
require(result["interval_delta"] == dict(requests=800, successful=400, failed=400, ended=400), "interval counters")
require(result["last_checkpoint"]["pending"] == result["last_checkpoint"]["live"] == 0, "closed interval")
final = module.analyze(lines, end_marker=100)
require((final["requests"], final["successful"], final["failed"], final["ended"]) == (801,401,400,401), "final counters")
live = module.analyze(lines, end_marker=99, allow_live=True)
require(len(live["live_map_ids"]) == 1 and live["last_checkpoint"]["live"] == 1, "live marker")
rejected = []

def reject(name, rows, expected, **kwargs):
    try:
        module.analyze(rows, **kwargs)
    except ValueError as error:
        require(expected in str(error), f"{name}: unexpected rejection: {error}")
        rejected.append(name)
    else:
        raise ValueError(f"{name}: accepted invalid evidence")

first = next(line for line in lines if "event=begin " in line)
mid = int(re.search(r" map=(\d+)", first).group(1))
removed = [line for line in lines if not re.search(rf" map={mid}(?: |$)", line)]
reject("whole_map_removed", removed, "event sequence", end_marker=12)
renumbered = [re.sub(r" seq=\d+", f" seq={i}", line) for i,line in enumerate(removed,1)]
reject("whole_map_removed_and_resequenced", renumbered, "checkpoint requests mismatch", end_marker=12)
reject("missing_required_marker", [line for line in lines if "marker=12 " not in line], "event sequence", end_marker=12)
cut = next(i for i,line in enumerate(lines) if "marker=12 " in line)
reject("truncated_before_boundary", lines[:cut], "end marker not observed", end_marker=12)
reject("forged_request_counter", [line.replace("requests=800", "requests=801") if "marker=12 " in line else line for line in lines], "checkpoint requests mismatch", end_marker=12)
reject("forged_live_counter", [line.replace("live=0", "live=1") if "marker=12 " in line else line for line in lines], "checkpoint live mismatch", end_marker=12)
reject("live_map_disallowed", lines, "incomplete trace", end_marker=99)
reject("missing_start_marker", lines, "start marker not observed", start_marker=10, end_marker=12)
reject("reversed_marker_interval", lines, "invalid marker interval", start_marker=12, end_marker=11)
reject("absent_end_marker", lines, "end marker not observed", end_marker=101)
reject("mixed_legacy_events", [line.replace(re.search(r" seq=\d+",line).group(0), "") if i==1 else line for i,line in enumerate(lines)], "mixed sequenced/legacy")
print(json.dumps({"interval":result,"final_counts":{k:final[k] for k in ["requests","successful","failed","ended"]},"live_ids":live["live_map_ids"],"rejected":rejected},indent=2))
