from pathlib import Path
import sys
r=Path(sys.argv[1]);o=Path(sys.argv[2]);e=Path(__file__).resolve().parent
def get(s,marker):
 a=s.index(marker);b=s.index('{',a);depth=1;i=b+1
 while depth:
  if s[i]=='{':depth+=1
  elif s[i]=='}':depth-=1
  i+=1
 return s[a:i]
s=(e/'startup-coordinator-test-prefix.c').read_text()
g=(r/'driver/kmd/gfx.c').read_text()
for name in ['static BOOLEAN GfxSubmitArmed(', 'static BOOLEAN GfxSubmitReadyAccess(', 'static BOOLEAN GfxPagingSubmitReadyAccess(', 'BOOLEAN GfxStartupResources(']:
 s+=get(g,name)+'\n'
s+=(e/'startup-coordinator-test-mocks.c').read_text()
s+=get((r/'driver/kmd/startup.c').read_text(),'NTSTATUS GpuStartupInitialize(')
if '--omit-unwind' in sys.argv:
 s=s.replace('    IhStop(Device);','    if (0) IhStop(Device);')
if '--omit-readiness' in sys.argv:
 s=s.replace('if (!GfxStartupResources(Device,TRUE))','if (0)')
s+=(e/'startup-coordinator-test-suffix.c').read_text()
o.write_text(s)
