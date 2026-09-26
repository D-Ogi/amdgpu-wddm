from pathlib import Path
import sys,time,json,subprocess
from datetime import datetime,timezone
root=Path('P:/bc-250');sys.path.insert(0,str(root/'bc250-win/tools/win'))
from target import Target,answers
folder=root/'scratch/m9/cold145'
shutdown=(folder/'shutdown.log').read_text(encoding='utf-8-sig')
assert 'shutdown_request_accepted' in shutdown
requested=datetime.fromisoformat(shutdown.split('full_shutdown_requested=',1)[1].splitlines()[0])
assert (datetime.now(timezone.utc)-requested).total_seconds()>=45
obs=[json.loads(l) for l in (folder/'shutdown-observation.jsonl').read_text().splitlines()]
assert len(obs)>=4 and not any(x['configured_ssh_port_open'] for x in obs)
power=json.loads((folder/'power-after-shutdown.json').read_text(encoding='utf-8-sig'))
assert power['relay_on'] and power['power_w']<20
initial=json.loads((folder/'power-before.json').read_text(encoding='utf-8-sig'))
assert initial['power_w']>power['power_w']*3
cfg=Target().cfg
assert not any(answers(a,cfg['port'],0.5) for a in cfg['addresses'])
with (folder/'power-cycle.jsonl').open('x',encoding='utf-8') as log:
 def action(command):
  r=subprocess.run([sys.executable,str(root/'scratch/smartplug/plug.py'),command],capture_output=True,text=True,timeout=20)
  if r.returncode:raise RuntimeError('Identified plug command failed; inspect state')
  status=json.loads(r.stdout);row={'utc':datetime.now(timezone.utc).isoformat(),'command':command,**status}
  log.write(json.dumps(row)+'\n');log.flush();print(json.dumps(row),flush=True)
  return status
 assert action('status')['relay_on'] is True
 assert action('off')['relay_on'] is False
 time.sleep(30)
 assert action('on')['relay_on'] is True
