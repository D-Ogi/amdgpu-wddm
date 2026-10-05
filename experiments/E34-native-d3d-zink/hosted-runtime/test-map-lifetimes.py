import importlib.util
import json
from pathlib import Path
import sys
path = Path(__file__).with_name("analyze-map-lifetimes.py")
spec = importlib.util.spec_from_file_location("lifetimes", path)
module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
lines = Path(sys.argv[1]).read_text().splitlines()
result = module.analyze(lines)
assert (result["requests"], result["successful"], result["failed"], result["ended"]) == (4,2,2,2)
assert result["object_replacements"] == result["staging_successes"] == 1
assert "layer_stride=4294967297" in lines[1]
negative = {
 "empty": [], "missing_begin": lines[1:], "missing_result": lines[:1]+lines[2:],
 "duplicate_end": lines[:3]+[lines[2]]+lines[3:],
 "failed_end": lines+[lines[2].replace("4294967296", "4294967301")],
 "missing_end": lines[:2]+lines[3:],
 "zero_identity": [lines[0].replace("resource_id=4294967294", "resource_id=0")]+lines[1:],
 "duplicate_field": [lines[0]+" map=1"]+lines[1:],
 "invalid_event": lines+["BC250 audit lifetime event=invalid reason=id_wrap"],
}
for name, bad in negative.items():
 try: module.analyze(bad)
 except ValueError: pass
 else: raise AssertionError(name)
live=module.analyze(negative["missing_end"], allow_live=True)
assert live["live_map_ids"] == [4294967296]
print(json.dumps({"positive":result,"negative_rejections":list(negative),"allow_live_detected":live["live_map_ids"]},indent=2))
