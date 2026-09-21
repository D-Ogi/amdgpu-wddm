#!/usr/bin/env python3
"""Generate lists.json for regs2.py from the witness read list (tools/win/bc250rd/reglist.txt, itself generated through
tools/regcalc): named registers only, no offset is typed here. Names that are not on the read list are refused.

Run:  python experiments/E13-linux-reference-2/gen_lists.py
"""

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LINE = re.compile(r"^(\w+)\.(\w+)\s+0x([0-9a-fA-F]+)")

# Per-queue registers: read under an SRBM selection (me, pipe, queue) that amdgpu makes for us under its own mutex.
HQD = ["CP_MQD_BASE_ADDR", "CP_MQD_BASE_ADDR_HI", "CP_HQD_ACTIVE", "CP_HQD_VMID", "CP_HQD_PERSISTENT_STATE",
       "CP_HQD_PIPE_PRIORITY", "CP_HQD_QUEUE_PRIORITY", "CP_HQD_PQ_BASE", "CP_HQD_PQ_BASE_HI", "CP_HQD_PQ_RPTR",
       "CP_HQD_PQ_RPTR_REPORT_ADDR", "CP_HQD_PQ_RPTR_REPORT_ADDR_HI", "CP_HQD_PQ_WPTR_POLL_ADDR",
       "CP_HQD_PQ_WPTR_POLL_ADDR_HI", "CP_HQD_PQ_DOORBELL_CONTROL", "CP_HQD_PQ_CONTROL", "CP_HQD_DEQUEUE_REQUEST",
       "CP_HQD_EOP_BASE_ADDR", "CP_HQD_EOP_BASE_ADDR_HI", "CP_HQD_EOP_CONTROL", "CP_HQD_EOP_RPTR", "CP_HQD_EOP_WPTR",
       "CP_HQD_PQ_WPTR_LO", "CP_HQD_PQ_WPTR_HI", "CP_HQD_IB_CONTROL", "CP_HQD_IQ_TIMER", "CP_MQD_CONTROL",
       "CPC_INT_CNTL", "CPC_INT_STATUS", "CP_ME1_PIPE0_INT_CNTL", "CP_ME1_PIPE1_INT_CNTL", "CP_ME1_PIPE2_INT_CNTL",
       "CP_ME1_PIPE3_INT_CNTL", "CP_ME2_PIPE0_INT_CNTL", "CP_ME2_PIPE1_INT_CNTL"]
# Global state, no selection.
STATE = {
    "GC": ["GRBM_STATUS", "GRBM_STATUS2", "CP_STAT", "CP_ME_CNTL", "CP_MEC_CNTL", "RLC_CNTL", "RLC_STAT", "RLC_GPM_STAT",
           "RLC_PG_CNTL", "SDMA0_F32_CNTL", "SDMA1_F32_CNTL", "SDMA0_STATUS_REG", "SDMA1_STATUS_REG", "SDMA0_CNTL",
           "SDMA1_CNTL", "SDMA0_GFX_RB_CNTL", "SDMA1_GFX_RB_CNTL", "SDMA0_GFX_RB_RPTR", "SDMA0_GFX_RB_WPTR",
           "CP_INT_CNTL_RING0", "CP_RB0_RPTR", "CP_RB0_WPTR", "CP_RB_DOORBELL_CONTROL", "SCRATCH_REG0",
           "CP_PQ_WPTR_POLL_CNTL", "RLC_CP_SCHEDULERS", "CP_MEC_DOORBELL_RANGE_LOWER", "CP_MEC_DOORBELL_RANGE_UPPER"],
    "OSSSYS": ["IH_RB_CNTL", "IH_RB_BASE", "IH_RB_BASE_HI", "IH_RB_RPTR", "IH_RB_WPTR", "IH_RB_WPTR_ADDR_LO",
               "IH_RB_WPTR_ADDR_HI", "IH_DOORBELL_RPTR", "IH_CNTL", "IH_STATUS", "IH_INT_FLOOD_CNTL"],
    "NBIO": ["INTERRUPT_CNTL", "INTERRUPT_CNTL2", "BIF_IH_DOORBELL_RANGE", "RCC_DEV0_EPF0_RCC_DOORBELL_APER_EN",
             "BIF_BX_DEV0_EPF0_VF0_DOORBELL_SELFRING_GPA_APER_CNTL", "BIF_BX_DEV0_EPF0_VF0_DOORBELL_SELFRING_GPA_APER_BASE_LOW",
             "BIF_BX_DEV0_EPF0_VF0_DOORBELL_SELFRING_GPA_APER_BASE_HIGH"],
    "MP0": ["MP0_SMN_C2PMSG_64", "MP0_SMN_C2PMSG_81"],
}


def main():
    offsets = {}
    for line in (ROOT / "tools/win/bc250rd/reglist.txt").read_text(encoding="utf-8").splitlines():
        m = LINE.match(line)
        if m:
            offsets[(m.group(1), m.group(2))] = int(m.group(3), 16)
    missing = []

    def look(ip, name):
        if (ip, name) not in offsets:
            missing.append(f"{ip}.{name}")
            return None
        return [f"{ip}.{name}", offsets[(ip, name)]]

    out = {"hqd": [r for r in (look("GC", n) for n in HQD) if r],
           "state": [r for ip, names in STATE.items() for r in (look(ip, n) for n in names) if r],
           # me, pipe, queue: the gfx queue, the eight compute queues amdgpu uses, and every queue of MEC pipe/me
           # combinations the KIQ could be on (it says which in its ring name; all are read, the active one shows).
           "queues": [[0, 0, 0]] + [[1, p, q] for p in range(4) for q in range(2)] + [[2, p, q] for p in range(2) for q in range(2)]}
    target = Path(__file__).resolve().parent / "lists.json"
    target.write_text(json.dumps(out, indent=1), encoding="utf-8", newline="\n")
    print(f"{len(out['hqd'])} per-queue registers, {len(out['state'])} state registers, {len(out['queues'])} queues -> {target}")
    if missing:
        print("not on the read list, left out: " + ", ".join(missing))
    return 0


if __name__ == "__main__":
    sys.exit(main())
