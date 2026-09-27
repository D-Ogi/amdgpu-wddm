from pathlib import Path
import re,json
p=Path(__file__).resolve().parent/'result'
done=json.loads((p/'done.json').read_text(encoding='utf-8-sig'))
s=(p/('dwm-'+str(done['gpu_pid'])+'.log')).read_text(encoding='utf-8')
samples={}
for line in s.splitlines():
 if not line.startswith(('BC250 audit maps ','BC250 audit bucket_summary ','BC250 audit bucket ')):continue
 f=dict(re.findall(r'(\w+)=([^ ]+)',line));key=(f['ctx'],int(f['sample']))
 v=samples.setdefault(key,{'buckets':[]})
 if line.startswith('BC250 audit maps '):v['totals']={k:int(f[k]) for k in ['image_calls','image_request_bytes','buffer_calls','buffer_request_bytes','persistent_calls']}
 elif line.startswith('BC250 audit bucket_summary '):v['overflow']=int(f['overflow'])
 else:
  for k in ['id','target','calls','bytes','user_ptr']:f[k]=int(f[k])
  f['usage']=int(f['usage'],16);v['buckets'].append(f)
out=[]
for (ctx,sample),v in samples.items():
 assert 'totals' in v and 'overflow' in v
 t=v['totals'];b=v['buckets'];assert len({x['id'] for x in b})==len(b)
 missing={}
 for kind,image in [('image',True),('buffer',False)]:
  known=[x for x in b if (x['target']!=0)==image]
  calls=t[kind+'_calls']-sum(x['calls'] for x in known)
  size=t[kind+'_request_bytes']-sum(x['bytes'] for x in known)
  assert calls>=0 and size>=0 and (calls or not size)
  missing[kind]={'calls':calls,'bytes':size}
 assert sum(x['calls'] for x in missing.values())==v['overflow']
 known_persistent=sum(x['calls'] for x in b if x['usage']&256 or x['user_ptr'])
 assert t['persistent_calls']>=known_persistent
 out.append({'ctx':ctx,'sample':sample,'totals':t,'overflow':v['overflow'],'unclassified':missing,'unclassified_persistent_calls':t['persistent_calls']-known_persistent})
last=[max((v for v in out if v['ctx']==ctx),key=lambda v:v['sample']) for ctx in sorted({v['ctx'] for v in out})]
f=p/'map-reconciliation.json';assert not f.exists();f.write_text(json.dumps({'scope':'Logical map requests only. Exact reconciliation does not count CPU stores or classify missing bucket identities.','snapshots':len(out),'last':last,'samples':out},indent=2)+'\n',newline='\n')
print(json.dumps({'snapshots':len(out),'last':last},indent=2))
