from pathlib import Path
import sys
here=Path(__file__).resolve().parent
s=Path(sys.argv[1]).read_text()
def function(marker):
 a=s.index(marker);b=s.index('{',a);depth=1;i=b+1
 while depth:
  if s[i]=='{':depth+=1
  if s[i]=='}':depth-=1
  i+=1
 return s[a:i]+'\n'
actual=function('static int PagingCopyStorageInit(')+function('static void PagingCopyStorageFree(')+function('static int SetUp(')
Path(sys.argv[2]).write_text((here/'pte-copy-storage-prefix.c').read_text()+actual+(here/'pte-copy-storage-suffix.c').read_text())
