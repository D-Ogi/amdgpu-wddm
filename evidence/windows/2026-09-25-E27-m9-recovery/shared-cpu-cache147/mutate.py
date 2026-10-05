from pathlib import Path
b=Path(r'P:\bc-250\scratch\m9\shared-cpu-cache');s=Path(r'P:\bc-250\bc250-win\driver\kmd\wddm.c').read_text()
for name,old,new in [
 ('cache-primary',' && (words[3]&1ul)==0',''),
 ('legacy-wc','info->FlagsWddm2.Cached = cachedCpu &&','info->FlagsWddm2.Cached = FALSE &&'),
 ('erase-reserved','    Flags->CpuVisible = 1;','    Flags->Value = 0;\n    Flags->CpuVisible = 1;')]:
 assert s.count(old)==1,(name,s.count(old))
 t=s.replace(old,new,1)
 if name=='legacy-wc':t=t.replace('BOOLEAN sharedCpu = FALSE, cachedCpu = FALSE;','BOOLEAN sharedCpu = FALSE, cachedCpu = FALSE;')
 (b/(name+'.c')).write_text(t)
