from pathlib import Path
import json,re
p=Path(__file__).resolve().parent
def read(f):
    raw=f.read_bytes()
    return raw.decode('utf-16' if raw.startswith(b'\xff\xfe') else 'utf-8-sig')
expected=['batched-scissor-cb-update','batched-vb-cb-discard','batched-indexed-discard']
for run in ['083','084','085']:
    suffix=run if run=='085' else ''
    out=read(p/run/f'stdout{suffix}.txt')
    rows=re.findall(r'case=(batched-\S+) draws=(\d+) mismatches=(\d+)/4096 fnv64=(\w+)',out)
    assert [x[0] for x in rows]==expected
    assert all(x[1:]==('256','0','02e630a05dd4a325') for x in rows)
    assert 'PASS native graphics state' in out
    done=json.loads(read(p/run/f'done{suffix}.json'))
    assert done['exit']==0 and done['dwm_before']==done['dwm_after']==[5748]
    assert not re.search(r'device lost|VK_ERROR|failed',read(p/run/f'stderr{suffix}.txt'),re.I)
print('PASS three matched batch cases on WARP/CPU/GPU; BD-043 remains unrepresented')
