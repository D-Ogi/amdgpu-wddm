from pathlib import Path
import json,re
p=Path(__file__).resolve().parent
def read(f):
 b=f.read_bytes();return b.decode('utf-16' if b.startswith(b'\xff\xfe') else 'utf-8-sig')
v=json.loads(read(p/'verification.json'))
for run,delta in [('090',29),('091',23),('092',6000),('093',123)]:
 d=v[run];assert d['completion']['exit']==0 and d['restored']
 assert d['dwm_before']==d['dwm_after']==5748
 counts=[]
 for phase in ['before','after']:
  counts.append(tuple(map(int,re.search(r'(\d+) submitted, (\d+) completed, (\d+) timeouts, (\d+) refused',d[phase]['node 0 hardware:']).groups())))
  assert re.search(r'186 alive',d[phase]['objects created/destroyed:'])
 assert tuple(b-a for a,b in zip(*counts))==(delta,delta,0,0)
s=read(p/'090/stdout090.txt')
images=re.findall(r'textured=(\S+) pass=(\d) mismatches=(\d+)/4096 fnv64=(\w+)',s)
assert len(images)==8
for form in ['t0-s0','t1-s0']:
 assert [(x[1],x[2],x[3]) for x in images if x[0]==form]==[(str(i),'0','4da4ffd8a1ace325' if i%2==0 else '45946e4d9c66a325') for i in range(4)]
s=read(p/'091/stdout091.txt')
assert len(re.findall(r'case=batched-\S+ draws=256 mismatches=0/4096 fnv64=02e630a05dd4a325',s))==3
assert 'PASS iterations=1000 generations=10 parent_pixels=2702400 child_pixels=2702400' in read(p/'092/stdout092.txt')
s=read(p/'093/stdout093.txt')
assert 'PASS' in s and 'mismatches=0/76800' in s
print('PASS M575 exact images, sharing, Present and completed-work deltas; no DWM visual claim.')
