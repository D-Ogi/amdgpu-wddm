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

headers = {}; owners = {}; starts = {}; pairs = []; unmatched = []; duplicates = []
context_rows = []; lost = None; last_time = -1.0
with (p / 'events.csv').open(encoding='utf-8-sig', newline='') as f:
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
            owners[ctx] = bool(re.search(r'\(\s*12608\s*\)', e.get('Process Name ( PID)', '')))
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
assert len(pairs) == 589 and not starts and not unmatched and not duplicates
save('etw-proof.json', dict(dwm_pid=12608, trace_loss=lost, matched=len(pairs), pending=len(starts), unmatched=len(unmatched), duplicate_starts=len(duplicates), contexts=context_rows, pairs=pairs))

log = read(p / 'dwm-12608.log'); rows = []; progress = []; sample_positions = {}
for lineno, line in enumerate(log.splitlines(), 1):
    if 'BC250 audit progress ' in line:
        fields = dict(re.findall(r'(\w+)=([^ ]+)', line)); fields['line'] = lineno; progress.append(fields)
    if 'BC250 audit bucket ctx=' in line:
        fields = dict(re.findall(r'(\w+)=([^ ]+)', line))
        for k in ['sample','id','target','format','runtime','user_ptr','calls','bytes']: fields[k] = int(fields[k])
        for k in ['bind','usage']: fields[k] = int(fields[k],16)
        fields['line'] = lineno; rows.append(fields)
        sample_positions[fields['sample']] = lineno
assert not re.search(r'overflow=[1-9]', log)
assert len({x['ctx'] for x in rows}) == 1
samples = sorted({x['sample'] for x in rows})
full = [x for x in rows if x['target'] != 0 and x['size'] == '1920x1200x1']
persistent = [x for x in rows if x['sample'] == samples[-1] and (x['usage'] & 256 or x['user_ptr'])]
assert all(x['target'] == 0 for x in persistent)
save('map-proof.json', dict(samples=samples, bucket_overflow=0, full_frame_history=full, persistent_final=persistent, progress=progress, sample_log_lines=sample_positions, scope='Cumulative bounded snapshots, not a final teardown snapshot or a count of stores through persistent pointers.'))
summary = {}
for name in ['kmd-start.log','kmd-end.log']:
    lines = [x for x in read(p/name).splitlines() if 'blit gate open' in x]
    assert lines
    summary[name] = lines[-1]
save('kmd-summary.json', summary)
save('private-artifact-hashes.json', {x.name: dict(bytes=x.stat().st_size, sha256=hashlib.sha256(x.read_bytes()).hexdigest()) for x in p.iterdir() if x.is_file()})
print('Verified: 589 DMA pairs, zero trace loss, zero bucket overflow; reports written.')
