"""Register names for dword offsets of amdgpu's register trace (development PC only).

Names come only from bc250-win/tools/regcalc over the AMD headers in bc250-win/third_party/linux-amdgpu
(repo rule 1 and 2). The trace carries dword indices (amdgpu_device_rreg/wreg 'reg'), regcalc works in bytes.
"""
import os
import sys
from pathlib import Path

ROOT = Path(os.environ.get("BC250_ROOT", r"P:\BC-250"))
REPO = ROOT / "bc250-win"
sys.path.insert(0, str(REPO / "tools" / "regcalc"))
from regcalc import RegMap, HDR_DIR  # noqa: E402

# (IP in cyan_skillfish_ip_offset.h, register header). MP0 and MP1 share one header and one base.
IP_HEADERS = [
    ("GC", "gc_10_1_0_offset.h"),
    ("MMHUB", "mmhub_2_0_0_offset.h"),
    ("MP0", "mp_11_0_8_offset.h"),
    ("NBIO", "nbio_2_3_offset.h"),
    ("OSSSYS", "osssys_5_0_0_offset.h"),
    ("HDP", "hdp_5_0_0_offset.h"),
    ("DMU", "dcn_2_0_1_offset.h"),
    ("THM", "thm_10_0_offset.h"),
    ("CLK", "clk_11_0_1_offset.h"),
]


class Names:
    def __init__(self):
        self.by_dword = {}
        self.by_name = {}
        for ip, hdr in IP_HEADERS:
            rm = RegMap(ip=ip, reg_header=HDR_DIR / hdr)
            for name, (mm, idx) in rm.regs.items():
                if idx not in rm.segs:
                    continue
                dw = rm.segs[idx] + mm
                self.by_dword.setdefault(dw, set()).add(name)
                self.by_name.setdefault(name, dw)

    def name(self, dword):
        n = self.by_dword.get(dword)
        if not n:
            return f"?0x{dword * 4:05X}"
        return "|".join(sorted(n))

    def dword(self, name):
        """dword index of mmNAME (KeyError when regcalc has no such register)."""
        if not name.startswith("mm"):
            name = "mm" + name
        return self.by_name[name]

    def byte(self, name):
        return self.dword(name) * 4


if __name__ == "__main__":
    n = Names()
    for a in sys.argv[1:]:
        if a.startswith("0x"):
            print(a, n.name(int(a, 16)))
        else:
            print(a, f"dword 0x{n.dword(a):05X} byte 0x{n.byte(a):05X}")
