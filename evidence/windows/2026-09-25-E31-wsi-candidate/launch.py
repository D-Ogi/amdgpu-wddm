from pathlib import Path
import sys,json
from PIL import Image
r=Path('P:/bc-250');sys.path.insert(0,str(r/'bc250-win/tools/win'));from target import Target
src=Path(__file__).parent; fmt=sys.argv[1];tag=sys.argv[2] if len(sys.argv)>2 else fmt;w=r/('scratch/m10/color-oracle-'+tag);w.mkdir(exist_ok=True)
script=(src/'run-37.ps1').read_text().replace('color-oracle-37','color-oracle-'+tag).replace('Color37','Color'+fmt).replace('BC250_TEST_FORMAT=37','BC250_TEST_FORMAT='+fmt)
script=script.replace("   Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:2250/screenshot?scale=1", "   Start-Sleep -Milliseconds 750\n   Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:2250/screenshot?scale=1")
(w/'run.ps1').write_text(script)
t=Target();t.push([str(src/'color-oracle.exe')],'C:\\BC250\\m10\\color-oracle-'+tag)
p=t.run_script(str(w/'run.ps1'),timeout=75);(w/'run.log').write_text(p.stdout+p.stderr);print(p.stdout)
for name in ['native.out','native.err','native.exit','frame.csv','frame-1.ready','screen-1.png']:
 t.pull('C:\\BC250\\m10\\color-oracle-'+tag+'\\'+name,str(w/name),timeout=20)
assert p.returncode==0
assert r'C:\BC250\m10\wsi-fifo' in (w/'native.err').read_text()
rect=json.loads((w/'frame-1.ready').read_text());im=Image.open(w/'screen-1.png').convert('RGB')
x,y,width,height=map(rect.get,['x','y','width','height'])
im=im.crop((x,y,x+width,y+height));im.save(w/'client.png')
palette=[(255,0,0),(0,255,0),(0,0,255),(255,255,255)]
bad=0; examples=[]
for py in range(height):
 for px in range(width):
  expected=palette[(2 if py>=height//2 else 0)+(1 if px>=width//2 else 0)]
  actual=im.getpixel((px,py))
  if actual!=expected:
   bad+=1
   if len(examples)<8:examples.append([px,py,actual,expected])
result={'format':int(fmt),'width':width,'height':height,'pixels':width*height,'mismatches':bad,'examples':examples}
(w/'pixels.json').write_text(json.dumps(result,indent=2));print(result)
assert bad==0
