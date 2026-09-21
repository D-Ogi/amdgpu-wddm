#!/usr/bin/env python3
"""Generate sweep.json: every named register of the IP blocks we care about, for a read-only sweep.

Used by sweep.py on the probe to snapshot the GPU twice: as the BIOS hands it over, and after
amdgpu has initialized it. The difference is what a driver has to program.

Also emits the offsets that the prior-art projects' *rules* would produce for the same key
registers, so that their measurements can be confronted with what is really at those addresses:

  keshas : segment_base + mm * 4        (base used as bytes, only mm scaled)
  zero   : mm * 4                       (no segment base at all)
  psp    : literal byte offsets from AMD-BC-250-PSP-Driver inc/PspIoctl.h (MP0/MP1 base 0x16000 as bytes)

Run:  python tools/diagusb/gen_sweep.py [out.json]
"""

import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "regcalc"))
from regcalc import HDR_DIR, RegMap  # noqa: E402

from gen_probes import SPEC  # noqa: E402  (ip, header, registers) - reuse the header mapping

BAR5_SIZE = 0x80000
# Indexed data ports and ucode/RAM windows: a read moves an internal pointer. Never swept.
SKIP = re.compile(r"DATA|UCODE|RAM|INDEX(?!$)|_IND_")
# Registers whose read changes state, measured (facts M25): a read of an invalidation semaphore acquires it,
# the header dumps advance per read, a read of GRBM_GFX_CNTL latches GRBM_READ_ERROR. Never swept.
SIDE_EFFECT = re.compile(r"_INVALIDATE_ENG\d+_SEM$|_HEADER_DUMP$|^GRBM_GFX_CNTL$")
# Literal offsets quoted from the prior-art PSP driver (inc/PspIoctl.h @3bfa7a2). Read-only here.
PSP_DRIVER_LITERALS = {
    "PSP_C2PMSG_35": 0x1056C, "PSP_C2PMSG_36": 0x10570, "PSP_C2PMSG_37": 0x10574,
    "PSP_C2PMSG_64": 0x105E0, "PSP_C2PMSG_65": 0x105E4, "PSP_C2PMSG_66": 0x105E8,
    "PSP_C2PMSG_67": 0x105EC, "PSP_C2PMSG_68": 0x105F0, "PSP_C2PMSG_69": 0x105F4,
    "PSP_C2PMSG_70": 0x105F8, "PSP_C2PMSG_71": 0x105FC, "PSP_C2PMSG_81": 0x10614,
    "SMU_C2PMSG_66": 0x16000 + 0xA08, "SMU_C2PMSG_82": 0x16000 + 0xA48,
    "SMU_C2PMSG_83": 0x16000 + 0xA4C, "SMU_C2PMSG_90": 0x16000 + 0xA68,
}


def build():
    regs, seen = [], set()
    rules = []
    for ip, header, key_regs in SPEC:
        rm = RegMap(ip=ip, reg_header=HDR_DIR / header)
        # by offset, not by name: an alias of a side-effect register is the same register
        bad = {(rm.segs[idx] + mm) * 4 for name, (mm, idx) in rm.regs.items()
               if idx in rm.segs and SIDE_EFFECT.search(name[2:] if name.startswith("mm") else name)}
        for name, (mm, idx) in sorted(rm.regs.items()):
            if idx not in rm.segs:
                continue
            off = (rm.segs[idx] + mm) * 4
            short = name[2:] if name.startswith("mm") else name
            if off >= BAR5_SIZE or SKIP.search(short) or off in bad or (ip, off) in seen:
                continue
            seen.add((ip, off))
            regs.append([ip, short, off])
        for entry in key_regs:
            full = entry[0] if isinstance(entry, (tuple, list)) else entry
            short = full[2:] if full.startswith("mm") else full
            mm, idx = rm.regs[full]
            rules.append({"ip": ip, "n": short, "true": (rm.segs[idx] + mm) * 4,
                          "keshas": rm.segs[idx] + mm * 4, "zero": mm * 4})
    regs.sort(key=lambda r: r[2])
    return {"bar_size": BAR5_SIZE, "regs": regs, "rules": rules,
            "psp_literals": sorted(PSP_DRIVER_LITERALS.items(), key=lambda kv: kv[1])}


if __name__ == "__main__":
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE / "sweep.json"
    data = build()
    out.write_text(json.dumps(data, separators=(",", ":")) + "\n", encoding="utf-8", newline="\n")
    per_ip = {}
    for ip, _, _ in data["regs"]:
        per_ip[ip] = per_ip.get(ip, 0) + 1
    print(f"{out}: {len(data['regs'])} registers {per_ip}, {len(data['rules'])} rule rows")
