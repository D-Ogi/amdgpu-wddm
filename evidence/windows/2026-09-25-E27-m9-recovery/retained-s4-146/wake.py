from pathlib import Path
import sys,time,json,subprocess
from datetime import datetime,timezone
root=Path('P:/bc-250');sys.path.insert(0,str(root/'bc250-win/tools/win'))
from target import Target,answers
folder=root/'scratch/m9/resume146'
assert 'hibernate_task_started' in (folder/'hibernate.log').read_text(encoding='utf-8-sig')
obs=[json.loads(l) for l in (folder/'hibernate-observation.jsonl').read_text().splitlines()]
assert len(obs)>=4 and not any(x['configured_ssh_port_open'] for x in obs)
assert all(x['power']['relay_on'] and x['power']['power_w']<20 for x in obs)
initial=json.loads((folder/'power-before.json').read_text(encoding='utf-8-sig'))
assert initial['power_w']>obs[-1]['power']['power_w']*3
cfg=Target().cfg
assert not any(answers(a,cfg['port'],0.5) for a in cfg['addresses'])
with (folder/'s4-wake-power.jsonl').open('x') as f:
 def action(cmd):
  p=subprocess.run([sys.executable,str(root/'scratch/smartplug/plug.py'),cmd],capture_output=True,text=True,timeout=20)
  if p.returncode:raise RuntimeError('Plug command failed; inspect before retry')
  data=json.loads(p.stdout);row={'utc':datetime.now(timezone.utc).isoformat(),'command':cmd,**data}
  print(json.dumps(row),flush=True);f.write(json.dumps(row)+'\n');f.flush();return data
 assert action('status')['relay_on']
 assert not action('off')['relay_on']
 time.sleep(30)
 assert action('on')['relay_on']
