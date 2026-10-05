import sys,io,tarfile
from pathlib import Path
sys.path.insert(0,r'P:\bc-250\bc250-win\tools\win')
from target import Target
t=Target(); out=Path('scratch/m9/dcn135-artifacts'); out.mkdir(exist_ok=False)
for label,remote in [('compute',r'C:\BC250\m9\candidate07135-control'),('residency',r'C:\BC250\m9\dcn135-residency'),('d3d',r'C:\BC250\m13\dcn135-control')]:
 r=t.ssh('cmd /c "tar -cf - -C '+remote+' ."',timeout=90,binary=True)
 if r.returncode: raise RuntimeError('archive read failed: '+label)
 target=out/label;target.mkdir()
 with tarfile.open(fileobj=io.BytesIO(r.stdout),mode='r:') as a:
  for m in a:
   if not m.isfile():continue
   name=Path(m.name).name
   if Path(name).suffix.lower() not in ('.txt','.log','.out','.err','.exit','.ps1','.cmd','.bmp'):continue
   with (target/name).open('xb') as f:f.write(a.extractfile(m).read())
 print(label+' collected')
