from pathlib import Path
import sys
b=Path(r'P:\bc-250\scratch\m9\shared-cpu-cache');s=Path(sys.argv[1] if len(sys.argv)>1 else r'P:\bc-250\bc250-win\driver\kmd\wddm.c').read_text()
def fn(sig):
 p=s.index(sig); q=s.index('{',p); n=1;i=q+1
 while n:
  n+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[p:i]+'\n'
body=fn('static void WddmCpuVisibleAllocationFlags(')+fn('static NTSTATUS WddmSurfaceResourcePolicy(')+fn('static NTSTATUS Bc250WddmCreateAllocation(')
(b/'test.c').write_text((b/'prefix.c').read_text()+body+(b/'suffix.c').read_text())
