# Name the amdgpu_dc_wreg/rreg lines of a flip trace with regcalc (DMU header), and print the writes
# around every write of a HUBPREQ surface address or an OTG master update lock.
import re, sys
sys.path.insert(0, "P:/BC-250/bc250-win/tools/regcalc")
from regcalc import RegMap
rm = RegMap(ip="DMU", reg_header="P:/BC-250/bc250-win/third_party/linux-amdgpu/dcn_2_0_1_offset.h")
gc = RegMap()
path = sys.argv[1]
want = re.compile(r"PRIMARY_SURFACE_ADDRESS|MASTER_UPDATE_LOCK|SURFACE_INUSE|FLIP_CONTROL|SURFACE_PITCH|GLOBAL_SYNC|VUPDATE|VERTICAL_INTERRUPT")
rows = []
for line in open(path, encoding="utf-8", errors="replace"):
    m = re.search(r"(\d+\.\d+): amdgpu_dc_(w|r)reg: reg=0x([0-9a-f]+), value=0x([0-9a-f]+)", line)
    if not m:
        if "tracing_mark_write" in line or "atomic_commit_tail" in line or "pipe_state" in line:
            rows.append((line.strip()[:160], None, None, None, None))
        continue
    t, kind, reg, val = m.group(1), m.group(2), int(m.group(3), 16), m.group(4)
    off = reg * 4
    names = rm.reverse(off) or gc.reverse(off)
    rows.append((t, kind, off, names[0] if names else "?", val))
uniq = {}
for r in rows:
    if r[1] == "w": uniq[r[3]] = uniq.get(r[3], 0) + 1
print("writes: %d, distinct named registers: %d, unnamed: %d" % (sum(uniq.values()), len([k for k in uniq if k != "?"]), uniq.get("?", 0)))
print("--- writes to the flip-related registers, with 3 writes of context before each")
last = []
for i, r in enumerate(rows):
    if r[1] is None:
        print("   ", r[0]); continue
    if r[1] == "w" and want.search(r[3]):
        for c in last[-3:]: print("      %s %s 0x%05X %-55s %s" % c)
        print("  ** %s %s 0x%05X %-55s %s" % r)
        last = []
    elif r[1] == "w":
        last.append(r)
print("--- top written registers")
for k, v in sorted(uniq.items(), key=lambda kv: -kv[1])[:25]: print("  %5d %s" % (v, k))
