"""Bounded C100 two-known-word SMU transfer with kernel-owned C096 buffers."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import runpy
import signal
import struct
import subprocess
import sys
import time

HERE=Path(__file__).resolve().parent
MODULE='dma_provider_C096'
KO_SHA='c105d26517bb3a29f12efe8c2d154d7cb5da9d4b3fe53acf27883848a4f55bbf'
PREFLIGHT_SHA='48c3dcd0bc53788ade998065cc8daa051f1945903558a14f7d0576dd4642408e'
BUILD_ID='d073d7b7da060ba3dc9d883f76438aa242cd28b1'

def live_build_ids():
    raw=Path('/sys/kernel/notes').read_bytes();off=0;ids=[]
    while off+12<=len(raw):
        n,d,t=struct.unpack_from('<III',raw,off);off+=12
        if off+((n+3)&~3)+((d+3)&~3)>len(raw):raise RuntimeError('invalid kernel notes')
        name=raw[off:off+n];off+=(n+3)&~3
        desc=raw[off:off+d];off+=(d+3)&~3
        if name.rstrip(b'\0')==b'GNU' and t==3:ids.append(desc.hex())
    return ids

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--execute',action='store_true')
    a=p.parse_args()
    pins=json.loads((HERE/'pins.json').read_text())
    for name,digest in pins['files'].items():
        if Path(name).name!=name or hashlib.sha256((HERE/name).read_bytes()).hexdigest()!=digest:
            raise RuntimeError('source pin changed: '+name)
    if not a.execute:
        print('PLAN: exact kernel/provider; q0 controls; setup00/restore04; two known-word transfers to distinct C096 slots; retain until cold reset; no activation.')
        return 0
    if sys.flags.optimize or sys.platform!='linux':raise RuntimeError('requires Linux without -O')
    result_path=HERE/'result.json'
    if result_path.exists() or (HERE/'events.jsonl').exists():raise RuntimeError('prior run exists; no retry')
    if os.uname().release!='6.18.52-0-lts' or live_build_ids()!=[BUILD_ID]:raise RuntimeError('kernel identity mismatch')
    if hashlib.sha256((HERE/'dma-provider-C096-r4.ko').read_bytes()).hexdigest()!=KO_SHA:raise RuntimeError('module changed')
    preflight=Path('/root/dpm-preflight.py')
    if hashlib.sha256(preflight.read_bytes()).hexdigest()!=PREFLIGHT_SHA:raise RuntimeError('preflight changed')
    mod=Path('/sys/module')/MODULE
    if mod.exists():raise RuntimeError('module already loaded; inspect instead of retrying')
    # The worker alone holds the q0 flock; do not deadlock it from the parent.
    helpers=runpy.run_path(str(HERE/'dma_process_C099.py'))
    stop=helpers['StopLatch']();stop.install();stop.check()
    env=runpy.run_path(str(preflight))
    sensor=env['sensor']
    monitor=helpers['TrialGuard'](stop,sensor.read_text)
    start=monitor.start;temperatures=monitor.temperatures;events=[]
    output=(HERE/'events.jsonl').open('x',encoding='utf-8',buffering=1)
    module_attempted=False
    outcome=dict(passed=False,incomplete=True,no_activation=True)

    def emit(value):
        value=dict(monotonic=time.monotonic(),**value)
        events.append(value);output.write(json.dumps(value)+'\n')

    def guard():
        monitor.check()

    def run(label,argv,bound):
        guard()
        stdout=HERE/(label+'.stdout');stderr=HERE/(label+'.stderr')
        emit(dict(kind='process-start',label=label))
        with stdout.open('xb') as out,stderr.open('xb') as err:
            exitcode=helpers['run_owned'](argv,out,err,guard,bound)
        guard()
        if stdout.stat().st_size>1048576 or stderr.stat().st_size>65536:raise RuntimeError('unexpected output size')
        row=dict(kind='process-result',label=label,exit=exitcode,
                 stdout_bytes=stdout.stat().st_size,stderr=stderr.read_text(),
                 stdout_sha256=hashlib.sha256(stdout.read_bytes()).hexdigest())
        emit(row)
        if exitcode:raise RuntimeError(label+' failed, no automatic cleanup/retry')
        return row

    try:
        monitor.check(starting=True)
        emit(dict(kind='preflight',kernel=os.uname().release,build_id=BUILD_ID,
                  identity=env['info'],no_activation=True))
        module_attempted=True
        run('load-provider',['insmod',str(HERE/'dma-provider-C096-r4.ko')],10)
        run('transfer',[sys.executable,'-I','-B',str(HERE/'postcopy-C100.py'),
                        '--execute','--kit','/root/bc250-t001-q5-control-r1',
                        '--preflight','/root/dpm-preflight.py',
                        '--output',str(HERE/'transfer.jsonl'),
                        '--status',str(HERE/'transfer-status.json')],25)
        status=json.loads((HERE/'transfer-status.json').read_text())
        if status.get('outcome',{}).get('passed') is not True:
            raise RuntimeError('worker transfer not successful')
        if int((mod/'refcnt').read_text())!=1:
            raise RuntimeError('kernel retention reference missing')
        outcome=dict(passed=True,no_activation=True,retained=True,reset_required=True,
                     min_tctl_mC=min(temperatures),max_tctl_mC=max(temperatures),
                     elapsed_s=time.monotonic()-start)
    except BaseException as exc:
        outcome=dict(passed=False,no_activation=True,error=type(exc).__name__+': '+str(exc),
                     module_attempted=module_attempted,reset_required=module_attempted,no_retry=True)
    finally:
        # Never unload after a module attempt. Missing worker output or any
        # transfer uncertainty requires inspection and cold reset.
        outcome['module_present']=mod.exists()
        if mod.exists():outcome['module_refcnt']=(mod/'refcnt').read_text().strip()
        with result_path.open('x',encoding='utf-8') as f:json.dump(outcome,f,indent=2)
        emit(dict(kind='result',**outcome))
        output.close()
    print(json.dumps(outcome),flush=True)
    return 0 if outcome['passed'] else 2

if __name__=='__main__':raise SystemExit(main())
