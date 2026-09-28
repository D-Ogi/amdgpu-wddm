from pathlib import Path
import json,re,datetime as dt
p=Path('P:/BC-250/scratch/g0-hosted/dwm049-ops/result')
def text(p):
 b=p.read_bytes();return b.decode('utf-16' if b.startswith(b'\xff\xfe') else 'utf-8-sig')
trace_start=dt.datetime(1601,1,1,tzinfo=dt.timezone.utc)+dt.timedelta(microseconds=134350429652537650//10)
rows=[]
for f in sorted(p.glob('startup-log-*.txt.json')):
 r=json.loads(text(f));s=text(f.with_suffix(''));heads=list(re.finditer(r'wddm summary: vsync (?:enabled|not enabled by ControlInterrupt),',s))
 if not heads:continue
 s=s[heads[-1].start():]
 m=re.search(r'armed (\d+) acked \d+ refused (\d+) completion-deferred (\d+) old-buffer-reports',s)
 if not m:continue
 r.update(ticks=int(m[1]),deferred=int(m[2]),old_buffer=int(m[3]));r['skipped']=r['deferred']-r['old_buffer']
 r['trace_seconds']=(dt.datetime.fromisoformat(r['utc'].replace('Z','+00:00'))-trace_start).total_seconds();rows.append(r)
out={'scope':'Sample timestamps precede summary escape/read. Coarse bracketing only, not per-event branch trace.','trace_start':trace_start.isoformat(),'gap_start_us':30655600,'gap_end_us':30688985,'samples':rows}
(p.parent/'deferred-timeline.json').write_text(json.dumps(out,indent=2)+'\n',newline='\n')
prev=None
for i,r in enumerate(rows):
 key=(r['deferred'],r['old_buffer'])
 if key!=prev or 28<r['trace_seconds']<34:
  print({k:r[k] for k in ['sample','utc','trace_seconds','ticks','deferred','old_buffer','skipped']})
 prev=key
