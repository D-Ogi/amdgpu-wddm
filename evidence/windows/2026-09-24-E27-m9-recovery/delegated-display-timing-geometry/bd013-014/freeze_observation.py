from pathlib import Path
import shutil,json,hashlib
r=Path('bc250-win');out=Path('scratch/m9/bd013-014/observation-source');out.mkdir(exist_ok=True)
files=['driver/kmd/dcn.c','driver/kmd/display.c','driver/kmd/bc250kmd.h','driver/kmd/bc250kmd_escape.h','driver/kmd/display_timing_snapshot.h','driver/kmd/gen_regs.py','driver/kmd/regs.generated.h','driver/kmd/test/generate_dcn_observe_test.py','driver/kmd/test/dcn_observe_test.c','driver/kmd/test/run_dcn_observe.ps1']
manifest={}
for name in files:
    target=out/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(r/name,target);manifest[name]=hashlib.sha256(target.read_bytes()).hexdigest()
(out/'sha256.json').write_text(json.dumps(manifest,indent=2)+'\n')
print('Captured',len(files),'files')
