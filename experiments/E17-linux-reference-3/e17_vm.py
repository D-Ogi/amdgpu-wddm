#!/usr/bin/env python3
"""Map a set of buffers at known GPU virtual addresses with known flags, and hold them.

    python3 -u e17_vm.py --hold 900            map, print the table, sleep, unmap on exit
    python3 -u e17_vm.py --once                map, print the table, unmap at once
    python3 -u e17_vm.py --dry                 print the planned layout, open nothing

This is the "known BO" the page-table half of E17 needs: driver/shim/bc250_pte.c has to produce the
entries amdgpu produces, and a comparison needs entries whose address, size, aperture and requested
flags we chose ourselves. RADV's own buffers are a second, messier witness; these are the clean one.

NOTHING IS SUBMITTED. No context is created, no IB is built, no doorbell is rung. The only ioctls
are GEM_CREATE, GEM_MMAP, GEM_VA (MAP and UNMAP) and GEM_CLOSE, which is what any client does when
it allocates memory, plus the INFO query that asks the kernel for its own VA range. A VMID is
therefore never assigned to this process: amdgpu hands one out in amdgpu_vmid_grab() at submission
time, so the VM context registers keep belonging to whoever ran last. That is deliberate - the
process that shows a live VMID in E17 is RADV in the `radv` phase, which is an ordinary Vulkan
client, and this one stays on the side of the fence where nothing can hang.

The ioctl numbers, the struct layouts and every AMDGPU_* constant come from E13's dispatch.json,
which gen_dispatch.py built on the PC out of the kernel UAPI header; this file imports E13's
dispatch.py rather than repeating any of it (repo rule 7: import, do not retype).

Each buffer's first 32-bit word is written through the CPU mapping with a value that names it. That
word is the positive control for the whole page-table walk: pt_walk.py --check follows the VA to a
physical address and reads it back through amdgpu_vram or amdgpu_iomem, and if what comes out is
this word, the walk is right.
"""

import argparse
import json
import mmap
import os
import struct
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
E13 = HERE.parent / "E13-linux-reference-2"
sys.path.insert(0, str(E13))
import dispatch                                                         # noqa: E402

PAGE = 4096
K64 = 64 * 1024
M2 = 2 * 1024 * 1024
GB = 1024 * 1024 * 1024

# label, offset from the base VA, size, domain, VM page flags, the word written at byte 0.
#
# The layout is chosen so that one dump of one page table answers several questions at once:
#
#   rw / ro / nx / noalloc sit in four consecutive 4 KB pages, so their four entries are adjacent
#     dwords in the same PTB page and the only difference between them is the flag under test;
#   k64 is a 64 KB buffer at a 64 KB aligned address, which is where AMDGPU_PTE_FRAG(4) would
#     appear if amdgpu uses a fragment there - the correspondence bc250_pte.h calls an inference;
#   m2 is 2 MB at a 2 MB aligned address, the size at which a PDB0 entry could become a huge page
#     (AMDGPU_PDE_PTE) instead of pointing at a PTB;
#   vram4k and vram2m are the same two questions in local memory, where the entry carries a
#     physical VRAM address rather than a bus address and SYSTEM is clear;
#   far1g and far512g force a second PDB0 and a second PDB1/PDB2 entry, which is the only way to
#     see how many levels this VM really uses without trusting the number amdgpu prints.
#
# The last two rows are the reason this experiment still exists after the inventory
# (scratch/tmp/e17_inventory.md). Everything above them is already answered offline by
# offline/check_pte_from_trace.py, which reconstructs 33348 entries out of the traces we have:
# four levels, block_size 9, the PDE format, MTYPE NC, FRAG(4) for 64 KB and the 2 MB huge page
# all came back CONFIRMED without hardware. What did NOT, in 600 events across five sessions, is
# any entry with PRT or NOALLOC set - RADV never asks for one in the workloads we run. Those two
# are exactly what WDDM will ask bc250_pte.c for (ReserveGpuVirtualAddress becomes a sparse
# mapping, and VidMm sets NoAllocate on entries it does not want cached), so they are the only
# page table question left that hardware alone can answer.
# PRT rows carry AMDGPU_VM_PAGE_PRT alone: amdgpu_gem_va_ioctl admits only DELAY_UPDATE|PRT for a
# sparse mapping and returns EINVAL for PRT with R/W (first run on unit A, 2026-10-06).
PLAN = [
    ("rw",      0 * PAGE,        PAGE, "GTT",  ("R", "W", "X"), 0xE17B0001),
    ("ro",      1 * PAGE,        PAGE, "GTT",  ("R",),          0xE17B0002),
    ("nx",      2 * PAGE,        PAGE, "GTT",  ("R", "W"),      0xE17B0003),
    ("noalloc", 3 * PAGE,        PAGE, "GTT",  ("R", "W", "N"), 0xE17B0004),
    ("k64",     K64,             K64,  "GTT",  ("R", "W"),      0xE17B0005),
    ("m2",      M2,              M2,   "GTT",  ("R", "W"),      0xE17B0006),
    ("vram4k",  4 * M2,          PAGE, "VRAM", ("R", "W"),      0xE17B0007),
    ("vram2m",  6 * M2,          M2,   "VRAM", ("R", "W"),      0xE17B0008),
    ("far1g",   1 * GB,          PAGE, "GTT",  ("R", "W"),      0xE17B0009),
    ("far512g", 512 * GB,        PAGE, "GTT",  ("R", "W"),      0xE17B000A),
    ("prt4k",   8 * M2,          PAGE, "PRT",  ("P",),          0),
    ("prt2m",   10 * M2,         M2,   "PRT",  ("P",),          0),
]

FLAG_NAMES = {"R": "AMDGPU_VM_PAGE_READABLE", "W": "AMDGPU_VM_PAGE_WRITEABLE",
              "X": "AMDGPU_VM_PAGE_EXECUTABLE", "N": "AMDGPU_VM_PAGE_NOALLOC",
              "P": "AMDGPU_VM_PAGE_PRT"}


class Mapper:
    """GEM_CREATE + GEM_MMAP + GEM_VA MAP, undone in reverse on close()."""

    def __init__(self, cfg, fd):
        self.cfg, self.fd, self.made = cfg, fd, []

    def c(self, name):
        return self.cfg["const"][name]

    def t(self, name):
        return self.cfg["types"][name]

    def info(self, query, size):
        import ctypes
        reply = ctypes.create_string_buffer(size)
        blob = dispatch.Blob(self.t("drm_amdgpu_info"))
        blob.set("return_pointer", ctypes.addressof(reply)).set("return_size", size).set("query", query)
        dispatch.ioctl(self.fd, self.cfg["ioctl"]["INFO"], blob.buf, f"INFO query {query:#x}")
        return reply.raw

    def create(self, label, size, domain, cpu_access):
        create = dispatch.Blob(self.t("drm_amdgpu_gem_create"))
        flags = self.c("AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED") if cpu_access else 0
        create.set("in.bo_size", size).set("in.alignment", PAGE)
        create.set("in.domains", self.c(f"AMDGPU_GEM_DOMAIN_{domain}")).set("in.domain_flags", flags)
        dispatch.ioctl(self.fd, self.cfg["ioctl"]["GEM_CREATE"], create.buf, f"GEM_CREATE {label}")
        return create.get("out.handle")

    def mmap(self, label, handle, size):
        gm = dispatch.Blob(self.t("drm_amdgpu_gem_mmap")).set("in.handle", handle)
        dispatch.ioctl(self.fd, self.cfg["ioctl"]["GEM_MMAP"], gm.buf, f"GEM_MMAP {label}")
        return mmap.mmap(self.fd, size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE,
                         offset=gm.get("out.addr_ptr"))

    def va(self, label, handle, va, size, flags, operation):
        blob = dispatch.Blob(self.t("drm_amdgpu_gem_va"))
        blob.set("handle", handle).set("operation", self.c(f"AMDGPU_VA_OP_{operation}"))
        blob.set("flags", flags).set("va_address", va).set("offset_in_bo", 0).set("map_size", size)
        dispatch.ioctl(self.fd, self.cfg["ioctl"]["GEM_VA"], blob.buf, f"GEM_VA {operation} {label}")

    def add(self, label, va, size, domain, letters, word):
        flags = 0
        for letter in letters:
            flags |= self.c(FLAG_NAMES[letter])
        if domain == "PRT":
            # A sparse mapping has no buffer behind it: handle 0 and AMDGPU_VM_PAGE_PRT, which is
            # what amdgpu_gem_va_ioctl() accepts as "reserved, backed by nothing"
            # (amdgpu_gem.c, the AMDGPU_VM_PAGE_PRT arm). There is nothing to mmap and no word to
            # write, so this row has no positive control of its own - its control is that the
            # four ordinary rows above it still read back correctly from the same page table.
            self.va(label, 0, va, size, flags, "MAP")
            self.made.append({"label": label, "handle": 0, "va": va, "size": size,
                              "domain": domain, "flags": flags,
                              "flag_names": [FLAG_NAMES[c] for c in letters], "word": None,
                              "mapping": None})
            print(f"{label:<8} va {va:#018x} {size:>8} bytes PRT  flags {flags:#06x} "
                  f"({'|'.join(letters)}) no buffer")
            return
        handle = self.create(label, size, domain, cpu_access=True)
        mapping = self.mmap(label, handle, size)
        self.va(label, handle, va, size, flags, "MAP")
        mapping[0:4] = struct.pack("<I", word)
        self.made.append({"label": label, "handle": handle, "va": va, "size": size,
                          "domain": domain, "flags": flags,
                          "flag_names": [FLAG_NAMES[c] for c in letters], "word": word,
                          "mapping": mapping})
        print(f"{label:<8} va {va:#018x} {size:>8} bytes {domain:<4} flags {flags:#06x} "
              f"({'|'.join(letters)}) handle {handle:<4} word {word:#010x}")

    def close(self):
        for bo in reversed(self.made):
            if bo["mapping"] is not None:
                try:
                    bo["mapping"].close()
                except (BufferError, ValueError) as e:
                    print(f"note: munmap of {bo['label']} deferred ({e})")
            try:
                self.va(bo["label"], bo["handle"], bo["va"], bo["size"],
                        bo["flags"] if bo["domain"] == "PRT" else 0, "UNMAP")
                if bo["handle"]:
                    close = dispatch.Blob(self.t("drm_gem_close")).set("handle", bo["handle"])
                    dispatch.ioctl(self.fd, self.cfg["ioctl"]["GEM_CLOSE"], close.buf, "GEM_CLOSE")
            except dispatch.IoctlError as e:
                print(f"note: cleanup {e}")
        self.made = []


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--json", default=str(E13 / "dispatch.json"))
    p.add_argument("--node", default="/dev/dri/renderD128")
    p.add_argument("--va-base", default="0x200000000")
    p.add_argument("--hold", type=float, default=0.0, help="seconds to keep the mappings alive")
    p.add_argument("--once", action="store_true", help="map and unmap without holding")
    p.add_argument("--dry", action="store_true", help="print the plan, open nothing")
    p.add_argument("--out", default="/tmp/e17/hold", help="where bos.json is written")
    args = p.parse_args()

    base = int(args.va_base, 0)
    print(f"# pid {os.getpid()}, va base {base:#x}, node {args.node}")
    for label, offset, size, domain, letters, word in PLAN:
        print(f"# plan {label:<8} {base + offset:#018x} {size:>8} {domain:<4} {'|'.join(letters)}")
    if args.dry:
        print("# --dry: nothing was opened")
        return 0

    cfg = json.load(open(args.json))
    fd = os.open(args.node, os.O_RDWR)
    mapper = Mapper(cfg, fd)
    rc = 0
    try:
        layout = cfg["types"]["drm_amdgpu_info_device"]
        raw = mapper.info(cfg["const"]["AMDGPU_INFO_DEV_INFO"], layout["size"])
        blob = dispatch.Blob(layout)
        blob.buf = bytearray(raw)
        va_lo = blob.get("virtual_address_offset")
        va_hi = blob.get("virtual_address_max")
        va_align = blob.get("virtual_address_alignment")
        print(f"# kernel VA range {va_lo:#x}..{va_hi:#x}, alignment {va_align}")
        top = base + max(offset + size for _, offset, size, _, _, _ in PLAN)
        if base < va_lo or top > va_hi:
            raise SystemExit(f"the layout {base:#x}..{top:#x} is outside the kernel's range")
        if base % max(va_align, M2):
            raise SystemExit(f"--va-base {base:#x} is not 2 MB aligned")

        for label, offset, size, domain, letters, word in PLAN:
            mapper.add(label, base + offset, size, domain, letters, word)

        os.makedirs(args.out, exist_ok=True)
        record = {"pid": os.getpid(), "va_base": base, "node": args.node,
                  "bos": [{k: v for k, v in bo.items() if k != "mapping"} for bo in mapper.made]}
        Path(args.out, "bos.json").write_text(json.dumps(record, indent=1), encoding="utf-8")
        Path(args.out, "holder.pid").write_text(f"{os.getpid()}\n", encoding="utf-8")
        # The same table as one line per buffer, for the shell: `label va size domain flags word`,
        # every number in hex with no 0x, so that a POSIX sh can cut a column out of it without a
        # JSON parser. session.sh builds pt_walk.py's argument list from this.
        Path(args.out, "bos.txt").write_text(
            # A PRT row has no buffer and so no witness word; it is written as `-`, which
            # session.sh's bos_checks() skips and pt_walk.py --check never receives.
            "".join(f"{bo['label']} {bo['va']:x} {bo['size']:x} {bo['domain']} "
                    f"{bo['flags']:x} {'-' if bo['word'] is None else format(bo['word'], 'x')}\n"
                    for bo in mapper.made), encoding="utf-8")
        print(f"# {len(mapper.made)} buffers mapped, described in {args.out}/bos.json and bos.txt")
        print(f"# client directory: grep {os.getpid()} /sys/kernel/debug/dri/client-*/proc_info")

        if args.once:
            print("# --once: unmapping at once")
        else:
            print(f"# holding for {args.hold} s; the other phases can read this VM now")
            sys.stdout.flush()
            time.sleep(args.hold)
    except dispatch.IoctlError as e:
        print(f"FAIL: {e}")
        rc = 2
    except SystemExit as e:
        print(f"FAIL: {e}")
        rc = 2
    finally:
        mapper.close()
        os.close(fd)
    print(f"# done, rc {rc}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
