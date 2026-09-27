from pathlib import Path
import json
p=Path(__file__).resolve().parent
def load(n):
 b=(p/n).read_bytes();return json.loads(b.decode('utf-16' if b.startswith(b'\xff\xfe') else 'utf-8-sig'))
s=load('summary.json');e=load('etw-proof.json');r=load('roi-analysis.json');c=load('dynamic-color-analysis.json')
assert s['classification'].startswith('VISUAL FAIL')
assert s['runner_completion']['gpu_pid']==e['dwm_pid']==952
assert s['runner_completion']['measured_seconds']>=180
assert e['trace_loss']=={'events':0,'buffers':0}
assert e['matched']==len(e['pairs'])==3872
assert not(e['pending'] or e['unmatched'] or e['duplicate_starts'])
assert all(x['submission_id']==x['completion_id'] and x['preempted'] in ['false','0','0x0'] for x in e['pairs'])
assert sum(x['pixels'] for x in r.values())==8000
assert sum(x['baseline_gpu_mismatch'] for x in r.values())==0
assert sum(x['gpu_screen_mismatch'] for x in r.values())==8000
assert c['dynamic-26.bmp']['cyan_outside_control_motion_envelope']==167179
assert c['screen.png']['cyan_outside_control_motion_envelope']==51070
assert load('restored.json')['after']==[4400]
print('Evidence checks pass; DWM014 visual acceptance remains FAIL.')
