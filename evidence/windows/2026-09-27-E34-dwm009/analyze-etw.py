import csv,collections,json,re
from pathlib import Path
p=Path('P:/BC-250/scratch/g0-hosted/dwm009/result');headers={};events=[];inside=False
with (p/'events-partial.csv').open(encoding='utf-8-sig',newline='') as f:
 for row in csv.reader(f):
  if not row:continue
  row=[x.strip() for x in row];name=row[0]
  if name=='BeginHeader':inside=True;continue
  if name=='EndHeader':inside=False;continue
  if inside:headers[name]=row;continue
  if name in headers and any('/'+x+'/' in name for x in ['Context','QueuePacket','DmaPacket','Present','Device']):
   events.append(dict(zip(headers[name],row)))
contexts={e.get('hContext'):e for e in events if '/Context/win:Start' in next(iter(e.values()),'') and re.search(r'\(\s*12804\s*\)',e.get('Process Name ( PID)',''))}
print('DWM contexts', [{k:e.get(k) for k in ['hContext','ContextHandle','NodeOrdinal','EngineAffinity','hDevice']} for e in contexts.values()])
print('PID format examples',[e.get('Process Name ( PID)') for e in events if '/Context/win:Start' in next(iter(e.values()),'')][:5])
counts=collections.Counter()
for e in events:
 if e.get('hContext') in contexts:counts[next(iter(e.values()))]+=1
print('Direct context counts',dict(counts))
(p/'etw-contexts-partial.json').write_text(json.dumps({'contexts':list(contexts.values()),'counts':dict(counts)},indent=2)+'\n',encoding='utf-8')
