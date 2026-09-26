from pathlib import Path
import sys,json,re,io,tarfile
sys.path.insert(0,r'P:\bc-250\bc250-win\tools\win')
from target import Target
r=Path(r'P:\bc-250\bc250-win');out=r/'evidence/windows/2026-09-24-E27-m9-recovery/candidate07109';t=Target()
query="@($b=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime; Get-ChildItem C:\\BC250\\kmdlog -Filter '*.log' | Where-Object { $_.LastWriteTime -ge $b } | Select-Object -ExpandProperty Name) | ConvertTo-Json -Compress"
names=json.loads(t.run(query,timeout=30))
if isinstance(names,str):names=[names]
assert names and all(re.fullmatch(r'ring-[0-9-]+\.log',n) for n in names)
assert all(not (out/n).exists() for n in names)
x=t.ssh('cmd /c "tar -cf - -C C:\\BC250\\kmdlog '+ ' '.join(names)+'"',binary=True,timeout=45)
assert x.returncode==0 and x.stdout,'Log archive failed'
with tarfile.open(fileobj=io.BytesIO(x.stdout),mode='r:') as tar:
 for n in names:
  member=tar.getmember(n);assert member.isfile();data=tar.extractfile(member).read()
  with (out/n).open('xb') as f:f.write(data)
probe=out/'gpu-probe';probe.mkdir(exist_ok=True)
for n in ['vram64k.out','vram64k.err','vram64k.exit','vram64k-before.log','vram64k-after.log']:
 assert not (probe/n).exists();t.pull('C:\\BC250\\m9\\gpu-residency07109-control\\'+n,str(probe/n))
print('Collected',len(names),'boot logs and native probe files')
