from pathlib import Path
import json

p = Path(__file__).resolve().parent
def load(name):
    raw = (p / name).read_bytes()
    return json.loads(raw.decode('utf-16' if raw.startswith(b'\xff\xfe') else 'utf-8-sig'))

s = load('summary.json')
assert s['classification'].startswith('VISUAL FAILURE')
assert s['runner_completion']['success'] and s['runner_completion']['measured_seconds'] >= 180
etw = load('etw-proof.json')
assert etw['dwm_pid'] == s['runner_completion']['gpu_pid'] == 4120
assert etw['trace_loss'] == {'events': 0, 'buffers': 0}
assert etw['matched'] == len(etw['pairs']) == 3818
assert not etw['pending'] and not etw['unmatched'] and not etw['duplicate_starts']
assert all(x['submission_id'] == x['completion_id'] and x['preempted'] in ['false', '0', '0x0'] for x in etw['pairs'])
roi = load('roi-analysis.json')
assert sum(v['pixels'] for v in roi.values()) == 8000
for name, value in roi.items():
    expected = [255, 0, 0] if name == 'red' else [127, 0, 128]
    assert value['baseline_gpu_mismatch'] == value['gpu_screen_mismatch'] == 0
    assert all(value[k] == [[expected, value['pixels']]] for k in ['baseline', 'gpu', 'screen'])
assert len(s['partial_map_audit']['incomplete_rows']) == 1
assert not s['partial_map_audit']['overflow']
restore = load('restored.json')
assert restore['before'] == [4120] and restore['after'] == [8008]
cpu = load('cpu013.json')
assert cpu['success'] and cpu['dwm_before'] == cpu['dwm_after'] == [8008]
print('Evidence consistent: static pixels and GPU execution pass; dynamic visual result remains FAILED')
