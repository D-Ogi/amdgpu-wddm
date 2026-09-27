"""Validate the staged small-client copying control, not desktop G0 acceptance."""
import argparse
import importlib.util
import json
import re
from pathlib import Path

spec = importlib.util.spec_from_file_location("lifetimes", Path(__file__).with_name("analyze-map-lifetimes.py"))
lifetime = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lifetime)
require = lifetime.require


def analyze(directory, run_name="audit-client001"):
    require(run_name in ("audit-client001", "audit-client002"), "unsupported run identity")
    def receipt(name):
        return json.loads((directory / name).read_text(encoding="utf-8-sig"))
    for name in ("done.json", "watchdog-done.json", "control-result.json"):
        require(receipt(name)["exit"] == 0, f"{name}: nonzero exit")
    manifest = receipt("manifest.json")
    modules = receipt("modules.json")["modules"]
    for name in ("bc250d3d_zink.dll", "vulkan_radeon.dll"):
        expected = "C:\\BC250\\m13\\" + run_name + "\\" + name
        found = [m for m in modules if m["path"].lower() == expected.lower()]
        require(len(found) == 1 and found[0]["sha256"] == manifest[name], f"module identity: {name}")
    stdout = (directory / "stdout.log").read_text()
    require("PASS GPU clears, deliberate CPU-copy frames, isolated readbacks and8 acknowledged checkpoints" in stdout, "missing application pass")
    require(re.findall(r"checkpoint_ack marker=(\d+) ", stdout) == [str(i) for i in range(1,9)], "application marker acknowledgements")
    require("deliberate_cpu_copy frames=12 bytes=3686400" in stdout, "copy amount")
    for value in ("ff00ff00", "ff00ffff"):
        require(f"pixels expected={value} bad=0 total=76800" in stdout, "pixel control")
    lines = (directory / "stderr.log").read_text().splitlines()
    complete = lifetime.analyze(lines, allow_live=True, end_marker=8)
    rows = []
    for line in lines:
        if lifetime.PREFIX not in line:
            continue
        row = dict(item.split("=",1) for item in line.split(lifetime.PREFIX,1)[1].split())
        rows.append(row)
        if row.get("event") == "checkpoint" and row.get("marker") == "8":
            break
    checkpoints = {int(r["marker"]):r for r in rows if r["event"] == "checkpoint" and int(r["marker"])}
    require(set(checkpoints) == set(range(1,9)), "audit marker inventory")
    for row in checkpoints.values():
        require(int(row["pending"]) == 0, "map pending at interval boundary")
    begins = {int(r["map"]):r for r in rows if r["event"] == "begin"}
    results = {int(r["map"]):r for r in rows if r["event"] == "result"}
    def full_frame(row):
        target = int(row["target"])
        if target == 0:
            return int(row["box_width"]) == 320*240*4
        return (target == 2 and int(row["box_width"]) == 320 and
                int(row["box_height"]) == 240 and int(row["box_depth"]) == 1)
    phases = {}
    for name,start,end in (("gpu",1,2),("gpu_readback",3,4),("cpu_copy",5,6),("cpu_readback",7,8)):
        low,high = int(checkpoints[start]["seq"]),int(checkpoints[end]["seq"])
        selected = [(mid,b) for mid,b in begins.items() if low < int(b["seq"]) < high and int(results[mid]["success"])]
        image_writes = [mid for mid,b in selected if int(b["target"]) != 0 and int(b["usage"],16)&2]
        frame_writes = [mid for mid,b in selected if full_frame(b) and int(b["usage"],16)&2]
        frame_reads = [mid for mid,b in selected if full_frame(b) and int(b["usage"],16)&1]
        phases[name] = dict(start_marker=start,end_marker=end,successful_maps=len(selected),image_write_ids=image_writes,full_frame_write_ids=frame_writes,full_frame_read_ids=frame_reads)
    require(not phases["gpu"]["image_write_ids"] and not phases["gpu"]["full_frame_write_ids"], "GPU-only arm contains CPU frame/image-write map")
    require(len(phases["cpu_copy"]["full_frame_write_ids"]) == 12, "deliberate CPU copies not detected exactly12 times")
    for name in ("gpu_readback","cpu_readback"):
        require(len(phases[name]["full_frame_read_ids"]) == 1, "isolated readback not detected")
    live_images = [mid for mid in complete["live_map_ids"] if int(begins[mid]["target"]) != 0]
    require(not live_images, "unclosed image maps")
    overflows = re.findall(r"BC250 audit bucket_summary .*? overflow=(\d+)", "\n".join(lines))
    require(overflows and all(int(n)==0 for n in overflows), "missing/overflowed aggregate map audit")
    return {"phases":phases,"through_marker8":complete,"known_application_copy_bytes":3686400,
            "live_buffer_map_ids":complete["live_map_ids"],
            "scope":"small-client positive control only; persistent pointer stores, desktop ownership and full G0 remain separate"}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory",type=Path)
    parser.add_argument("--run-name",choices=("audit-client001","audit-client002"),default="audit-client001")
    args = parser.parse_args()
    print(json.dumps(analyze(args.directory,args.run_name),indent=2))
