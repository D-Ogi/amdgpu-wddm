from pathlib import Path
import tarfile,hashlib,json,time
b=Path('scratch/m12');files={}
def add(root,prefix,filter_=lambda p:True):
 for p in sorted(root.rglob('*')):
  if p.is_file() and '.git' not in p.parts and '__pycache__' not in p.parts and filter_(p):files[prefix+'/'+p.relative_to(root).as_posix()]=p
add(b/'piglit-src','piglit')
add(b/'piglit-build/generated_tests','piglit/generated_tests')
add(b/'piglit-build/tests','piglit/tests',lambda p:p.name.endswith(('.xml','.xml.gz')))
add(b/'piglit-build/bin','piglit/bin',lambda p:p.suffix in ('.exe','.dll'))
add(b/'piglit-runtime001','python')
for n in ['opengl32.dll','libgallium_wgl.dll','wflinfo.exe']:files['piglit/bin/'+n]=b/'zink-control-package006'/n
(b/'piglit-runtime001/python314._pth').write_text('python314.zip\n.\n../piglit\nimport site\n')
out=b/'piglit-full001.tar.gz';records=[]
with tarfile.open(out,'w:gz',compresslevel=1) as tar:
 for i,(arc,p) in enumerate(files.items()):
  data=p.read_bytes();records.append({'name':arc,'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()});tar.add(p,arcname='piglit-full001/'+arc,recursive=False)
  if i%10000==0:print('packaged',i,flush=True)
m={'piglit_commit':'0cc014230c0701d7a61bf240009ddd0711462d4e','profile':'quick','case_count':45037,'runtime':'Python3.14.0 embedded; Mako1.4.1; MarkupSafe3.0.3','zink_sha256':hashlib.sha256((b/'zink-control-package006/libgallium_wgl.dll').read_bytes()).hexdigest(),'archive_sha256':hashlib.sha256(out.read_bytes()).hexdigest(),'archive_bytes':out.stat().st_size,'files':records}
(b/'piglit-full001-manifest.json').write_text(json.dumps(m,indent=2)+'\n');print('archive ready',m['archive_bytes'],len(records),flush=True)
