from pathlib import Path
import csv,json,statistics,collections
w=Path('P:/bc-250/scratch/m10/timing-03')
rows=list(csv.DictReader((w/'frame.csv').open()));freq=int(rows[0]['qpc_frequency'])
start=int(rows[0]['start_qpc']);end=int(rows[-1]['present_return_qpc'])
def stats(v):
 s=sorted(v);return {'n':len(s),'min':s[0],'median':statistics.median(s),'p95':s[int((len(s)-1)*.95)],'max':s[-1]}
intervals=[(int(b['present_return_qpc'])-int(a['present_return_qpc']))/freq for a,b in zip(rows,rows[1:]) if int(a['frame']) not in [1,61]]
headers={};vs=[];header=True
with (w/'etw-events.csv').open(encoding='utf-8',errors='replace',newline='') as f:
 for row in csv.reader(f,skipinitialspace=True):
  row=[x.strip() for x in row]
  if not row:continue
  name=row[0]
  if name=='BeginHeader':continue
  if name=='EndHeader':header=False;continue
  if header:headers[name]=row;continue
  if '/VSyncDPC/' in name:
   d=dict(zip(headers[name],row));qpc=int(d['FrameQPCTime'])
   if start<=qpc<=end:vs.append(qpc)
delta=[(b-a)/freq for a,b in zip(vs,vs[1:])]
result={'frames':len(rows),'native_results':dict(collections.Counter(r['result'] for r in rows)),
 'capture_holds_after_frames':[1,61],'present_return_fps_excluding_capture_holds':len(intervals)/sum(intervals),
 'present_intervals_ms':stats([x*1000 for x in intervals]),
 'draw_present_ms':stats([(int(r['present_return_qpc'])-int(r['start_qpc']))*1000/freq for r in rows]),
 'hardware_vsync_samples':len(vs),'hardware_refresh_hz':1/statistics.mean(delta),
 'hardware_vsync_intervals_ms':stats([x*1000 for x in delta]),
 'dwm_hr':dict(collections.Counter(r['dwm_hr'] for r in rows)),
 'dwm_refresh_delta':dict(collections.Counter(int(b['refresh_count'])-int(a['refresh_count']) for a,b in zip(rows,rows[1:]) if int(a['frame']) not in [1,61]))}
(w/'timing.json').write_text(json.dumps(result,indent=2))
(w/'vsync-qpc.json').write_text(json.dumps({'qpc_frequency':freq,'samples':vs}))
print(json.dumps(result,indent=2))
