from pathlib import Path
import json,statistics,xml.etree.ElementTree as ET
from datetime import datetime,timezone
w=Path('P:/bc-250/scratch/m9/s4-isolation146')
b=(w/'power-events.xml').read_bytes();s=b.decode('utf-16') if b.startswith(b'\xff\xfe') else b.decode('utf-8-sig')
ns={'e':'http://schemas.microsoft.com/win/2004/08/events/event'}
events=ET.fromstring('<Root>'+s+'</Root>');controls=[]
for e in events:
 provider=e.find('e:System/e:Provider',ns)
 if provider is not None and provider.attrib.get('Name')=='Microsoft-Windows-Power-Troubleshooter':
  d={x.attrib['Name']:x.text for x in e.findall('e:EventData/e:Data',ns)}
  if d.get('SleepTime') and d.get('WakeTime'):controls.append(d)
assert controls
control=max(controls,key=lambda x:x['WakeTime']);sleep=datetime.fromisoformat(control['SleepTime']);wake=datetime.fromisoformat(control['WakeTime'])
raw=(w/'cpu-timeline.jsonl').read_text(encoding='utf-8-sig')
trailing_nuls=len(raw)-len(raw.rstrip('\x00'))
rows=[json.loads(l) for l in raw.rstrip('\x00').splitlines() if l.strip()]
result={'sleep':control['SleepTime'],'wake':control['WakeTime'],'effective_state':control.get('EffectiveState'),'samples':len(rows),'trailing_nul_bytes_excluded':trailing_nuls,'groups':{}}
for label,select in [('before',lambda t:t<sleep),('after',lambda t:t>=wake),('after30s',lambda t:(t-wake).total_seconds()>=30)]:
 selected=[r for r in rows if select(datetime.fromisoformat(r['Utc']))]
 group={'samples':len(selected)}
 for k in ['Cpu','Dpc','Irq','Interrupts','AvailableMB','PagesIn','PagesOut']:
  values=sorted(float(r[k]) for r in selected if r.get(k) is not None)
  if values:group[k]={'min':min(values),'median':statistics.median(values),'max':max(values),'p95':values[min(len(values)-1,int(len(values)*.95))]}
 result['groups'][label]=group
(w/'cpu-analysis.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
