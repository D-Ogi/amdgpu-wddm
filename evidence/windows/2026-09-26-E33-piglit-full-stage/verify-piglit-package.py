from pathlib import Path
import json,hashlib,gzip,xml.etree.ElementTree as ET,sys
base=Path(sys.argv[1]);manifest=json.loads(Path(sys.argv[2]).read_text());count=0
for f in manifest['files']:
 p=base/f['name'];assert p.stat().st_size==f['bytes'],f['name'];assert hashlib.sha256(p.read_bytes()).hexdigest()==f['sha256'],f['name'];count+=1
names=[]
for profile in ('quick_gl','quick_shader','glslparser'):
 with gzip.open(base/'piglit/tests'/f'{profile}.xml.gz','rb') as f:root=ET.parse(f).getroot()
 ns=[x.attrib['name'] for x in root.iter('Test')];assert len(ns)==int(root.attrib['count']);names.extend(ns)
assert len(names)==45037 and len(set(names))==45037
(base/'verified-cases.txt').write_text('\n'.join(names)+'\n',newline='\n')
(base/'verification.json').write_text(json.dumps({'files_verified':count,'unique_cases':len(names),'case_sha256':hashlib.sha256((base/'verified-cases.txt').read_bytes()).hexdigest(),'status':'PACKAGE_VERIFIED_NOT_GPU_RUN'},indent=2)+'\n')
print('verified',count,'files;',len(names),'unique cases',flush=True)
