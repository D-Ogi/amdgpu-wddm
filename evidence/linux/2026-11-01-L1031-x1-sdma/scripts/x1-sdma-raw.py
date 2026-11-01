#!/usr/bin/env python3
"""X1: SDMA COPY_LINEAR at growing sizes through raw amdgpu CS ioctls (one packet per submission, the shape of
sdma_v5_0_emit_copy_buffer). Tests the community claim that stock cyan_skillfish2 SDMA microcode never completes a
copy above 16384 bytes (akandr/bc250-rocm).

PC side:  python x1-sdma-raw.py --gen <navi10_sdma_pkt_open.h> <out x1-packets.json>
Lab side: python3 x1-sdma-raw.py --ref <dir with dispatch.py + dispatch.json> --packets x1-packets.json
          [--ring 0|1] [--sizes 4096,16384,...]
One submission per size, 5 s wait, stop at the first busy or mismatch. A busy job is left to amdgpu's SDMA job
timeout, which on this kernel takes the per-queue reset path (E29, M388). Exit: 0 all pass, 1 mismatch,
2 setup error, 3 busy."""
import json
import os
import re
import struct
import sys

SIZES = [4096, 16384, 16388, 20480, 65536, 262144, 1048576, 4194304]


def gen(header, out):
    text = open(header, encoding="utf-8").read()

    def val(name):
        m = re.search(r"#define\s+" + name + r"\s+(0x[0-9A-Fa-f]+|\d+)\b", text)
        if not m:
            raise SystemExit("header: no " + name)
        return int(m.group(1), 0)

    p = "SDMA_PKT_COPY_LINEAR_"
    doc = {
        "op_copy": val("SDMA_OP_COPY"), "subop_linear": val("SDMA_SUBOP_COPY_LINEAR"), "op_nop": val("SDMA_OP_NOP"),
        "op_shift": val(p + "HEADER_op_shift"), "subop_shift": val(p + "HEADER_sub_op_shift"),
        "count_mask": val(p + "COUNT_count_mask"),
        "dw": {k: val(p + n) for k, n in (("count", "COUNT_count_offset"),
                                           ("param", "PARAMETER_dst_sw_offset"),
                                           ("src_lo", "SRC_ADDR_LO_src_addr_31_0_offset"),
                                           ("src_hi", "SRC_ADDR_HI_src_addr_63_32_offset"),
                                           ("dst_lo", "DST_ADDR_LO_dst_addr_31_0_offset"),
                                           ("dst_hi", "DST_ADDR_HI_dst_addr_63_32_offset"))},
        "cite": "navi10_sdma_pkt_open.h SDMA_PKT_COPY_LINEAR; count = bytes - 1 as sdma_v5_0_emit_copy_buffer",
    }
    with open(out, "w", newline="\n") as fh:
        json.dump(doc, fh, indent=1)
    print(json.dumps(doc))


def copy_packet(pk, src, dst, nbytes):
    words = [0] * (pk["dw"]["dst_hi"] + 1)
    words[0] = (pk["op_copy"] << pk["op_shift"]) | (pk["subop_linear"] << pk["subop_shift"])
    if nbytes - 1 > pk["count_mask"]:
        raise ValueError("size above the COUNT field")
    words[pk["dw"]["count"]] = nbytes - 1
    words[pk["dw"]["param"]] = 0
    words[pk["dw"]["src_lo"]], words[pk["dw"]["src_hi"]] = src & 0xFFFFFFFF, src >> 32
    words[pk["dw"]["dst_lo"]], words[pk["dw"]["dst_hi"]] = dst & 0xFFFFFFFF, dst >> 32
    return words


def bo_in(s, label, size, va, domain):
    """Session.bo with a chosen domain; a VRAM buffer is created CPU-visible so the CPU can fill and check it."""
    if domain == "gtt":
        return s.bo(label, size, va, executable=False)
    import mmap
    from dispatch import Blob, PAGE, ioctl
    create = Blob(s.t("drm_amdgpu_gem_create"))
    create.set("in.bo_size", size).set("in.alignment", PAGE)
    create.set("in.domains", s.c("AMDGPU_GEM_DOMAIN_VRAM"))
    create.set("in.domain_flags", s.c("AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED"))
    ioctl(s.fd, s.cfg["ioctl"]["GEM_CREATE"], create.buf, f"GEM_CREATE {label}")
    handle = create.get("out.handle")
    gm = Blob(s.t("drm_amdgpu_gem_mmap")).set("in.handle", handle)
    ioctl(s.fd, s.cfg["ioctl"]["GEM_MMAP"], gm.buf, f"GEM_MMAP {label}")
    mapping = mmap.mmap(s.fd, size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=gm.get("out.addr_ptr"))
    va_blob = Blob(s.t("drm_amdgpu_gem_va"))
    va_blob.set("handle", handle).set("operation", s.c("AMDGPU_VA_OP_MAP"))
    va_blob.set("flags", s.c("AMDGPU_VM_PAGE_READABLE") | s.c("AMDGPU_VM_PAGE_WRITEABLE"))
    va_blob.set("va_address", va).set("offset_in_bo", 0).set("map_size", size)
    ioctl(s.fd, s.cfg["ioctl"]["GEM_VA"], va_blob.buf, f"GEM_VA MAP {label}")
    s.bos.append([handle, va, size, mapping])
    print(f"BO   {label:<7} {size:>7} bytes  -> handle {handle:<4} VRAM va 0x{va:016x}")
    return mapping


def lab(args):
    ref = args[args.index("--ref") + 1]
    route = args[args.index("--route") + 1] if "--route" in args else "gtt2gtt"
    src_dom, dst_dom = route.split("2")
    pk = json.load(open(args[args.index("--packets") + 1]))
    ring = int(args[args.index("--ring") + 1]) if "--ring" in args else 0
    sizes = [int(s) for s in args[args.index("--sizes") + 1].split(",")] if "--sizes" in args else SIZES
    sys.path.insert(0, ref)
    from dispatch import Session, Blob, PAGE
    cfg = json.load(open(os.path.join(ref, "dispatch.json")))
    fd = os.open("/dev/dri/renderD128", os.O_RDWR | os.O_CLOEXEC)
    s = Session(cfg, fd)
    rc = 2
    try:
        layout = s.t("drm_amdgpu_info_device")
        dev = Blob(layout)
        dev.buf = bytearray(s.info(s.c("AMDGPU_INFO_DEV_INFO"), layout["size"]))
        ip = s.c("AMDGPU_HW_IP_DMA")
        layout = s.t("drm_amdgpu_info_hw_ip")
        hw = Blob(layout)
        hw.buf = bytearray(s.info(s.c("AMDGPU_INFO_HW_IP_INFO"), layout["size"], ip, 0))
        print(json.dumps({"device_id": hex(dev.get("device_id")), "dma_rings": hw.get("available_rings"),
                          "ib_start_align": hw.get("ib_start_alignment"), "ib_size_align": hw.get("ib_size_alignment"),
                          "ring": ring}), flush=True)
        if not hw.get("available_rings") & (1 << ring):
            raise RuntimeError("SDMA ring unavailable")
        big = max(sizes)
        span = 2 << 20
        buf = ((big + PAGE + span - 1) // span) * span
        base = ((dev.get("virtual_address_offset") + span - 1) // span) * span
        va_ib, va_src, va_dst = base, base + span, base + span + buf
        if va_dst + buf > dev.get("virtual_address_max"):
            raise RuntimeError("no GPU VA")
        s.ctx_alloc()
        ib = s.bo("ib", PAGE, va_ib)
        src = bo_in(s, "src", buf, va_src, src_dom)
        dst = bo_in(s, "dst", buf, va_dst, dst_dom)
        pattern = b"".join(struct.pack("<I", (0x5A000000 | i) & 0xFFFFFFFF) for i in range(buf // 4))
        src[:] = pattern
        align = max(4, hw.get("ib_size_alignment"))
        rc = 0
        for n in sizes:
            dst[:] = b"\xAD\xDE\xFE\xCA" * (buf // 4)
            words = copy_packet(pk, va_src, va_dst, n)
            while len(words) * 4 % align:
                words.append(pk["op_nop"])
            ib[:PAGE] = bytes(PAGE)
            struct.pack_into("<%dI" % len(words), ib, 0, *words)
            print(f"== size {n} words {' '.join('%08x' % w for w in words)}", flush=True)
            seq = s.submit(va_ib, len(words), ring, ip)
            busy = s.wait(seq, ring, ip, 5)
            if busy:
                print(f"RESULT size {n} BUSY after 5 s", flush=True)
                rc = 3
                break
            got = dst[:n]
            guard = dst[n:n + 64]
            same = got == pattern[:n]
            guard_ok = guard == (b"\xAD\xDE\xFE\xCA" * 16)[:len(guard)]
            first_bad = next((i for i in range(0, n, 4) if got[i:i + 4] != pattern[i:i + 4]), None) if not same else None
            print(f"RESULT size {n} copied={'yes' if same else 'NO'} guard={'intact' if guard_ok else 'OVERRUN'}"
                  + (f" first_bad_byte={first_bad}" if first_bad is not None else ""), flush=True)
            if not same or not guard_ok:
                rc = 1
                break
        print("X1 " + {0: "PASS", 1: "MISMATCH", 3: "BUSY"}[rc] + f" ring {ring} route {route}", flush=True)
    except Exception as exc:
        print("X1_ERROR " + type(exc).__name__ + ": " + str(exc), flush=True)
        rc = 2
    finally:
        s.close()
        os.close(fd)
    return rc


if __name__ == "__main__":
    if sys.argv[1:2] == ["--gen"]:
        gen(sys.argv[2], sys.argv[3])
    else:
        sys.exit(lab(sys.argv[1:]))
