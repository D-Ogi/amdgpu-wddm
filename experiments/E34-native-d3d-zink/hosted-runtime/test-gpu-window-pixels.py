import importlib.util
from pathlib import Path
from PIL import Image
s=importlib.util.spec_from_file_location("checker",Path(__file__).with_name("check-gpu-window.py"));m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
h=dict(frozen=True,frames=30,expected_bgra=0xff00ff00,client=[600,300,920,540])
i=Image.new("RGB",(1200,800),"black");i.paste((0,255,0),(600,300,920,540))
assert m.analyze(i,h)["pass_pixels"]
i.putpixel((600,300),(255,0,0));r=m.analyze(i,h);assert not r["pass_pixels"] and r["mismatches"]==1
for key,value in [("frozen",False),("frames",0),("expected_bgra",0),("client",[600,300,921,540]),("client",[600,300,920,801])]:
    changed=dict(h);changed[key]=value
    try:m.analyze(i,changed)
    except ValueError:pass
    else:raise AssertionError("invalid capture reference accepted: "+key)
print("PASS full-client reference, one-pixel mismatch, and five invalid receipts; no lab used")
