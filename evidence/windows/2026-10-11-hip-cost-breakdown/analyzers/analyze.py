"""Summarize immutable current-run evidence; retain per-token data and input hashes."""
from pathlib import Path
import json,hashlib,statistics
here=Path(__file__).resolve().parent
out={'runs':[],'source_hashes':{},'llama_seconds':0,'supervisor_seconds':0}
def read(path):
    raw=path.read_bytes()
    out['source_hashes'][path.relative_to(here).as_posix()]=hashlib.sha256(raw).hexdigest()
    return raw.decode('utf-8-sig')
for run in sorted((here/'runs').iterdir()):
    s=json.loads(read(run/'supervisor.json')); a=json.loads(read(run/'arm.json'))['Arm']
    w=json.loads(read(run/'worker-result.json')) if (run/'worker-result.json').exists() else {}
    err=read(run/'stderr.txt') if (run/'stderr.txt').exists() else ''
    row={'run':run.name,'arm':a['id'],'parameters':a,'supervisor':{k:s.get(k) for k in ('Verdict','Reason','ElapsedSeconds','TctlMax','CleanupKnown','Leftovers','TaskAbsent','ProfileClosed')},'worker':w}
    text=read(run/'stdout.txt') if (run/'stdout.txt').exists() else ''
    try:
        data=json.loads(text)
        assert len(data)==1 and data[0]['n_gen']==128
        row['bench']=data[0];row['ms_per_token']=data[0]['avg_ns']/128/1e6
    except (ValueError,AssertionError,KeyError): row['bench_invalid']=True
    events=[json.loads(line) for line in err.splitlines() if line.startswith('{"event":"bc250-decode-profile"')]
    gen=[e for e in events if e['phase'].startswith('generation')]
    if gen:
        assert len(gen)==129 and all(e['counter_valid'] and e['decode_status']==0 and e['tokens']==1 for e in gen)
        measured=gen[1:];row['warmups_excluded']=1;row['token_profiles']=measured
        fields=('wall_ms','dispatches','submissions','batches','dispatches_batched','waits','waits_fast')
        total={k:sum(e[k] for e in measured) for k in fields}
        mean={k:v/128 for k,v in total.items()}
        mean['nonfast_waits']=mean['waits']-mean['waits_fast']
        mean['dispatches_per_submission']=total['dispatches']/total['submissions']
        row['profile_means']=mean;row['profile_totals']=total
        row['profile_ranges']={k:[min(e[k] for e in measured),max(e[k] for e in measured)] for k in fields}
        row['profile_median_ms']=statistics.median(e['wall_ms'] for e in measured)
        row['inter_profile_and_logging_ms']=(measured[-1]['qpc_end']-measured[0]['qpc_start'])/measured[0]['qpc_hz']*1000-total['wall_ms']
    row['context_lines']=[line for line in err.splitlines() if any(x in line for x in ('n_ctx','flash_attn','graph splits','graph nodes'))]
    row['thermal']=s['Thermal']
    if w.get('Known') and 'EndQpc' in w: out['llama_seconds']+=(w['EndQpc']-w['StartQpc'])/10000000
    out['supervisor_seconds']+=s['ElapsedSeconds']
    read(run/'health-before.txt');read(run/'health-after.txt')
    out['runs'].append(row)
(here/'summary.json').write_text(json.dumps(out,indent=2)+'\n')
print(json.dumps({'llama_seconds':out['llama_seconds'],'supervisor_seconds':out['supervisor_seconds'],'runs':[{k:r.get(k) for k in ('arm','ms_per_token','profile_means','supervisor')} for r in out['runs']]},indent=2))
