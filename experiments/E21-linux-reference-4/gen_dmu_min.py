# Minimal DMU register set for the pre-driver read (session plan section 5), resolved through regcalc.
# BC250_ROOT is the workspace root, by default the parent directory of this repository.
import json, sys, io, os
from pathlib import Path
ROOT = Path(os.environ.get("BC250_ROOT", str(Path(__file__).resolve().parents[2].parent)))
REPO = ROOT / "bc250-win"
sys.path.insert(0, str(REPO / "tools" / "regcalc"))
from regcalc import RegMap
rm = RegMap(ip="DMU", reg_header=str(REPO / "third_party" / "linux-amdgpu" / "dcn_2_0_1_offset.h"))
names = []
for n in range(4):
    for r in ("DCSURF_PRIMARY_SURFACE_ADDRESS", "DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH", "DCSURF_SURFACE_INUSE",
              "DCSURF_SURFACE_INUSE_HIGH", "DCSURF_SURFACE_CONTROL", "DCSURF_FLIP_CONTROL", "DCSURF_SURFACE_PITCH",
              "DCSURF_SURFACE_FLIP_INTERRUPT"):
        names.append("mmHUBPREQ%d_%s" % (n, r))
    for r in ("DCHUBP_CNTL", "DCSURF_SURFACE_CONFIG", "DCSURF_ADDR_CONFIG", "DCSURF_TILING_CONFIG"):
        names.append("mmHUBP%d_%s" % (n, r))
    names.append("mmHUBPRET%d_HUBPRET_CONTROL" % n)
for m in range(2):
    for r in ("OTG_MASTER_UPDATE_LOCK", "OTG_CONTROL", "OTG_STATUS", "OTG_DOUBLE_BUFFER_CONTROL", "OTG_H_TOTAL",
              "OTG_V_TOTAL", "OTG_STATUS_POSITION", "OTG_VERTICAL_INTERRUPT0_CONTROL", "OTG_VERTICAL_INTERRUPT2_CONTROL",
              "OTG_GLOBAL_CONTROL0", "OTG_VUPDATE_PARAM"):
        names.append("mmOTG%d_%s" % (m, r))
names.append("mmDCHUBBUB_CTRL_STATUS")
regs, lists, missing = [], [], []
for n in names:
    try:
        off = rm.byte_offset(n)
    except Exception:
        missing.append(n); continue
    regs.append(["DMU", n, off]); lists.append([n, off])
extra = ROOT / "scratch" / "lx" / "extra"
io.open(str(extra / "dmusweep.json"), "w").write(json.dumps({"regs": regs}, indent=0))
io.open(str(extra / "dmulists.json"), "w").write(json.dumps({"state": lists}, indent=0))
print(len(regs), "registers;", "NOT FOUND:", missing)
for r in regs[:3]: print(r)
