from pathlib import Path
import subprocess, shutil
src=Path('scratch/mesa-wddm2'); dst=Path('scratch/mesa-main-20260924')
patch=subprocess.check_output(['git','-C',str(src),'diff','--binary','HEAD','--','src/gallium'])
p=Path('scratch/m13/mesa-current-gallium.patch'); p.write_bytes(patch)
r=subprocess.run(['git','-C',str(dst),'apply','--check',str(p.resolve())],capture_output=True)
Path('scratch/m13/mesa-main-patch-check.log').write_bytes(r.stdout+r.stderr)
print('patch_check_exit='+str(r.returncode))
if r.returncode==0:
 subprocess.run(['git','-C',str(dst),'apply',str(p.resolve())],check=True)
 for name in ['bc250_ttn_control.c','bc250_lp_test_main.c']:
  rel=Path('src/gallium/targets/d3d10umd')/name
  shutil.copy2(src/rel,dst/rel)
 print('gallium patches applied')
