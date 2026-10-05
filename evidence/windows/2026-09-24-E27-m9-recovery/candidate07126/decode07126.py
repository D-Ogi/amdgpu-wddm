from pathlib import Path
import re,json,hashlib
h=Path('ref/linux-src/drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_1_0_sh_mask.h');s=h.read_text()
masks={n:int(v,16) for n,v in re.findall(r'^#define (GRBM_STATUS(?:2)?__\w+_MASK)\s+(0x[0-9A-Fa-f]+)',s,re.M)}
shifts={n:int(v,16) for n,v in re.findall(r'^#define (GRBM_STATUS(?:2)?__\w+__SHIFT)\s+(0x[0-9A-Fa-f]+)',s,re.M)}
rows={}
for label,file in [('first','candidate07126-first-start.log'),('stop','candidate07126-stop-inspect.log'),('warm','candidate07126-warm-snapshots.log')]:
 text=(Path('scratch/m9')/file).read_text()
 if label=='warm':text=text.split('snapshot=')[1]
 samples=re.findall(r'gfx: GRBM (.*?) STATUS (0x[0-9A-Fa-f]+) read (0x[0-9A-Fa-f]+)',text)
 assert samples and all(int(status,16)==0 for _,_,status in samples)
 rows[label]={'source':file,'distinct_values':sorted(set(v for _,v,_ in samples)), 'all_read_statuses_zero':True}
values=sorted({v for row in rows.values() for v in row['distinct_values']})
decoded={v:{name.removeprefix('GRBM_STATUS__').removesuffix('_MASK'):(int(v,16)&mask)>>shifts[name.removesuffix('_MASK')+'__SHIFT'] for name,mask in masks.items() if name.startswith('GRBM_STATUS__') and int(v,16)&mask} for v in values}
result={'header_sha256':hashlib.sha256(h.read_bytes()).hexdigest(),'samples':rows,'decoded_nonzero_fields':decoded,'note':'Repeated log printouts are not independent measurements. Decode only successful reads; no reset inferred.'}
p=Path('bc250-win/evidence/windows/2026-09-24-E27-m9-recovery/candidate07126/grbm-decode.json')
with p.open('x') as f:json.dump(result,f,indent=2)
print(json.dumps(result,indent=2))
