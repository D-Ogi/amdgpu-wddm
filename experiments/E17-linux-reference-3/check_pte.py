#!/usr/bin/env python3
"""Hold the measured page tables against what driver/shim/bc250_pte.c was written from.

Runs on the PC, after the session, over the files the `hold` phase pulled back:

    python experiments/E17-linux-reference-3/check_pte.py <pulled hold directory>

    <dir>/vm_pagetable_info.txt   the five numbers amdgpu prints for that VM
    <dir>/bos.txt                 label, va, size, domain, flags, word - what we asked for
    <dir>/pt/*.hex                every page table page the walk visited, 4 KB each

Nothing from the probe is trusted to have decoded anything: this walks the dumped pages again, on
the PC, and prints one line per claim of `driver/shim/include/bc250_pte.h`'s "TRANSCRIPTION ONLY"
list with CONFIRMED, REFUTED or NOT COVERED next to it. A REFUTED line is a defect in the driver
and a row for docs/facts.md; a NOT COVERED line is a gap in this session, not a pass.

This is a third decoder over the same bytes (umr on the probe, pt_walk.py on the probe, this one
here). Three readings of one 64-bit value is not paranoia: every claim below ends up as a constant
in a kernel driver that will write those bits into hardware.
"""

import json
import re
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
PAGE = 4096
ENTRY = 8


def load_pages(directory):
    """{(space, base): bytes} from the hex dumps pt_walk.py wrote."""
    pages = {}
    for path in sorted(Path(directory).glob("*.hex")):
        m = re.match(r"(vram|sys)-([0-9a-f]+)\.hex$", path.name)
        if not m:
            continue
        data = bytearray(PAGE)
        for line in path.read_text().splitlines():
            parts = line.split()
            if len(parts) != 5:
                continue
            offset = int(parts[0], 16)
            for i, word in enumerate(parts[1:]):
                struct.pack_into("<Q", data, offset + i * 8, int(word, 16))
        pages[(m.group(1), int(m.group(2), 16))] = bytes(data)
    return pages


def load_info(path):
    text = Path(path).read_text()
    out = {}
    for key in ("pd_address", "max_pfn", "num_level", "block_size", "fragment_size"):
        m = re.search(rf"^{key}:\s*(0x[0-9a-fA-F]+|\d+)", text, re.M)
        if m:
            out[key] = int(m.group(1), 0)
    return out


def load_bos(path):
    bos = []
    for line in Path(path).read_text().splitlines():
        parts = line.split()
        if len(parts) == 6:
            bos.append({"label": parts[0], "va": int(parts[1], 16), "size": int(parts[2], 16),
                        "domain": parts[3], "flags": int(parts[4], 16),
                        # A PRT row has no buffer, so e17_vm.py writes "-" for its word.
                        "word": None if parts[5] == "-" else int(parts[5], 16)})
    return bos


class Decoder:
    def __init__(self, bits):
        self.b = bits
        self.bit = bits["bits"]

    def has(self, entry, name):
        return bool(entry >> self.bit[name] & 1)

    def mtype(self, entry):
        value = entry >> self.b["mtype"]["shift"] & self.b["mtype"]["mask"]
        return self.b["mtype"]["names"].get(str(value), str(value))

    def frag(self, entry):
        return entry >> self.b["frag"]["shift"] & self.b["frag"]["mask"]

    def bfs(self, entry):
        return entry >> self.b["bfs"]["shift"] & 0x1F


def walk(bits, dec, pages, info, va):
    """Root to leaf over the dumped pages. Returns [(level, entry, is_leaf)] and a note."""
    pdb0, ptb = bits["pdb0_index"], bits["ptb_index"]
    root = ptb - info["num_level"]
    page_shift = bits["addr"]["page_shift"]
    pfn = va >> page_shift
    table, space = info["pd_address"] & bits["addr"]["pde_mask"], "vram"
    path = []
    for level in range(root, ptb + 1):
        shift = 0 if level == ptb else 9 * (pdb0 - level) + info["block_size"]
        index = pfn >> shift
        if level != root:
            index &= (1 << (info["block_size"] if level == ptb else 9)) - 1
        data = pages.get((space, table))
        if data is None:
            return path, f"no dump of the {space} page at {table:#x}"
        entry = struct.unpack_from("<Q", data, index * ENTRY)[0]
        leaf = level == ptb or dec.has(entry, "AMDGPU_PDE_PTE")
        path.append((level, entry, leaf))
        if entry == 0 or not dec.has(entry, "AMDGPU_PTE_VALID") or leaf:
            return path, None
        table = entry & bits["addr"]["pde_mask"]
        space = "sys" if dec.has(entry, "AMDGPU_PTE_SYSTEM") else "vram"
    return path, None


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    directory = Path(sys.argv[1])
    bits = json.load(open(HERE / "pte_bits.json"))
    dec = Decoder(bits)
    info = load_info(directory / "vm_pagetable_info.txt")
    bos = load_bos(directory / "bos.txt")
    pages = load_pages(directory / "pt")
    print(f"{len(pages)} table pages, {len(bos)} buffers, VM " +
          " ".join(f"{k}={v:#x}" for k, v in info.items()))

    results = []

    def claim(name, verdict, detail):
        results.append((name, verdict, detail))
        print(f"  {verdict:<11} {name}: {detail}")

    print("\nThe shape of the tables (bc250_pte.h, 'the multi-level table for VMIDs 1..15')")
    if not info:
        claim("num_level/block_size", "NOT COVERED", "vm_pagetable_info.txt was not captured")
    else:
        want = {"num_level": 3, "block_size": 9, "fragment_size": 9}
        for key, expected in want.items():
            got = info.get(key)
            claim(key, "CONFIRMED" if got == expected else "REFUTED",
                  f"amdgpu says {got}, the transcription assumed {expected}")
        pdb0, ptb = bits["pdb0_index"], bits["ptb_index"]
        page_shift = bits["addr"]["page_shift"]
        # Each directory level consumes nine address bits above its own shift; the root consumes
        # as many as max_pfn needs, which is why its top bit is printed from max_pfn and not from
        # a constant (amdgpu_vm_pt_num_entries(), amdgpu_vm_pt.c:75-91).
        split = []
        for level in range(ptb - info.get("num_level", 3), ptb):
            shift = 9 * (pdb0 - level) + info.get("block_size", 9)
            split.append(f"level {level} -> VA[{shift + page_shift + 8}:{shift + page_shift}]")
        split.append(f"leaf VA[{page_shift + info.get('block_size', 9) - 1}:{page_shift}]")
        print("              the VA split this implies: " + ", ".join(split))
        print(f"              max_pfn {info.get('max_pfn', 0):#x} = "
              f"{info.get('max_pfn', 0) << page_shift >> 40} TB of address space")

    print("\nWhat each buffer's leaf entry carries")
    leaves = {}
    for bo in bos:
        path, note = walk(bits, dec, pages, info, bo["va"])
        if note or not path:
            claim(bo["label"], "NOT COVERED", note or "no entries")
            continue
        level, entry, leaf = path[-1]
        leaves[bo["label"]] = (level, entry, leaf)
        flags = [n.replace("AMDGPU_PTE_", "").replace("AMDGPU_PDE_", "PDE_")
                 for n in bits["order"] if dec.has(entry, n)]
        print(f"  {bo['label']:<8} {bo['domain']:<4} level {level} {entry:016x} "
              f"{' '.join(flags)} MTYPE={dec.mtype(entry)} FRAG={dec.frag(entry)}")

    print("\nThe directory entries (PDE format: VALID | SYSTEM | SNOOPED and nothing else)")
    checked = 0
    for bo in bos:
        path, note = walk(bits, dec, pages, info, bo["va"])
        for level, entry, leaf in path:
            if leaf or not entry or not dec.has(entry, "AMDGPU_PTE_VALID"):
                continue
            checked += 1
            extra = [n for n in ("AMDGPU_PTE_READABLE", "AMDGPU_PTE_WRITEABLE",
                                 "AMDGPU_PTE_EXECUTABLE", "AMDGPU_PTE_TF", "AMDGPU_PTE_NOALLOC")
                     if dec.has(entry, n)]
            if extra or dec.mtype(entry) != "MTYPE_NC" or dec.bfs(entry):
                claim(f"PDE at level {level} for {bo['label']}", "REFUTED",
                      f"{entry:016x} carries {extra} mtype {dec.mtype(entry)} bfs {dec.bfs(entry)}")
                break
    if checked and not any(v == "REFUTED" and "PDE" in n for n, v, _ in results):
        claim("PDE format", "CONFIRMED",
              f"{checked} directory entries, none with a permission bit, a non-zero MTYPE field "
              f"or a block fragment size")
    elif not checked:
        claim("PDE format", "NOT COVERED", "no directory entry was dumped")

    print("\nThe default memory type of a VM leaf entry (transcription says NC, not the GART's UC)")
    if "rw" in leaves:
        mtype = dec.mtype(leaves["rw"][1])
        claim("leaf MTYPE", "CONFIRMED" if mtype == "MTYPE_NC" else "REFUTED",
              f"the plain read/write GTT buffer's entry has {mtype}")
    else:
        claim("leaf MTYPE", "NOT COVERED", "the `rw` buffer has no leaf entry here")

    print("\nThe permission bits, which is what DXGK_PTE's ReadOnly and NoExecute become")
    for label, name, expect in (("ro", "AMDGPU_PTE_WRITEABLE", False),
                                ("rw", "AMDGPU_PTE_EXECUTABLE", True),
                                ("nx", "AMDGPU_PTE_EXECUTABLE", False),
                                ("noalloc", "AMDGPU_PTE_NOALLOC", True)):
        if label not in leaves:
            claim(f"{label}: {name}", "NOT COVERED", "no leaf entry")
            continue
        got = dec.has(leaves[label][1], name)
        claim(f"{label}: {name}", "CONFIRMED" if got == expect else "REFUTED",
              f"{'set' if got else 'clear'}, expected {'set' if expect else 'clear'}")

    print("\n64 KB pages as AMDGPU_PTE_FRAG(4) - bc250_pte.h calls this correspondence an inference")
    if "k64" in leaves:
        frag = dec.frag(leaves["k64"][1])
        claim("64 KB fragment", "CONFIRMED" if frag == 4 else "REFUTED",
              f"the 64 KB buffer's leaf entry has FRAG={frag} "
              f"({1 << (12 + frag)} bytes), the driver assumes FRAG=4 (65536)")
    else:
        claim("64 KB fragment", "NOT COVERED", "no leaf entry for the 64 KB buffer")

    print("\n2 MB: does a PDB0 entry ever become a page itself (AMDGPU_PDE_PTE)?")
    for label in ("m2", "vram2m"):
        if label not in leaves:
            claim(f"{label}: huge page", "NOT COVERED", "no entry")
            continue
        level, entry, leaf = leaves[label]
        huge = dec.has(entry, "AMDGPU_PDE_PTE")
        claim(f"{label}: huge page", "CONFIRMED",
              f"level {level}, PDE_PTE {'set' if huge else 'clear'}, FRAG={dec.frag(entry)} - "
              f"{'a 2 MB page in the directory' if huge else 'an ordinary table of 4 KB entries'}")

    print("\nVRAM against system memory in a leaf entry")
    for label in ("vram4k", "rw"):
        if label not in leaves:
            continue
        system = dec.has(leaves[label][1], "AMDGPU_PTE_SYSTEM")
        want = label.startswith("vram") is False
        claim(f"{label}: SYSTEM", "CONFIRMED" if system == want else "REFUTED",
              f"{'set' if system else 'clear'}, expected {'set' if want else 'clear'}")

    bad = sum(1 for _, v, _ in results if v == "REFUTED")
    gaps = sum(1 for _, v, _ in results if v == "NOT COVERED")
    print(f"\n{len(results)} claims: {len(results) - bad - gaps} confirmed, {bad} refuted, "
          f"{gaps} not covered")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
