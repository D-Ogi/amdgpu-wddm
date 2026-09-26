from pathlib import Path
import sys,time,json,subprocess
from datetime import datetime,timezone
r=Path('P:/bc-250');sys.path.insert(0,str(r/'bc250-win/tools/win'))
from target import Target,answers
cfg=Target().cfg
p=r/'scratch/m9/resume146/hibernate-observation.jsonl'
with p.open('a') as f:
 for i in range(4):
  up=any(answers(a,cfg['port'],0.5) for a in cfg['addresses'])
  result=subprocess.run([sys.executable,str(r/'scratch/smartplug/plug.py'),'telemetry'],capture_output=True,text=True,timeout=15)
  row={'utc':datetime.now(timezone.utc).isoformat(),'configured_ssh_port_open':up,'power':json.loads(result.stdout) if result.returncode==0 else {'error':result.returncode}}
  print(json.dumps(row),flush=True);f.write(json.dumps(row)+'\n');f.flush()
  if i<3:time.sleep(10)
