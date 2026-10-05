"""Join successful Zink runtime imports to complete D3D Present fence witnesses.

Validates this stream only. Map/store coverage, pixels, GPU execution and KMD
copy counters must be validated separately. No event-loss or no-copy conclusion
is inferred from an empty stream. End at an explicit map checkpoint boundary.
"""
import argparse
import json
from pathlib import Path
import re


def require(ok, message):
    if not ok: raise ValueError(message)


def number(row, key, base=10):
    return int(row[key], base)


def pointer(row,key):
    value=row[key]
    return 0 if value in ("(nil)","NULL") else int(value,16)


def success(row):
    require(number(row,"hr",16)<0x80000000,"failed callback")


def analyze(text,end_marker,expected_presents=None):
    active={}; imports={}; destroyed=set(); sequence=0
    pending={}; last_present={}; last_signal={}; completed=[]; runtime_maps=[]
    stopped=False; checkpoints=0
    pending_snapshot=None; snapshot_schema=None; final_snapshot=None
    for line in text.splitlines():
        if not line.startswith("BC250 audit "): continue
        row=dict(re.findall(r"(\w+)=([^ ]+)",line))
        if line.startswith("BC250 audit runtime "):
            sequence+=1
            require(number(row,"seq")==sequence,"runtime sequence gap")
            rid=number(row,"resource_id"); ptr=pointer(row,"resource")
            require(rid>0 and ptr>0 and number(row,"object_id")>0,"invalid runtime identity")
            if row["event"]=="import":
                require(ptr not in active and rid not in imports,"reused live identity")
                require(pointer(row,"allocation") and pointer(row,"identity") and number(row,"va"),"missing import owner/allocation/VA")
                active[ptr]=row;imports[rid]=row
            elif row["event"]=="destroy":
                require(ptr in active and number(active[ptr],"resource_id")==rid,"unknown destruction")
                require(active[ptr]["object_id"]==row["object_id"],"destroyed object differs")
                del active[ptr];destroyed.add(rid)
            else: raise ValueError("unknown runtime event")
        elif line.startswith("BC250 audit lifetime event=begin "):
            if number(row,"runtime"):
                runtime_maps.append(dict(map=number(row,"map"),resource_id=number(row,"resource_id"),usage=number(row,"usage",16)))
        elif line.startswith("BC250 audit lifetime event=checkpoint "):
            require(number(row,"runtime_events")==sequence,"runtime checkpoint count mismatch")
            has_snapshot=pending_snapshot is not None
            if snapshot_schema is None: snapshot_schema=has_snapshot
            require(has_snapshot==snapshot_schema,"missing/mixed Present checkpoint snapshots")
            if pending_snapshot is not None:
                require(number(pending_snapshot,"checkpoint_seq")==number(row,"seq") and number(pending_snapshot,"marker")==number(row,"marker"),"Present snapshot boundary mismatch")
                if number(row,"marker")==end_marker: final_snapshot=pending_snapshot
                pending_snapshot=None
            checkpoints+=1
            if number(row,"marker")==end_marker:
                stopped=True;break
        elif line.startswith("BC250 audit present "):
            if row["event"]=="snapshot":
                require(pending_snapshot is None,"duplicate unbound Present snapshot")
                device=pointer(row,"device");sync=pointer(row,"sync")
                require(device>0 and number(row,"status",16)==0,"Present counter query unavailable")
                require(number(row,"completed")==last_present.get(device,0),"completed Present counter mismatch")
                require(number(row,"signaled")==last_signal.get((device,sync),0),"Present signal counter mismatch")
                require(sync>0 or number(row,"completed")==0,"missing Present sync identity")
                pending_snapshot=row
                continue
            device=pointer(row,"device"); ordinal=number(row,"present");key=(device,ordinal)
            event=row["event"]
            if event=="wait":
                require(key not in pending,"duplicate wait")
                require(ordinal==last_present.get(device,0)+1,"present ordinal gap")
                success(row);require(0<number(row,"count")<=16 and pointer(row,"context")>0,"missing GPU render fences/context")
                pending[key]=dict(wait=row,fences=[])
            elif event=="wait_fence":
                require(key in pending,"fence without wait")
                entry=pending[key];require("signal" not in entry,"wait after signal")
                require(number(row,"index")==len(entry["fences"]),"fence index gap")
                require(pointer(row,"sync")>0 and number(row,"value")>=0,"empty render fence")
                entry["fences"].append(row)
            elif event=="signal":
                require(key in pending and "signal" not in pending[key],"signal without unique wait")
                entry=pending[key];success(row)
                require(len(entry["fences"])==number(entry["wait"],"count"),"missing render fence")
                require(any(number(f,"value")>0 for f in entry["fences"]),"no submitted render work")
                require(pointer(row,"context")==pointer(entry["wait"],"context"),"signal context differs")
                sync=pointer(row,"sync");value=number(row,"value")
                require(sync>0 and value==last_signal.get((device,sync),0)+1,"signal value gap")
                entry["signal"]=row
            elif event=="complete":
                require(key in pending and "signal" in pending[key],"incomplete Present ordering")
                entry=pending.pop(key);success(row)
                require(pointer(row,"context")==pointer(entry["wait"],"context"),"present context differs")
                joined={}
                for side in ("src","dst"):
                    ptr=pointer(row,side);allocation=pointer(row,side+"_allocation")
                    if side=="dst" and ptr==0:
                        require(allocation==0,"destination allocation without resource");continue
                    require(ptr in active,"presented resource not imported/live")
                    imp=active[ptr]
                    require(pointer(imp,"allocation")==allocation,"allocation mismatch")
                    require(pointer(imp,"identity")==device,"cross-device imported resource")
                    if side=="src":
                        require(all(imp[k]==row[k] for k in ("width","height","format")),"surface shape mismatch")
                    joined[side]=number(imp,"resource_id")
                completed.append(dict(device=device,present=ordinal,resources=joined,waits=[dict(sync=pointer(f,"sync"),value=number(f,"value")) for f in entry["fences"]],signal=dict(sync=pointer(entry["signal"],"sync"),value=number(entry["signal"],"value"))))
                last_present[device]=ordinal
                last_signal[(device,pointer(entry["signal"],"sync"))]=number(entry["signal"],"value")
            else: raise ValueError("unknown present event")
    require(stopped and checkpoints>0,"missing final checkpoint")
    require(imports and completed,"no imported/presented surfaces")
    require(not pending,"pending Present at boundary")
    if expected_presents is not None:
        require(len(completed)==expected_presents,"Present total differs from independent expectation")
    presented={rid for p in completed for rid in p["resources"].values()}
    checkpoint_count_checked=final_snapshot is not None and set(last_present)=={pointer(final_snapshot,"device")}
    return dict(checkpoint_present_count_checked=checkpoint_count_checked,final_present_snapshot=final_snapshot,expected_present_count_checked=expected_presents is not None,scope="Import/resource/allocation and runtime callback witnesses only; no whole-stack CPU-copy verdict",runtime_events=sequence,imports=list(imports.values()),destroyed_resource_ids=sorted(destroyed),presents=completed,presented_resource_ids=sorted(presented),runtime_maps=runtime_maps,presented_resource_maps=[m for m in runtime_maps if m["resource_id"] in presented])


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log",type=Path);parser.add_argument("--end-marker",type=int,required=True)
    parser.add_argument("--expected-presents",type=int)
    args=parser.parse_args()
    print(json.dumps(analyze(args.log.read_text(encoding="utf-8-sig"),args.end_marker,args.expected_presents),indent=2))
