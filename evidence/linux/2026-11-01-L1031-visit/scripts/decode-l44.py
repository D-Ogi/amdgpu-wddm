"""Decode the L44 register tables: every register whose value differs between the states, field by field, from
dcn_2_0_1_sh_mask.h (REG__FIELD_MASK / __SHIFT). Usage: decode-l44.py <dir with l44-regs-*.txt> [states...]"""
import os
import re
import sys

HDR = os.path.join(os.environ.get("BC250_ROOT", r"P:\BC-250"), "bc250-win", "third_party", "linux-amdgpu",
                   "dcn_2_0_1_sh_mask.h")
d = sys.argv[1]
states = sys.argv[2:] or ["fbcon", "XR24", "XB30", "XB4H", "after"]
vals = {}
for s in states:
    for line in open(os.path.join(d, f"l44-regs-{s}.txt")):
        p = line.split()
        if len(p) == 3 and p[2].startswith("0x"):
            vals.setdefault(p[0], {})[s] = int(p[2], 16)
fields = {}
pat = re.compile(r"#define\s+(\w+?)__(\w+)__SHIFT\s+(0x[0-9a-fA-F]+|\d+)")
masks = re.compile(r"#define\s+(\w+?)__(\w+)_MASK\s+(0x[0-9a-fA-F]+)L?")
shift = {}
for line in open(HDR, encoding="utf-8", errors="replace"):
    m = pat.match(line)
    if m:
        shift[(m.group(1), m.group(2))] = int(m.group(3), 0)
        continue
    m = masks.match(line)
    if m:
        fields.setdefault(m.group(1), []).append((m.group(2), int(m.group(3), 16)))


def base_name(reg):
    # HUBP0_DCSURF_SURFACE_CONFIG -> HUBP0_DCSURF_SURFACE_CONFIG in the sh_mask header (instance 0 names exist there)
    return reg


print("| Register | Field | " + " | ".join(states) + " |")
print("|---|---|" + "---|" * len(states))
for reg, per in vals.items():
    if len(set(per.values())) <= 1:
        continue
    flds = fields.get(base_name(reg)) or fields.get(re.sub(r"(\D)1_", r"\g<1>0_", reg), [])
    if not flds:
        print(f"| {reg} | (raw) | " + " | ".join(f"0x{per.get(s, 0):08X}" for s in states) + " |")
        continue
    for fname, mask in flds:
        sh = shift.get((base_name(reg), fname), shift.get((re.sub(r"(\D)1_", r"\g<1>0_", reg), fname), 0))
        fv = [(per.get(s, 0) & mask) >> sh for s in states]
        if len(set(fv)) > 1:
            print(f"| {reg} | {fname} | " + " | ".join(f"0x{v:X}" for v in fv) + " |")
