"""Mutation controls over a preserved completed trial; no lab operations."""
import importlib.util
import json
import re
from pathlib import Path
import sys
from unittest.mock import patch

spec=importlib.util.spec_from_file_location("counter_analysis",Path(__file__).with_name("analyze-present-copy-counters.py"))
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
directory=Path(sys.argv[1]);baseline=m.analyze(directory)
assert baseline["observed_software_blits_zero"] and not baseline["runtime_map_ids"]
assert baseline["positive_after_rollback"]["blits"]>0
original=m.text
cases=[]
# A real runtime-map record must be reported, not silently excluded.
def runtime(path):
    value=original(path)
    if path.name.startswith("dwm-") and path.suffix==".log":
        assert "runtime=0" in value
        value,count=re.subn(r"(^BC250 audit lifetime event=begin [^\n]*?)runtime=0", r"\1runtime=1", value, count=1, flags=re.M)
        assert count==1
    return value
with patch.object(m,"text",runtime):
    assert len(m.analyze(directory)["runtime_map_ids"])==1
cases.append("injected_runtime_map_detected")
# Removing CPU positive evidence must reject the purported zero-copy observation.
def no_positive(path):
    value=original(path)
    return value.replace(" blits,", " missing,") if path.name=="closure-driver.log" else value
with patch.object(m,"text",no_positive):
    try: m.analyze(directory)
    except ValueError: pass
    else: raise AssertionError("missing positive control accepted")
cases.append("missing_positive_rejected")
# Ignore old ring history only by an explicit sequence ordering, not regex first-match.
line="%d %.3f wddm summary: blit gate open, %d blits, 0 skips, 1 sources translated contiguous"
assert m.latest((line%(9,2.0,7))+"\n"+(line%(10,3.0,8)))["blits"]==8
cases.append("newest_summary_selected")
try:m.latest("wddm summary: blit gate open, 0 blits")
except ValueError:pass
else:raise AssertionError("truncated summary accepted")
cases.append("truncated_counter_rejected")
print(json.dumps(dict(pass_controls=True,cases=cases,samples=len(baseline["samples"])),indent=2))
