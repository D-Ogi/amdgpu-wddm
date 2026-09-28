"""Compare checkpoint-bounded UMD Presents with a unique ETW context sequence.

Runtime context handles are opaque: association is inferred from exact ordered
allocation/fence sequences, never from numeric equality with KMT handles.
Supports one presenting UMD device and one waited/signaled object per Present.
Requires a separate loss-free trace witness. Does not prove scanout or no-copy.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import re


def require(ok, message):
    if not ok: raise ValueError(message)


def fields(line):
    return [s.strip() for s in re.split(r",(?![^\[]*\])", line.rstrip())]


def array(row, key, base):
    return [int(s.strip(), base) for s in re.split(r"[, :]",row[key].strip("[]")) if s.strip()]


def matches(umd, events):
    require(len(events) >= 3*len(umd), "ETW sequence shorter than checkpoint count")
    wait_objects=set(); signal_objects=set(); tids=set()
    for i,u in enumerate(umd):
        w,p,s=events[3*i:3*i+3]
        require([w[0],p[0],s[0]] == ["wait","present","signal"], "ETW wait/Present/signal order")
        w,p,s=w[1],p[1],s[1]
        require(int(w["TimeStamp"]) <= int(p["TimeStamp"]) <= int(s["TimeStamp"]), "ETW timestamp order")
        tids.update([w["ThreadID"],p["ThreadID"],s["ThreadID"]])
        require(int(p["ReturnStatus"]) == 0, "ETW failed Present")
        require(int(p["hSrcAllocHandle"],16)==u["src"] and int(p["hDstAllocHandle"],16)==u["dst"], "ETW allocation mismatch")
        require(int(w["ObjectCount"])==1 and int(s["ObjectCount"])==1 and int(s["ContextCount"])==1, "unsupported ETW object/context count")
        require(array(w,"MonitoredFenceValue[ObjectCount]",10)==[u["wait"]], "ETW render wait value mismatch")
        require(array(s,"MonitoredFenceValue[ObjectCount]",10)==[u["signal"]], "ETW Present signal value mismatch")
        wo=array(w,"ObjectArray[ObjectCount]",16);so=array(s,"ObjectArray[ObjectCount]",16)
        require(len(wo)==len(so)==1 and wo[0] and so[0], "missing kernel sync object")
        wait_objects.update(wo);signal_objects.update(so)
    require(len(tids)==len(wait_objects)==len(signal_objects)==1, "mixed thread/sync identities")
    return dict(matched=len(umd),thread=next(iter(tids)),kernel_wait_object=next(iter(wait_objects)),kernel_signal_object=next(iter(signal_objects)),post_boundary_events=len(events)-3*len(umd))


def analyze(log, csv_path, proof, marker):
    sp=importlib.util.spec_from_file_location("identities",Path(__file__).with_name("analyze-present-identities.py"))
    m=importlib.util.module_from_spec(sp);sp.loader.exec_module(m)
    joined=m.analyze(log,marker)
    require(joined["checkpoint_present_count_checked"], "missing independent checkpoint count")
    complete=[]
    for line in log.splitlines():
        row=dict(re.findall(r"(\w+)=([^ ]+)",line))
        if line.startswith("BC250 audit lifetime event=checkpoint ") and int(row["marker"])==marker: break
        if line.startswith("BC250 audit present event=complete "): complete.append(row)
    require(len(complete)==len(joined["presents"]), "UMD complete census differs")
    pids={int(r["pid"]) for r in complete};require(len(pids)==1,"mixed UMD pids");pid=pids.pop()
    require(proof["dwm_pid"]==pid and proof["trace_loss"]=={"events":0,"buffers":0} and proof["matched"]>0 and proof["pending"]==proof["unmatched"]==proof["duplicate_starts"]==0, "missing loss-free GPU witness")
    require(len({r["context"] for r in complete})==1,"multiple runtime contexts unsupported")
    umd=[]
    for c,j in zip(complete,joined["presents"]):
        require(len(j["waits"])==1,"multiple runtime waits unsupported")
        umd.append(dict(src=int(c["src_allocation"],16),dst=int(c["dst_allocation"],16),wait=j["waits"][0]["value"],signal=j["signal"]["value"]))
    headers={};inside=True;contexts={};devices={};events=[];adapters=set()
    with csv_path.open(encoding="utf-8-sig") as f:
        for line in f:
            line=line.strip()
            if line=="EndHeader":inside=False;continue
            if not line.startswith("Microsoft-Windows-DxgKrnl/"):continue
            v=fields(line);name=v[0]
            if inside:headers[name]=v;continue
            require(name in headers,"missing ETW schema")
            row=dict(zip(headers[name],v))
            if name.endswith("/Adapter/win:Start"):adapters.add(row["pDxgAdapter"])
            if not re.search(r"\(\s*%d\s*\)"%pid,row["Process Name ( PID)"]):continue
            if name.endswith("/Device/win:Start"):devices[row["hDevice"]]=row
            elif name.endswith("/Context/win:Start"):
                handle=int(row["ContextHandle"],16)
                require(handle not in contexts,"context reuse unsupported")
                contexts[handle]=row
            elif name.endswith("/Present/win:Info"):events.append(("present",int(row["hContext"]),row))
            elif name.endswith("/WaitForSynchronizationObjectFromGpu/win:Info"):events.append(("wait",int(row["hContext"],16),row))
            elif name.endswith("/SignalSynchronizationObjectFromGpu/win:Info"):
                for ctx in array(row,"hContext[ContextCount]",16):events.append(("signal",ctx,row))
    candidates=[];failures={}
    for handle,c in contexts.items():
        kernel=int(c["hContext"],16)
        selected=[(kind,r) for kind,ctx,r in events if ctx==(handle if kind=="present" else kernel)]
        if not any(k=="present" for k,r in selected):continue
        # Other DWM synchronization objects share this context. Enumerate objects
        # whose first expected signal is present, then require a unique full match.
        objects={tuple(array(r,"ObjectArray[ObjectCount]",16)) for kind,r in selected
                 if kind=="signal" and array(r,"MonitoredFenceValue[ObjectCount]",10)==[umd[0]["signal"]]}
        for obj in objects:
            if len(obj)!=1:continue
            filtered=[(kind,r) for kind,r in selected if kind!="signal" or obj[0] in array(r,"ObjectArray[ObjectCount]",16)]
            try:result=matches(umd,filtered)
            except ValueError as e:failures[hex(handle)+":"+hex(obj[0])]=str(e);continue
            dev=devices.get(c["hDevice"]);require(dev is not None,"missing context device")
            result.update(context=handle,device=dev["hThunkHandle"],adapter=dev["pDxgAdapter"],adapter_started_in_trace=dev["pDxgAdapter"] in adapters,other_signal_events=len(selected)-len(filtered))
            candidates.append(result)
    require(len(candidates)==1,"expected one matching ETW context: "+repr(failures))
    return dict(pid=pid,marker=marker,association="Unique exact allocation/wait/signal sequence; opaque runtime context is not numerically mapped",scope="No independent hardware adapter identity or scanout/no-copy claim",match=candidates[0],rejected_contexts=failures)


if __name__=="__main__":
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("log",type=Path);p.add_argument("csv",type=Path);p.add_argument("proof",type=Path);p.add_argument("--marker",type=int,default=8)
    a=p.parse_args();print(json.dumps(analyze(a.log.read_text(),a.csv,json.loads(a.proof.read_text()),a.marker),indent=2))
