from pathlib import Path
import hashlib,shutil
r=Path('P:/bc-250/bc250-win'); o=Path('P:/bc-250/scratch/m9/bd028')
owned=['driver/kmd/test/paging_mc_test.c','driver/kmd/test/dcn_translate_test.c','tools/wddm_contract_check/host/qai_bridge.c','tools/wddm_contract_check/host/qai_bridge.h','tools/wddm_contract_check/host/qai_test.c']
inputs=owned+['driver/kmd/paging_mc.c','driver/kmd/paging_mc.h','driver/kmd/dcn_translate.c','driver/kmd/dcn_translate.h','driver/kmd/bc250kmd.h','driver/kmd/test/run_paging_mc.ps1','driver/kmd/test/run_dcn_translate.ps1']
lines=[]
for rel in inputs:
 p=o/'source'/rel;p.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(r/rel,p)
 lines.append(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+rel)
(o/'source/SHA256SUMS.txt').write_text('\n'.join(lines)+'\n')
mut=o/'negative'; shutil.copytree(o/'source/driver',mut/'driver',dirs_exist_ok=True)
p=mut/'driver/kmd/paging_mc.c';s=p.read_text().replace('if (offset >= vramLength)', 'if (offset >= vramLength || offset >= (8ull << 30))');p.write_text(s)
p=mut/'driver/kmd/dcn_translate.c';s=p.read_text().replace('if (offset >= VramLength)', 'if (offset >= VramLength || offset >= (8ull << 30))');p.write_text(s)
