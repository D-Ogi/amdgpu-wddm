from pathlib import Path
from PIL import Image
import json,collections
p=Path('P:/BC-250/scratch/g0-hosted/dwm016/result');a=Image.open(p/'baseline.bmp').convert('RGB');b=Image.open(p/'gpu.bmp').convert('RGB');s=Image.open(p/'screen.png').convert('RGB');out={}
for name,box in [('red',(110,180,145,220)),('overlap',(180,170,290,230))]:
 aa=list(a.crop(box).getdata());bb=list(b.crop(box).getdata());ss=list(s.crop(box).getdata());out[name]={'pixels':len(aa),'baseline':collections.Counter(aa).most_common(3),'gpu':collections.Counter(bb).most_common(3),'screen':collections.Counter(ss).most_common(3),'baseline_gpu_mismatch':sum(x!=y for x,y in zip(aa,bb)),'gpu_screen_mismatch':sum(x!=y for x,y in zip(bb,ss))}
(p/'roi-analysis.json').write_text(json.dumps(out,indent=2)+'\n',encoding='utf-8');print(json.dumps(out))


assert a.size == b.size == s.size
for name,entry in out.items():
 expected=(255,0,0) if name=='red' else (127,0,128)
 assert all(entry[k]==[(expected,entry['pixels'])] for k in ['baseline','gpu','screen'])
 assert entry['baseline_gpu_mismatch']==entry['gpu_screen_mismatch']==0
print('PASS: 8000 exact expected composition pixels in all three images')
