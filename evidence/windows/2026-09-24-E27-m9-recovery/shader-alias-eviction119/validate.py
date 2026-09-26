from pathlib import Path
import re,json
base=Path(__file__).resolve().parent
def read(name):
 b=(base/name).read_bytes()
 return b.decode('utf-16' if b.startswith((b'\xff\xfe',b'\xfe\xff')) else 'utf-8-sig')
result={}
for name in ['baseline','evict','stale']:
 out=read(name+'.out');err=read(name+'.err');code=int(read(name+'.exit').strip())
 assert 'shader-eviction-icd\\vulkan_radeon.dll' in err and 'bc250: progress before submit' in err
 assert 'BUFFER_ALIAS distinct_buffers=1 same_memory=1 offset=0 bytes=4194304' in out
 samples=re.findall(r'coherency(\d+)\s+.*?hash=(0x[0-9a-f]+) cpu_hash=(0x[0-9a-f]+) match=(yes|NO)',out)
 cycles={'baseline':0,'evict':3,'stale':1}[name]
 departures=re.findall(r'EVICT_TEST evicted status=00000000 residency=[23]',err)
 restores=re.findall(r'EVICT_TEST complete status=00000000 residency=1',err)
 rounds=re.findall(r'EVICT_ALIAS after_producer=1 round=(\d+) bytes=4194304',out)
 assert len(departures)==len(restores)==len(rounds)==cycles
 assert rounds==({'baseline':[],'evict':['1','5','9'],'stale':['1']}[name])
 if name=='stale':
  assert code==1 and len(samples)==2 and samples[0][1]==samples[0][2] and samples[1][1]!=samples[1][2]
  assert samples[1][1]==samples[0][1] and samples[1][3]=='NO'
  assert 'COHERENCY completed=1 mismatches=1' in out
 else:
  assert code==0 and len(samples)==16 and all(a==b and m=='yes' for _,a,b,m in samples)
  assert len(set(a for _,a,_,_ in samples))==16
  assert 'COHERENCY completed=16 mismatches=0' in out
 result[name]={'native_exit':code,'eviction_cycles':cycles,'samples':samples}
assert result['baseline']['samples']==result['evict']['samples']
for name in ['before','after']:
 s=read(name+'.log');c={}
 for key,pat in [('gfx',r'node 0 hardware: (\d+) submitted, (\d+) completed, (\d+) timeouts, (\d+) refused'),('paging',r'node 1 \(paging, open\): (\d+) hardware submitted, (\d+) completed, (\d+) timeouts, (\d+) refused')]:
  v=list(map(int,re.findall(pat,s)[-1]));assert v[0]==v[1] and v[2:]==[0,0];c[key]=v
 assert 'no TDR (ResetEngine' in s
 c['captures']=list(map(int,re.findall(r'capture plans reserved (\d+) heap (\d+)',s)[-1]));assert c['captures'][1]==0
 result[name]=c
print(json.dumps(result,indent=2))
