from pathlib import Path
import sys, re, csv, json, hashlib
root=Path('P:/bc-250')
sys.path.insert(0,str(root/'bc250-win/tools/regcalc'))
from regcalc import RegMap
rm=RegMap()
out=root/'bc250-win/evidence/linux/2026-09-24-E29-sdma-reset/comparison'
raw=(root/'scratch/m9/e29-sdma-delayed.log').read_text()
start=raw.index('sdma_v5_0_stop_queue_enter:')
end=raw.index('sdma_v5_0_restore_queue_return:',start)
rows=[]
for line in raw[start:end].splitlines():
 m=re.search(r'(\d+\.\d+): amdgpu_device_([rw])reg: 0x13fe, (0x[0-9a-f]+), (0x[0-9a-f]+)',line)
 if not m: continue
 ts,kind,reg,value=m.groups()
 byte=int(reg,16)*4
 rows.append(dict(time=ts,operation=kind,byte_offset=hex(byte),register='|'.join(rm.reverse(byte)),value=value))
assert rows
print('unresolved_registers='+str([r['byte_offset'] for r in rows if not r['register']]))
with (out/'decoded-reset.csv').open('x',newline='') as f:
 w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
safe=[r for r in rows if r['operation']=='w' and 'mmRLC_SAFE_MODE' in r['register'].split('|')]
assert not safe
control=(root/'scratch/m9/e29-sdma1-control.log').read_text()
a,b=control.split('after_fences',1)
def counters(text,name):
 m=re.search(r'--- ring \d+ \('+name+r'\) ---\s+Last signaled fence\s+(0x[0-9a-f]+)\s+Last emitted\s+(0x[0-9a-f]+)',text)
 assert m
 return [int(x,16) for x in m.groups()]
counts={n:{'before':counters(a,n),'after':counters(b,n)} for n in ['sdma0','sdma1']}
assert counts['sdma0']['before']==counts['sdma0']['after']
assert counts['sdma1']['after'][0]>counts['sdma1']['before'][0]
assert counts['sdma1']['after'][0]==counts['sdma1']['after'][1]
assert '"executed": true' in control and 'restored_mask=3' in control
assert 'physical_sdma1_control_complete' in control
(out/'validation.json').write_text(json.dumps({'decoded_accesses':len(rows),'rlc_safe_mode_writes':len(safe),'fences':counts},indent=2))
for name in ['e29-engine-inspect.log','e29-sdma1-control.log','e29-sdma1-control.sh']:
 with (out/name).open('xb') as f: f.write((root/'scratch/m9'/name).read_bytes())
for name,path in [('windows-sdma.c','driver/shim/bc250_sdma.c'),('windows-gfx.c','driver/shim/bc250_gfx.c'),('windows-gmc.c','driver/shim/bc250_gmc.c')]:
 (out/name).write_bytes((root/'bc250-win'/path).read_bytes())
manifest={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in out.iterdir() if p.is_file()}
(out/'analysis-manifest.json').write_text(json.dumps(manifest,indent=2))
print(json.dumps({'accesses':len(rows),'safe_mode_writes':len(safe),'fences':counts}))
