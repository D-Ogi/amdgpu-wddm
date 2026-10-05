from pathlib import Path
import sys,re,json,hashlib,importlib
r=Path('bc250-win');e=r/'evidence/windows/2026-09-24-E27-m9-recovery/candidate07127';d=e/'inference';d.mkdir()
sys.path.insert(0,str((r/'tools/win').resolve()));from target import Target
sys.path.insert(0,str((r/'experiments/E27-m9-inference').resolve()));validator=importlib.import_module('validate-gpu-residency')
t=Target()
def redact(v):
 v=re.sub(r'(?im)^.*# interface.*$','[interface identity redacted]',v)
 return re.sub(r'(PCI\\VEN_[^\s\\]+)\\[^\s]+',r'\1\\[instance-redacted]',v,flags=re.I)
def write(p,v):
 with p.open('x',encoding='utf-8') as f:f.write(v)
for name in ['m8.out','m8.err','m8.exit','stories15M.out','stories15M.err','stories15M.exit','tinyllama.out','tinyllama.err','tinyllama.exit','before.log','after.log']:
 p=Path('scratch/m9')/('inference07127-'+name);assert not p.exists();t.pull('C:\\BC250\\m9\\inference07127\\'+name,str(p));b=p.read_bytes();v=b.decode('utf-16') if b.startswith(b'\xff\xfe') else b.decode('utf-8-sig');write(d/name,redact(v))
results={'residency':[validator.validate(e,n,size) for n,size in [('vram64k',65536),('warm64k',65536),('warm1g',1<<30)]], 'models':{}}
for n in ['m8','stories15M','tinyllama']:
 assert int((d/(n+'.exit')).read_text())==0
 err=(d/(n+'.err')).read_text();assert 'cache-intent-v2\\vulkan_radeon.dll' in err and 'bc250: progress before submit' in err
 if n=='m8':
  rows=re.findall(r'^\w+\s+n=\d+.*?hash=(0x[0-9a-f]+) cpu_hash=(0x[0-9a-f]+) match=(\w+)',(d/'m8.out').read_text(),re.M)
  assert len(rows)==8 and all(a==b and m=='yes' for a,b,m in rows);results['m8_matches']=8
 else:
  ref=r/('evidence/linux/2026-09-21-E14-vulkan-compute-reference/llama/'+n+'-ngl99.out')
  a=(d/(n+'.out')).read_text().replace('\r','');b=ref.read_text().replace('\r','');assert a==b,(n,'immutable reference mismatch')
  off=re.search(r'offloaded (\d+)/(\d+) layers to GPU',err);assert off and off[1]==off[2]
  results['models'][n]={'normalized_sha256':hashlib.sha256(a.encode()).hexdigest(),'offload':off[0],'reference':str(ref)}
  write(d/(n+'-reference.out'),b)
for name in ['inference07127-run.log','candidate07127-inference-final.log']:
 write(e/name,redact((Path('scratch/m9')/name).read_text()))
write(e/'validation.json',json.dumps(results,indent=2))
print(json.dumps(results,indent=2))
