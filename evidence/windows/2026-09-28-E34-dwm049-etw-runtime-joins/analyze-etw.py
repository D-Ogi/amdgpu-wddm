from pathlib import Path
import csv, collections, hashlib, json, re

root = Path(__file__).resolve().parent
p = root / 'result'
out = root / 'verified'
out.mkdir(exist_ok=True)
def save(name, value):
    (out / name).write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')
def read(path):
    raw = path.read_bytes()
    return raw.decode('utf-16' if raw.startswith(b'\xff\xfe') else 'utf-8-sig')

done = json.loads(read(p / 'done.json'))
assert done['success'] is False and 'Native client did not complete' in done['failure']
assert done['restoration_succeeded'] is True
# Analyze execution independently; this does not override whole-trial failure.
assert 0 < done['measured_seconds'] <= 180
dwm_pid = int(done['gpu_pid'])
headers = {}; owners = {}; starts = {}; pairs = []; unmatched = []; duplicates = []
context_rows = []; lost = None; last_time = -1.0
with (p / 'events-xperf.csv').open(encoding='utf-8-sig', newline='') as f:
    inside = False
    for row in csv.reader(f):
        if not row:
            continue
        row = [x.strip() for x in row]; name = row[0]
        if name == 'BeginHeader': inside = True; continue
        if name == 'EndHeader': inside = False; continue
        if inside: headers[name] = row; continue
        if name.startswith('OS Version:'):
            text = ', '.join(row)
            lost = {key: int(re.search(label + r':\s*(\d+)', text)[1]) for key, label in [('events', 'Events Lost'), ('buffers', 'Buffers lost')]}
        if name not in headers or not any('/' + x + '/' in name for x in ['Context', 'DmaPacket']):
            continue
        e = dict(zip(headers[name], row)); timestamp = float(e['TimeStamp'])
        assert timestamp >= last_time
        last_time = timestamp
        ctx = e.get('hContext')
        if '/Context/win:Start' in name:
            owners[ctx] = bool(re.search(rf'\(\s*{dwm_pid}\s*\)', e.get('Process Name ( PID)', '')))
            if owners[ctx]: context_rows.append({k: e.get(k) for k in ['TimeStamp','hContext','ContextHandle','NodeOrdinal','EngineAffinity']})
        if owners.get(ctx) and '/DmaPacket/' in name:
            key = (ctx, e['ulQueueSubmitSequence'])
            if name.endswith('/win:Start'):
                if key in starts: duplicates.append(key)
                starts[key] = e
            elif name.endswith('/win:Stop'):
                a = starts.pop(key, None)
                if not a: unmatched.append(e); continue
                pairs.append(dict(context=ctx, queue_sequence=e['ulQueueSubmitSequence'], start=a['TimeStamp'], stop=e['TimeStamp'], packet_type_start=a['PacketType'], packet_type_stop=e['PacketType'], submission_id=a['uliSubmissionId'], completion_id=e['uliCompletionId'], preempted=e['bPreempted']))
        if name.endswith('/Context/win:Stop'): owners.pop(ctx, None)
assert lost == {'events': 0, 'buffers': 0}
assert len(pairs) > 0 and not starts and not unmatched and not duplicates
save('etw-proof.json', dict(dwm_pid=dwm_pid, trace_loss=lost, matched=len(pairs), pending=len(starts), unmatched=len(unmatched), duplicate_starts=len(duplicates), contexts=context_rows, pairs=pairs))


assert all(x['submission_id']==x['completion_id'] and x['preempted'] in ['false','0','0x0'] for x in pairs)
print('DWM DMA pairs',len(pairs),'loss',lost)

