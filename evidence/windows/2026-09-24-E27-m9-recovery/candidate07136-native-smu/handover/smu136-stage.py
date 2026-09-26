from pathlib import Path
import sys,json,hashlib
sys.path.insert(0,r"P:\bc-250\bc250-win\tools\win")
from target import Target
r=Path(r"P:\bc-250"); t=Target(); manifest=[]
groups=[('scratch/build/bc250kmd-07136/package-umd','package-umd',None),('scratch/build/smu136-client','client',['bc250kmd_cli.exe','bc250control.dll']),('scratch/build/smu136-reader','reader',['bc250rd.sys','bc250rd_cli.exe','bc250control.dll']),('scratch/build/smu136-monitor','monitor',['bc250mon.exe','bc250control.dll','graphics-modules.json'])]
for local,sub,names in groups:
    d=r/local
    files=[d/n for n in names] if names else sorted(p for p in d.iterdir() if p.is_file())
    for p in files: manifest.append({'relative':sub+'/'+p.name,'sha256':hashlib.sha256(p.read_bytes()).hexdigest().upper(),'bytes':p.stat().st_size})
    t.push([str(p) for p in files], remote_dir=r'C:\BC250\m9\candidate07136'+'\\'+sub)
p=r/'scratch/m9/smu136-stage-manifest.json';p.write_text(json.dumps(manifest,indent=2)+'\n')
t.push([str(p)],remote_dir=r'C:\BC250\m9\candidate07136')
print('Staged',len(manifest),'artifacts; no device/service changes')
