from pathlib import Path
import shutil
src=Path('scratch/mesa-wddm2'); dst=Path('scratch/mesa-main-20260924')
p=dst/'src/gallium/auxiliary/nir/tgsi_to_nir.c'
s=p.read_text(); a=s.index('<<<<<<< ours'); b=s.index('>>>>>>> theirs',a)+len('>>>>>>> theirs')
s=s[:a]+'   if (opcode == TGSI_OPCODE_SAMPLE_I ||\n       opcode == TGSI_OPCODE_TXF) {'+s[b:]
assert '<<<<<<<' not in s
p.write_text(s)
for name in ['bc250_ttn_control.c','bc250_lp_test_main.c']:
 rel=Path('src/gallium/targets/d3d10umd')/name
 shutil.copy2(src/rel,dst/rel)
p=Path('scratch/m13/build-mesa-llvm23.cmd')
s=p.read_text().replace('mesa-llvm23-build','mesa-main-llvm23-build').replace('mesa-wddm2','mesa-main-20260924')
Path('scratch/m13/build-mesa-main-llvm23.cmd').write_text(s)
