#!/usr/bin/env python3
"""Generate payload/bc250/probes.json: the registers the diagnostic USB samples.

Every offset is computed by regcalc from the AMD kernel headers. The only literal
offsets allowed in this file are PREDECESSOR_CLAIMS, which exist precisely to show
what the hand-computed addresses of the previous driver attempt really hit.

Run:  python tools/diagusb/gen_probes.py
"""

import hashlib
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "regcalc"))
from regcalc import HDR_DIR, RegMap  # noqa: E402

KERNEL_REF = "torvalds/linux master 93f51579e7df (fetched 2026-09-21)"

# (ip, header, [(register, banked_by_se_sh)])
# Read-only sampling of these registers has no side effects (no read-to-clear, no index/data pairs).
SPEC = [
    ("GC", "gc_10_1_0_offset.h", [
        ("mmGRBM_STATUS", False),
        ("mmGRBM_STATUS2", False),
        ("mmGRBM_STATUS_SE0", False),
        ("mmGRBM_GFX_INDEX", False),
        ("mmCC_GC_SHADER_ARRAY_CONFIG", True),
        ("mmGC_USER_SHADER_ARRAY_CONFIG", True),
        ("mmSPI_PG_ENABLE_STATIC_WGP_MASK", True),
        ("mmRLC_PG_ALWAYS_ON_WGP_MASK", True),
        ("mmCC_RB_BACKEND_DISABLE", True),
        ("mmGB_ADDR_CONFIG", False),
        ("mmSCRATCH_REG0", False),
        ("mmSCRATCH_REG1", False),
        ("mmCP_STAT", False),
        ("mmCP_CPC_STATUS", False),
        ("mmCP_CPF_STATUS", False),
        ("mmCP_ME_CNTL", False),
        ("mmCP_MEC_CNTL", False),
        ("mmCP_RB0_BASE", False),
        ("mmCP_RB0_BASE_HI", False),
        ("mmCP_RB0_CNTL", False),
        ("mmCP_RB0_RPTR", False),
        ("mmCP_RB0_WPTR", False),
        ("mmCP_HQD_ACTIVE", False),
        ("mmCP_HQD_PQ_BASE", False),
        ("mmCP_HQD_PQ_BASE_HI", False),
        ("mmCP_HQD_PQ_CONTROL", False),
        ("mmRLC_CNTL", False),
        ("mmRLC_STAT", False),
        ("mmRLC_GPM_STAT", False),
        ("mmRLC_PG_CNTL", False),
        ("mmRLC_CP_SCHEDULERS", False),
        ("mmRLC_RLCS_BOOTLOAD_STATUS", False),
    ]),
    ("MMHUB", "mmhub_2_0_0_offset.h", [
        ("mmMMVM_CONTEXT0_CNTL", False),
        ("mmMMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32", False),
        ("mmMMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32", False),
        ("mmMMVM_L2_PROTECTION_FAULT_STATUS", False),
        ("mmMMMC_VM_FB_LOCATION_BASE", False),
        ("mmMMMC_VM_FB_LOCATION_TOP", False),
        ("mmMMMC_VM_FB_OFFSET", False),
        ("mmMMMC_VM_SYSTEM_APERTURE_LOW_ADDR", False),
        ("mmMMMC_VM_SYSTEM_APERTURE_HIGH_ADDR", False),
    ]),
    ("OSSSYS", "osssys_5_0_0_offset.h", [
        ("mmIH_RB_CNTL", False),
        ("mmIH_RB_BASE", False),
        ("mmIH_RB_BASE_HI", False),
        ("mmIH_RB_RPTR", False),
        ("mmIH_RB_WPTR", False),
        ("mmIH_CNTL", False),
    ]),
    ("HDP", "hdp_5_0_0_offset.h", [
        ("mmHDP_NONSURFACE_BASE", False),
        ("mmHDP_NONSURFACE_INFO", False),
        ("mmHDP_HOST_PATH_CNTL", False),
        ("mmHDP_MISC_CNTL", False),
    ]),
    ("MP0", "mp_11_0_8_offset.h", [
        ("mmMP0_SMN_C2PMSG_35", False),
        ("mmMP0_SMN_C2PMSG_64", False),
        ("mmMP0_SMN_C2PMSG_81", False),
    ]),
    ("MP1", "mp_11_0_8_offset.h", [
        ("mmMP1_SMN_C2PMSG_66", False),
        ("mmMP1_SMN_C2PMSG_82", False),
        ("mmMP1_SMN_C2PMSG_90", False),
    ]),
    ("NBIO", "nbio_2_3_offset.h", [
        ("mmRCC_DEV0_EPF0_RCC_CONFIG_MEMSIZE", False),
    ]),
]

# Byte offsets used by Keshas-dev/AMD-BC-250-Windows-Driver @63f8956 and what they claimed to be.
PREDECESSOR_CLAIMS = [
    (0x3260, "GRBM_STATUS"),
    (0x3264, "CC_GC_SHADER_ARRAY_CONFIG (early)"),
    (0x9C1C, "CC_GC_SHADER_ARRAY_CONFIG (late)"),
    (0x34FC, "SPI_PG_ENABLE_STATIC_WGP_MASK (early)"),
    (0x5C3C, "SPI_PG_ENABLE_STATIC_WGP_MASK (late)"),
    (0x3D64, "RLC_PG_ALWAYS_ON_WGP_MASK"),
    (0x34D0, "GRBM_GFX_INDEX"),
    (0x32D4, "CP_SCRATCH_REG0"),
    (0x4A74, "CP_ME_CNTL"),
    (0xDA60, "CP_RING0_BASE_LO"),
    (0xE060, "KIQ_BASE_LO"),
]


def build():
    regs = []
    for ip, header, names in SPEC:
        rm = RegMap(ip=ip, reg_header=HDR_DIR / header)
        for name, banked in names:
            if name not in rm.regs:
                raise SystemExit(f"{name} not found in {header}: fix SPEC, do not guess")
            regs.append({"n": name[2:], "ip": ip, "off": rm.byte_offset(name), "banked": banked})
    offs = [r["off"] for r in regs]
    if len(set(offs)) != len(offs):
        raise SystemExit("duplicate offsets in SPEC")
    claims = [{"off": off, "as": what} for off, what in PREDECESSOR_CLAIMS]
    ident = hashlib.sha256(json.dumps([regs, claims], sort_keys=True).encode()).hexdigest()[:8].upper()
    return {"id": ident, "kernel_ref": KERNEL_REF, "regs": regs, "claims": claims}


def main():
    probes = build()
    out = HERE / "payload" / "bc250" / "probes.json"
    out.write_text(json.dumps(probes, indent=1) + "\n", encoding="utf-8", newline="\n")
    print(f"{out}: {len(probes['regs'])} registers, {len(probes['claims'])} predecessor claims, id {probes['id']}")


if __name__ == "__main__":
    main()
