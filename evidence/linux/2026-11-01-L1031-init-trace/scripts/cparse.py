"""Small parsers for the AMD C headers the decoders need (development PC only).

Enum values, integer #defines and the natural-alignment layout of plain C structs, read from the headers themselves
so that no packet code, message number or struct offset is typed by hand. Handles what these headers use: the
fixed-width integer types, enums (4 bytes), nested structs of the same file, arrays with a literal or #define size.
"""
import re
from pathlib import Path

ROOT = Path(r"P:\BC-250")
LINUX = ROOT / "ref" / "linux-src" / "drivers" / "gpu" / "drm" / "amd"
THIRD = ROOT / "bc250-win" / "third_party" / "linux-amdgpu"

_TYPES = {"uint8_t": (1, "B"), "int8_t": (1, "b"), "uint16_t": (2, "H"), "int16_t": (2, "h"),
          "uint32_t": (4, "I"), "int32_t": (4, "i"), "uint64_t": (8, "Q"), "int64_t": (8, "q"),
          "unsigned int": (4, "I"), "int": (4, "i"), "unsigned": (4, "I"), "u32": (4, "I"), "u16": (2, "H"),
          "u8": (1, "B"), "u64": (8, "Q"), "bool": (1, "B")}


def _strip(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def _int(tok, defines):
    tok = tok.strip().rstrip("uUlL")
    if tok in defines:
        return defines[tok]
    return int(tok, 0)


def defines(path, pattern=r"\w+"):
    """{NAME: int} for '#define NAME <integer literal>' lines whose name matches pattern."""
    rx = re.compile(r"^\s*#\s*define\s+(" + pattern + r")\s+\(?\s*(0x[0-9A-Fa-f]+|\d+)[uUlL]*\s*\)?\s*(?:/[/*].*)?$")
    out = {}
    for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        m = rx.match(line)
        if m:
            out[m.group(1)] = int(m.group(2), 0)
    return out


def enum(path, name):
    """{value: NAME} of 'enum <name> { ... }', implicit values counted on."""
    text = _strip(Path(path).read_text(encoding="utf-8", errors="replace"))
    m = re.search(r"enum\s+" + re.escape(name) + r"\s*\{(.*?)\}", text, flags=re.S)
    if not m:
        raise KeyError(f"enum {name} not in {path}")
    out, nxt, known = {}, 0, {}
    for item in m.group(1).split(","):
        item = item.strip()
        if not item:
            continue
        if "=" in item:
            k, v = (s.strip() for s in item.split("=", 1))
            try:
                val = _int(v, known)
            except ValueError:
                continue
        else:
            k, val = item, nxt
        known[k] = val
        out.setdefault(val, k)
        nxt = val + 1
    return out


def struct(path, name, extra_defines=None):
    """[(field, offset, size_of_one, count, fmt)] of 'struct <name>' or 'typedef struct ... <name>;' with natural
    alignment. Nested structs from the same file are flattened with a dotted name."""
    text = _strip(Path(path).read_text(encoding="utf-8", errors="replace"))
    defs = dict(extra_defines or {})
    defs.update(defines(path))
    bodies = {}
    for m in re.finditer(r"struct\s+(\w+)\s*\{(.*?)\}\s*;", text, flags=re.S):
        bodies[m.group(1)] = m.group(2)
    for m in re.finditer(r"typedef\s+struct\s*(\w*)\s*\{(.*?)\}\s*(\w+)\s*;", text, flags=re.S):
        bodies[m.group(3)] = m.group(2)
        if m.group(1):
            bodies[m.group(1)] = m.group(2)

    def layout(sname):
        if sname not in bodies:
            raise KeyError(f"struct {sname} not in {path}")
        fields, off, align_max = [], 0, 1
        for decl in bodies[sname].split(";"):
            decl = " ".join(decl.split())
            if not decl:
                continue
            m = re.match(r"^(?:const\s+)?(?:(struct|enum)\s+)?([\w ]+?)\s+(\w+)\s*((?:\[[^\]]+\])*)$", decl)
            if not m:
                raise ValueError(f"cannot parse '{decl}' in {sname}")
            kind, typ, fname, dims = m.groups()
            count = 1
            for d in re.findall(r"\[([^\]]+)\]", dims):
                count *= _int(d, defs)
            if kind == "struct" or (kind is None and typ in bodies):
                sub, size, al = layout(typ)
                off = (off + al - 1) // al * al
                for i in range(count):
                    base = off + i * size
                    tag = f"{fname}[{i}]" if count > 1 else fname
                    fields += [(f"{tag}.{n}", base + o, s, c, f) for n, o, s, c, f in sub]
                off += size * count
                align_max = max(align_max, al)
                continue
            size, fmt = (4, "I") if kind == "enum" else _TYPES[typ]
            off = (off + size - 1) // size * size
            fields.append((fname, off, size, count, fmt))
            off += size * count
            align_max = max(align_max, size)
        total = (off + align_max - 1) // align_max * align_max
        return fields, total, align_max

    fields, total, _ = layout(name)
    return fields, total


def unpack(fields, data):
    """{field: value or [values]} from bytes, for a struct() layout."""
    import struct as st
    out = {}
    for name, off, size, count, fmt in fields:
        if off + size * count > len(data):
            break
        vals = list(st.unpack_from("<" + fmt * count, data, off))
        out[name] = vals[0] if count == 1 else vals
    return out


def sh_fields(reg_name, headers):
    """[(FIELD, shift, mask)] of register reg_name (without 'mm') from AMD *_sh_mask.h headers."""
    out = {}
    for h in headers:
        for line in Path(h).read_text(encoding="utf-8", errors="replace").splitlines():
            m = re.match(r"^#define\s+" + re.escape(reg_name) + r"__(\w+?)(__SHIFT|_MASK)\s+(0x[0-9A-Fa-f]+|\d+)", line)
            if m:
                d = out.setdefault(m.group(1), {})
                d["shift" if m.group(2) == "__SHIFT" else "mask"] = int(m.group(3), 0)
        if out:
            break
    return sorted(((k, v.get("shift", 0), v.get("mask", 0)) for k, v in out.items()), key=lambda t: t[1])
