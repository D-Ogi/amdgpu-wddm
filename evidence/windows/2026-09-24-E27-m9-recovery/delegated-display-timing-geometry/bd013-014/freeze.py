from pathlib import Path
import hashlib,json,difflib
root=Path('P:/bc-250');repo=root/'bc250-win';out=root/'scratch/m9/bd013-014';base=root/'scratch/m9/combined138-build-source'
files=['driver/kmd/dcn.c','driver/kmd/display.c','driver/kmd/wddm.c','driver/kmd/bc250kmd.h','driver/kmd/test/generate_display_visibility_test.py','driver/kmd/test/display_visibility_test.c','driver/kmd/test/run_display_visibility.ps1','driver/kmd/test/generate_post_display_test.py','driver/kmd/test/post_display_test.c']
records=[];diff=[]
def function(s,sig):
    start=s.index(sig);return s[start:s.index('\n}\n',start)+3]
for name in files:
    src=repo/name;dst=out/'source'/name;dst.parent.mkdir(parents=True,exist_ok=True);dst.write_bytes(src.read_bytes())
    records.append({'file':name,'sha256':hashlib.sha256(src.read_bytes()).hexdigest()})
    old=(base/name).read_text() if (base/name).exists() else '';new=src.read_text()
    if name=='driver/kmd/wddm.c':
        merged=old
        for sig in ['static void WddmVSyncArm(', 'void WddmSourceVisibility(', 'void WddmStop(']:merged=merged.replace(function(old,sig),function(new,sig))
        new=merged
    if name=='driver/kmd/display.c':new=old.replace(function(old,'NTSTATUS Bc250SetVidPnSourceVisibility('),function(new,'NTSTATUS Bc250SetVidPnSourceVisibility('))
    diff.extend(difflib.unified_diff(old.splitlines(True),new.splitlines(True),fromfile='frozen138/'+name,tofile='working/'+name))
(out/'changes.patch').write_text(''.join(diff));(out/'source-sha256.json').write_text(json.dumps(records,indent=2)+'\n')
