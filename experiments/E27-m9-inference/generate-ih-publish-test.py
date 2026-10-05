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
code=(e/'ih-publish-test-prefix.c').read_text()
a=s.index('typedef struct _BC250_IH_ENABLE_CONTEXT')
b=s.index('} BC250_IH_ENABLE_CONTEXT;',a)+len('} BC250_IH_ENABLE_CONTEXT;')
code+=s[a:b]+'\n'
for marker in ['BOOLEAN IhInterrupt(', 'static BOOLEAN IhEnableRoutine(', 'static NTSTATUS IhPublishAndEnable(']:
 code+=get(marker)
if '--late-active' in sys.argv:
 code=code.replace('InterlockedExchange(&enable->Ih->Active,1);','')
 code=code.replace('enable->Result=bc250_ih_hw_enable(enable->Adev);','enable->Result=bc250_ih_hw_enable(enable->Adev); InterlockedExchange(&enable->Ih->Active,1);')
code+=(e/'ih-publish-test-suffix.c').read_text()
out.write_text(code)
