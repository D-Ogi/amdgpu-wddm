from pathlib import Path
import sys
r=Path(sys.argv[1]);out=Path(sys.argv[2]);e=Path(__file__).resolve().parent
s=(r/'driver/kmd/ih.c').read_text()
def get(marker):
 a=s.index(marker);b=s.index('{',a);depth=1;i=b+1
 while depth:
  if s[i]=='{':depth+=1
  elif s[i]=='}':depth-=1
  i+=1
 return s[a:i]+'\n'
code=(e/'ih-stop-test-prefix.c').read_text()
a=s.index('typedef struct _BC250_IH_CLOSE_CONTEXT');b=s.index('} BC250_IH_CLOSE_CONTEXT;',a)+len('} BC250_IH_CLOSE_CONTEXT;')
code+=s[a:b]+'\n'
for marker in ['static BOOLEAN IhCloseRoutine(', 'static BOOLEAN IhCloseInterruptAdmission(', 'static BOOLEAN Fini(', 'void IhStop(']:
 code+=get(marker)
if '--omit-sync' in sys.argv:
 code=code.replace('else status=Device->Dxgk.DxgkCbSynchronizeExecution(Device->Dxgk.DeviceHandle,\n            IhCloseRoutine,&close,0,&returned);',
                   'else {returned=IhCloseRoutine(&close);status=0;}')
code+=(e/'ih-stop-test-suffix.c').read_text()
out.write_text(code)
