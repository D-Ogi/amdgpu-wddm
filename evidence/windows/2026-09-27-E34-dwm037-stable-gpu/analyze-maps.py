from pathlib import Path
import re,json,collections
p=Path(__file__).resolve().parent/'result'
done=json.loads((p/'done.json').read_text(encoding='utf-8-sig'));s=(p/('dwm-'+str(done['gpu_pid'])+'.log')).read_text(encoding='utf-8');rows=[]
for line in s.splitlines():
 if 'BC250 audit bucket ctx=' not in line:continue
 f=dict(re.findall(r'(\w+)=([^ ]+)',line))
 for k in ['sample','id','target','format','runtime','user_ptr','calls','bytes']:f[k]=int(f[k])
 for k in ['bind','usage']:f[k]=int(f[k],16)
 rows.append(f)
assert rows and not re.search(r'overflow=[1-9]',s)
contexts={}
for ctx in sorted({x['ctx'] for x in rows}):
 rr=[x for x in rows if x['ctx']==ctx];last=max(x['sample'] for x in rr)
 final=[x for x in rr if x['sample']==last]
 contexts[ctx]={'last_sample':last,'final_image_buckets':[x for x in final if x['target']!=0],'persistent_buckets':[x for x in final if x['usage']&256]}
blits={}
for name in ['kmd-start.log','kmd-end.log']:
 b=(p/name).read_bytes();t=b.decode('utf-16' if b.startswith(b'\xff\xfe') else 'utf-8-sig')
 vals=re.findall(r'wddm summary: blit gate (\w+), (\d+) blits, (\d+) skips, (\d+) sources translated contiguous',t);assert vals
 blits[name]=vals[-1]
out={'overflow':False,'contexts':contexts,'kmd_blits':blits}
(p/'map-audit-summary.json').write_text(json.dumps(out,indent=2)+'\n')
print(json.dumps(out,indent=2))
