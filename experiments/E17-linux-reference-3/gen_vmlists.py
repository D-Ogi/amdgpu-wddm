#!/usr/bin/env python3
"""Generate vmlists.json for E13's regs2.py: the GPU virtual memory registers of both hubs.

Named registers only. Every offset comes from the witness read list (tools/win/bc250rd/reglist.txt,
itself generated through tools/regcalc from the AMD headers); a name that is not on that list is
refused and reported, never computed here. No BAR5 offset is typed in this file.

Run:  python experiments/E17-linux-reference-3/gen_vmlists.py

The result is read by `python3 regs2.py vmlists.json state` on the probe, which is E13's reader
unchanged: it reads through amdgpu's own amdgpu_regs2 debugfs file, with no SRBM selection, and
never writes.

Two deliberate omissions, both safety:

  - `*_INVALIDATE_ENG*_SEM`. Reading a VM invalidation semaphore ACQUIRES it (that is what the
    register is), and our pre-driver sweep doing exactly that is what cost E03 its VM flush timeout
    (facts M25). The E13 read list dropped every `*_SEM` for that reason and this one keeps the
    line.
  - `GRBM_GFX_CNTL` and anything of MMEA, for the same reason E13 left them out.

Everything else here is an ordinary 32-bit read of a status or configuration register.
"""

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LINE = re.compile(r"^(\w+)\.(\w+)\s+0x([0-9a-fA-F]+)")

CONTEXTS = range(16)            # VMID 0 (the GART) and the fifteen per-process contexts
ENGINES = range(18)             # the invalidation engines a ring can be assigned (ring->vm_inv_eng)

# Per-context registers, one set per VMID. Which VMID a process holds is read from these:
# gmc_v10_0_emit_flush_gpu_tlb() writes PAGE_TABLE_BASE_ADDR_LO32/HI32 of the context it is
# switching to from the ring itself, so the register holds the page directory address of whatever
# ran last under that VMID (gmc_v10_0.c:396-402).
CONTEXT_REGS = ["CNTL", "PAGE_TABLE_BASE_ADDR_LO32", "PAGE_TABLE_BASE_ADDR_HI32",
                "PAGE_TABLE_START_ADDR_LO32", "PAGE_TABLE_START_ADDR_HI32",
                "PAGE_TABLE_END_ADDR_LO32", "PAGE_TABLE_END_ADDR_HI32"]

# Per-invalidation-engine registers. REQ and ACK are the two halves of the TLB flush handshake the
# same function emits (gmc_v10_0.c:404-408); the ADDR_RANGE pair carries a ranged invalidation.
ENGINE_REGS = ["REQ", "ACK", "ADDR_RANGE_LO32", "ADDR_RANGE_HI32"]

# Hub-wide registers: the L2 configuration, the fault reporting, and the apertures that say where
# VRAM, the AGP window and the system aperture sit in the GPU's address space.
HUB_REGS = ["CONTEXTS_DISABLE", "L2_CNTL", "L2_CNTL2", "L2_CNTL3", "L2_CNTL4", "L2_CNTL5",
            "L2_PROTECTION_FAULT_CNTL", "L2_PROTECTION_FAULT_CNTL2",
            "L2_PROTECTION_FAULT_STATUS",
            "L2_PROTECTION_FAULT_ADDR_LO32", "L2_PROTECTION_FAULT_ADDR_HI32",
            "L2_PROTECTION_FAULT_DEFAULT_ADDR_LO32", "L2_PROTECTION_FAULT_DEFAULT_ADDR_HI32"]

MC_REGS = ["FB_LOCATION_BASE", "FB_LOCATION_TOP", "FB_OFFSET", "AGP_BASE", "AGP_TOP", "AGP_BOT",
           "SYSTEM_APERTURE_LOW_ADDR", "SYSTEM_APERTURE_HIGH_ADDR", "MX_L1_TLB_CNTL"]

# (ip block in the read list, VM register prefix, MC register prefix)
HUBS = [("GC", "GCVM_", "GCMC_VM_"), ("MMHUB", "MMVM_", "MMMC_VM_")]


def main():
    offsets = {}
    for line in (ROOT / "tools/win/bc250rd/reglist.txt").read_text(encoding="utf-8").splitlines():
        m = LINE.match(line)
        if m:
            offsets[(m.group(1), m.group(2))] = int(m.group(3), 16)

    state, missing = [], []

    def look(ip, name):
        if (ip, name) not in offsets:
            missing.append(f"{ip}.{name}")
            return
        state.append([f"{ip}.{name}", offsets[(ip, name)]])

    for ip, vm, mc in HUBS:
        for reg in HUB_REGS:
            look(ip, vm + reg)
        for reg in MC_REGS:
            look(ip, mc + reg)
        for n in CONTEXTS:
            for reg in CONTEXT_REGS:
                look(ip, f"{vm}CONTEXT{n}_{reg}")
        for n in ENGINES:
            for reg in ENGINE_REGS:
                look(ip, f"{vm}INVALIDATE_ENG{n}_{reg}")

    out = {"state": state, "hqd": [], "queues": []}
    target = Path(__file__).resolve().parent / "vmlists.json"
    target.write_text(json.dumps(out, indent=1), encoding="utf-8", newline="\n")
    print(f"{len(state)} registers -> {target}")
    if missing:
        print(f"not on the witness read list, left out ({len(missing)}): " + ", ".join(missing))
    return 0


if __name__ == "__main__":
    sys.exit(main())
