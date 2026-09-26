from pathlib import Path
import re,subprocess,json
rows=[]
for line in Path('scratch/m13/observe139-samples.txt').read_text().splitlines():
 if line.startswith('OBS '):
  d=dict(re.findall(r'(\w+)=([^ ]+)',line));d['words']=[int(x,16) for x in d['raw'].split(',')];rows.append(d)
assert len(rows)==16 and all(r['valid']=='0x003FFFFF' and r['nt']=='0x00000000' and r['reason']=='0x00000000' for r in rows)
assert all(r['words'][11:]==rows[0]['words'][11:] for r in rows)
assert all(r['before']==r['after'] and r['words'][:2]==r['words'][2:4] and r['words'][4]==0 for r in rows)
args=['scratch/build/bd018-final/timing_test.exe','--snapshot','1920','1200']+[hex(x) for x in rows[0]['words'][11:]]
p=subprocess.run(args,text=True,capture_output=True);assert p.returncode==0,p.stdout+p.stderr
first,last=rows[0],rows[-1];freq=int(first['frequency']);frames=last['words'][10]-first['words'][10]
lo=(int(last['begin'])-int(first['end']))/freq;hi=(int(last['end'])-int(first['begin']))/freq
expected=154000000/(2080*1235);elapsed=(lo+hi)/2
assert abs(frames-expected*elapsed)<1.01
result={'snapshots':16,'stable_timing_tuples':16,'latch_match_pending_clear':16,'reference_hz':600000000,'pixel_hz':154000000,'decoded_refresh_hz':expected,'frames':frames,'interval_seconds_min':lo,'interval_seconds_max':hi,'counter_rate_hz':frames/elapsed,'frame_quantization_error':frames-expected*elapsed,'decoder_output':p.stdout,'limitations':'Observation idles graphics scheduling; stable scanned buffer only, not a pending-flip stress test.'}
Path('scratch/m13/observe139-analysis.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
