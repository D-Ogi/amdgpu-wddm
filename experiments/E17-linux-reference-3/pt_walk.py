#!/usr/bin/env python3
"""Walk a process's GPU page tables on the probe, read-only, and print every entry decoded.

    python3 -u pt_walk.py pte_bits.json --client 5 --va 0x200000000 0x200100000 --dump /tmp/e17/pt
    python3 -u pt_walk.py pte_bits.json --client 5 --scan
    python3 -u pt_walk.py pte_bits.json --pd 0xf40012000 --levels 3 --block-size 9 --va 0x200000000
    python3 -u pt_walk.py pte_bits.json --check 0x200000000 0xE17E17E1

Why this exists next to umr: umr has the same walk, debugged, and it is what the session runs first
(`--vm-decode`). This is the second, independent decoder, written from the kernel's own definitions
through gen_pte_bits.py, so that a surprising entry is either confirmed by two decoders or is a
decoder bug. Neither of them is allowed to be the only witness of a bit that ends up in
driver/shim/bc250_pte.c.

WHAT IT TOUCHES, AND WHY EACH OF THOSE IS SAFE

  /sys/kernel/debug/dri/client-<n>/vm_pagetable_info   opened O_RDONLY, read once. amdgpu prints
      five numbers for the VM of that open file and nothing else (amdgpu_debugfs.c:2134-2168):
      pd_address, max_pfn, num_level, block_size, fragment_size. Reading it takes one BO reservation
      inside the kernel and changes nothing.
  /sys/kernel/debug/dri/<dev>/amdgpu_vram            opened O_RDONLY, pread at page-aligned offsets.
      The file IS video memory addressed by physical VRAM address (amdgpu_ttm.c, the same file umr
      reads for a page table walk). A read never writes and never allocates.
  /sys/kernel/debug/dri/<dev>/amdgpu_iomem           opened O_RDONLY, pread. The same for pages of
      system memory, addressed by their DMA address. amdgpu maps the page, copies out, unmaps.

Nothing else in debugfs is opened. In particular the names that DO something when read - amdgpu_
test_ib, amdgpu_gpu_recover, amdgpu_evict_vram, amdgpu_evict_gtt, amdgpu_benchmark - are never
formed here, not even by a glob: the only glob in this file is over `dri/*/amdgpu_vram`, and every
other path is spelled out.

WHAT A RESULT MEANS

Every level's entry is printed with the raw 64-bit value first, so a reader who distrusts the
decode can redo it by hand. `--dump` additionally writes each 4 KB table page that was visited as a
hex file, which is the artefact the PC side re-decodes: an evidence file nobody has to trust this
script to have read correctly.

`--check <va> <word>` is the positive control. e17_vm.py writes a known 32-bit word at the start of
each of its buffers through the CPU mapping; if this walk lands on a physical address whose first
word is that value, the whole chain - vm_pagetable_info, the level split, the address masks, the
aperture choice - is right for that VA. If it does not, nothing else in the run's page-table half
means anything yet.
"""

import argparse
import glob
import json
import os
import re
import struct
import sys

PAGE = 4096
ENTRY = 8


def die(message):
    sys.exit(f"pt_walk: {message}")


class Bits:
    """The AMD entry layout, as gen_pte_bits.py parsed it out of the kernel headers."""

    def __init__(self, path):
        d = json.load(open(path))
        self.d = d
        self.bits = d["bits"]
        self.order = d["order"]
        self.frag_shift = d["frag"]["shift"]
        self.frag_mask = d["frag"]["mask"]
        self.mtype_shift = d["mtype"]["shift"]
        self.mtype_mask = d["mtype"]["mask"]
        self.mtype_names = {int(k): v for k, v in d["mtype"]["names"].items()}
        self.bfs_shift = d["bfs"]["shift"]
        self.pde_mask = d["addr"]["pde_mask"]
        self.pte_mask = d["addr"]["pte_mask"]
        self.page_shift = d["addr"]["page_shift"]
        self.known = 0
        for bit in self.bits.values():
            self.known |= 1 << bit
        self.known |= self.frag_mask << self.frag_shift
        self.known |= self.mtype_mask << self.mtype_shift
        self.known |= 0x1F << self.bfs_shift

    def has(self, entry, name):
        return bool(entry >> self.bits[name] & 1) if name in self.bits else False

    def describe(self, entry, leaf):
        if entry == 0:
            return "0 (not present)"
        addr = entry & (self.pte_mask if leaf else self.pde_mask)
        flags = [name.replace("AMDGPU_PTE_", "").replace("AMDGPU_PDE_", "PDE_")
                 for name in self.order if self.has(entry, name)]
        mtype = entry >> self.mtype_shift & self.mtype_mask
        frag = entry >> self.frag_shift & self.frag_mask
        text = f"addr={addr:#014x} " + " ".join(flags)
        if leaf:
            text += f" MTYPE={self.mtype_names.get(mtype, mtype)}"
            if frag:
                text += f" FRAG={frag} ({1 << (self.page_shift + frag):#x} bytes)"
        else:
            bfs = entry >> self.bfs_shift & 0x1F
            if bfs:
                text += f" BFS={bfs}"
        unknown = entry & ~(self.known | self.pde_mask) & 0xFFFFFFFFFFFFFFFF
        if unknown:
            text += f" UNKNOWN_BITS={unknown:#018x}"
        return text


class Memory:
    """Physical video memory and system memory, through amdgpu's own read-only debugfs files."""

    def __init__(self):
        vram = sorted(glob.glob("/sys/kernel/debug/dri/*/amdgpu_vram"))
        if not vram:
            die("no amdgpu_vram: debugfs not mounted, or amdgpu not loaded")
        self.dri = os.path.dirname(vram[0])
        self.vram = os.open(vram[0], os.O_RDONLY)
        iomem = os.path.join(self.dri, "amdgpu_iomem")
        self.iomem = os.open(iomem, os.O_RDONLY) if os.path.exists(iomem) else None
        print(f"# debugfs {self.dri}, amdgpu_vram fd {self.vram}, "
              f"amdgpu_iomem {'present' if self.iomem is not None else 'ABSENT'}")

    def read(self, address, size, system):
        fd = self.iomem if system else self.vram
        where = "system" if system else "vram"
        if fd is None:
            return None, f"no amdgpu_iomem to read {where} address {address:#x}"
        try:
            data = os.pread(fd, size, address)
        except OSError as e:
            return None, f"{where} read at {address:#x} failed: errno {e.errno}"
        if len(data) != size:
            return None, f"{where} read at {address:#x} returned {len(data)} of {size} bytes"
        return data, None


def client_info(client):
    """The five numbers amdgpu prints for one open file's VM."""
    path = f"/sys/kernel/debug/dri/client-{client}/vm_pagetable_info"
    if not os.path.exists(path):
        die(f"no {path}: that client id has no open amdgpu file")
    text = open(path).read()
    out = {}
    for key in ("pd_address", "max_pfn", "num_level", "block_size", "fragment_size"):
        m = re.search(rf"^{key}:\s*(0x[0-9a-fA-F]+|\d+)", text, re.M)
        if not m:
            die(f"{path} has no {key}; amdgpu printed: {text!r}")
        out[key] = int(m.group(1), 0)
    proc = f"/sys/kernel/debug/dri/client-{client}/proc_info"
    out["proc_info"] = open(proc).read().strip() if os.path.exists(proc) else ""
    return out


def level_shift(level, num_level, block_size, pdb0):
    """amdgpu_vm_pt_level_shift(), amdgpu_vm_pt.c:50-64. The leaf is 0; a directory level is
    9 * (PDB0 - level) + block_size. Levels are absolute here (PDB2 = 0), as in the kernel."""
    ptb = pdb0 + 1
    if level == ptb:
        return 0
    return 9 * (pdb0 - level) + block_size


def walk(bits, mem, info, va, dump=None, quiet=False):
    """One virtual address, root to leaf. Returns the leaf entry, or None."""
    num_level, block_size = info["num_level"], info["block_size"]
    pdb0 = bits.d["pdb0_index"]
    ptb = bits.d["ptb_index"]
    root = ptb - num_level                      # amdgpu_vm.c:2392-2401
    pfn = va >> bits.page_shift
    # The root directory can be larger than 512 entries: amdgpu_vm_pt_num_entries() rounds max_pfn
    # up to the root's own shift (amdgpu_vm_pt.c:75-91), which is why the root index is not masked
    # to nine bits the way every level below it is.
    table, system = info["pd_address"] & bits.pde_mask, False
    if not quiet:
        print(f"va {va:#018x}  pfn {pfn:#x}  root level {root}, {num_level} levels, "
              f"block_size {block_size}")
    entry = None
    for level in range(root, ptb + 1):
        shift = level_shift(level, num_level, block_size, pdb0)
        index = pfn >> shift
        if level != root:
            index &= (1 << (block_size if level == ptb else 9)) - 1
        page, error = mem.read(table, PAGE, system)
        if error:
            print(f"  level {level}: {error}")
            return None
        if dump is not None:
            name = os.path.join(dump, f"{'sys' if system else 'vram'}-{table:012x}.hex")
            if not os.path.exists(name):
                with open(name, "w") as f:
                    for off in range(0, PAGE, 32):
                        f.write(f"{off:04x} " + " ".join(f"{w:016x}" for w in
                                struct.unpack_from("<4Q", page, off)) + "\n")
        offset = index * ENTRY
        if offset + ENTRY > PAGE:
            print(f"  level {level}: index {index} is past the end of a 4 KB table")
            return None
        entry = struct.unpack_from("<Q", page, offset)[0]
        leaf = level == ptb or bits.has(entry, "AMDGPU_PDE_PTE")
        if not quiet:
            print(f"  level {level} shift {shift:2d} index {index:5d} @ "
                  f"{'sys' if system else 'vram'} {table + offset:#014x}: "
                  f"{entry:016x}  {bits.describe(entry, leaf)}")
        if entry == 0 or not bits.has(entry, "AMDGPU_PTE_VALID"):
            return entry
        if leaf:
            return entry
        table = entry & bits.pde_mask
        system = bits.has(entry, "AMDGPU_PTE_SYSTEM")
    return entry


def leaf_physical(bits, entry, va):
    """The physical address a leaf entry gives for `va`, with the page offset put back."""
    frag = entry >> bits.frag_shift & bits.frag_mask
    size = 1 << (bits.page_shift + frag)
    return (entry & bits.pte_mask) + (va & (size - 1)), bits.has(entry, "AMDGPU_PTE_SYSTEM"), size


def scan(bits, mem, info, limit):
    """Every non-zero entry of the root directory, and of one level below it, so that a VM can be
    described without knowing where its mappings are."""
    pdb0, ptb = bits.d["pdb0_index"], bits.d["ptb_index"]
    root = ptb - info["num_level"]
    page, error = mem.read(info["pd_address"] & bits.pde_mask, PAGE, False)
    if error:
        print(f"root: {error}")
        return
    shown = 0
    for index in range(PAGE // ENTRY):
        entry = struct.unpack_from("<Q", page, index * ENTRY)[0]
        if not entry:
            continue
        shift = level_shift(root, info["num_level"], info["block_size"], pdb0)
        base = index << (shift + bits.page_shift)
        print(f"root[{index:3d}] covers {base:#018x}: {entry:016x}  {bits.describe(entry, False)}")
        shown += 1
        if shown >= limit:
            print(f"... stopping at {limit} entries (--limit)")
            return
    print(f"{shown} non-zero entries in the root directory")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("bits", help="pte_bits.json, generated on the PC by gen_pte_bits.py")
    p.add_argument("--client", type=int, help="drm client id under /sys/kernel/debug/dri/client-N")
    p.add_argument("--pd", type=lambda s: int(s, 0), help="page directory address, if not --client")
    p.add_argument("--levels", type=int, default=3)
    p.add_argument("--block-size", type=int, default=9)
    p.add_argument("--va", nargs="*", default=[], help="virtual addresses to walk")
    p.add_argument("--check", nargs=2, action="append", default=[],
                   metavar=("VA", "WORD"), help="positive control: the first 32-bit word at VA")
    p.add_argument("--scan", action="store_true", help="list the root directory instead")
    p.add_argument("--limit", type=int, default=64)
    p.add_argument("--dump", help="directory for the hex dump of every table page visited")
    args = p.parse_args()

    bits = Bits(args.bits)
    if args.client is not None:
        info = client_info(args.client)
        print(f"# client-{args.client}: " + " ".join(f"{k}={v:#x}" for k, v in info.items()
                                                     if k != "proc_info"))
        if info["proc_info"]:
            print("# " + info["proc_info"].replace("\n", " | "))
    elif args.pd is not None:
        info = {"pd_address": args.pd, "num_level": args.levels, "block_size": args.block_size,
                "fragment_size": 9, "max_pfn": 0}
        print(f"# page directory {args.pd:#x} given on the command line")
    else:
        die("one of --client or --pd is required")

    mem = Memory()
    if args.dump:
        os.makedirs(args.dump, exist_ok=True)

    if args.scan:
        scan(bits, mem, info, args.limit)

    for text in args.va:
        entry = walk(bits, mem, info, int(text, 0), args.dump)
        if entry:
            pa, system, size = leaf_physical(bits, entry, int(text, 0))
            print(f"  -> {'system' if system else 'vram'} {pa:#014x}, page size {size:#x}")
        print()

    failures = 0
    for text, word in args.check:
        va, want = int(text, 0), int(word, 0) & 0xFFFFFFFF
        entry = walk(bits, mem, info, va, args.dump, quiet=True)
        if not entry:
            print(f"CONTROL va {va:#x}: no valid leaf entry - FAILED")
            failures += 1
            continue
        pa, system, _ = leaf_physical(bits, entry, va)
        data, error = mem.read(pa & ~3, 4, system)
        if error:
            print(f"CONTROL va {va:#x} -> {'system' if system else 'vram'} {pa:#x}: {error} - FAILED")
            failures += 1
            continue
        got = struct.unpack("<I", data)[0]
        ok = got == want
        failures += 0 if ok else 1
        print(f"CONTROL va {va:#x} -> {'system' if system else 'vram'} {pa:#014x}: "
              f"read {got:08x}, expected {want:08x} - {'ok' if ok else 'FAILED'}")
    if args.check:
        print(f"CONTROL: {len(args.check) - failures} of {len(args.check)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
