"""Bounded KMD software-blit counter observations and runtime-map census.

This is not a whole-stack no-copy verdict. Requires completed checkpoint evidence
and a positive counter observation after rollback. Never substitutes missing data
with zero. Raw ring dumps contain history: select the newest summary in each file.
"""
import argparse
import datetime as dt
import importlib.util
import json
from pathlib import Path
import re

PATTERN = re.compile(r"^\s*(\d+)\s+([0-9.]+) wddm summary: blit gate (open|closed), (\d+) blits, (\d+) skips, (\d+) sources translated contiguous$", re.M)

def latest(text):
    rows = list(PATTERN.finditer(text))
    if not rows:
        raise ValueError("missing complete CPU-blit counter summary")
    row = max(rows, key=lambda m: int(m[1]))
    return dict(sequence=int(row[1]),seconds=float(row[2]),gate=row[3],
                blits=int(row[4]),skips=int(row[5]),translated=int(row[6]),raw=row[0].strip())

def utc(value):
    return dt.datetime.fromisoformat(value.replace("Z", "+00:00"))

def text(path):
    raw=path.read_bytes()
    return raw.decode("utf-16" if raw.startswith(b"\xff\xfe") else "utf-8-sig").replace("\r\n", "\n")

def read(path):
    return json.loads(text(path))

def analyze(directory):
    done=read(directory/"done.json")
    if not done["success"] or not done["restoration_succeeded"]:
        raise ValueError("trial not completed/restored")
    bounds=[read(p) for p in directory.glob("boundary-*.json")]
    start=next(b for b in bounds if b["label"]=="render-start")
    end=next(b for b in bounds if b["label"]=="render-end")
    if any(b["pid"]!=done["gpu_pid"] for b in bounds):
        raise ValueError("mixed boundary process identities")
    log=text(directory/("dwm-%d.log" % done["gpu_pid"]))
    spec=importlib.util.spec_from_file_location("checkpoints",Path(__file__).with_name("analyze-dwm-checkpoints.py"))
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    checked=module.analyze(log,bounds)
    stop=checked["lifetime"]["last_checkpoint"]["seq"]
    runtime=[];count=0
    for line in log.splitlines():
        if not line.startswith("BC250 audit lifetime event=begin "): continue
        fields=dict(re.findall(r"(\w+)=([^ ]+)",line))
        if int(fields["seq"])>stop: break
        count+=1
        if "runtime" not in fields: raise ValueError("missing runtime discriminator")
        if int(fields["runtime"]): runtime.append(int(fields["map"]))
    if count!=checked["lifetime"]["requests"]: raise ValueError("map census differs")
    samples=[]
    for path in sorted(directory.glob("startup-log-*.txt.json")):
        receipt=read(path)
        if not utc(start["utc"]) <= utc(receipt["utc"]) <= utc(end["utc"]): continue
        row=latest(text(path.with_suffix("")))
        row.update(sample=receipt["sample"],utc=receipt["utc"])
        samples.append(row)
    samples.sort(key=lambda r:r["sample"])
    if len(samples)<2: raise ValueError("insufficient in-window samples")
    for a,b in zip(samples,samples[1:]):
        if b["sequence"]<=a["sequence"] or b["seconds"]<=a["seconds"]:
            raise ValueError("counter history reset/reversed within selected samples")
        if any(b[k]<a[k] for k in ("blits","skips","translated")):
            raise ValueError("counters decreased")
    positive=latest(text(directory/"closure-driver.log"))
    if positive["blits"]<=0: raise ValueError("missing CPU rollback positive control")
    return dict(scope="Observed KMD software-present blits plus audited Zink runtime-map requests; not all CPU writers or final-resource identity proof",start_utc=start["utc"],end_utc=end["utc"],samples=samples,
        observed_software_blits_zero=all(s["blits"]==0 for s in samples),
        runtime_map_ids=runtime,map_requests=count,positive_after_rollback=positive)

if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument("directory",type=Path)
    args=parser.parse_args();print(json.dumps(analyze(args.directory),indent=2))
