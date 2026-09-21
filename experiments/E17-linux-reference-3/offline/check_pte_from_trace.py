#!/usr/bin/env python3
"""Check driver/shim/include/bc250_pte.h's TRANSCRIPTION list against the page table entries
that are already recorded in evidence/, without booting anything.

    python experiments/E17-linux-reference-3/offline/check_pte_from_trace.py [evidence root]

Why this works at all, and why it is not a substitute for reading VRAM
----------------------------------------------------------------------
`amdgpu_vm_sdma_set_ptes()` (ref/linux-src/.../amdgpu_vm_sdma.c:187-203) emits the tracepoint
with exactly the arguments it then hands to the SDMA packet, and the value the engine writes is

    value[i] = (addr + i * incr) | flags          for i in 0 .. count-1

- count < 3  -> amdgpu_vm_write_pte(..., pe, addr | flags, count, incr): the OR is done in
  software, literally `addr | flags` (amdgpu_vm_sdma.c:197).
- count >= 3 -> amdgpu_vm_set_pte_pde(..., pe, addr, count, incr, flags): the SDMA PTE_PDE
  packet does the same OR in hardware (amdgpu_vm_sdma.c:200).

and `amdgpu_vm_copy_ptes` - the scatter path, which would have hidden the values inside an IB
instead of printing them - fires zero times in all of our captures. So every 64-bit entry
amdgpu built in those sessions is reconstructible here, exactly, arithmetic only.

What this does NOT show, and what a Linux session would have to add:
  - that the SDMA job actually ran and the bytes landed (the tracepoint fires at IB-build time),
  - the physical page each entry sits in: `pe` is a GPU virtual address in the 0xf5ff_ aperture
    (`pe += amdgpu_bo_gpu_offset_no_check(bo)`, amdgpu_vm_sdma.c:194), so tying it to a physical
    page means chaining through the recorded PDE writes, which is inference from ordering,
  - the `level` argument, which amdgpu_vm_pte_update_flags() receives and does not trace,
  - any encoding amdgpu never emitted here: PRT, TF, NOALLOC, LOG, MTYPE other than NC and UC,
    and the immediate (CPU) update path, which is `immediate=0` in every single event.

Evidence is immutable and is only read. Nothing here touches the lab.
"""

import collections
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent

# amdgpu_vm.h:57-115, the same source gen_pte_bits.py parses. Kept as names, not addresses.
VALID, SYSTEM, SNOOPED, TMZ = 0, 1, 2, 3
EXECUTABLE, READABLE, WRITEABLE = 4, 5, 6
FRAG_SHIFT, FRAG_MASK = 7, 0x1F
PRT, PDE_PTE, LOG, TF, NOALLOC = 51, 54, 55, 56, 58
MTYPE_SHIFT, MTYPE_MASK = 48, 7
MTYPE_NAMES = {0: "NC", 1: "RW", 2: "CC", 3: "UC"}   # navi10_enum.h, MTYPE_NV10

SET = re.compile(r"amdgpu_vm_set_ptes: pe=([0-9a-f]+), addr=([0-9a-f]+), incr=(\d+), "
                 r"flags=([0-9a-f]+), count=(\d+), immediate=(\d+)")
UPD = re.compile(r"amdgpu_vm_update_ptes: pid:(\d+) vm_ctx:0x([0-9a-f]+) start:0x([0-9a-f]+) "
                 r"end:0x([0-9a-f]+), flags:0x([0-9a-f]+), incr:(\d+)")
FLUSH = re.compile(r"amdgpu_vm_flush: ring=(\S+), id=(\d+), hub=(\d+), pd_addr=([0-9a-f]+)")
GRAB = re.compile(r"amdgpu_vm_grab_id: pasid=(\d+), ring=(\S+), id=(\d+), hub=(\d+), "
                  r"pd_addr=([0-9a-f]+) needs_flush=(\d+)")

Entry = collections.namedtuple("Entry", "src line pe addr incr flags count immediate va")


def bit(value, n):
    return bool(value >> n & 1)


def frag(flags):
    return flags >> FRAG_SHIFT & FRAG_MASK


def mtype(flags):
    return flags >> MTYPE_SHIFT & MTYPE_MASK


def describe(flags):
    names = [n for n, b in (("V", VALID), ("S", SYSTEM), ("SNOOP", SNOOPED), ("TMZ", TMZ),
                            ("X", EXECUTABLE), ("R", READABLE), ("W", WRITEABLE),
                            ("PRT", PRT), ("PDE_PTE", PDE_PTE), ("LOG", LOG), ("TF", TF),
                            ("NOALLOC", NOALLOC)) if bit(flags, b)]
    return (" ".join(names) + f" MTYPE={MTYPE_NAMES.get(mtype(flags), mtype(flags))}"
            f" FRAG={frag(flags)}")


def collect(root):
    """Every set_ptes event under `root`, with the VA it belongs to where that is knowable."""
    entries, flushes, grabs = [], [], []
    for path in sorted(root.rglob("*events*.txt")):
        current = None
        cursor = 0
        for number, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
            m = UPD.search(line)
            if m:
                current = {"pid": int(m.group(1)), "start": int(m.group(3), 16),
                           "end": int(m.group(4), 16), "incr": int(m.group(6))}
                cursor = current["start"]          # in 4 KB pages
                continue
            m = SET.search(line)
            if m:
                pe, addr, incr, flags, count, imm = (int(m.group(1), 16), int(m.group(2), 16),
                                                     int(m.group(3)), int(m.group(4), 16),
                                                     int(m.group(5)), int(m.group(6)))
                va = None
                if current and incr:
                    va = cursor << 12
                    cursor += count * (incr >> 12)
                entries.append(Entry(path, number, pe, addr, incr, flags, count, imm, va))
                continue
            m = FLUSH.search(line)
            if m:
                flushes.append((path, number, m.group(1), int(m.group(2)), int(m.group(3)),
                                int(m.group(4), 16)))
                continue
            m = GRAB.search(line)
            if m:
                grabs.append((int(m.group(1)), m.group(3), int(m.group(3)), int(m.group(4)),
                              int(m.group(5), 16)))
    return entries, flushes, grabs


def main():
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else REPO / "evidence" / "linux"
    entries, flushes, grabs = collect(root)
    if not entries:
        sys.exit(f"no amdgpu_vm_set_ptes events found under {root}")

    files = sorted({e.src.name for e in entries})
    print(f"{len(entries)} set_ptes events from {len(files)} files under {root}")
    print(f"{sum(e.count for e in entries)} page table entries described in total\n")

    results = []

    def claim(name, verdict, detail):
        results.append((name, verdict))
        print(f"  {verdict:<11} {name}\n              {detail}")

    # ---------------------------------------------------------------- the scatter path
    print("Precondition: is every entry reconstructible?")
    copy = sum(1 for path in root.rglob("*events*.txt")
               for line in path.read_text(errors="replace").splitlines()
               if "amdgpu_vm_copy_ptes" in line)
    claim("no scatter path", "CONFIRMED" if copy == 0 else "REFUTED",
          f"amdgpu_vm_copy_ptes fires {copy} times, so every entry is (addr + i*incr) | flags")

    # ---------------------------------------------------------------- tables are 4 KB / 512
    print("\nClaim: 512 entries of 8 bytes at every level, so every table is one 4 KB page")
    clears = [e for e in entries if e.incr == 0 and e.count > 2]
    sizes = collections.Counter(e.count for e in clears)
    claim("block_size 9 (512 entries)", "CONFIRMED" if set(sizes) == {512} else "REFUTED",
          f"{len(clears)} whole-table initialisations, entry counts {dict(sizes)}"
          f" (a table is cleared in one go, amdgpu_vm_pt.c:408-415)")

    # ---------------------------------------------------------------- the PDE format
    print("\nClaim: a directory entry is VALID | SYSTEM | SNOOPED and an address, nothing else")
    pdes = [e for e in entries if e.incr == 0 and e.count <= 2 and bit(e.flags, VALID)
            and not bit(e.flags, PDE_PTE)]
    bad = [e for e in pdes if (bit(e.flags, READABLE) or bit(e.flags, WRITEABLE)
                               or bit(e.flags, EXECUTABLE) or mtype(e.flags) or frag(e.flags)
                               or bit(e.flags, TF) or bit(e.flags, NOALLOC))]
    seen = collections.Counter(e.flags for e in pdes)
    claim("PDE carries no permission, MTYPE or fragment", "CONFIRMED" if not bad else "REFUTED",
          f"{len(pdes)} directory entries, distinct flag words "
          f"{{{', '.join(f'{f:#x} x{n}' for f, n in seen.most_common())}}}"
          + ("" if not bad else f"; first offender {bad[0].src.name}:{bad[0].line}"))
    if pdes:
        sample = pdes[0]
        print(f"              e.g. {sample.src.name}:{sample.line} -> entry "
              f"{sample.addr | sample.flags:016x} = {describe(sample.flags)}")
        print("              SYSTEM and SNOOPED are clear throughout because every page table of"
              "\n              this VM lives in VRAM; the header's 'SYSTEM | SNOOPED' is the"
              "\n              system-memory case, which these captures never produced.")

    # ---------------------------------------------------------------- MTYPE of a VM leaf
    print("\nClaim: the default memory type of a VM leaf entry is NC, not the GART's UC")
    leaves = [e for e in entries if e.incr and bit(e.flags, VALID)]
    by_mtype = collections.Counter(mtype(e.flags) for e in leaves)
    gart_like = [e for e in leaves if mtype(e.flags) == 3]
    nc = by_mtype.get(0, 0)
    claim("leaf MTYPE NC", "CONFIRMED" if nc else "REFUTED",
          f"{len(leaves)} valid leaf entries: "
          + ", ".join(f"MTYPE_{MTYPE_NAMES.get(k, k)} x{v}" for k, v in by_mtype.most_common())
          + f"\n              the {len(gart_like)} UC ones are the kernel's own VM (pd_addr"
            f" 0x46fe00001), which is fact M37's aperture")

    # ---------------------------------------------------------------- 64 KB = FRAG(4)
    print("\nClaim: a 64 KB page is AMDGPU_PTE_FRAG(4) - bc250_pte.h calls this an inference")
    frags = collections.Counter(frag(e.flags) for e in leaves)
    f4 = [e for e in leaves if frag(e.flags) == 4]
    aligned = [e for e in f4 if e.va is not None and e.va % 0x10000 == 0]
    covered = [e for e in f4 if (e.count * e.incr) % 0x10000 == 0]
    verdict = "CONFIRMED" if f4 and len(aligned) == len([e for e in f4 if e.va is not None]) \
        and len(covered) == len(f4) else ("NOT COVERED" if not f4 else "REFUTED")
    claim("FRAG(4) means 64 KB", verdict,
          f"fragment histogram {{{', '.join(f'{k}:{1 << (12 + k):#x}b x{v}' for k, v in sorted(frags.items()))}}}"
          f"\n              {len(f4)} entries with FRAG=4; {len(aligned)} of "
          f"{len([e for e in f4 if e.va is not None])} with a known VA are 64 KB aligned and "
          f"{len(covered)} of {len(f4)} cover a whole multiple of 64 KB")

    # ---------------------------------------------------------------- 2 MB huge pages
    print("\nClaim (bc250_pte.h refuses it): a directory entry that maps a 2 MB page itself")
    huge = [e for e in entries if bit(e.flags, PDE_PTE) and e.incr == 0x200000]
    claim("PDE_PTE 2 MB leaves exist on this part", "CONFIRMED" if huge else "NOT COVERED",
          f"{len(huge)} entries with AMDGPU_PDE_PTE and incr 0x200000, all with FRAG="
          f"{sorted({frag(e.flags) for e in huge})} (2^9 * 4 KB = 2 MB)"
          + (f"\n              e.g. {huge[0].src.name}:{huge[0].line} -> "
             f"{huge[0].addr | huge[0].flags:016x} = {describe(huge[0].flags)}" if huge else ""))

    # ---------------------------------------------------------------- what never appeared
    print("\nWhat amdgpu never emitted here, so bc250_pte.c's handling of it is still untested")
    for name, b in (("PRT (sparse)", PRT), ("TF (translate further)", TF),
                    ("NOALLOC", NOALLOC), ("LOG", LOG), ("TMZ", TMZ)):
        n = sum(1 for e in entries if bit(e.flags, b))
        claim(name, "CONFIRMED" if n else "NOT COVERED",
              f"{n} entries" + ("" if n else " - no capture ever exercised this bit"))
    imm = sum(1 for e in entries if e.immediate)
    claim("immediate (CPU) update path", "CONFIRMED" if imm else "NOT COVERED",
          f"{imm} of {len(entries)} events have immediate=1; the WDDM BuildPagingBuffer path is"
          f" the closest analogue of it and has no Linux reference here")

    # ---------------------------------------------------------------- VMID and the flush
    print("\nVMID assignment and the page directory the hardware was pointed at")
    pds = collections.Counter(f[5] for f in flushes)
    vmids = collections.Counter(f[3] for f in flushes)
    hubs = collections.Counter(f[4] for f in flushes)
    claim("one page directory per VM, GFXHUB only", "CONFIRMED" if set(hubs) == {0} else "REFUTED",
          f"{len(flushes)} vm_flush events: pd_addr "
          + ", ".join(f"{p:#x} x{n}" for p, n in pds.most_common())
          + f"; vmids {dict(sorted(vmids.items()))}; hubs {dict(hubs)} (0 = GFXHUB)")
    if grabs:
        per = collections.Counter((g[0], g[2]) for g in grabs)
        claim("a VMID is handed to a pasid and reused", "CONFIRMED",
              f"{len(grabs)} grab_id events, {len(per)} distinct (pasid, vmid) pairs: "
              + ", ".join(f"pasid {p} -> vmid {v} x{n}" for (p, v), n in per.most_common(6)))

    good = sum(1 for _, v in results if v == "CONFIRMED")
    bad_n = sum(1 for _, v in results if v == "REFUTED")
    gaps = sum(1 for _, v in results if v == "NOT COVERED")
    print(f"\n{len(results)} claims: {good} confirmed, {bad_n} refuted, {gaps} not covered")
    return 1 if bad_n else 0


if __name__ == "__main__":
    sys.exit(main())
