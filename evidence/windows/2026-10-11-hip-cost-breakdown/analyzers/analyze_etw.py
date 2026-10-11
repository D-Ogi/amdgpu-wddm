"""Reuse the existing ETW packet parser and QPC clock fitting for token windows."""
from pathlib import Path
import sys,json,hashlib,subprocess,collections,re
here=Path(__file__).resolve().parent;root=here.parents[3]
sys.path.insert(0,str(root/'scratch/m15/gpu-timeline/align'))
import etwdump,etwclock
run,=(here/'runs').glob('hipcost-trace-gemma-hip-*')
etl=run/'etw/gpu-B.etl';analysis=here/'etw-analysis';analysis.mkdir(exist_ok=True)
dump=analysis/'gpu-B-dump.txt'
if not dump.exists():
    subprocess.run([etwclock.XPERF,'-i',str(etl),'-o',str(dump),'-a','dumper'],check=True,timeout=80,capture_output=True)
clock_type,span,start=etwclock.stats(str(etl),analysis)
if clock_type!='PerfCounter': raise SystemExit('ETW clock is not QPC')
stats=(analysis/'tracestats.txt').read_text()
lost={k:int(re.search('Total # Lost '+k+r'\s*:\s*(\d+)',stats)[1]) for k in ('Buffers','Events')}
assert not any(lost.values()), 'Trace loss invalidates complete token attribution'
end_us=int(span*1e6)
points=etwclock.pairs(str(etl),end_us//10,end_us//10+5000,analysis,'first')+etwclock.pairs(str(etl),end_us*9//10,end_us*9//10+5000,analysis,'last')
qpc0,freq,residual=etwclock.fit(points)
assert residual<=1 and abs(freq-round(freq))<1e-3*round(freq)
clock={'qpc0':round(qpc0),'freq':round(freq),'residual_ticks':residual,'points':len(points),'span_s':span,'start_utc':start}
data=etwdump.load(str(dump),str(analysis/'packets.pickle'))
events=[json.loads(l) for l in (run/'stderr.txt').read_text(encoding='utf-8-sig').splitlines() if l.startswith('{"event":"bc250-decode-profile"')]
gen=[e for e in events if e['phase'].startswith('generation')][1:]
assert len(gen)==128 and len({e['pid'] for e in gen})==1
assert all(e['qpc_hz']==clock['freq'] for e in gen)
pid=gen[0]['pid']; pk=[p for p in data['packets'] if p['pid']==pid]
def merge(iv):
    result=[]
    for a,b in sorted(iv):
        if result and a<=result[-1][1]: result[-1][1]=max(b,result[-1][1])
        else: result.append([a,b])
    return result
def interval_us(iv,a,b): return sum(max(0,min(y,b)-max(x,a)) for x,y in iv)
def rel(qpc): return (qpc-clock['qpc0'])*1e6/clock['freq']
dma=[p for p in pk if p['ds'] is not None]
iv_stop=merge([(p['ds'],p['de']) for p in dma if p['de'] is not None and p['de']>=p['ds']])
iv_irq=merge([(p['ds'],p['di']) for p in dma if p['di'] is not None and p['di']>=p['ds']])
pending=merge([(p['q'],p['ds']) for p in dma if p['q'] is not None and p['ds']>=p['q']])
rows=[]
for i,e in enumerate(gen):
    a,b=rel(e['qpc_start']),rel(e['qpc_end'])
    rows.append({'token':i,'qpc_start':e['qpc_start'],'qpc_end':e['qpc_end'],'decode_ms':e['wall_ms'],
        'dma_start_to_stop_union_ms':interval_us(iv_stop,a,b)/1000,
        'dma_start_to_irq_union_ms':interval_us(iv_irq,a,b)/1000,
        'queued_before_dma_union_ms':interval_us(pending,a,b)/1000,
        'dma_starts':sum(a<=p['ds']<b for p in dma)})
result={'clock':clock,'lost':lost,'pid':pid,'packet_count':len(pk),'dma_count':len(dma),
    'dma_missing_stop':sum(p['de'] is None for p in dma),'dma_missing_irq':sum(p['di'] is None for p in dma),
    'nodes':dict(collections.Counter(str(data['node'].get(p['ctx'])) for p in dma)),
    'per_token':rows,'means':{k:sum(r[k] for r in rows)/128 for k in rows[0] if k not in ('token','qpc_start','qpc_end')},
    'scope':'OS DMA packet residence and queued intervals, not shader GPU timestamps; interval classes overlap and must not be added.',
    'source_sha256':{str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in (etl,root/'scratch/m15/gpu-timeline/align/etwdump.py',root/'scratch/m15/gpu-timeline/align/etwclock.py')}}
(analysis/'result.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps({k:v for k,v in result.items() if k!='per_token'},indent=2))
