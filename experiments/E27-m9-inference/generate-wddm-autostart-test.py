from pathlib import Path
import sys
r=Path(sys.argv[1]);out=Path(sys.argv[2]);e=Path(__file__).resolve().parent
def get(file,marker):
 s=(r/file).read_text();a=s.index(marker);b=s.index('{',a);depth=1;i=b+1
 while depth:
  if s[i]=='{':depth+=1
  elif s[i]=='}':depth-=1
  i+=1
 return s[a:i]+'\n'
code=(e/'wddm-autostart-test-prefix.c').read_text()
code+=get('driver/kmd/wddm.c','NTSTATUS WddmStart(')
code+=get('driver/kmd/display.c','static BOOLEAN WddmDiagnosticAllowed(')
if '--omit-startup' in sys.argv:
 code=code.replace('status=GpuStartupInitialize(Device,startup);','if (0) status=GpuStartupInitialize(Device,startup);')
if '--omit-vidmm-cleanup' in sys.argv:
 code=code.replace('if (vidmmPrepared) VidMmStop();','if (vidmmPrepared) (void)vidmmPrepared;')
code+=(e/'wddm-autostart-test-suffix.c').read_text()
out.write_text(code)
