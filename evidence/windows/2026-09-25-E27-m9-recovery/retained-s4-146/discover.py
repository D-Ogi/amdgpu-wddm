import sys,json,ipaddress,concurrent.futures
from pathlib import Path
from datetime import datetime,timezone
root=Path('P:/bc-250');sys.path.insert(0,str(root/'bc250-win/tools/win'))
from target import Target,answers,load
cfg=load();net=ipaddress.ip_network(str(cfg['addresses'][0])+'/24',strict=False)
with concurrent.futures.ThreadPoolExecutor(max_workers=24) as pool:
 hits=list(pool.map(lambda a:str(a) if answers(str(a),cfg['port'],0.35) else None,net.hosts()))
hits=[a for a in hits if a];verified=[]
for a in hits:
 c=dict(cfg);c.update(addresses=[a],connect_timeout=3,connection_attempts=1)
 try:
  r=Target(c).run("(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')",timeout=12).strip()
  if r.startswith('2026-'):verified.append({'address':a,'boot':r})
 except Exception:pass
(root/'scratch/m9/resume146/discovery-private.json').write_text(json.dumps({'utc':datetime.now(timezone.utc).isoformat(),'ssh_candidates':hits,'verified':verified},indent=2),encoding='utf-8')
print('SSH candidates:',len(hits),'pinned Windows matches:',len(verified))
if verified:
 print('Verified boot:',verified[0]['boot'])
 p=root/'secrets/client/target.json';original=p.read_text(encoding='utf-8');data=json.loads(original)
 addr=verified[0]['address']
 if data['addresses'][0]!=addr:
  (root/'secrets/client/target-before-resume146-dhcp.json').write_text(original,encoding='utf-8')
  data['addresses']=[addr]+[x for x in data['addresses'] if x!=addr]
  p.write_text(json.dumps(data,indent=2)+'\n',encoding='utf-8');print('Private target endpoint order updated')
