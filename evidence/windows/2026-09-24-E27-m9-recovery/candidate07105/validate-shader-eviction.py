"""Validate shader eviction files, independent of task orchestration."""
import json,re,sys
from pathlib import Path

def read(path):
    b=path.read_bytes()
    return b.decode('utf-16' if b.startswith((b'\xff\xfe',b'\xfe\xff')) else 'utf-8-sig')

def validate(root):
    result={};samples={}
    for name,rounds,exitcode,cycles in [('positive',16,0,0),('evict',16,0,3),('stale',2,1,1)]:
        stem='type3-'+name
        out=read(root/(stem+'.out'));err=read(root/(stem+'.err'))
        code=int(read(root/(stem+'.exit')))
        assert code==exitcode,(name,'native exit',code)
        assert 'shader-eviction-icd\\vulkan_radeon.dll' in err and 'bc250: progress before submit' in err
        buffers=re.findall(r'BUFFER memory_type=(\d+) heap=(\d+) flags=0x([0-9a-f]+) bytes=(\d+)',out)
        assert len(buffers)==3
        assert all((int(t),int(h),int(f,16),int(n))==(3,1,7,4194304) for t,h,f,n in buffers)
        rows=re.findall(r'coherency(\d+)\s+n=1048576\s+hash=0x([0-9a-f]+) cpu_hash=0x([0-9a-f]+) match=(yes|NO)',out)
        assert [int(x[0]) for x in rows]==list(range(rounds)),(name,'rounds')
        if name=='stale':
            assert 'COHERENCY completed=1 mismatches=1' in out
            assert rows[0][1]==rows[0][2] and rows[0][3]=='yes'
            assert rows[1][1]!=rows[1][2] and rows[1][3]=='NO'
        else:
            assert 'COHERENCY completed=16 mismatches=0' in out
            assert all(gpu==cpu and match=='yes' for _,gpu,cpu,match in rows)
            assert len({x[1] for x in rows})==16
        dep=len(re.findall(r'EVICT_TEST evicted status=00000000 residency=[23]',err))
        ret=len(re.findall(r'EVICT_TEST complete status=00000000 residency=1',err))
        assert dep==ret==cycles,(name,'residency witnesses')
        result[name]={'exit':code,'rounds':len(rows),'evictions':dep,'restores':ret};samples[name]=rows
    assert samples['positive']==samples['evict']
    assert samples['stale'][1][1]==samples['positive'][0][1]
    assert samples['stale'][1][2]==samples['positive'][1][2]
    counters={}
    for name in ['before','after']:
        log=read(root/(name+'.log'))
        rows=re.findall(r'node (\d+) (?:hardware|\([^\n]+?\)): (\d+) (?:hardware )?submitted, (\d+) completed, (\d+) timeouts, (\d+) refused',log)
        assert len(rows)==2 and 'no TDR (ResetEngine' in log
        counters[name]={node:list(map(int,values)) for node,*values in rows}
        assert all(v[0]==v[1] and v[2:]==[0,0] for v in counters[name].values())
    result['counters']=counters
    result['scope']='4MiB buffers, explicit diagnostic eviction; no arbitrary alias or PFN relocation proof'
    return result
if __name__=='__main__':
    print(json.dumps(validate(Path(sys.argv[1])),indent=2))
