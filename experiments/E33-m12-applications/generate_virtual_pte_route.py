from pathlib import Path
import sys
r=Path(sys.argv[1]);out=Path(sys.argv[2]);e=Path(__file__).resolve().parent
def extract(path,marker):
 s=(r/path).read_text();a=s.index(marker);b=s.index('{',a);depth=1;i=b+1
 while depth:
  if s[i]=='{':depth+=1
  elif s[i]=='}':depth-=1
  i+=1
 return s[a:i]
code=(e/'virtual-pte-route-prefix.c').read_text()
code+='\n'+extract('driver/kmd/gfx.c','NTSTATUS GfxPagingBuildVirtualPtes(')
code+='\n'+extract('driver/kmd/wddm.c','static NTSTATUS WddmBuildNativePagingCopies(')
code+='\n'+(e/'virtual-pte-route-suffix.c').read_text()
out.write_text(code,newline='\n')
