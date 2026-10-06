"""L1: compare the pre-driver sweeps of the 2026-10-06 boots (pulled-c/out/<label>/collect/sweep-pre.log)."""
import re
import sys
from pathlib import Path

OUT = Path(__file__).with_name("pulled-c") / "out"
LINE = re.compile(r"^([A-Z0-9]+\.[A-Z0-9_]+) (0x[0-9a-f]+) ([0-9A-F]{8})$")


def load(label):
    regs = {}
    for line in (OUT / label / "collect" / "sweep-pre.log").read_text().splitlines():
        m = LINE.match(line.strip())
        if m:
            regs[m.group(1)] = m.group(3)
    return regs


labels = ["warm-after-windows", "cold1", "cold2", "cold3", "warm-after-amdgpu"]
s = {l: load(l) for l in labels}
for l in labels:
    print(f"{l}: {len(s[l])} registers")
pairs = [("cold1", "cold2"), ("cold2", "cold3"), ("cold1", "cold3"),
         ("cold1", "warm-after-windows"), ("cold3", "warm-after-amdgpu")]
limit = int(sys.argv[1]) if len(sys.argv) > 1 else 15
for a, b in pairs:
    diff = sorted(k for k in s[a].keys() & s[b].keys() if s[a][k] != s[b][k])
    print(f"\n== {a} vs {b}: {len(diff)} differ")
    for k in diff[:limit]:
        print(f"  {k:<48} {s[a][k]} {s[b][k]}")
# registers that vary across the three cold boots alone: uninitialised state (M19's claim)
cold_var = sorted(k for k in s["cold1"] if len({s[c].get(k) for c in ("cold1", "cold2", "cold3")}) > 1)
print(f"\n== vary across cold1-3: {len(cold_var)}")
by_ip = {}
for k in cold_var:
    by_ip[k.split(".")[0]] = by_ip.get(k.split(".")[0], 0) + 1
print("  by block:", by_ip)
