#!/usr/bin/env python3
"""Generate pte_bits.json: the AMD page table entry bit layout, parsed out of the kernel headers.

Run on the PC:  python experiments/E17-linux-reference-3/gen_pte_bits.py

pt_walk.py reads the result on the probe, where the kernel sources are not. Nothing in the layout is
typed by hand here, for the same reason no register offset ever is: every number below is parsed out
of a file in ref/, and the file and line it came from travel with it in the JSON, so a decoded entry
can always be traced back to the definition it was decoded with.

Sources (all read-only, all in ref/linux-src at the tree the lab's kernel 6.18.52 matches):

  amdgpu_vm.h        the AMDGPU_PTE_* / AMDGPU_PDE_* flag bits, the FRAG and MTYPE_NV10 fields,
                     and the four-level enum PDB2 -> PDB1 -> PDB0 -> PTB
  gmc_v10_0.c        the BUG_ON in gmc_v10_0_get_vm_pde() that states which bits of a directory
                     entry's address field are legal; inverting it gives the address mask
  navi10_enum.h      the MTYPE names (this one from third_party/linux-amdgpu, the same headers
                     regcalc computes register offsets from)
"""

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORKSPACE = ROOT.parent
AMDGPU = WORKSPACE / "ref" / "linux-src" / "drivers" / "gpu" / "drm" / "amd" / "amdgpu"
VM_H = AMDGPU / "amdgpu_vm.h"
GMC_C = AMDGPU / "gmc_v10_0.c"
GART_H = AMDGPU / "amdgpu_gart.h"
ENUM_H = ROOT / "third_party" / "linux-amdgpu" / "navi10_enum.h"

BIT = re.compile(r"^#define\s+(AMDGPU_(?:PTE|PDE)_[A-Z0-9_]+)\s+\(1ULL\s*<<\s*(\d+)\)")
FRAG = re.compile(r"^#define\s+AMDGPU_PTE_FRAG\(x\)\s+\(\(x\s*&\s*(0x[0-9a-fA-F]+)ULL\)\s*<<\s*(\d+)\)")
MTYPE = re.compile(r"^#define\s+AMDGPU_PTE_MTYPE_NV10_SHIFT\(mtype\)\s+\(\(uint64_t\)\(mtype\)\s*<<\s*(\d+)\)")
MTYPE_MASK = re.compile(r"^#define\s+AMDGPU_PTE_MTYPE_NV10_MASK\s+AMDGPU_PTE_MTYPE_NV10_SHIFT\((0x[0-9a-fA-F]+|\d+)ULL\)")
BFS = re.compile(r"^#define\s+AMDGPU_PDE_BFS\(a\)\s+\(\(uint64_t\)a\s*<<\s*(\d+)\)")
LEVEL = re.compile(r"^\s*(AMDGPU_VM_(?:PDB2|PDB1|PDB0|PTB)),?\s*$")
BUGON = re.compile(r"BUG_ON\(\*addr\s*&\s*(0x[0-9a-fA-F]+)ULL\)")
PAGE_SHIFT = re.compile(r"^#define\s+AMDGPU_GPU_PAGE_SHIFT\s+(\d+)")
MTYPE_NAME = re.compile(r"^\s*(MTYPE_[A-Z0-9_]+)\s*=\s*(0x[0-9a-fA-F]+|\d+),")

# Flag bits worth naming in a decoded line, in the order a reader wants to see them. A bit that is
# defined in the header but not listed here is still decoded (the "unknown bits" field of a decode
# is what catches a surprise); this only fixes the printing order.
ORDER = ["AMDGPU_PTE_VALID", "AMDGPU_PTE_SYSTEM", "AMDGPU_PTE_SNOOPED", "AMDGPU_PTE_TMZ",
         "AMDGPU_PTE_EXECUTABLE", "AMDGPU_PTE_READABLE", "AMDGPU_PTE_WRITEABLE",
         "AMDGPU_PTE_PRT", "AMDGPU_PDE_PTE", "AMDGPU_PTE_LOG", "AMDGPU_PTE_TF",
         "AMDGPU_PTE_NOALLOC"]

# GFX12 spellings of the same ideas, which this part does not use. Kept out so that a decode cannot
# quietly report a GFX12 flag on a GFX10 entry.
NOT_OURS = ("_GFX12", "_IS_PTE", "_DCC")


def grab(path, pattern, what):
    """The first line of `path` that matches, with the citation that says where it was."""
    for n, line in enumerate(Path(path).read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        m = pattern.search(line)
        if m:
            return m, f"{Path(path).name}:{n}"
    sys.exit(f"not found in {path}: {what}")


def main():
    text = VM_H.read_text(encoding="utf-8", errors="replace").splitlines()
    bits, cites = {}, {}
    for n, line in enumerate(text, 1):
        m = BIT.match(line)
        if m and not any(s in m.group(1) for s in NOT_OURS):
            bits[m.group(1)] = int(m.group(2))
            cites[m.group(1)] = f"amdgpu_vm.h:{n}"

    frag, frag_cite = grab(VM_H, FRAG, "AMDGPU_PTE_FRAG")
    mt, mt_cite = grab(VM_H, MTYPE, "AMDGPU_PTE_MTYPE_NV10_SHIFT")
    mtm, mtm_cite = grab(VM_H, MTYPE_MASK, "AMDGPU_PTE_MTYPE_NV10_MASK")
    bfs, bfs_cite = grab(VM_H, BFS, "AMDGPU_PDE_BFS")
    bug, bug_cite = grab(GMC_C, BUGON, "the BUG_ON of gmc_v10_0_get_vm_pde")
    page, page_cite = grab(GART_H, PAGE_SHIFT, "AMDGPU_GPU_PAGE_SHIFT")

    # The directory entry's address field is everything the BUG_ON forbids to be set, inverted:
    # gmc_v10_0.c refuses an address with any bit above 47 or below 6, so the field is 47:6.
    forbidden = int(bug.group(1), 16)
    pde_mask = (~forbidden) & 0xFFFFFFFFFFFFFFFF
    frag_shift, frag_width = int(frag.group(2)), int(frag.group(1), 16)
    # A leaf entry names a GPU page, so its address field starts at AMDGPU_GPU_PAGE_SHIFT: 47:12.
    # The bits between 12 and 6, which a directory entry could use, carry the fragment size there.
    page_shift = int(page.group(1))
    pte_mask = pde_mask & ~((1 << page_shift) - 1)

    levels = []
    for line in text:
        m = LEVEL.match(line)
        if m and m.group(1) not in levels:
            levels.append(m.group(1))

    mtypes = {}
    for line in ENUM_H.read_text(encoding="utf-8", errors="replace").splitlines():
        m = MTYPE_NAME.match(line)
        if m and m.group(1) in ("MTYPE_NC", "MTYPE_WC", "MTYPE_CC", "MTYPE_UC"):
            mtypes[int(m.group(2), 0)] = m.group(1)

    out = {
        "generated_by": "experiments/E17-linux-reference-3/gen_pte_bits.py",
        "sources": {"vm_h": str(VM_H.relative_to(WORKSPACE)), "gmc_c": str(GMC_C.relative_to(WORKSPACE)),
                    "enum_h": str(ENUM_H.relative_to(WORKSPACE))},
        "bits": bits,
        "bit_cites": cites,
        "order": [name for name in ORDER if name in bits],
        "frag": {"shift": frag_shift, "mask": frag_width, "cite": frag_cite},
        "mtype": {"shift": int(mt.group(1)), "mask": int(mtm.group(1), 0), "names": mtypes,
                  "cite": mt_cite + " " + mtm_cite},
        "bfs": {"shift": int(bfs.group(1)), "cite": bfs_cite},
        "addr": {"pde_mask": pde_mask, "pte_mask": pte_mask, "page_shift": page_shift,
                 "cite": f"{bug_cite} BUG_ON(*addr & {bug.group(1)}ULL); {page_cite}"},
        # PDB2 -> PDB1 -> PDB0 -> PTB, amdgpu_vm.h:187-195. amdgpu_vm_pt_level_shift()
        # (amdgpu_vm_pt.c:50-64) is 9 * (PDB0 - level) + block_size for the three directory levels
        # and 0 for the leaf; root_level is PDB2 when num_level is 3 (amdgpu_vm.c:2392-2401).
        "levels": levels,
        "pdb0_index": levels.index("AMDGPU_VM_PDB0") if "AMDGPU_VM_PDB0" in levels else 2,
        "ptb_index": levels.index("AMDGPU_VM_PTB") if "AMDGPU_VM_PTB" in levels else 3,
    }
    target = Path(__file__).resolve().parent / "pte_bits.json"
    target.write_text(json.dumps(out, indent=1), encoding="utf-8", newline="\n")
    print(f"{len(bits)} flag bits, frag {frag_shift}+{frag_width:#x}, mtype shift {out['mtype']['shift']}, "
          f"pde mask {pde_mask:#018x}, pte mask {pte_mask:#018x} -> {target}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
