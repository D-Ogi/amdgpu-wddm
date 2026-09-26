import re,subprocess
from pathlib import Path
base=140722275483648
s=Path('scratch/m13/llvmpipe-symbols.log').read_text()
addrs=[]
for l in s.splitlines():
 m=re.match(r'([0-9a-f`]+)\s+([0-9a-f`]+)\s+(bc250d3d!)',l)
 if m:
  # A displayed return address names the caller on the following row.
  addrs.append(int(m[2].replace('`',''),16)-base-1)
cmd=['scratch/llvm1917-build/bin/llvm-symbolizer.exe','--obj=scratch/mesa-llvmpipe-build/src/gallium/targets/d3d10umd/bc250d3d.dll','--relative-address']+[hex(a) for a in addrs if 0<=a<61898752]
r=subprocess.run(cmd,capture_output=True,text=True)
Path('scratch/m13/llvmpipe-symbolized.txt').write_text(r.stdout)
print(r.stdout)
