"""Validate captured shader coherency runs and stale-input control; no lab access."""
import argparse
import json
import re
import sys
from pathlib import Path

def read(p):
    data=p.read_bytes()
    return data.decode('utf-16' if data.startswith((b'\xff\xfe',b'\xfe\xff')) else 'utf-8-sig')

def validate(root,prefix="",icd_dir="quiet-submit"):
    rows={}
    for name in ('positive','stale','repeat'):
        output=read(root/(prefix+name+'.out'))
        trace=read(root/(prefix+name+'.err'))
        code=int(read(root/(prefix+name+'.exit')).strip())
        if prefix:
            memory_type=int(prefix.removeprefix('type').removesuffix('-'))
            heap,flags={2:(0,6),3:(1,7),5:(0,14)}[memory_type]
            buffers=re.findall(r'BUFFER memory_type=(\d+) heap=(\d+) flags=0x([0-9a-f]+) bytes=(\d+)',output)
            assert len(buffers)==3
            assert all((int(t),int(h),int(f,16),int(b))==(memory_type,heap,flags,4194304) for t,h,f,b in buffers)

        assert icd_dir+'\\vulkan_radeon.dll' in trace and 'bc250: progress before submit' in trace, name
        parsed=re.findall(r'coherency(\d+)\s+n=1048576\s+hash=0x([0-9a-f]+) cpu_hash=0x([0-9a-f]+) match=(yes|NO)',output)
        assert [int(x[0]) for x in parsed]==list(range(len(parsed))), name
        if name=='stale':
            assert code==1 and len(parsed)==2 and 'COHERENCY completed=1 mismatches=1' in output
            assert parsed[0][1]==parsed[0][2] and parsed[0][3]=='yes'
            assert parsed[1][1]!=parsed[1][2] and parsed[1][3]=='NO'
        else:
            assert code==0 and len(parsed)==16 and 'COHERENCY completed=16 mismatches=0' in output
            assert all(gpu==cpu and match=='yes' for _,gpu,cpu,match in parsed)
            assert len({x[1] for x in parsed})==16
        rows[name]=parsed
    assert rows['positive']==rows['repeat']
    assert rows['stale'][1][1]==rows['positive'][0][1]
    assert rows['stale'][1][2]==rows['positive'][1][2]
    counters={}
    for name in ('before','after'):
        log=read(root/(name+'.log'))
        found=re.findall(r'node (\d+) (?:hardware|\([^\n]+?\)): (\d+) (?:hardware )?submitted, (\d+) completed, (\d+) timeouts, (\d+) refused',log)
        assert len(found)==2 and 'no TDR (ResetEngine' in log
        counters[name]={node:[int(v) for v in values] for node,*values in found}
        assert all(v[0]==v[1] and v[2:]==[0,0] for v in counters[name].values())
    return dict(positive_rounds=32,stale_control='detected round1, previous GPU hash retained',counters=counters,
                scope='host-coherent reused Vulkan allocations; no eviction or CPU cache-alias type proof')

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root',type=Path)
    parser.add_argument('--memory-types',action='store_true')
    parser.add_argument('--icd-dir',default='quiet-submit')
    args=parser.parse_args()
    result={str(t):validate(args.root,f'type{t}-',args.icd_dir) for t in (2,3,5)} if args.memory_types else validate(args.root,icd_dir=args.icd_dir)
    print(json.dumps(result,indent=2))
