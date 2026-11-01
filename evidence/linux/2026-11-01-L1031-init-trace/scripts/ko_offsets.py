#!/usr/bin/env python3
"""Offsets for the init-trace probes, read from the amdgpu module that unit A actually loads (development PC).

    python ko_offsets.py <i0 dir>          (expects amdgpu.ko.* and, if the kernel has BTF, vmlinux.btf there)

Writes <i0 dir>/offsets.json and prints the evidence. Three independent sources, all from that one module file:
  1. its ELF symbol table: is every probe target a real function symbol, or was it renamed by GCC
     (name.isra.N / .constprop.N / .part.N), or is it gone (inlined)?
  2. its split BTF (.BTF section, base = the running kernel's /sys/kernel/btf/vmlinux): member offsets of
     amdgpu_device.rmmio and of the structs whose offsets gen_probes.py takes from the headers;
  3. its machine code (llvm-objdump): the displacement amdgpu_device_rreg and amdgpu_device_wreg use to load
     adev->rmmio before the readl/writel, and the rmmio_size compare just before it.
agreed_rmmio is set only when every available source gives the same value (BTF absent: rreg, wreg and
rmmio == rmmio_size + 8 must agree). Otherwise it is null and gen_probes.py arms no register read.
"""
import gzip
import hashlib
import json
import lzma
import re
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import gen_probes  # noqa: E402

OBJDUMP = Path(r"P:\BC-250\toolchain\llvm-23.1.4-win64\bin\llvm-objdump.exe")


def decompress(data):
    if data[:2] == b"\x1f\x8b":
        return gzip.decompress(data)
    if data[:6] == b"\xfd7zXZ\x00":
        return lzma.decompress(data)
    if data[:4] == b"\x28\xb5\x2f\xfd":
        from compression import zstd  # Python 3.14
        return zstd.decompress(data)
    if data[:4] == b"\x7fELF":
        return data
    raise ValueError("unknown module compression")


def elf_sections(elf):
    shoff, = struct.unpack_from("<Q", elf, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", elf, 0x3A)
    secs = []
    for i in range(shnum):
        name, typ, flags, addr, off, size, link, info, align, entsize = struct.unpack_from(
            "<IIQQQQIIQQ", elf, shoff + i * shentsize)
        secs.append(dict(name_off=name, type=typ, off=off, size=size, link=link))
    strtab = secs[shstrndx]
    for s in secs:
        n = elf[strtab["off"] + s["name_off"]:]
        s["name"] = n[:n.index(b"\0")].decode()
    return secs


def elf_funcs(elf, secs):
    out = {}
    for s in secs:
        if s["type"] != 2:      # SHT_SYMTAB
            continue
        st = secs[s["link"]]
        for i in range(s["size"] // 24):
            name, info, other, shndx, value, size = struct.unpack_from("<IBBHQQ", elf, s["off"] + i * 24)
            if info & 0xF != 2:     # STT_FUNC
                continue
            n = elf[st["off"] + name:]
            out[n[:n.index(b"\0")].decode()] = (shndx, value, size)
    return out


# ---- BTF -------------------------------------------------------------------------------------------------------
_EXTRA = {1: 4, 3: 12, 14: 4, 17: 4}            # INT, ARRAY, VAR, DECL_TAG: fixed tail
_PER_VLEN = {4: 12, 5: 12, 6: 8, 13: 8, 15: 12, 19: 12}   # STRUCT, UNION, ENUM, FUNC_PROTO, DATASEC, ENUM64


def btf_parse(blob):
    magic, ver, flags, hdr_len, type_off, type_len, str_off, str_len = struct.unpack_from("<HBBIIIII", blob, 0)
    if magic != 0xEB9F:
        raise ValueError("not BTF")
    types, p, end = [], hdr_len + type_off, hdr_len + type_off + type_len
    while p < end:
        name_off, info, size_type = struct.unpack_from("<III", blob, p)
        kind, vlen, kflag = (info >> 24) & 0x1F, info & 0xFFFF, info >> 31
        q = p + 12
        members = []
        if kind in (4, 5):
            for i in range(vlen):
                mname, mtype, moff = struct.unpack_from("<III", blob, q + i * 12)
                bit = (moff & 0xFFFFFF) if kflag else moff
                members.append((mname, bit))
        q += _EXTRA.get(kind, 0) + _PER_VLEN.get(kind, 0) * vlen
        types.append(dict(kind=kind, name_off=name_off, size=size_type, members=members))
        p = q
    strings = blob[hdr_len + str_off:hdr_len + str_off + str_len]
    return types, strings, str_len


def btf_struct_offsets(mod_blob, base_blob, wanted):
    """{struct: {member: byte offset}} for the structs/members in wanted, from split module BTF."""
    base_types, base_str, base_len = btf_parse(base_blob)
    types, mstr, _ = btf_parse(mod_blob)

    def s(off):
        src, o = (base_str, off) if off < base_len else (mstr, off - base_len)
        return src[o:src.index(b"\0", o)].decode()

    out = {}
    for t in types:
        if t["kind"] != 4 or not t["name_off"]:
            continue
        sname = s(t["name_off"])
        if sname not in wanted or sname in out:
            continue
        offs = {}
        for mname, bit in t["members"]:
            m = s(mname) if mname else ""
            if m in wanted[sname]:
                offs[m] = bit // 8
        out[sname] = offs
    return out


# ---- machine code ------------------------------------------------------------------------------------------------
_INS = re.compile(r"^\s*[0-9a-f]+:\s+(?:(?:[0-9a-f]{2} )+\s*)?(\w+)\s*(.*)$")
_MEM = re.compile(r"^(-?0x[0-9a-f]+|-?\d+)?\((%\w+)?(?:,(%\w+))?(?:,(\d))?\)$")


def disasm(ko_path, funcs):
    r = subprocess.run([str(OBJDUMP), "-d", "--no-show-raw-insn", "--disassemble-symbols=" + ",".join(funcs),
                        str(ko_path)], capture_output=True, text=True)
    cur, out = None, {f: [] for f in funcs}
    for line in r.stdout.splitlines():
        m = re.match(r"^[0-9a-f]+ <([\w.]+)>:", line)
        if m:
            cur = m.group(1) if m.group(1) in out else None
            continue
        if cur:
            m = _INS.match(line)
            if m:
                ops = [o.strip() for o in re.split(r",(?![^(]*\))", m.group(2).split("#")[0]) if o.strip()]
                out[cur].append((m.group(1), ops, line.strip()))
    return out, r.stderr


def _mem(op):
    m = _MEM.match(op.replace(" ", ""))
    if not m:
        return None
    disp = int(m.group(1), 0) if m.group(1) else 0
    return disp, m.group(2), m.group(3)


def _is32(reg):
    return bool(re.match(r"^%(e\w\w|r\d+d)$", reg))


def rmmio_from_code(ins, store):
    """Displacement of the adev field holding the MMIO base, as used by the 32-bit load (rreg) or store (wreg)."""
    adev = {"%rdi"}
    defs = {}
    hits, cmps, used = [], [], []
    for mnem, ops, text in ins:
        base_m = mnem.rstrip("qlwb") if mnem not in ("movl", "movq") else "mov"
        if mnem.startswith("mov") and len(ops) == 2:
            src, dst = ops
            if src in adev and dst.startswith("%r"):
                adev.add(dst)
                continue
            msrc, mdst = _mem(src), _mem(dst)
            if msrc and msrc[1] in adev and dst.startswith("%r") and not _is32(dst):
                defs[dst] = msrc[0]
                used.append(text)
                continue
            plain32 = mnem in ("mov", "movl")       # not movzwl/movzbl/movslq: readl/writel are plain 32-bit moves
            if plain32 and not store and msrc and _is32(dst) and msrc[1] in defs and msrc[1] not in adev:
                hits.append((defs[msrc[1]], text))
            if plain32 and store and mdst and _is32(src) and mdst[1] in defs and mdst[1] not in adev:
                hits.append((defs[mdst[1]], text))
            if dst in defs and not msrc:
                defs.pop(dst, None)
        elif base_m in ("add",) and len(ops) == 2:
            msrc = _mem(ops[0])
            if msrc and msrc[1] in adev:
                defs[ops[1]] = msrc[0]
                used.append(text)
        elif base_m == "cmp" and len(ops) == 2:
            for o in ops:
                mo = _mem(o)
                if mo and mo[1] in adev:
                    cmps.append((mo[0], text))
    return hits, cmps, used


def main():
    d = Path(sys.argv[1])
    kos = sorted(p for p in d.iterdir() if p.name.startswith("amdgpu.ko"))
    if not kos:
        sys.exit(f"no amdgpu.ko* in {d}")
    raw = kos[0].read_bytes()
    elf = decompress(raw)
    elf_path = d / "amdgpu.ko.elf"
    elf_path.write_bytes(elf)
    res = {"module_file": kos[0].name, "module_sha256": hashlib.sha256(raw).hexdigest(),
           "elf_sha256": hashlib.sha256(elf).hexdigest(), "checks": {}, "evidence": []}
    secs = elf_sections(elf)
    funcs = elf_funcs(elf, secs)
    targets = sorted({fn for _, fn, *_ in gen_probes.PROBES})
    res["symbols"] = {}
    for fn in targets:
        if fn in funcs:
            res["symbols"][fn] = "present"
        else:
            clones = sorted(n for n in funcs if n.startswith(fn + "."))
            res["symbols"][fn] = ("clone:" + ",".join(clones)) if clones else "absent"
    # BTF
    btf_sec = next((s for s in secs if s["name"] == ".BTF"), None)
    base = d / "vmlinux.btf"
    btf = None
    if btf_sec and base.exists():
        wanted = {"amdgpu_device": {"rmmio", "rmmio_size", "rmmio_base"}, "amdgpu_ip_block": {"adev"},
                  "psp_context": {"adev"}, "amdgpu_ring": {"adev"}, "smu_context": {"adev"},
                  "amdgpu_ib": {"length_dw", "gpu_addr", "ptr", "flags"},
                  "amdgpu_firmware_info": {"ucode_id", "tmr_mc_addr_lo", "tmr_mc_addr_hi"}}
        btf = btf_struct_offsets(elf[btf_sec["off"]:btf_sec["off"] + btf_sec["size"]], base.read_bytes(), wanted)
        res["btf"] = btf
        exp = {("amdgpu_ip_block", "adev"): gen_probes.IP_BLOCK_ADEV, ("psp_context", "adev"): gen_probes.PSP_ADEV,
               ("amdgpu_ring", "adev"): gen_probes.RING_ADEV, ("smu_context", "adev"): gen_probes.SMU_ADEV,
               ("amdgpu_ib", "length_dw"): gen_probes.IB_LEN, ("amdgpu_ib", "gpu_addr"): gen_probes.IB_VA,
               ("amdgpu_ib", "ptr"): gen_probes.IB_PTR, ("amdgpu_ib", "flags"): gen_probes.IB_FLAGS,
               ("amdgpu_firmware_info", "ucode_id"): 0}
        for (st, m), v in exp.items():
            got = btf.get(st, {}).get(m)
            res["checks"][f"{st}.{m}"] = "ok" if got == v else f"MISMATCH btf={got} header={v}"
    else:
        res["btf"] = None
        res["evidence"].append("no module BTF or no vmlinux.btf: header-derived struct offsets are not cross-checked")
    # machine code
    # GCC may split the body into <name>.part.0 and leave a tail jump in <name> (6.18.52-0-lts does): analyse both.
    funcs = ["amdgpu_device_rreg", "amdgpu_device_wreg", "amdgpu_device_rreg.part.0", "amdgpu_device_wreg.part.0"]
    code, err = disasm(elf_path, funcs)
    if err.strip():
        res["evidence"].append("llvm-objdump stderr: " + err.strip()[:300])
    if code["amdgpu_device_rreg.part.0"] or code["amdgpu_device_wreg.part.0"]:
        res["evidence"].append("rreg/wreg bodies are in .part.0 (GCC partial inlining); analysed there")
    rr, cr, ur = rmmio_from_code(code["amdgpu_device_rreg"] + code["amdgpu_device_rreg.part.0"], store=False)
    rw, cw, uw = rmmio_from_code(code["amdgpu_device_wreg"] + code["amdgpu_device_wreg.part.0"], store=True)
    res["disasm"] = {"rreg_loads": [[hex(o), t] for o, t in rr], "wreg_stores": [[hex(o), t] for o, t in rw],
                     "rreg_cmps": [[hex(o), t] for o, t in cr], "wreg_cmps": [[hex(o), t] for o, t in cw],
                     "rreg_adev_field_loads": ur, "wreg_adev_field_loads": uw}
    cand_r, cand_w = {o for o, _ in rr}, {o for o, _ in rw}
    size_c = {o for o, _ in cr} | {o for o, _ in cw}
    agreed = None
    if len(cand_r) == 1 and cand_r == cand_w:
        code_rmmio = next(iter(cand_r))
        if btf and "rmmio" in btf.get("amdgpu_device", {}):
            b = btf["amdgpu_device"]
            ok = b["rmmio"] == code_rmmio and b.get("rmmio_size") == code_rmmio - 8
            res["checks"]["rmmio"] = "ok" if ok else f"MISMATCH btf={b} code={code_rmmio:#x}"
            agreed = code_rmmio if ok else None
        else:
            ok = (code_rmmio - 8) in size_c
            res["checks"]["rmmio"] = ("ok (code only: rreg = wreg, rmmio_size compare at rmmio - 8)" if ok else
                                      f"UNCONFIRMED code={code_rmmio:#x}, no rmmio_size compare at -8 ({sorted(size_c)})")
            agreed = code_rmmio if ok else None
    else:
        res["checks"]["rmmio"] = f"NO AGREEMENT rreg={sorted(map(hex, cand_r))} wreg={sorted(map(hex, cand_w))}"
    if any(str(v).startswith("MISMATCH") for v in res["checks"].values()):
        agreed = None
        res["evidence"].append("a header-derived offset is contradicted by BTF: do not arm any state probe")
    res["agreed_rmmio"] = agreed
    (d / "offsets.json").write_text(json.dumps(res, indent=1), newline="\n")
    print(json.dumps({k: res[k] for k in ("module_sha256", "agreed_rmmio", "checks")}, indent=1))
    for fn, st in res["symbols"].items():
        if st != "present":
            print(f"symbol {fn}: {st}")
    for k in ("rreg_loads", "wreg_stores", "rreg_cmps"):
        for o, t in res["disasm"][k]:
            print(f"{k}: {o}  {t}")


if __name__ == "__main__":
    main()
