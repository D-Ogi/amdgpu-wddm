"""Describe measured image-write maps without inferring their CPU writers."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path


def summarize(report, width, height):
    if width <= 0 or height <= 0:
        raise ValueError("Positive desktop dimensions required")
    ids = report["render_image_write_ids"]
    if len(ids) != len(set(ids)):
        raise ValueError("Duplicate write IDs")
    maps = {row["map"]: row for row in report["maps"]}
    if len(maps) != len(report["maps"]) or not set(ids) <= maps.keys():
        raise ValueError("Incomplete or duplicate map inventory")
    writes = [maps[mid] for mid in ids]
    if any(r["phase"] != "render" or not r["write"] or r["target"] == 0 for r in writes):
        raise ValueError("Write list disagrees with map inventory")
    groups = Counter((tuple(r["box"]), r["usage"], r["bind"]) for r in writes)
    exact = [r["map"] for r in writes if r["box"] == [width, height, 1]]
    large = [r["map"] for r in writes if r["box"][0]*r["box"][1]*r["box"][2] >= width*height]
    return dict(desktop=[width,height], render_image_write_count=len(writes),
        exact_desktop_box_ids=exact, at_least_desktop_texel_count_ids=large,
        requested_texels=sum(r["box"][0]*r["box"][1]*r["box"][2] for r in writes),
        groups=[dict(box=list(k[0]),usage=k[1],bind=k[2],count=n)
                for k,n in sorted(groups.items(), key=lambda item:(-item[1],item[0]))],
        boundary_spanning_image_ids=report["boundary_spanning_image_ids"],
        live_buffer_ids=report["live_buffer_ids"],
        scope="Requested map extents only. No CPU-store count, caller attribution, byte count, or exclusion of tiled/partial/persistent frame copies. Matching a window size is not resource identity.")


if __name__ == "__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report",type=Path)
    parser.add_argument("--desktop",type=int,nargs=2,required=True)
    args=parser.parse_args()
    raw=args.report.read_bytes()
    result=summarize(json.loads(raw),*args.desktop)
    result["source_sha256"]=hashlib.sha256(raw).hexdigest()
    print(json.dumps(result,indent=2))
