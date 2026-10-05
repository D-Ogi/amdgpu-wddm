from pathlib import Path
import sys,json,time,socket
from datetime import datetime,timezone
sys.path.insert(0,r'P:\bc-250\bc250-win\tools\win')
sys.path.insert(0,r'P:\bc-250\scratch\smartplug')
from target import Target
import plug
out=Path(r'P:\bc-250\scratch\m9');events=[]
def event(name,on):
 events.append(dict(utc=datetime.now(timezone.utc).isoformat(),event=name,relay_on=on))
 (out/'candidate07108-clean-ac.json').write_text(json.dumps(events,indent=2))
t=Target();d=plug.connect()
try:
 assert plug.state(d),'Plug must be on'
 done=t.ssh('shutdown.exe /s /t 0',timeout=20)
 record=dict(utc=datetime.now(timezone.utc).isoformat(),shutdown_exit=done.returncode,stdout=done.stdout,stderr=done.stderr)
 with (out/'candidate07108-shutdown.json').open('x') as f:json.dump(record,f,indent=2)
 assert done.returncode==0,'Shutdown failed'
 print('Normal shutdown accepted; waiting 30 seconds',flush=True)
 time.sleep(30)
 # The selected pinned target was verified by the successful SSH shutdown.
 alive=False
 for host in t.cfg['addresses']:
  try:
   with socket.create_connection((host,int(t.cfg['port'])),timeout=1):alive=True
  except OSError:pass
 assert not alive,'SSH still accepts connections; inspect before cutting power'
 event('post_shutdown_before',plug.state(d))
 try:
  event('off_verified',plug.set_state(d,False));time.sleep(8)
 finally:event('on_verified',plug.set_state(d,True))
 print('One OFF/8s/ON completed with relay verification',flush=True)
finally:d.close()

