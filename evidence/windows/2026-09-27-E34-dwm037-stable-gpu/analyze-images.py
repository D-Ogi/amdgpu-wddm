from pathlib import Path
import json,sys,hashlib
sys.path.insert(0,str(Path('P:/bc-250/scratch/py')))
from PIL import Image
p=Path(__file__).resolve().parent/'result'
def read(name):return json.loads((p/name).read_text(encoding='utf-8-sig'))
base=read('baseline-ready.json')['heartbeat'];final=read('freeze-receipt.json')
assert base['frames']==0 and not base['animating'] and base['baseline_result']==0
assert final['frozen'] and final['freeze_result']==0 and final['frames']>0
rows=[]
for name,state in [('baseline.bmp',base),('gpu.bmp',final),('screen.png',final)]:
 im=Image.open(p/name).convert('RGB');regions=[]
 for label,rect,color in [('red',[110,180,145,220],(255,0,0)),('overlap',[180,170,290,230],(127,0,128)),('full_cyan',state['moving_client'],(0,255,255))]:
  x,y,r,b=rect;assert 0<=x<r<=im.width and 0<=y<b<=im.height
  crop=im.crop(rect);bad=sum(pixel!=color for pixel in crop.getdata())
  regions.append(dict(name=label,bounds=rect,pixels=crop.width*crop.height,mismatches=bad,rgb_sha256=hashlib.sha256(crop.tobytes()).hexdigest()))
 rows.append(dict(image=name,sha256=hashlib.sha256((p/name).read_bytes()).hexdigest(),regions=regions))
out={'scope':'Exact selected static regions and entire cyan client at acknowledged stationary/frozen states; not a full-desktop or live-animation oracle.','images':rows,'pass':all(r['mismatches']==0 for x in rows for r in x['regions'])}
f=p/'independent-images.json';assert not f.exists();f.write_text(json.dumps(out,indent=2));print(json.dumps(out,indent=2))
assert out['pass']
