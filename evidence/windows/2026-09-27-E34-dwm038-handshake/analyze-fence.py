from pathlib import Path
import re,json
p=Path(__file__).resolve().parent/'result'
b=(p/'done.json').read_bytes();done=json.loads(b.decode('utf-16' if b.startswith(b'\xff\xfe') else 'utf-8-sig'));pid=done['gpu_pid']
s=(p/f'dwm-{pid}.log').read_text(encoding='utf-8');rows=[]
for line in s.splitlines():
 if not line.startswith('BC250 audit progress '):continue
 fields=dict(re.findall(r'(\w+)=([^ ]+)',line))
 for key in ['pid','tick','present','context','submitted','completed','submits']:fields[key]=int(fields[key])
 assert fields['pid']==pid
 assert fields['completed']<=fields['submitted']
 rows.append(fields)
assert rows
keys={tuple(x[k] for k in ['device','context','sync']) for x in rows};summary=[]
for key in sorted(keys):
 a=[x for x in rows if tuple(x[k] for k in ['device','context','sync'])==key]
 for first,last in zip(a,a[1:]):
  for n in ['tick','present','submitted','completed','submits']:assert last[n]>=first[n]
 summary.append({'identity':key,'samples':len(a),'first':a[0],'last':a[-1],'samples_caught_up':sum(x['completed']==x['submitted'] for x in a)})
out=p/'fence-progress.json';assert not out.exists();out.write_text(json.dumps({'scope':'Sampled per-Present queue fence progress, not a one-to-one ETW ID mapping or a display-scanout timestamp.','queues':summary},indent=2)+'\n',newline='\n');print(json.dumps(summary,indent=2))
