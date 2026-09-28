"""Check the frozen native GPU client's full visible rectangle in a desktop capture.
Image and heartbeat acquisition/ordering, module identity, GPU work and no-copy
are external gates. This checker establishes pixels only, not those properties.
"""
import argparse
import json
from pathlib import Path
from PIL import Image


def analyze(image, heartbeat):
    if heartbeat.get("frozen") is not True or type(heartbeat.get("frames")) is not int or heartbeat["frames"]<2:
        raise ValueError("missing frozen animation receipt")
    if heartbeat.get("expected_bgra")!=0xff00ff00:raise ValueError("unexpected reference colour")
    bounds=heartbeat.get("client")
    if not isinstance(bounds,list) or len(bounds)!=4 or any(type(x) is not int for x in bounds):raise ValueError("invalid client rectangle")
    left,top,right,bottom=bounds
    if not (0<=left<right<=image.width and 0<=top<bottom<=image.height and right-left==320 and bottom-top==240):raise ValueError("client rectangle outside image or wrong size")
    pixels=image.convert("RGB").crop(tuple(bounds))
    bad=sum(p!=(0,255,0) for p in pixels.getdata())
    return dict(pass_pixels=bad==0,pixels=320*240,mismatches=bad,client=bounds,scope="Frozen full client pixels only; capture ordering and GPU identity require separate evidence")

if __name__=="__main__":
    p=argparse.ArgumentParser(description=__doc__);p.add_argument("image",type=Path);p.add_argument("heartbeat",type=Path);a=p.parse_args()
    with Image.open(a.image) as image:r=analyze(image,json.loads(a.heartbeat.read_text(encoding="utf-8-sig")))
    print(json.dumps(r,indent=2))
    if not r["pass_pixels"]:raise SystemExit(2)
