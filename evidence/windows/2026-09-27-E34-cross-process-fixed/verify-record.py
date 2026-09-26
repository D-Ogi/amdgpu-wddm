from pathlib import Path
import re,json,hashlib,shutil
w=Path('P:/BC-250');p=Path(__file__).resolve().parent
e=w/'bc250-win/evidence/windows/2026-09-27-E34-cross-process-fixed'
e.mkdir(exist_ok=False)
def read(f):
 b=f.read_bytes();return b.decode('utf-16' if b.startswith(b'\xff\xfe') else 'utf-8-sig')
summary={}
for run in ['072','073','074','075']:
 stdout=read(p/run/('stdout'+run+'.txt'));stderr=read(p/run/('stderr'+run+'.txt'))
 refs=re.findall(r'release runtime resource refs=(\d+) object_refs=(\d+)',stderr)
 errors=re.findall(r'.*(?:device lost|VK_ERROR|failed).*',stderr,re.I)
 summary[run]={'stdout':stdout.splitlines(),'release_references':refs,'errors':errors,'successful_deallocations':stderr.count('Deallocate 00000000 flags=0')}
 if run in ['072','074','075']:
  assert not errors and refs and all(x==('1','1') for x in refs)
 if run=='074':
  assert len(refs)==20 and 'iterations=1000 generations=10 parent_pixels=2702400 child_pixels=2702400' in stdout
 if run=='075':assert 'final green mismatches=0/76800 removed=00000000' in stdout
 t=e/run;t.mkdir()
 for f in (p/run).iterdir():
  if f.name.startswith('kmd-'):continue
  shutil.copy2(f,t/f.name)
 if run=='075':shutil.copy2(p.parent/'package034/run075.ps1',t/'runner.ps1')
 else:
  shutil.copy2(p/('run'+run+'.ps1'),t/'runner.ps1')
  shutil.copy2(p/('run'+run+'.log'),t/'runner-output.log')
t=e/'071';t.mkdir()
for name in ['run071.ps1','run071.log']:shutil.copy2(p/name,t/name)
patterns=['objects created/destroyed:','allocations opened/closed:','node 0 hardware:']
counter={};private_hashes={}
for run in ['073','074','075']:
 counter[run]={}
 for phase in ['before','after']:
  f=p/run/f'kmd-{phase}{run}.log';lines=read(f).splitlines()
  counter[run][phase]={key:[s for s in lines if key in s][-1] for key in patterns}
  private_hashes[str(f.relative_to(p))]=hashlib.sha256(f.read_bytes()).hexdigest()
for phase in ['before','after']:
 assert counter['074'][phase][patterns[0]].endswith('195 alive')
f=p/'075-screen.png';private_hashes[f.name]=hashlib.sha256(f.read_bytes()).hexdigest()
for name,obj in [('verified-results.json',summary),('kmd-counter-extract.json',counter),('private-artifact-hashes.json',private_hashes)]:
 (e/name).write_text(json.dumps(obj,indent=2)+'\n',encoding='utf-8',newline='\n')
for name in ['build-pitch.log','pitch-replay.log']:shutil.copy2(p/name,e/name)
shutil.copy2(p.parent/'package034/manifest.json',e/'manifest.json')
shutil.copy2(p/'record-fixed.py',e/'verify-record.py')
print('Verified controls072/074/075,20 shared closes,195 live objects before/after074')
