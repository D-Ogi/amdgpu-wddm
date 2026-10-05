from pathlib import Path
import sys,re,json,hashlib
sys.path.insert(0,'bc250-win/tools/win');from target import Target
t=Target();base=Path('scratch/m9/radv-main-collected');base.mkdir(exist_ok=True)
trials={1:['before.log','m8.out','m8.err','run.cmd','worker.ps1'],2:['before.log','m8.out','m8.err','m8.exit','worker.exit','run.cmd','worker.ps1','progress.log','after-failure.log','stacks.txt'],3:['before.log','after.log','m8.out','m8.err','m8.exit','stories15M.out','stories15M.err','stories15M.exit','tinyllama.out','tinyllama.err','tinyllama.exit','worker.exit','run.cmd','worker.ps1']}
for n,files in trials.items():
 d=base/('control'+str(n));d.mkdir(exist_ok=True)
 for name in files:t.pull('C:\\BC250\\m9\\radv-main-control'+str(n)+'\\'+name,str(d/name))
 print('Collected control'+str(n),flush=True)
def read(p):
 b=p.read_bytes();return b.decode('utf-16') if b.startswith(b'\xff\xfe') else b.decode('utf-8-sig')
d=base/'control3';validation={}
rows=re.findall(r'^\w+\s+n=\d+.*?hash=(0x[0-9a-f]+) cpu_hash=(0x[0-9a-f]+) match=(\w+)',read(d/'m8.out'),re.M)
assert len(rows)==8 and all(a==b and m=='yes' for a,b,m in rows);validation['shader_hash_matches']=8
assert int(read(d/'worker.exit'))==0
for n in ['m8','stories15M','tinyllama']:
 assert int(read(d/(n+'.exit')))==0
 err=read(d/(n+'.err'));assert 'radv-main-icd2\\vulkan_radeon.dll' in err and 'bc250: progress before submit' in err
 if n=='m8':continue
 expected=Path('bc250-win/evidence/linux/2026-09-21-E14-vulkan-compute-reference/llama')/(n+'-ngl99.out');actual=read(d/(n+'.out')).replace('\r','');assert actual==read(expected).replace('\r','')
 m=re.search(r'offloaded (\d+)/(\d+) layers to GPU',err);assert m and m[1]==m[2]
 validation[n]={'reference_match':True,'layers':m[0],'normalized_sha256':hashlib.sha256(actual.encode()).hexdigest()}
(base/'validation.json').write_text(json.dumps(validation,indent=2));print(json.dumps(validation,indent=2))
