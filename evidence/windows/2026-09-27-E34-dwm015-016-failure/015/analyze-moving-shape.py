from pathlib import Path
from PIL import Image
import json,sys

def components(im):
 pix=im.load();parent=[];stats=[];prev=[]
 def root(i):
  while parent[i]!=i:
   parent[i]=parent[parent[i]];i=parent[i]
  return i
 for y in range(im.height):
  runs=[];x=0
  while x<im.width:
   if pix[x,y]!=(0,255,255):x+=1;continue
   start=x
   while x<im.width and pix[x,y]==(0,255,255):x+=1
   i=len(parent);parent.append(i);stats.append([x-start,start,y,x,y+1])
   for left,right,j in prev:
    if left<x and right>start:
     a,b=root(i),root(j)
     if a!=b:parent[b]=a
   runs.append((start,x,i))
  prev=runs
 grouped={}
 for i,s in enumerate(stats):
  k=root(i)
  if k not in grouped:grouped[k]=s.copy()
  else:
   t=grouped[k];t[0]+=s[0];t[1]=min(t[1],s[1]);t[2]=min(t[2],s[2]);t[3]=max(t[3],s[3]);t[4]=max(t[4],s[4])
 return sorted(grouped.values(),reverse=True)

p=Path(sys.argv[1]) if len(sys.argv)>1 else Path(__file__).resolve().parent/'result'
result={}
for f in sorted([p/'baseline.bmp',p/'gpu.bmp',p/'screen.png']+list(p.glob('dynamic-*.bmp'))+list(p.glob('dynamic-*.png'))):
 if not f.exists():continue
 im=Image.open(f).convert('RGB');allc=components(im);large=[c for c in allc if c[0]>64]
 valid=False
 if len(large)==1:
  n,x0,y0,x1,y1=large[0];fill=n/((x1-x0)*(y1-y0))
  valid=600<=x0<x1<=1016 and 120<=y0<y1<=362 and 150<=x1-x0<=264 and 70<=y1-y0<=188 and fill>=.99
 result[f.name]={'large_components':[{'pixels':c[0],'bbox':c[1:],'fill':c[0]/((c[3]-c[1])*(c[4]-c[2]))} for c in large], 'small_component_pixels':sum(c[0] for c in allc if c[0]<=64),'shape_check':valid}
out=p/'moving-shape-analysis.json'
if out.exists():raise RuntimeError('Preserve existing evidence; output already exists')
out.write_text(json.dumps({'scope':'Diagnostic shape check, not full-desktop pixel oracle or proof of absence of intermittent artifacts. Components <=64 pixels ignored. CPU baseline is the positive control.','captures':result},indent=2)+'\n',encoding='utf-8',newline='\n')
print(json.dumps({n:v['shape_check'] for n,v in result.items()}))
