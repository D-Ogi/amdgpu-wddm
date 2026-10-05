from pathlib import Path
import sys,importlib,json,re,statistics,hashlib
sys.path.insert(0,'bc250-win/tools/win');from target import Target
t=Target();s=Path('scratch/m13')
for suffix in ['.out','.err','.exit','-before.log','-after.log']:
 t.pull('C:\\BC250\\m9\\gpu-residency07127-control\\llvm23-64k'+suffix,str(s/('llvm23-64k'+suffix)))
t.pull(r'C:\BC250\m13\llvm23-control\render-probe.txt',str(s/'llvm23-render-probe.txt'))
t.pull(r'C:\BC250\m13\llvm23-control\scanout-0.bmp',str(s/'llvm23-control0.bmp'))
sys.path.insert(0,'bc250-win/experiments/E27-m9-inference');v=importlib.import_module('validate-gpu-residency')
result=v.validate(s,'llvm23-64k',65536)
(s/'llvm23-validation.json').write_text(json.dumps(result,indent=2))
print(json.dumps(result))
log=(s/'final-llvm23.log').read_text()
fields=['frame','gap_ms','draws','draw_ms','max_draw_ms','present_ms','render_wait_ms']
rows=[dict(zip(fields,map(float,m))) for m in re.findall(r'BC250 Perf frame (\d+) gap_ms ([0-9.]+) draws (\d+) draw_ms ([0-9.]+) max_draw_ms ([0-9.]+) present_ms ([0-9.]+) render_wait_ms ([0-9.]+)',log)]
steady=[r['draw_ms']+r['render_wait_ms'] for r in rows if r['frame']>=14 and r['draws']==2]
stats={'criterion':'frame >=14 and exactly two draws; includes later samples, not a matched animation benchmark','n':len(steady),'median_draw_plus_wait_ms':statistics.median(steady),'min_ms':min(steady),'max_ms':max(steady),'frames':rows}
(s/'llvm23-frames.json').write_text(json.dumps(stats,indent=2));print({k:v for k,v in stats.items() if k!='frames'})
print('\n'.join(l for l in log.splitlines() if any(x in l for x in ['Tctl','vidpn flip open','boot=','final_time=','UnconfirmedStarts','no TDR'])))
