#!/usr/bin/env python3
"""Run libdrm's gfx10 memset compute dispatch on this GPU through raw amdgpu ioctls, as the Linux reference for
milestone M6 ("a compute queue runs a dispatch that writes a known pattern to memory").

python3 stdlib only. No libdrm, no compiler, no pip. Everything the run needs - ioctl numbers, struct field
offsets, the 9 shader dwords, the PM4 packet stream, the register offsets - comes out of dispatch.json, which
gen_dispatch.py generates on the PC from the kernel UAPI headers, the libdrm-2.4.114 test source and tools/regcalc.
Nothing here is typed from memory or from a datasheet.

    python3 -u dispatch.py --dry                 build and print the IB, do not open the device
    python3 -u dispatch.py --groups 1            one workgroup, 1 KiB written        (do this first)
    python3 -u dispatch.py --groups 16           16 workgroups, 16 KiB written       (libdrm's own size)

WHAT THIS DOES TO THE GPU, AND WHAT HAPPENS IF IT GOES WRONG
------------------------------------------------------------
It allocates three GTT buffers, maps them into this process's GPU virtual address space, writes a 72-dword
command buffer, and submits it ONCE as an indirect buffer on AMDGPU_HW_IP_COMPUTE ring 0 through the normal
DRM_IOCTL_AMDGPU_CS path. It is an ordinary user-mode submission: the same thing any OpenCL or Vulkan client
does. It does not touch MMIO, does not load firmware, does not reset anything.

MEDIUM RISK. If the shader hangs or faults, amdgpu's job timeout fires (default 10000 ms for compute,
drivers/gpu/drm/amd/amdgpu/amdgpu_device.c:4294) and the driver starts GPU recovery. On this SoC amdgpu's
recovery path is untested by us (wishlist L6), and in the E13 session one module operation already hung the
machine hard enough to need the owner. Assume a failed run can cost a power cycle. Mitigations built in:
one submission, no retries, no loop, a 5 s wait that gives up before the driver's own timeout, and --groups 1
as the first thing to try.

Exit codes: 0 pattern verified, 1 pattern mismatch, 2 an ioctl failed, 3 the wait timed out or came back busy.

WHERE THIS DIFFERS FROM THE SHIM'S DIRECT-RING VERSION
-------------------------------------------------------
The 72 dwords are identical. What wraps them is not, and the difference is forced by the UAPI:

  - Cache flush. On a compute ring amdgpu_cs_p2_ib() calls amdgpu_ib_get() with size 0 (compute ring funcs have
    no .parse_cs, amdgpu_cs.c:389-391) and then overwrites ib->flags from the chunk (amdgpu_cs.c:399). So the
    EMIT_MEM_SYNC that amdgpu_ib_get() would otherwise set (amdgpu_ib.c:80) never reaches a user submission:
    the ACQUIRE_MEM at amdgpu_ib.c:211-212 is emitted only if WE ask for it. libdrm's test asks for nothing,
    so by default neither do we. Pass --mem-sync to make the kernel prepend exactly the ACQUIRE_MEM the shim
    emits by hand, and the two ring streams line up.
  - Everything else around the IB (INDIRECT_BUFFER, VM flush, pipeline sync, the RELEASE_MEM fence, the pad to
    256 dwords) is the kernel's, not ours, and will show up in the ring but not in this IB.
  - VMID. The addresses here are per-process GPU virtual addresses in a real VM, not VMID 0 GART addresses.
    COMPUTE_PGM_LO still takes the address shifted right by 8 and COMPUTE_USER_DATA_0/1 still take it raw, so
    the packet arithmetic is the same; only the numbers differ.
"""

import argparse
import ctypes
import json
import mmap
import os
import struct
import sys
import time

try:
    import fcntl                        # absent on the PC, where only --dry is ever run
except ImportError:
    fcntl = None

DEFAULT_SENTINEL = 0xCAFEDEAD       # what the destination holds before the dispatch, so that "unchanged" and
                                    # "wrong value" are distinguishable. Same sentinel gfx_v10_0_ring_test_ib()
                                    # uses (gfx_v10_0.c:4088). Not a register or a hardware constant.
VA_SPACING = 1 << 20
PAGE = 4096


# ---------------------------------------------------------------- ioctl plumbing

class IoctlError(Exception):
    pass


def ioctl(fd, request, buf, what):
    """fcntl.ioctl with the request number accepted whether or not this CPython build takes it unsigned."""
    if fcntl is None:
        raise SystemExit("no fcntl module: this host cannot talk to the device, use --dry")
    signed = request - (1 << 32) if request >= (1 << 31) else request
    for op in (request, signed):
        try:
            fcntl.ioctl(fd, op, buf, True)
            return buf
        except OverflowError:
            continue                    # this CPython build wants the request number signed
        except OSError as e:
            raise IoctlError(f"{what}: errno {e.errno} ({e.strerror})") from None
    raise IoctlError(f"{what}: request 0x{request:08x} rejected by fcntl.ioctl")


class Blob:
    """A struct argument, packed and unpacked by the field table gen_dispatch.py computed from the header."""

    def __init__(self, layout):
        self.layout = layout
        self.buf = bytearray(layout["size"])

    def set(self, path, value, index=0):
        f = self.layout["fields"][path]
        struct.pack_into("<" + f["fmt"], self.buf, f["offset"] + index * (f["size"] // f["count"]), value)
        return self

    def get(self, path, index=0):
        f = self.layout["fields"][path]
        return struct.unpack_from("<" + f["fmt"], self.buf, f["offset"] + index * (f["size"] // f["count"]))[0]


# ---------------------------------------------------------------- IB construction

def resolve(value, ctx):
    """Turn one entry of a dispatch.json packet into a dword. Ints pass through; the placeholders reproduce the
    arithmetic of the libdrm expression they were parsed from, using the literals carried in "args"."""
    if isinstance(value, int):
        return value & 0xFFFFFFFF
    sym, a = value["sym"], value["args"]
    if sym == "SHADER_ADDR_SHR":
        return (ctx["shader_va"] >> a[0]) & 0xFFFFFFFF
    if sym == "DST_ADDR":
        return ctx["dst_va"] & 0xFFFFFFFF
    if sym == "DST_ADDR_SHR_OR":
        return ((ctx["dst_va"] >> a[0]) | a[1]) & 0xFFFFFFFF
    if sym == "DST_NUM_RECORDS":
        return (ctx["dst_size"] // a[0]) & 0xFFFFFFFF
    if sym == "GROUPS_X":
        return ((ctx["dst_size"] // a[0] + a[1] - 1) // a[2]) & 0xFFFFFFFF
    raise KeyError(f"unknown placeholder {sym} (dispatch.json is newer than dispatch.py)")


def find_arg(cfg, sym, index):
    """The literal libdrm used at position `index` of the first `sym` placeholder in the packet stream."""
    for packet in cfg["packets"]:
        for value in packet["values"]:
            if isinstance(value, dict) and value["sym"] == sym:
                return value["args"][index]
    raise KeyError(sym)


def reg_value(cfg, name):
    """The value the packet stream writes to a named register."""
    for packet in cfg["packets"]:
        for reg, value in zip(packet.get("regs") or [], packet["values"]):
            if reg == name:
                return value
    raise KeyError(name)


def geometry(cfg, groups):
    """Destination size and the fill pattern, all derived from the packet stream rather than assumed.

    libdrm computes the workgroup count from the destination size as (size/16 + 0x40 - 1) / 0x40, where 16 is the
    V# record size and 0x40 is COMPUTE_NUM_THREAD_X. We go the other way: pick the size that makes that formula
    produce exactly the requested number of workgroups, with every thread writing one whole record."""
    record = find_arg(cfg, "DST_NUM_RECORDS", 0)
    per_group = find_arg(cfg, "GROUPS_X", 2)
    threads_x = reg_value(cfg, "mmCOMPUTE_NUM_THREAD_X")
    if per_group != threads_x:
        raise SystemExit(f"dispatch.json is inconsistent: GROUPS_X divides by {per_group} but "
                         f"mmCOMPUTE_NUM_THREAD_X is {threads_x}")
    fill = [reg_value(cfg, f"mmCOMPUTE_USER_DATA_{n}") for n in (4, 5, 6, 7)]
    if any(not isinstance(v, int) for v in fill) or len(set(fill)) != 1:
        raise SystemExit(f"the fill constant is not four equal dwords: {fill}")
    return groups * per_group * record, record, per_group, fill[0]


def build_ib(cfg, ctx):
    """The command buffer, as (dwords, annotation lines)."""
    dwords, notes = [], []
    for packet in cfg["packets"]:
        base = len(dwords)
        dwords.append(packet["header"])
        notes.append((base, packet["header"],
                      f"{packet['op_name']} count={packet['count']} shader_type={packet['shader_type']}"
                      f"   [{packet['cite']}]"))
        regs = packet.get("regs")
        if regs is not None:
            raw = (packet["index"] << 28) | packet["offset"]
            dwords.append(raw)
            first = regs[0] or f"dword 0x{packet['reg_base'] + packet['offset']:04x}"
            notes.append((base + 1, raw, f"  offset 0x{packet['offset']:03x}"
                                         + (f" index {packet['index']}" if packet["index"] else "")
                                         + f" -> {first}"))
        for n, value in enumerate(packet["values"]):
            word = resolve(value, ctx)
            name = (regs[n] if regs and regs[n] else None) if regs is not None else None
            label = name or (f"unnamed dword 0x{packet['reg_base'] + packet['offset'] + n:04x}" if regs is not None
                             else DISPATCH_FIELDS[n] if packet["op_name"] == "PACKET3_DISPATCH_DIRECT" and
                             n < len(DISPATCH_FIELDS) else f"data[{n}]")
            origin = "" if isinstance(value, int) else f"   <- {value['c']}"
            notes.append((len(dwords), word, f"    {label} = 0x{word:08x}{origin}"))
            dwords.append(word)
    while len(dwords) % cfg["pm4"]["pad_align_dw"]:
        notes.append((len(dwords), cfg["pm4"]["pad_word"], f"    pad [{cfg['pm4']['pad_cite']}]"))
        dwords.append(cfg["pm4"]["pad_word"])
    return dwords, notes


DISPATCH_FIELDS = ["DIM_X (workgroups)", "DIM_Y", "DIM_Z", "mmCOMPUTE_DISPATCH_INITIATOR"]


def print_ib(cfg, ctx, dwords, notes):
    print(f"# IB: {len(dwords)} dwords, {len(cfg['packets'])} packets, "
          f"{len(dwords) * 4} bytes at va 0x{ctx['cmd_va']:016x}")
    print(f"# shader va 0x{ctx['shader_va']:016x} ({cfg['shader']['dwords']} dwords, {cfg['shader']['cite']})")
    print(f"# dst    va 0x{ctx['dst_va']:016x} size {ctx['dst_size']} bytes")
    for index, word, text in notes:
        print(f"[{index:03d}] {word:08x} {text}")
    print("# raw:")
    for i in range(0, len(dwords), 8):
        print("  " + " ".join(f"{w:08x}" for w in dwords[i:i + 8]))


# ---------------------------------------------------------------- the run

class Session:
    """Everything opened against the render node, so that cleanup happens in one place."""

    def __init__(self, cfg, fd):
        self.cfg, self.fd = cfg, fd
        self.bos = []                       # (handle, va, size, mapping)
        self.ctx_id = None
        self.timed_out = False

    def t(self, name):
        return self.cfg["types"][name]

    def c(self, name):
        return self.cfg["const"][name]

    def info(self, query, size, a=0, b=0):
        reply = ctypes.create_string_buffer(size)
        blob = Blob(self.t("drm_amdgpu_info"))
        blob.set("return_pointer", ctypes.addressof(reply)).set("return_size", size).set("query", query)
        blob.set("query_hw_ip.type", a).set("query_hw_ip.ip_instance", b)
        ioctl(self.fd, self.cfg["ioctl"]["INFO"], blob.buf, f"INFO query 0x{query:x}")
        return reply.raw

    def ctx_alloc(self):
        blob = Blob(self.t("drm_amdgpu_ctx")).set("in.op", self.c("AMDGPU_CTX_OP_ALLOC_CTX"))
        ioctl(self.fd, self.cfg["ioctl"]["CTX"], blob.buf, "CTX ALLOC")
        self.ctx_id = blob.get("out.alloc.ctx_id")
        print(f"CTX  alloc                  -> ctx_id {self.ctx_id}")
        return self.ctx_id

    def bo(self, label, size, va, executable=True):
        create = Blob(self.t("drm_amdgpu_gem_create"))
        create.set("in.bo_size", size).set("in.alignment", PAGE)
        create.set("in.domains", self.c("AMDGPU_GEM_DOMAIN_GTT")).set("in.domain_flags", 0)
        ioctl(self.fd, self.cfg["ioctl"]["GEM_CREATE"], create.buf, f"GEM_CREATE {label}")
        handle = create.get("out.handle")

        gm = Blob(self.t("drm_amdgpu_gem_mmap")).set("in.handle", handle)
        ioctl(self.fd, self.cfg["ioctl"]["GEM_MMAP"], gm.buf, f"GEM_MMAP {label}")
        offset = gm.get("out.addr_ptr")
        mapping = mmap.mmap(self.fd, size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=offset)

        flags = self.c("AMDGPU_VM_PAGE_READABLE") | self.c("AMDGPU_VM_PAGE_WRITEABLE")
        if executable:
            flags |= self.c("AMDGPU_VM_PAGE_EXECUTABLE")
        va_blob = Blob(self.t("drm_amdgpu_gem_va"))
        va_blob.set("handle", handle).set("operation", self.c("AMDGPU_VA_OP_MAP")).set("flags", flags)
        va_blob.set("va_address", va).set("offset_in_bo", 0).set("map_size", size)
        ioctl(self.fd, self.cfg["ioctl"]["GEM_VA"], va_blob.buf, f"GEM_VA MAP {label}")

        self.bos.append([handle, va, size, mapping])
        print(f"BO   {label:<7} {size:>7} bytes  -> handle {handle:<4} mmap offset 0x{offset:012x} "
              f"va 0x{va:016x} flags 0x{flags:x}")
        return mapping

    def submit(self, cmd_va, ib_dwords, ring, ip_type, ib_flags=0):
        stride = self.t("drm_amdgpu_bo_list_entry")["size"]
        entries = ctypes.create_string_buffer(stride * len(self.bos))
        for n, (handle, _, _, _) in enumerate(self.bos):
            entry = Blob(self.t("drm_amdgpu_bo_list_entry")).set("bo_handle", handle).set("bo_priority", 0)
            ctypes.memmove(ctypes.addressof(entries) + n * stride, bytes(entry.buf), stride)

        bo_list = Blob(self.t("drm_amdgpu_bo_list_in"))
        bo_list.set("operation", 0).set("list_handle", 0).set("bo_number", len(self.bos))
        bo_list.set("bo_info_size", self.t("drm_amdgpu_bo_list_entry")["size"])
        bo_list.set("bo_info_ptr", ctypes.addressof(entries))
        bo_list_buf = ctypes.create_string_buffer(bytes(bo_list.buf), len(bo_list.buf))

        ib = Blob(self.t("drm_amdgpu_cs_chunk_ib"))
        ib.set("flags", ib_flags).set("va_start", cmd_va).set("ib_bytes", ib_dwords * 4)
        ib.set("ip_type", ip_type).set("ip_instance", 0).set("ring", ring)
        ib_buf = ctypes.create_string_buffer(bytes(ib.buf), len(ib.buf))

        chunks = []
        for chunk_id, payload in ((self.c("AMDGPU_CHUNK_ID_IB"), ib_buf),
                                  (self.c("AMDGPU_CHUNK_ID_BO_HANDLES"), bo_list_buf)):
            # length_dw counts the chunk payload, and the kernel rejects a chunk shorter than the struct it
            # expects (amdgpu_cs.c:250 for BO_HANDLES). create_string_buffer(init, size) adds no NUL, so
            # len(payload) is exactly the struct size.
            chunk = Blob(self.t("drm_amdgpu_cs_chunk"))
            chunk.set("chunk_id", chunk_id).set("length_dw", len(payload) // 4)
            chunk.set("chunk_data", ctypes.addressof(payload))
            chunks.append(ctypes.create_string_buffer(bytes(chunk.buf), len(chunk.buf)))

        packed = struct.pack(f"<{len(chunks)}Q", *[ctypes.addressof(c) for c in chunks])
        pointers = ctypes.create_string_buffer(packed, len(packed))
        cs = Blob(self.t("drm_amdgpu_cs"))
        cs.set("in.ctx_id", self.ctx_id).set("in.bo_list_handle", 0).set("in.num_chunks", len(chunks))
        cs.set("in.flags", 0).set("in.chunks", ctypes.addressof(pointers))

        print(f"CS   ip_type {ip_type} ring {ring} ib_bytes {ib_dwords * 4} chunks {len(chunks)} "
              f"bo_handles {len(self.bos)} ib_flags 0x{ib_flags:x}")
        print(f"--- submit at monotonic {time.monotonic():.6f} ---")
        ioctl(self.fd, self.cfg["ioctl"]["CS"], cs.buf, "CS")
        handle = cs.get("out.handle")
        print(f"CS   accepted               -> seq_no {handle}")
        return handle

    def wait(self, seq_no, ring, ip_type, seconds):
        # WAIT_CS takes an ABSOLUTE deadline in ns off the kernel's ktime_get(), which is CLOCK_MONOTONIC
        # (amdgpu_gem_timeout(), amdgpu_gem.c:647, used by amdgpu_cs_wait_ioctl at amdgpu_cs.c:1490).
        # time.monotonic_ns() is that same clock on Linux.
        deadline = time.monotonic_ns() + int(seconds * 1e9)
        blob = Blob(self.t("drm_amdgpu_wait_cs"))
        blob.set("in.handle", seq_no).set("in.timeout", deadline)
        blob.set("in.ip_type", ip_type).set("in.ip_instance", 0).set("in.ring", ring)
        blob.set("in.ctx_id", self.ctx_id)
        start = time.monotonic()
        ioctl(self.fd, self.cfg["ioctl"]["WAIT_CS"], blob.buf, "WAIT_CS")
        status = blob.get("out.status")
        print(f"--- wait returned after {time.monotonic() - start:.3f} s, status {status} "
              f"({'completed' if status == 0 else 'STILL BUSY'}) ---")
        self.timed_out = status != 0
        return status

    def close(self):
        if self.timed_out:
            # The job is still in flight. Unmapping its VAs and closing its BOs now would only add ioctls to a
            # GPU that is probably on its way into amdgpu's recovery path. Let the fd close do it instead: the
            # kernel holds its own references until the fence settles.
            print("note: the fence never signalled, so nothing is being unmapped or closed here.")
            print("note: amdgpu's compute job timeout (10 s by default) will fire next and start GPU recovery.")
            return
        for entry in reversed(self.bos):
            handle, va, size, mapping = entry
            try:
                mapping.close()
            except (BufferError, ValueError) as e:
                print(f"note: munmap of handle {handle} deferred ({e})")
            try:
                blob = Blob(self.t("drm_amdgpu_gem_va"))
                blob.set("handle", handle).set("operation", self.c("AMDGPU_VA_OP_UNMAP"))
                blob.set("va_address", va).set("map_size", size)
                ioctl(self.fd, self.cfg["ioctl"]["GEM_VA"], blob.buf, "GEM_VA UNMAP")
                close = Blob(self.t("drm_gem_close")).set("handle", handle)
                ioctl(self.fd, self.cfg["ioctl"]["GEM_CLOSE"], close.buf, "GEM_CLOSE")
            except IoctlError as e:
                print(f"note: cleanup {e}")
        if self.ctx_id is not None:
            try:
                blob = Blob(self.t("drm_amdgpu_ctx"))
                blob.set("in.op", self.c("AMDGPU_CTX_OP_FREE_CTX")).set("in.ctx_id", self.ctx_id)
                ioctl(self.fd, self.cfg["ioctl"]["CTX"], blob.buf, "CTX FREE")
                print("CTX  free                   -> ok")
            except IoctlError as e:
                print(f"note: cleanup {e}")


def describe_device(session, cfg):
    """Print what libdrm's own test would have looked at, and return (virtual_address_offset, max, alignment)."""
    dev_layout = cfg["types"]["drm_amdgpu_info_device"]
    raw = session.info(cfg["const"]["AMDGPU_INFO_DEV_INFO"], dev_layout["size"])
    dev = Blob(dev_layout)
    dev.buf = bytearray(raw)
    print(f"DEV  device_id 0x{dev.get('device_id'):04x} family {dev.get('family')} "
          f"external_rev 0x{dev.get('external_rev'):x} cu_active {dev.get('cu_active_number')} "
          f"wave_front_size {dev.get('wave_front_size')}")
    va_lo, va_hi = dev.get("virtual_address_offset"), dev.get("virtual_address_max")
    va_align = dev.get("virtual_address_alignment")
    print(f"DEV  va range 0x{va_lo:016x} .. 0x{va_hi:016x} alignment {va_align} "
          f"gart_page_size {dev.get('gart_page_size')}")

    hw_layout = cfg["types"]["drm_amdgpu_info_hw_ip"]
    raw = session.info(cfg["const"]["AMDGPU_INFO_HW_IP_INFO"], hw_layout["size"],
                       cfg["const"]["AMDGPU_HW_IP_COMPUTE"], 0)
    hw = Blob(hw_layout)
    hw.buf = bytearray(raw)
    major = hw.get("hw_ip_version_major")
    rings = hw.get("available_rings")
    print(f"HWIP COMPUTE version {major}.{hw.get('hw_ip_version_minor')} "
          f"available_rings 0x{rings:08x} ib_start_alignment {hw.get('ib_start_alignment')}")
    # shader_test_util.c:103-116 selects the gfx10 shader on hw_ip_version_major == 10 and nothing else.
    if major != 10:
        print(f"WARNING: this kernel reports GFX IP major {major}; dispatch.json holds the gfx10 shader")
    if not rings:
        raise SystemExit("no compute ring available")
    return va_lo, va_hi, va_align


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--json", default=os.path.join(here, "dispatch.json"))
    ap.add_argument("--dry", action="store_true", help="build and print the IB, do not open the device")
    ap.add_argument("--groups", type=int, default=1, help="workgroups in X (default 1; libdrm's own run is 16)")
    ap.add_argument("--node", default="/dev/dri/renderD128")
    ap.add_argument("--va-base", default="0x100000000", help="base GPU virtual address for the three buffers")
    ap.add_argument("--ring", type=int, default=0)
    ap.add_argument("--timeout", type=float, default=5.0, help="seconds to wait for the fence (default 5)")
    ap.add_argument("--sentinel", default=hex(DEFAULT_SENTINEL))
    ap.add_argument("--mem-sync", action="store_true",
                    help="set AMDGPU_IB_FLAG_EMIT_MEM_SYNC on the IB chunk, which makes the kernel prepend "
                         "gfx_v10_0_emit_mem_sync()'s ACQUIRE_MEM. libdrm's own test does NOT set it; the shim's "
                         "direct-ring version emits that packet itself, so this is the switch that makes the two "
                         "ring streams comparable.")
    args = ap.parse_args()

    cfg = json.load(open(args.json))
    if args.groups < 1:
        raise SystemExit("--groups must be at least 1")
    dst_size, record, per_group, fill = geometry(cfg, args.groups)
    sentinel = int(args.sentinel, 0) & 0xFFFFFFFF

    va_base = int(args.va_base, 0)
    ctx = {"shader_va": va_base, "cmd_va": va_base + VA_SPACING, "dst_va": va_base + 2 * VA_SPACING,
           "dst_size": dst_size}
    shader_bytes = len(cfg["shader"]["words"]) * 4
    cmd_size = PAGE
    dst_alloc = max(dst_size, PAGE)

    dwords, notes = build_ib(cfg, ctx)
    # libdrm's own workgroup formula, run forwards over the size we chose, must give back the requested count.
    check = resolve({"sym": "GROUPS_X", "args": [find_arg(cfg, "GROUPS_X", i) for i in range(3)]}, ctx)
    if check != args.groups:
        raise SystemExit(f"internal: libdrm's formula gives {check} workgroups, not {args.groups}")

    print(f"# libdrm {cfg['sources']['libdrm_tag']} commit {cfg['sources']['libdrm_commit']}")
    print(f"# dispatch.json from {cfg['generated_by']}")
    print(f"# {args.groups} workgroup(s) x {per_group} threads x {record} bytes = {dst_size} bytes, "
          f"fill 0x{fill:08x}, sentinel 0x{sentinel:08x}")
    print(f"# IB chunk flags: "
          + ("AMDGPU_IB_FLAG_EMIT_MEM_SYNC (the kernel prepends ACQUIRE_MEM, like the shim does by hand)"
             if args.mem_sync else "0, exactly as libdrm's test submits it (no ACQUIRE_MEM)"))
    for c in cfg["cites"]:
        print(f"#   {c}")
    print()
    print_ib(cfg, ctx, dwords, notes)

    if args.dry:
        print("\n# --dry: nothing was opened, nothing was submitted")
        return 0

    if dst_alloc > VA_SPACING or cmd_size > VA_SPACING or shader_bytes > VA_SPACING:
        raise SystemExit("buffers would overlap in the VA layout; raise VA_SPACING")

    print(f"\n# opening {args.node}")
    fd = os.open(args.node, os.O_RDWR)
    session = Session(cfg, fd)
    rc = 0
    try:
        va_lo, va_hi, va_align = describe_device(session, cfg)
        if va_base < va_lo or ctx["dst_va"] + dst_alloc > va_hi:
            raise SystemExit(f"--va-base 0x{va_base:x} is outside the kernel's range "
                             f"0x{va_lo:x}..0x{va_hi:x}")
        if va_base % va_align:
            raise SystemExit(f"--va-base 0x{va_base:x} is not a multiple of {va_align}")

        session.ctx_alloc()
        shader_map = session.bo("shader", PAGE, ctx["shader_va"])
        cmd_map = session.bo("cmd", cmd_size, ctx["cmd_va"])
        dst_map = session.bo("dst", dst_alloc, ctx["dst_va"], executable=False)

        shader_map[:PAGE] = b"\0" * PAGE
        shader_map[:shader_bytes] = struct.pack(f"<{len(cfg['shader']['words'])}I", *cfg["shader"]["words"])
        cmd_map[:cmd_size] = b"\0" * cmd_size
        cmd_map[:len(dwords) * 4] = struct.pack(f"<{len(dwords)}I", *dwords)
        dst_map[:dst_size] = struct.pack("<I", sentinel) * (dst_size // 4)

        before = struct.unpack(f"<{dst_size // 4}I", bytes(dst_map[:dst_size]))
        print(f"DST  before: {len(before)} dwords, all 0x{sentinel:08x}: {all(w == sentinel for w in before)}, "
              f"first four {' '.join(f'{w:08x}' for w in before[:4])}")

        ib_flags = cfg["const"]["AMDGPU_IB_FLAG_EMIT_MEM_SYNC"] if args.mem_sync else 0
        seq_no = session.submit(ctx["cmd_va"], len(dwords), args.ring,
                                cfg["const"]["AMDGPU_HW_IP_COMPUTE"], ib_flags)
        status = session.wait(seq_no, args.ring, cfg["const"]["AMDGPU_HW_IP_COMPUTE"], args.timeout)

        after = struct.unpack(f"<{dst_size // 4}I", bytes(dst_map[:dst_size]))
        good = sum(1 for w in after if w == fill)
        bad = next((i for i, w in enumerate(after) if w != fill), None)
        print(f"DST  after : {good}/{len(after)} dwords equal 0x{fill:08x}, "
              f"first four {' '.join(f'{w:08x}' for w in after[:4])}, "
              f"last four {' '.join(f'{w:08x}' for w in after[-4:])}")
        if bad is None:
            print("RESULT PASS: the whole destination holds the fill pattern")
        else:
            print(f"RESULT FAIL: first mismatch at dword {bad} (byte 0x{bad * 4:x}): "
                  f"0x{after[bad]:08x}, expected 0x{fill:08x}"
                  + ("  (still the sentinel: the dispatch wrote nothing here)"
                     if after[bad] == sentinel else ""))
            rc = 1
        if status != 0:
            print("RESULT TIMEOUT: the fence had not signalled after "
                  f"{args.timeout} s, so the values above may be mid-flight. Expect amdgpu to declare the job "
                  "hung and start GPU recovery. Watch dmesg; the machine may need a power cycle.")
            rc = 3
    except IoctlError as e:
        print(f"RESULT FAIL: {e}")
        rc = 2
    except SystemExit as e:
        print(f"RESULT FAIL: {e}")
        rc = 2
    finally:
        session.close()
        os.close(fd)
    return rc


if __name__ == "__main__":
    sys.exit(main())
