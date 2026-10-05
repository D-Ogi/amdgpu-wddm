from pathlib import Path
import os
import sys
r=Path(sys.argv[1]);out=Path(sys.argv[2]);e=Path(__file__).resolve().parent
# BC250_ROOT is the workspace root, by default the parent directory of this repository.
ROOT=Path(os.environ.get("BC250_ROOT", str(Path(__file__).resolve().parents[2].parent)))
def get(path,marker):
 s=path.read_text();a=s.index(marker);b=s.index('{',a);depth=1;i=b+1
 while depth:
  if s[i]=='{':depth+=1
  elif s[i]=='}':depth-=1
  i+=1
 return s[a:i]+'\n'
code=(e/'oneshot-test-prefix.c').read_text()
path=r/'driver/kmd/guard.c'
if '--before' in sys.argv:path=ROOT/'scratch/m9/oneshot-before/guard.c'
code+=get(path,'ULONG GuardConsumeSetting(')
code+=get(r/'driver/kmd/wddm.c','BOOLEAN WddmGateOpen(')
code+=(e/'oneshot-test-suffix.c').read_text()
out.write_text(code)
