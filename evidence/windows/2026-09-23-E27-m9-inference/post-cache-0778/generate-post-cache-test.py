from pathlib import Path
import sys
root=Path(sys.argv[1]);out=Path(sys.argv[2]);here=Path(__file__).resolve().parent
def extract(file,marker):
 s=(root/file).read_text();a=s.index(marker);b=s.index('{',a);depth=1;i=b+1
 while depth:
  if s[i]=='{':depth+=1
  if s[i]=='}':depth-=1
  i+=1
 return s[a:i]+'\n'
s=extract('driver/kmd/display.c','static BOOLEAN IsPostFormatSupported(')+extract('driver/kmd/display.c','NTSTATUS DisplayMapFramebuffer(')+extract('driver/kmd/display.c','void DisplayUnmapFramebuffer(')+extract('driver/kmd/vram.c','ULONG VramMappingProtection(')
if '--always-nc' in sys.argv:s=s.replace('return Access|Device->FramebufferCacheProtect;','return Access|PAGE_NOCACHE;')
out.write_text((here/'post-cache-prefix.c').read_text()+s+(here/'post-cache-suffix.c').read_text())
