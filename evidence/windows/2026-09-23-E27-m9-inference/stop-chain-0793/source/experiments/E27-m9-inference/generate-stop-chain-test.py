from pathlib import Path
import sys
root=Path(sys.argv[1]);out=Path(sys.argv[2]);here=Path(__file__).resolve().parent
def function(file,marker):
 s=(root/file).read_text();a=s.index(marker);b=s.index('{',a);depth=1;i=b+1
 while depth:
  if s[i]=='{':depth+=1
  elif s[i]=='}':depth-=1
  i+=1
 return s[a:i]+'\n'
code=(here/'stop-chain-test-prefix.c').read_text()
for file,marker in [('driver/kmd/gfx.c','static BOOLEAN Fini('),('driver/kmd/gfx.c','void GfxStop('),('driver/kmd/psp.c','void PspStop('),('driver/kmd/gart.c','void GartStop(')]:
 code+=function(file,marker)
code+=(here/'stop-chain-test-suffix.c').read_text()
out.write_text(code)
