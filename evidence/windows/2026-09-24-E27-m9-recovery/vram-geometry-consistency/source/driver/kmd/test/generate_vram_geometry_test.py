from pathlib import Path
import argparse
p=argparse.ArgumentParser();p.add_argument('--source',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
parts=[]
for file,name in [('vram.c','NTSTATUS VramStart('),('vram.c','BOOLEAN VramFramebufferOffset('),('wddm.c','static BOOLEAN WddmMemoryLayout('),('gart.c','static int RunSetup(')]:
 s=(a.source/file).read_text();start=0
 while True:
  start=s.index(name,start);brace=s.index('{',start);semi=s.find(';',start,brace)
  if semi<0:break
  start+=len(name)
 end=s.index('\n}',brace)+2;parts.append(s[start:end])
fixture=Path(__file__).with_name('vram_geometry_test.c').read_text();a.out.write_text(fixture.replace('/* ACTUAL_SOURCE */','\n'.join(parts)))
print('Extracted VramStart, framebuffer resolver, segment layout and actual GART setup caller')
