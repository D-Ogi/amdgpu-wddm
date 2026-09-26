from pathlib import Path
import difflib,hashlib,json
root=Path('P:/bc-250');repo=root/'bc250-win';out=root/'scratch/m9/bd007-009'
p=repo/'driver/kmd/dcn.c';s=p.read_text().replace("// DcnFlipPending below is that DPC's own single, non-blocking read). CardAddress is what dxgkrnl's", "// DcnFlipPending below is that DPC's bounded, non-blocking observation). CardAddress is what dxgkrnl's");p.write_text(s)
files=['driver/kmd/wddm.c','driver/kmd/dcn.c','driver/kmd/bc250kmd.h','driver/kmd/gen_regs.py','driver/kmd/regs.generated.h','driver/kmd/test/vidpn_flip_test.c','driver/kmd/test/dcn_observation_test.c','driver/kmd/test/generate_dcn_observation_test.py','driver/kmd/test/run_dcn_observation.ps1']
records=[];diff=[]
for name in files:
    src=repo/name;dst=out/'source'/name;dst.parent.mkdir(parents=True,exist_ok=True);dst.write_bytes(src.read_bytes())
    records.append({'file':name,'sha256':hashlib.sha256(src.read_bytes()).hexdigest()})
    old=root/'scratch/m9/smu136-build-source'/name
    diff.extend(difflib.unified_diff(old.read_text().splitlines(True) if old.exists() else [],src.read_text().splitlines(True),fromfile='frozen136/'+name,tofile='working/'+name))
(out/'changes.patch').write_text(''.join(diff))
(out/'source-sha256.json').write_text(json.dumps(records,indent=2)+'\n')
print('Frozen',len(files),'changed files; diff lines',len(diff))
