from pathlib import Path
import json,re
base=Path('scratch/m9/radv-main-collected')
for model in ['stories15M','tinyllama']:
 a=json.loads((base/'bench-new'/(model+'.out')).read_text());b=json.loads((base/'bench-old'/(model+'.out')).read_text())
 for x,y in zip(a,b):print(model,x['n_prompt'],round((x['avg_ts']/y['avg_ts']-1)*100,2))
for trial in ['control3','bench-new','bench-old']:
 b=(base/trial/'after.log').read_bytes();s=b.decode('utf-16')
 print(trial)
 for line in s.splitlines()[-62:]:
  if any(x in line for x in ['node 0 hardware','node 1 (paging','capture','no TDR','vidpn flip']):print(line)
