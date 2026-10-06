"""L1 and L12 of 2026-10-06: compare the pre-driver sweeps and the M.2 port of five recorded boots.

    python analysis/l1_compare.py        (from the evidence directory; prints what analysis/l1-compare.txt holds)

A register is a sweep line `BLOCK.NAME 0xOFFSET VALUE` with an upper-case name and an 8-digit hex value
(the parse of cmp_sweeps.py). SKIPPED lines and the lower-case alias rules of E03 (`rule.*`,
`psp_literal.*`) are left out. Values are compared as strings.
"""
import re
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent / "boots"
LINE = re.compile(r"^([A-Z0-9]+\.[A-Z0-9_]+) (0x[0-9a-f]+) ([0-9A-F]{8})$")
LABELS = ["warm-after-windows", "cold1", "cold2", "cold3", "warm-after-amdgpu"]
COLD = ("cold1", "cold2", "cold3")
WARM = ("warm-after-windows", "warm-after-amdgpu")


def load(label):
    lines = (ROOT / label / "collect" / "sweep-pre.log").read_text().splitlines()
    regs = {m.group(1): m.group(3) for m in map(LINE.match, lines) if m}
    skipped = sum(line.endswith(" SKIPPED") for line in lines)
    return regs, skipped, lines[0]


s = {}
print("== sweeps")
for label in LABELS:
    s[label], skipped, head = load(label)
    boot = dict(x.split(" ", 1) for x in (ROOT / label / "boot.txt").read_text().splitlines() if " " in x)
    print(f"{label:<19} registers {len(s[label])}  skipped {skipped}  uptime {boot['uptime']}  "
          f"amdgpu_loaded {boot['amdgpu_loaded']}  | {head}")


def differ(a, b):
    return sorted(k for k in s[a].keys() & s[b].keys() if s[a][k] != s[b][k])


print("\n== registers that differ, pairwise")
for a, b in [("cold1", "cold2"), ("cold2", "cold3"), ("cold1", "cold3"), ("cold1", "warm-after-windows"),
             ("cold3", "warm-after-amdgpu"), ("warm-after-windows", "warm-after-amdgpu")]:
    print(f"{a} vs {b}: {len(differ(a, b))}")

cv = [k for k in s["cold1"] if len({s[c][k] for c in COLD}) > 1]
print(f"\n== registers that vary across the three cold starts: {len(cv)}")
print("by block:", dict(Counter(k.split(".")[0] for k in cv)))
print("outside GC:", [k for k in cv if not k.startswith("GC.")])
bits = sorted(max(bin(int(s[a][k], 16) ^ int(s[b][k], 16)).count("1") for a in COLD for b in COLD) for k in cv)
print(f"largest pairwise bit distance per register: median {bits[len(bits) // 2]}, "
      f"1 to 3 bits {sum(b <= 3 for b in bits)}, maximum {bits[-1]}")
wide = sorted(((max(bin(int(s[a][k], 16) ^ int(s[b][k], 16)).count("1") for a in COLD for b in COLD), k) for k in cv),
              reverse=True)[:6]
print("widest:", ", ".join(f"{k} {n}" for n, k in wide))
for w in WARM:
    print(f"{w}: of these {len(cv)}, {sum(s[w][k] == '00000000' for k in cv)} read 0, "
          f"{sum(s[w][k] == s['cold3'][k] for k in cv)} equal cold3, "
          f"{sum(s[w][k] in {s[c][k] for c in COLD} for k in cv)} equal one of the cold values")

print("\n== selected registers")
pick = ["GC.GB_ADDR_CONFIG", "GC.CC_GC_SHADER_ARRAY_CONFIG", "GC.CP_ME_CNTL", "GC.CP_MEC_CNTL", "GC.RLC_CNTL",
        "GC.RLC_STAT", "GC.SDMA0_F32_CNTL", "GC.SDMA1_F32_CNTL", "GC.SDMA0_STATUS_REG", "GC.SDMA1_STATUS_REG",
        "GC.GRBM_STATUS", "GC.GRBM_STATUS2", "GC.GRBM_STATUS_SE0", "GC.CP_RB0_BASE", "GC.CP_RB0_RPTR",
        "GC.CP_RB0_WPTR", "MP0.MP0_SMN_C2PMSG_33", "MP0.MP0_SMN_C2PMSG_35", "MP0.MP0_SMN_C2PMSG_64",
        "MP0.MP0_SMN_C2PMSG_81", "MP1.MP0_SMN_C2PMSG_81", "GC.COMPUTE_DIM_X", "GC.COMPUTE_DIM_Y", "GC.COMPUTE_DIM_Z",
        "GC.COMPUTE_NUM_THREAD_X", "GC.COMPUTE_NUM_THREAD_Y", "GC.COMPUTE_NUM_THREAD_Z", "GC.COMPUTE_PGM_LO",
        "GC.COMPUTE_PGM_HI", "GC.COMPUTE_PGM_RSRC1", "GC.COMPUTE_PGM_RSRC2", "GC.COMPUTE_PGM_RSRC3",
        "GC.COMPUTE_TMPRING_SIZE", "GC.COMPUTE_VMID", "GC.COMPUTE_DISPATCH_INITIATOR", "GC.COMPUTE_RESOURCE_LIMITS"]
print(f"{'register':<32}" + "".join(f"{l:<20}" for l in LABELS))
for k in pick:
    print(f"{k:<32}" + "".join(f"{s[l].get(k, '-'):<20}" for l in LABELS))

wd = differ("warm-after-windows", "warm-after-amdgpu")
print(f"\n== warm-after-windows vs warm-after-amdgpu: {len(wd)}, by name prefix:",
      dict(Counter(k.split(".")[1].split("_")[0] for k in wd).most_common(10)))

print("\n== M.2 root port 00:15.0 (m2-full.txt)")
for label in LABELS:
    t = (ROOT / label / "m2-full.txt").read_text()
    cap = re.search(r"LnkCap:\s+Port #\d+, Speed (\S+), Width (x\d+)", t)
    sta = re.search(r"LnkSta:\s+Speed (\S+), Width (\S+)", t)
    print(f"{label:<19} LnkCap {cap.group(1)} {cap.group(2)}  LnkSta {sta.group(1)} {sta.group(2)}  "
          f"nvme0 {'yes' if re.search(r'^nvme0$', t, re.M) else 'NO'}  "
          f"03:00.0 below the port {'yes' if '0000:03:00.0' in t else 'NO'}  "
          f"AER capability {'yes' if 'Advanced Error Reporting' in t else 'no'}  "
          f"aer_* files {'yes' if '== aer_' in t else 'none'}")
