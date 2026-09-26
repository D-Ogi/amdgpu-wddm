from pathlib import Path
import sys,json
from datetime import datetime,timezone
r=Path('P:/bc-250');sys.path.insert(0,str(r/'bc250-win/tools/win'))
from target import Target
x=Target();up=x.wait(35)
row={'utc':datetime.now(timezone.utc).isoformat(),'ssh_available':up}
print(json.dumps(row));p=r/'scratch/m9/resume146/wake-observation.jsonl'
with p.open('a') as f:f.write(json.dumps(row)+'\n')
if up:
 try:
  result=x.run_script(str(r/'scratch/m9/resume146/postwake.ps1'),timeout=35)
  (r/'scratch/m9/resume146/postwake.log').write_text(result.stdout+'\n'+result.stderr,encoding='utf-8')
  print('postwake_exit',result.returncode)
 except Exception as e:print('postwake_error',type(e).__name__)
