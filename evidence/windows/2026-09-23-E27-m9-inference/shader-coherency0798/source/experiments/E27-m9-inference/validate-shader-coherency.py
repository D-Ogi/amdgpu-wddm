"""Validate captured shader coherency runs and stale-input control; no lab access."""
import json
import re
import sys
from pathlib import Path

def read(p):
    data=p.read_bytes()
    return data.decode('utf-16' if data.startswith((b'\xff\xfe',b'\xfe\xff')) else 'utf-8-sig')

def validate(root):
    rows={}
    for name in ('positive','stale','repeat'):
        output=read(root/(name+'.out'))
        trace=read(root/(name+'.err'))
        code=int(read(root/(name+'.exit')).strip())
        assert 'quiet-submit\\vulkan_radeon.dll' in trace and 'bc250: progress before submit' in trace, name
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
    print(json.dumps(validate(Path(sys.argv[1])),indent=2))
