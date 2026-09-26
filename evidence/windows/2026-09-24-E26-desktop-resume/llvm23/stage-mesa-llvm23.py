from pathlib import Path
import hashlib,json,sys
sys.path.insert(0,'bc250-win/tools/win')
from target import Target
import subprocess
assert subprocess.check_output(["scratch/llvm2312-build/bin/llvm-config.exe","--version"],text=True).strip()=="23.1.2"
p=Path('scratch/mesa-llvm23-build/src/gallium/targets/d3d10umd/bc250d3d.dll')
h=hashlib.sha256(p.read_bytes()).hexdigest().upper()
print('llvmpipe_sha256='+h+' bytes='+str(p.stat().st_size))
s=Path('scratch/m13/mesa-profile07127.ps1').read_text().replace('profile-umd','llvm23-umd').replace('E1976C4254055F0B96E294E713DBC5798C07E41E61526DD6FC3738A712FC1CAD',h).replace('mesa-profile07127','mesa-llvm23-07127')
s=s.replace("$reg='HKLM:","if ((Invoke-RestMethod http://127.0.0.1:2250/state).stop) { throw 'Owner STOP requested' }\n$reg='HKLM:",1)
s=s.replace("-Pattern 'BC250 Perf'","-Pattern 'BC250 Perf|BC250 Renderer:'")
Path('scratch/m13/mesa-llvm23-07127.ps1').write_text(s)
m=Path('bc250-win/tools/win/bc250mon/graphics-modules.json')
v=json.loads(m.read_text());v[h]={'Renderer':'Mesa llvmpipe','Execution':'CPU software rendering (JIT)','Compiler':'LLVM 23.1.2 / native CPU code','Build':'rotation + fence + TGSI/NIR'}
m.write_text(json.dumps(v,indent=2)+'\n')
t=Target();t.push([str(p)],r'C:\BC250\m13\llvm23-umd');t.push([str(m)],r'C:\BC250\mon')
Path('scratch/m13/llvm23-sha256.txt').write_text(h+'\n')
