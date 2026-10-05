from pathlib import Path
import re,json,hashlib,sys,importlib
r=Path('bc250-win');e=r/'evidence/windows/2026-09-24-E27-m9-recovery/candidate07127';s=Path('scratch/m9');d=e/'inference-native';d.mkdir();vdir=e/'residency-native';vdir.mkdir()
def write(p,t):
 with p.open('x',encoding='utf-8',newline='') as f:f.write(t)
def read(p):
 b=p.read_bytes();return b.decode('utf-16') if b.startswith(b'\xff\xfe') else b.decode('utf-8-sig')
def scrub(t):
 t=re.sub(r'(?im)^.*# interface.*$','[interface identity redacted]',t)
 return re.sub(r'(PCI\\VEN_[^\s\\]+)\\[^\s]+',r'\1\\[instance-redacted]',t,flags=re.I)
for n in ['m8.out','m8.err','m8.exit','stories15M.out','stories15M.err','stories15M.exit','tinyllama.out','tinyllama.err','tinyllama.exit','before.log','after.log']:
 write(d/n,scrub(read(s/('inference07127-'+n))))
for name in ['vram64k','warm64k','warm1g']:
 for suffix in ['.out','.err','.exit','-before.log','-after.log']:
  write(vdir/(name+suffix),scrub(read(s/('candidate07127-'+name+suffix))))
sys.path.insert(0,str((r/'experiments/E27-m9-inference').resolve()));validator=importlib.import_module('validate-gpu-residency')
result={'residency':[validator.validate(vdir,n,size) for n,size in [('vram64k',65536),('warm64k',65536),('warm1g',1<<30)]],'models':{}}
for n in ['m8','stories15M','tinyllama']:
 assert int(read(d/(n+'.exit')))==0
 err=read(d/(n+'.err'));assert 'cache-intent-v2\\vulkan_radeon.dll' in err and 'bc250: progress before submit' in err
 if n=='m8':
  rows=re.findall(r'^\w+\s+n=\d+.*?hash=(0x[0-9a-f]+) cpu_hash=(0x[0-9a-f]+) match=(\w+)',read(d/'m8.out'),re.M)
  assert len(rows)==8 and all(a==b and m=='yes' for a,b,m in rows);result['m8_matches']=8
 else:
  ref=r/('evidence/linux/2026-09-21-E14-vulkan-compute-reference/llama/'+n+'-ngl99.out')
  a=read(d/(n+'.out')).replace('\r','');b=read(ref).replace('\r','');assert a==b
  off=re.search(r'offloaded (\d+)/(\d+) layers to GPU',err);assert off and off[1]==off[2]
  result['models'][n]={'normalized_sha256':hashlib.sha256(a.encode()).hexdigest(),'offload':off[0],'reference':str(ref)}
  write(d/(n+'-reference.out'),b)
for name in ['inference07127-run.log','candidate07127-inference-final.log']:
 write(e/name,scrub(read(s/name)))
write(e/'validation.json',json.dumps(result,indent=2))
write(e/'COLLECTION-NOTE.md','''# Text-copy correction

The initial inference/ text copies were written through Windows newline
translation after decoding CRLF, creating CRCRLF. Python universal-newline
reading then interpreted those as extra blank lines, so the first local text
comparison failed. The raw files retained in scratch match E14 after removing
CR characters. inference-native/ and residency-native/ are the authoritative
UTF-8 text copies written without newline translation; original UTF-16 summary
files are decoded and PCI/interface identities redacted as elsewhere.
Original first copies remain unchanged. validation.json uses the corrected
copies. No GPU workload was rerun because of this collection error.
''')
print(json.dumps(result,indent=2))
