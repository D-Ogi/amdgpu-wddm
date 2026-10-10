#!/usr/bin/env python3
"""BD-110: which kernels of a built ggml-hip.dll would our layer 1 refuse, and why.

Offline. It reads the clang offload bundles out of the PE image, and for every gfx1013 code
object inside them it reads the NT_AMDGPU_METADATA note (msgpack) and the 64-byte kernel
descriptor of every kernel, then applies the exact refusal rules of
compute/hip/bc250hsa/co_metadata.c, pm4_dispatch.c and submit.c.

    python scan-kernels.py <ggml-hip.dll> [--json out.json] [--names-of RULE]

Nothing is downloaded and nothing is executed from the image.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from collections import Counter

BUNDLE_MAGIC = b"__CLANG_OFFLOAD_BUNDLE__"

# compute/hip/bc250hsa/internal.h
KCP = {
    "PRIVATE_SEGMENT_BUFFER": 0x0001,
    "DISPATCH_PTR": 0x0002,
    "QUEUE_PTR": 0x0004,
    "KERNARG_SEGMENT_PTR": 0x0008,
    "DISPATCH_ID": 0x0010,
    "FLAT_SCRATCH_INIT": 0x0020,
    "PRIVATE_SEGMENT_SIZE": 0x0040,
    "WAVEFRONT_SIZE32": 0x0400,
    "USES_DYNAMIC_STACK": 0x0800,
}
KCP_KNOWN_MASK = 0x0001 | 0x0002 | 0x0004 | 0x0008 | 0x0010 | 0x0020 | 0x0040 | 0x0400 | 0x0800
# pm4_dispatch.c:86: the five items bc250hsa_plan_user_sgprs refuses by name.
KCP_REFUSED_SGPR = KCP["DISPATCH_PTR"] | KCP["QUEUE_PTR"] | KCP["DISPATCH_ID"] | \
                   KCP["FLAT_SCRATCH_INIT"] | KCP["PRIVATE_SEGMENT_SIZE"]
RSRC2_USER_SGPR_SHIFT = 1
RSRC2_USER_SGPR_MASK = 0x1F


# ---------------------------------------------------------------- PE and the bundles

def pe_sections(data: bytes):
    assert data[:2] == b"MZ", "not a PE image"
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[pe:pe + 4] == b"PE\0\0", "not a PE image"
    coff = pe + 4
    _machine, nsec, _, _symoff, _nsym, optsz, _ = struct.unpack_from("<HHIIIHH", data, coff)
    table = coff + 20 + optsz
    out = []
    for i in range(nsec):
        base = table + i * 40
        name = data[base:base + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, vaddr, rawsize, rawoff = struct.unpack_from("<IIII", data, base + 8)
        out.append((name, rawoff, rawsize, vaddr, vsize))
    return out


def find_bundles(data: bytes):
    """Every __CLANG_OFFLOAD_BUNDLE__ blob in the image, as (file offset, parsed entries)."""
    out = []
    at = 0
    while True:
        at = data.find(BUNDLE_MAGIC, at)
        if at < 0:
            break
        try:
            entries = parse_bundle(data, at)
        except Exception as exc:              # a byte sequence that only looks like a bundle
            entries = None
            sys.stderr.write(f"bundle at {at:#x}: {exc}\n")
        if entries:
            out.append((at, entries))
        at += 1
    return out


def parse_bundle(data: bytes, at: int):
    pos = at + len(BUNDLE_MAGIC)
    (count,) = struct.unpack_from("<Q", data, pos)
    pos += 8
    if count == 0 or count > 64:
        raise ValueError(f"entry count {count}")
    entries = []
    for _ in range(count):
        offset, size, idlen = struct.unpack_from("<QQQ", data, pos)
        pos += 24
        if idlen > 4096:
            raise ValueError(f"id length {idlen}")
        ident = data[pos:pos + idlen].decode("ascii", "replace")
        pos += idlen
        body = at + offset
        entries.append({"id": ident, "at": body, "bytes": size,
                        "elf": data[body:body + 4] == b"\x7fELF"})
    return entries


# ---------------------------------------------------------------- the ELF

def elf_sections(data: bytes, base: int):
    assert data[base:base + 4] == b"\x7fELF"
    assert data[base + 4] == 2, "only ELF64"
    e_shoff, = struct.unpack_from("<Q", data, base + 0x28)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, base + 0x3A)
    raw = []
    for i in range(e_shnum):
        s = base + e_shoff + i * e_shentsize
        (sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link, sh_info,
         sh_align, sh_entsize) = struct.unpack_from("<IIQQQQIIQQ", data, s)
        raw.append(dict(name=sh_name, type=sh_type, addr=sh_addr, offset=base + sh_offset,
                        size=sh_size, link=sh_link, entsize=sh_entsize))
    strtab = raw[e_shstrndx]
    out = []
    for s in raw:
        at = strtab["offset"] + s["name"]
        end = data.index(b"\0", at)
        s = dict(s)
        s["sname"] = data[at:end].decode("ascii", "replace")
        out.append(s)
    return out


def elf_notes(data: bytes, section):
    """Yield (type, name, descriptor bytes) of an SHT_NOTE section."""
    at = section["offset"]
    end = at + section["size"]
    while at + 12 <= end:
        namesz, descsz, ntype = struct.unpack_from("<III", data, at)
        at += 12
        name = data[at:at + namesz].rstrip(b"\0")
        at += (namesz + 3) & ~3
        desc = data[at:at + descsz]
        at += (descsz + 3) & ~3
        yield ntype, name, desc


def elf_symbols(data: bytes, sections):
    out = {}
    for sec in sections:
        if sec["type"] not in (2, 11) or sec["entsize"] != 24:   # SYMTAB, DYNSYM
            continue
        strs = sections[sec["link"]]
        n = sec["size"] // 24
        for i in range(n):
            s = sec["offset"] + i * 24
            st_name, st_info, _st_other, st_shndx, st_value, st_size = \
                struct.unpack_from("<IBBHQQ", data, s)
            at = strs["offset"] + st_name
            end = data.index(b"\0", at)
            name = data[at:end].decode("ascii", "replace")
            if name:
                out[name] = (st_shndx, st_value, st_size)
    return out


# ---------------------------------------------------------------- msgpack

def mp_read(data: bytes, at: int):
    tag = data[at]
    at += 1
    if tag <= 0x7F:
        return tag, at
    if tag >= 0xE0:
        return tag - 256, at
    if 0x80 <= tag <= 0x8F:
        return mp_map(data, at, tag & 0x0F)
    if 0x90 <= tag <= 0x9F:
        return mp_array(data, at, tag & 0x0F)
    if 0xA0 <= tag <= 0xBF:
        n = tag & 0x1F
        return data[at:at + n].decode("utf-8", "replace"), at + n
    if tag == 0xC0:
        return None, at
    if tag == 0xC2:
        return False, at
    if tag == 0xC3:
        return True, at
    if tag == 0xC4:
        n = data[at]; at += 1
        return data[at:at + n], at + n
    if tag == 0xC5:
        n, = struct.unpack_from(">H", data, at); at += 2
        return data[at:at + n], at + n
    if tag == 0xC6:
        n, = struct.unpack_from(">I", data, at); at += 4
        return data[at:at + n], at + n
    if tag == 0xCA:
        v, = struct.unpack_from(">f", data, at); return v, at + 4
    if tag == 0xCB:
        v, = struct.unpack_from(">d", data, at); return v, at + 8
    if tag == 0xCC:
        return data[at], at + 1
    if tag == 0xCD:
        v, = struct.unpack_from(">H", data, at); return v, at + 2
    if tag == 0xCE:
        v, = struct.unpack_from(">I", data, at); return v, at + 4
    if tag == 0xCF:
        v, = struct.unpack_from(">Q", data, at); return v, at + 8
    if tag == 0xD0:
        v, = struct.unpack_from(">b", data, at); return v, at + 1
    if tag == 0xD1:
        v, = struct.unpack_from(">h", data, at); return v, at + 2
    if tag == 0xD2:
        v, = struct.unpack_from(">i", data, at); return v, at + 4
    if tag == 0xD3:
        v, = struct.unpack_from(">q", data, at); return v, at + 8
    if tag == 0xD9:
        n = data[at]; at += 1
        return data[at:at + n].decode("utf-8", "replace"), at + n
    if tag == 0xDA:
        n, = struct.unpack_from(">H", data, at); at += 2
        return data[at:at + n].decode("utf-8", "replace"), at + n
    if tag == 0xDB:
        n, = struct.unpack_from(">I", data, at); at += 4
        return data[at:at + n].decode("utf-8", "replace"), at + n
    if tag == 0xDC:
        n, = struct.unpack_from(">H", data, at); at += 2
        return mp_array(data, at, n)
    if tag == 0xDD:
        n, = struct.unpack_from(">I", data, at); at += 4
        return mp_array(data, at, n)
    if tag == 0xDE:
        n, = struct.unpack_from(">H", data, at); at += 2
        return mp_map(data, at, n)
    if tag == 0xDF:
        n, = struct.unpack_from(">I", data, at); at += 4
        return mp_map(data, at, n)
    raise ValueError(f"msgpack tag {tag:#02x} at {at - 1}")


def mp_map(data, at, n):
    out = {}
    for _ in range(n):
        k, at = mp_read(data, at)
        v, at = mp_read(data, at)
        out[k] = v
    return out, at


def mp_array(data, at, n):
    out = []
    for _ in range(n):
        v, at = mp_read(data, at)
        out.append(v)
    return out, at


# ---------------------------------------------------------------- the rules

def descriptor_of(data: bytes, sections, symbols, name: str):
    sym = symbols.get(name + ".kd")
    if sym is None:
        return None
    shndx, value, size = sym
    if shndx >= len(sections):
        return None
    sec = sections[shndx]
    at = sec["offset"] + (value - sec["addr"])
    return data[at:at + 64]


def parse_descriptor(d: bytes):
    (group_segment_fixed_size, private_segment_fixed_size, kernarg_size, reserved0) = \
        struct.unpack_from("<IIII", d, 0)
    (kernel_code_entry_byte_offset,) = struct.unpack_from("<Q", d, 16)
    reserved1 = d[24:44]
    (compute_pgm_rsrc3, compute_pgm_rsrc1, compute_pgm_rsrc2) = struct.unpack_from("<III", d, 44)
    (kernel_code_properties, kernarg_preload) = struct.unpack_from("<HH", d, 56)
    (reserved2,) = struct.unpack_from("<I", d, 60)
    return dict(group_segment_fixed_size=group_segment_fixed_size,
                private_segment_fixed_size=private_segment_fixed_size,
                kernarg_size=kernarg_size, reserved0=reserved0,
                entry=kernel_code_entry_byte_offset, reserved1=reserved1,
                rsrc1=compute_pgm_rsrc1, rsrc2=compute_pgm_rsrc2, rsrc3=compute_pgm_rsrc3,
                kcp=kernel_code_properties, kernarg_preload=kernarg_preload,
                reserved2=reserved2)


def judge(meta: dict, desc: dict | None):
    """Every refusal of our stack that this kernel would hit, in the order it would hit it."""
    reasons = []
    name = meta.get(".name") or meta.get(".symbol", "?")
    if desc is None:
        reasons.append(("load/no-descriptor", "ENOTFOUND: no .kd symbol"))
        return name, reasons
    # co_metadata.c, read_descriptor
    if desc["reserved0"] or desc["reserved2"] or any(desc["reserved1"]):
        reasons.append(("load/EBADELF", "a reserved descriptor field is not zero"))
    if desc["kernarg_preload"]:
        reasons.append(("load/EUNSUPPORTED",
                        f"kernarg_preload {desc['kernarg_preload']}"))
    unknown = desc["kcp"] & ~KCP_KNOWN_MASK
    if unknown:
        reasons.append(("load/EUNSUPPORTED",
                        f"kernel_code_properties has unknown bits {unknown:#06x}"))
    if desc["kernarg_size"] != (meta.get(".kernarg_segment_size") or 0) or \
       desc["group_segment_fixed_size"] != (meta.get(".group_segment_fixed_size") or 0) or \
       desc["private_segment_fixed_size"] != (meta.get(".private_segment_fixed_size") or 0):
        reasons.append(("load/EBADELF", "the descriptor and the metadata disagree"))
    # pm4_dispatch.c, bc250hsa_pm4_check_dispatch
    if not (desc["kcp"] & KCP["WAVEFRONT_SIZE32"]):
        reasons.append(("dispatch/EUNSUPPORTED", "wave64 kernel"))
    if (meta.get(".wavefront_size") or 0) != 32:
        reasons.append(("dispatch/EUNSUPPORTED",
                        f"metadata wavefront size {meta.get('.wavefront_size')}"))
    dynamic_stack = 1 if (desc["kcp"] & KCP["USES_DYNAMIC_STACK"]) else 0
    if meta.get(".uses_dynamic_stack"):
        dynamic_stack = 1
    if dynamic_stack or desc["private_segment_fixed_size"]:
        reasons.append(("dispatch/EUNSUPPORTED",
                        f"needs scratch (dynamic stack {dynamic_stack}, private segment "
                        f"{desc['private_segment_fixed_size']})"))
    # pm4_dispatch.c, bc250hsa_plan_user_sgprs
    refused = desc["kcp"] & KCP_REFUSED_SGPR
    if refused:
        which = [k for k, v in KCP.items() if v & refused]
        reasons.append(("sgpr/EUNSUPPORTED", "enables " + ",".join(sorted(which))))
    else:
        want = (desc["rsrc2"] >> RSRC2_USER_SGPR_SHIFT) & RSRC2_USER_SGPR_MASK
        fills = 0
        if desc["kcp"] & KCP["PRIVATE_SEGMENT_BUFFER"]:
            fills += 4
        if desc["kcp"] & KCP["KERNARG_SEGMENT_PTR"]:
            fills += 2
        if fills != want:
            reasons.append(("sgpr/EUNSUPPORTED",
                            f"wants {want} user SGPRs, the plan fills {fills}"))
    return name, reasons


# ---------------------------------------------------------------- main

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--json")
    ap.add_argument("--names-of", action="append", default=[],
                    help="print every kernel name whose refusal rule starts with this")
    ap.add_argument("--target", default="gfx1013")
    args = ap.parse_args()

    data = open(args.image, "rb").read()
    print(f"image {args.image}: {len(data)} bytes")
    bundles = find_bundles(data)
    print(f"offload bundles: {len(bundles)}")

    kernels = []
    objects = 0
    for at, entries in bundles:
        for e in entries:
            if not e["elf"] or args.target not in e["id"]:
                continue
            objects += 1
            base = e["at"]
            sections = elf_sections(data, base)
            symbols = elf_symbols(data, sections)
            meta = None
            for sec in sections:
                if sec["type"] != 7:          # SHT_NOTE
                    continue
                for ntype, name, desc in elf_notes(data, sec):
                    if ntype == 32 and name == b"AMDGPU":   # NT_AMDGPU_METADATA
                        meta, _ = mp_read(desc, 0)
            if meta is None:
                print(f"  bundle {at:#x} entry '{e['id']}': no AMDGPU metadata note")
                continue
            for k in meta.get("amdhsa.kernels", []):
                desc = descriptor_of(data, sections, symbols, k.get(".symbol", "")[:-3]
                                     if k.get(".symbol", "").endswith(".kd")
                                     else k.get(".name", ""))
                if desc is None and k.get(".symbol"):
                    sym = symbols.get(k[".symbol"])
                    if sym is not None:
                        shndx, value, _ = sym
                        sec = sections[shndx]
                        desc = data[sec["offset"] + (value - sec["addr"]):][:64]
                parsed = parse_descriptor(desc) if desc is not None and len(desc) == 64 else None
                name, reasons = judge(k, parsed)
                kernels.append(dict(name=name, bundle=at, target=e["id"],
                                    reasons=reasons, meta={
                                        "kernarg": k.get(".kernarg_segment_size"),
                                        "group": k.get(".group_segment_fixed_size"),
                                        "private": k.get(".private_segment_fixed_size"),
                                        "wave": k.get(".wavefront_size"),
                                        "dynstack": k.get(".uses_dynamic_stack"),
                                        "maxflat": k.get(".max_flat_workgroup_size"),
                                        "args": [a.get(".value_kind")
                                                 for a in k.get(".args", [])],
                                    },
                                    desc=None if parsed is None else {
                                        "kcp": parsed["kcp"],
                                        "kernarg_preload": parsed["kernarg_preload"],
                                        "private": parsed["private_segment_fixed_size"],
                                        "group": parsed["group_segment_fixed_size"],
                                        "kernarg": parsed["kernarg_size"],
                                        "user_sgpr": (parsed["rsrc2"] >> RSRC2_USER_SGPR_SHIFT)
                                        & RSRC2_USER_SGPR_MASK,
                                    }))
    print(f"{args.target} code objects: {objects}")
    print(f"kernels: {len(kernels)}")

    rules = Counter()
    for k in kernels:
        for rule, _why in k["reasons"]:
            rules[rule] += 1
    refused = [k for k in kernels if k["reasons"]]
    print(f"kernels our stack would refuse: {len(refused)} of {len(kernels)}")
    for rule, n in rules.most_common():
        print(f"  {rule}: {n}")

    kcp = Counter(k["desc"]["kcp"] for k in kernels if k["desc"])
    print("kernel_code_properties values:")
    for v, n in kcp.most_common():
        which = ",".join(sorted(name for name, bit in KCP.items() if bit & v))
        print(f"  {v:#06x} ({which}): {n}")

    sgpr = Counter(k["desc"]["user_sgpr"] for k in kernels if k["desc"])
    print(f"user SGPR counts: {dict(sorted(sgpr.items()))}")

    for want in args.names_of:
        print(f"-- kernels refused by {want}:")
        for k in refused:
            for rule, why in k["reasons"]:
                if rule.startswith(want):
                    print(f"   {k['name']}  [{why}]  meta={k['meta']} desc={k['desc']}")

    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(dict(image=args.image, bytes=len(data), objects=objects,
                           kernels=len(kernels), rules=dict(rules),
                           refused=refused, all=kernels), fh, indent=1, default=str)
        print(f"wrote {args.json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
