"""Pick a short smoke list for the first lab call: per sparse_resources group (buffer: per buffer subgroup)
the fastest case that passed on the reference host, excluding device_group variants (one physical device
on unit A). Mustpass order.

    python make_smoke.py --cases lists/sparse-resources.txt --reference host-ref/results/rtx4090/merged/merged.tsv
                         --out package/batches/smoke.txt
"""

import argparse


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cases", required=True)
    ap.add_argument("--reference", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    with open(a.cases, encoding="utf-8") as f:
        order = [l.strip() for l in f if l.strip()]
    best = {}
    with open(a.reference, encoding="utf-8") as f:
        head = f.readline().rstrip("\n").split("\t")
        for line in f:
            r = dict(zip(head, line.rstrip("\n").split("\t")))
            c = r["case"]
            if r["status"] != "Pass" or not r.get("duration_us") or "device_group" in c:
                continue
            parts = c.split(".")
            g = ".".join(parts[2:4]) if parts[2] == "buffer" else parts[2]
            us = float(r["duration_us"])
            if g not in best or us < best[g][1]:
                best[g] = (c, us)
    picked = {c for c, _ in best.values()}
    smoke = [c for c in order if c in picked]
    with open(a.out, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(smoke) + "\n")
    print(f"{len(smoke)} smoke cases, host sum {sum(u for _, u in best.values()) / 1e6:.2f} s -> {a.out}")


if __name__ == "__main__":
    main()
