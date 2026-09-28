from pathlib import Path
import json,collections
p=Path('P:/BC-250/scratch/g0-hosted/dwm049-ops')
c=json.loads((p/'checkpoints.json').read_text(encoding='utf-8-sig'));d=json.loads((p/'ddi-origins.json').read_text(encoding='utf-8-sig'))
selected=set(c['render_image_write_ids']);by_map={mid:s for s in d['scopes'] for mid in s['maps']};hist=collections.Counter();unknown=[]
for m in c['maps']:
 if m['map'] not in selected:continue
 scope=by_map.get(m['map'])
 if scope is None:unknown.append(m['map']);continue
 hist[(scope['begin']['origin'],tuple(m['box']),scope['copied'])]+=1
out={'scope':'Checkpoint-bounded writable image mappings associated with same-thread frontend DDI scopes; mapped extents are not CPU byte counts or final-resource identity.','render_image_maps':len(selected),'unmatched':unknown,'groups':[{'origin':k[0],'box':k[1],'completed_frontend_copy':k[2],'count':v} for k,v in hist.most_common()]}
(p/'render-image-origins.json').write_text(json.dumps(out,indent=2)+'\n',newline='\n')
print(json.dumps(out,indent=2))
