from pathlib import Path
from PIL import Image
import json
p=Path(__file__).resolve().parent/'result';v={}
for f in sorted(list(p.glob('dynamic-*.bmp'))+list(p.glob('dynamic-*.png'))+[p/'gpu.bmp',p/'screen.png']):
 im=Image.open(f).convert('RGB');pixels=im.load();xs=[];ys=[];outside=0
 for y in range(im.height):
  for x in range(im.width):
   if pixels[x,y]==(0,255,255):
    xs.append(x);ys.append(y)
    if not(600<=x<1016 and 120<=y<362):outside+=1
 v[f.name]={'size':list(im.size),'cyan_pixels':len(xs),'cyan_bbox':None if not xs else [min(xs),min(ys),max(xs)+1,max(ys)+1],'cyan_outside_control_motion_envelope':outside}
(p/'dynamic-color-analysis.json').write_text(json.dumps(v,indent=2)+'\n',encoding='utf-8')
print(json.dumps(v,indent=2))
