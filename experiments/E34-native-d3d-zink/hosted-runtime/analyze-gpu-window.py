"""Validate native window captures and process/module/rollback receipts.
Not a whole-desktop GPU, no-copy or hardware-retirement proof.
"""
import argparse
import importlib.util
import json
from pathlib import Path
from PIL import Image

def require(ok,message):
    if not ok:raise ValueError(message)

def analyze(directory):
    def read(n):return json.loads((directory/n).read_text(encoding="utf-8-sig"))
    for n in ("done.json","watchdog-done.json","control-result.json"):require(read(n)["exit"]==0,n+" failure")
    manifest=read("manifest.json");mod=read("modules.json")
    for n in ("bc250d3d_zink.dll","vulkan_radeon.dll"):
        found=[x for x in mod["modules"] if x["path"].lower()==("C:/BC250/m13/window-client001/"+n).replace("/",chr(92)).lower()]
        require(len(found)==1 and found[0]["sha256"]==manifest[n],"module identity "+n)
    capture=read("capture.json");before=capture["before"];after=capture["after"]
    require(before["pid"]==after["pid"]==mod["pid"],"capture PID")
    require(before["frozen"] is True and after["frozen"] is True and before["frames"]==after["frames"] and before["frames"]>=30,"frozen frame count")
    require(before["client"]==after["client"],"capture rectangle changed")
    require(before["frequency"]==after["frequency"]==capture["frequency"] and capture["frequency"]>0,"QPC frequency")
    require(before["qpc"]<=capture["start_qpc"]<=capture["end_qpc"] and after["qpc"]>=before["qpc"],"capture QPC order")
    pre=read("preflight.json");post=read("closure.json")
    def dwm(x):return sorted((p["pid"],p["start"]) for p in x["dwm"])
    require(dwm(pre)==dwm(post) and pre["boot"]==post["boot"],"DWM/OS changed")
    for k in ("generation","epoch"):require(pre["confirmed"][k]==post["confirmed"][k],"health identity changed")
    closed=read("closed.json")
    require(closed["umd"]=="8279AC7F6342CD0A31CB96531194CEF4A224A0D010BCEE4B549D9606D5E405EA" and closed["icd"]=="CF3948D692CAF56FFCFFEA6EBCA5F0E5FDB517FDC55062EFAE389515A373D157","rollback identity")
    require("PASS gpu window frames=" in (directory/"stdout.log").read_text(),"application completion")
    spec=importlib.util.spec_from_file_location("pixels",Path(__file__).with_name("check-gpu-window.py"));m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
    images={}
    for n in ("gpu.bmp","screen.png"):
        with Image.open(directory/n) as image:images[n]=m.analyze(image,before)
    return dict(pass_control=all(r["pass_pixels"] for r in images.values()),frames=before["frames"],images=images,closure_utc=post["utc"],scope="Visible GPU-client control under unchanged CPU DWM; GPU execution and whole-stack no-copy remain separate")

if __name__=="__main__":
    p=argparse.ArgumentParser(description=__doc__);p.add_argument("directory",type=Path);a=p.parse_args();r=analyze(a.directory);print(json.dumps(r,indent=2))
    if not r["pass_control"]:raise SystemExit(2)
