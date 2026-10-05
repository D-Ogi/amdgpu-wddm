from pathlib import Path
import sys
root=Path(sys.argv[1]);out=Path(sys.argv[2]);before=Path(sys.argv[3]);here=Path(__file__).resolve().parent
def get(s,marker):
 a=s.index(marker);b=s.index('{',a);depth=1;i=b+1
 while depth:
  if s[i]=='{':depth+=1
  elif s[i]=='}':depth-=1
  i+=1
 return s[a:i]
code=(here/'init-entrypoints-test-prefix.c').read_text()
for title in ['Gart','Psp','Ih','Gfx']:
 file='driver/kmd/'+title.lower()+'.c';s=(root/file).read_text();old=(before/file).read_text()
 old_body=get(old,'void '+title+'Escape(').split('{',1)[1]
 new_body=get(s,'static void '+title+'Execute(').split('{',1)[1]
 assert old_body==new_body, title+' core changed during extraction'
 print(title+': execution body unchanged')
 code+=get(s,'void '+title+'Escape(')+'\n'
 code+=get(s,'NTSTATUS '+title+'InitializeHardware(')+'\n'
if '--omit-completion-check' in sys.argv:
 code=code.replace('status=STATUS_IO_DEVICE_ERROR;','status=STATUS_SUCCESS;')
code+=(here/'init-entrypoints-test-suffix.c').read_text()
out.write_text(code)
