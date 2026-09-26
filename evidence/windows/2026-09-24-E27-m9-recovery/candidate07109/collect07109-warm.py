from pathlib import Path
import sys,json,re,io,tarfile
sys.path.insert(0,r'P:\bc-250\bc250-win\tools\win')
from target import Target
r=Path(r'P:\bc-250\bc250-win');out=r/'evidence/windows/2026-09-24-E27-m9-recovery/candidate07109';t=Target()
query="@($b=[datetime]'2026-09-24T05:17:00'; Get-ChildItem C:\\BC250\\kmdlog -Filter '*.log' | Where-Object { $_.LastWriteTime -ge $b } | Select-Object -ExpandProperty Name) | ConvertTo-Json -Compress"
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
print('Collected',len(names),'warm/recovery logs')
