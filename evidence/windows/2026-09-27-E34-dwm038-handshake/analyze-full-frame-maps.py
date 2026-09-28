from pathlib import Path
import json,re
p=Path(__file__).resolve().parent/'result'
done=json.loads((p/'done.json').read_text(encoding='utf-8-sig'))
s=(p/('dwm-'+str(done['gpu_pid'])+'.log')).read_text()
last_progress=None;rows=[];previous={};changes=[]
for line in s.splitlines():
 if line.startswith('BC250 audit progress '):
  last_progress=dict(re.findall(r'(\w+)=([^ ]+)',line))
 if not line.startswith('BC250 audit bucket '):continue
 f=dict(re.findall(r'(\w+)=([^ ]+)',line))
 if f['target']=='0' or f['size']!='1920x1200x1':continue
 for k in ['sample','id','calls','bytes']:f[k]=int(f[k])
 key=(f['ctx'],f['id']);prior=previous.get(key)
 if prior:
  assert f['calls']>=prior['calls'] and f['bytes']>=prior['bytes']
 if not prior or f['calls']!=prior['calls']:
  changes.append(dict(bucket=f,delta_calls=f['calls']-(prior['calls'] if prior else 0),delta_bytes=f['bytes']-(prior['bytes'] if prior else 0),preceding_progress=last_progress))
 previous[key]=f;rows.append(f)
out={'scope':'Cumulative full-desktop-sized map requests, not CPU stores. Progress is preceding sampled log context, not an exact map timestamp. Initial positive deltas include preexisting calls.','snapshots':len(rows),'last':list(previous.values()),'changes':changes}
f=p/'full-frame-map-changes.json';assert not f.exists();f.write_text(json.dumps(out,indent=2))
print(json.dumps(out,indent=2))
