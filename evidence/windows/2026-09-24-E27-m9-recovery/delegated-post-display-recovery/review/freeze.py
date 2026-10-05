from pathlib import Path
import hashlib,json,difflib
root=Path('P:/bc-250');repo=root/'bc250-win';out=root/'scratch/m9/bd003-011';base=root/'scratch/m9/display137-build-source'
files=['driver/kmd/display.c','driver/kmd/pnp.c','driver/kmd/dcn.c','driver/kmd/bc250kmd.h','driver/kmd/wddm.c','driver/kmd/test/generate_dcn_observation_test.py','driver/kmd/test/generate_post_display_test.py','driver/kmd/test/post_display_test.c','driver/kmd/test/run_post_display.ps1','driver/kmd/test/generate_post_display_stop_test.py','driver/kmd/test/post_display_stop_test.c','driver/kmd/test/run_post_display_stop.ps1']
records=[];diff=[]
def function(s,sig):
    start=s.index(sig);return s[start:s.index('\n}\n',start)+3]
for name in files:
    src=repo/name;dst=out/'source'/name;dst.parent.mkdir(parents=True,exist_ok=True);dst.write_bytes(src.read_bytes())
    records.append({'file':name,'sha256':hashlib.sha256(src.read_bytes()).hexdigest()})
    old=(base/name).read_text() if (base/name).exists() else '';new=src.read_text()
    if name=='driver/kmd/wddm.c':
        # Peer agent owns firmware metadata in another function. Delta is only ours.
        new=old.replace(function(old,'void WddmStop('),function(new,'void WddmStop('))
    diff.extend(difflib.unified_diff(old.splitlines(True),new.splitlines(True),fromfile='frozen137/'+name,tofile='working/'+name))
(out/'changes.patch').write_text(''.join(diff));(out/'source-sha256.json').write_text(json.dumps(records,indent=2)+'\n')
print('Frozen',len(files),'files; patch excludes peer UMDRIVERPRIVATE changes')
