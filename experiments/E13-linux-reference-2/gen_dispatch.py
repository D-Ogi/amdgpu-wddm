#!/usr/bin/env python3
"""Generate dispatch.json for dispatch.py: everything the Linux reference run of libdrm's gfx10 memset compute
dispatch needs, parsed out of the original headers and the original libdrm test source. No ioctl number, struct
offset, register offset, shader word or PM4 constant is typed in this file.

Sources (all read-only, all outside the repo):

  A  P:/BC-250/ref/mesa/include/drm-uapi/drm.h              DRM_IOCTL_BASE, DRM_COMMAND_BASE, struct drm_gem_close
  B  P:/BC-250/ref/mesa/include/drm-uapi/amdgpu_drm.h       DRM_AMDGPU_* command numbers, AMDGPU_* constants and the
                                                            layout of every struct the ioctls take
  C  P:/BC-250/ref/libdrm/tests/amdgpu/shader_code_gfx10.h  bufferclear_cs_shader_gfx10[], sh_reg_base_gfx10
  D  P:/BC-250/ref/libdrm/tests/amdgpu/shader_code_gfx9.h   bufferclear_cs_shader_registers_gfx9[]
  E  P:/BC-250/ref/libdrm/tests/amdgpu/shader_test_util.c   the PACKET3 opcodes and the dispatch packet stream
  F  P:/BC-250/ref/linux-src/drivers/gpu/drm/amd/amdgpu/nvd.h   PACKET3_SET_UCONFIG_REG_START (libdrm has no name
                                                            for it), cross-check of PACKET3_SET_SH_REG_START
  G  tools/regcalc (over third_party/linux-amdgpu)          the authoritative register offsets, repo rule 1

What is DERIVED (parsed, computed, cross-checked - every number below):
  - ioctl request numbers, from the command numbers in B and the struct sizes computed from B's own declarations
  - struct field offsets, from a System V x86-64 layout pass over B's declarations
  - the 9 shader dwords and the 5-entry register table, from C and D
  - the packet stream: the `ptr[i++] = ...;` statements of E's five dispatch helpers are extracted in source order,
    the expressions are evaluated, and the flat dword list is regrouped into packets by decoding each type-3 header
  - every SET_SH_REG offset is resolved to an mm* register name through regcalc (G) and asserted equal to the
    literal E uses. A disagreement fails generation.

What is AUTHORED here (the only thing): the ORDER in which E's five helper functions run for
ip == AMDGPU_HW_IP_COMPUTE, version == AMDGPU_TEST_GFX_V10, cs_type == CS_BUFFERCLEAR, taken from
amdgpu_test_dispatch_memset() at E:609-617 and documented in docs/research/m6-compute-dispatch.md section 3.3.

Run:  python experiments/E13-linux-reference-2/gen_dispatch.py
"""

import ast
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "regcalc"))
from regcalc import RegMap                                             # noqa: E402  repo rule 1

UAPI = Path(r"P:\BC-250\ref\mesa\include\drm-uapi")
LIBDRM = Path(r"P:\BC-250\ref\libdrm\tests\amdgpu")
NVD = Path(r"P:\BC-250\ref\linux-src\drivers\gpu\drm\amd\amdgpu\nvd.h")

LIBDRM_TAG = "libdrm-2.4.114"
LIBDRM_COMMIT = "b9ca37b3134861048986b75896c0915cbf2e97f9"


def libdrm_commit():
    """The commit the local clone is actually on, so the recorded provenance cannot go stale unnoticed."""
    head = LIBDRM.parent.parent / ".git" / "HEAD"
    try:
        text = head.read_text(encoding="utf-8").strip()
    except OSError:
        return LIBDRM_COMMIT
    if text.startswith("ref:"):
        ref = (head.parent / text.split(None, 1)[1]).read_text(encoding="utf-8").strip()
        text = ref
    if text != LIBDRM_COMMIT:
        raise SystemExit(f"{LIBDRM.parent.parent} is on {text}, not {LIBDRM_TAG} ({LIBDRM_COMMIT})")
    return text


# ---------------------------------------------------------------- C preprocessor and struct layout

def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def defines(path, pattern):
    """{name: int} for every `#define NAME <integer expression>` whose name matches pattern. The pattern may
    contain groups, so both captures here are named."""
    out = {}
    rx = re.compile(r"^#define\s+(?P<name>" + pattern + r")\s+(?P<value>.+?)\s*$")
    for line in strip_comments(Path(path).read_text(encoding="utf-8", errors="replace")).splitlines():
        m = rx.match(line.strip())
        if not m:
            continue
        try:
            out[m.group("name")] = const_eval(m.group("value"), out)
        except (ValueError, SyntaxError, KeyError):
            pass                                                        # macros with arguments, type names, strings
    return out


_ALLOWED = (ast.Expression, ast.BinOp, ast.UnaryOp, ast.Constant, ast.Name, ast.Load,
            ast.Add, ast.Sub, ast.Mult, ast.FloorDiv, ast.Div, ast.LShift, ast.RShift,
            ast.BitOr, ast.BitAnd, ast.BitXor, ast.Invert, ast.USub, ast.UAdd)


def const_eval(expr, names=None):
    """Evaluate a C integer constant expression. Only literals, names from `names` and the operators above."""
    expr = expr.strip().rstrip(";")
    expr = re.sub(r"\b(0[xX][0-9a-fA-F]+|\d+)[uUlL]+\b", r"\1", expr)    # 1ULL -> 1
    tree = ast.parse(expr, mode="eval")
    for node in ast.walk(tree):
        if not isinstance(node, _ALLOWED):
            raise ValueError(f"not a constant expression: {expr}")
        if isinstance(node, ast.Name) and (names is None or node.id not in names):
            raise KeyError(node.id)
        if isinstance(node, ast.Constant) and not isinstance(node.value, int):
            raise ValueError(f"not an integer: {expr}")
    value = eval(compile(tree, "<c-const>", "eval"), {"__builtins__": {}}, dict(names or {}))
    if not isinstance(value, int):
        raise ValueError(f"not an integer: {expr}")
    return value


PRIM = {"__u8": (1, 1), "__s8": (1, 1), "char": (1, 1),
        "__u16": (2, 2), "__s16": (2, 2),
        "__u32": (4, 4), "__s32": (4, 4),
        "__u64": (8, 8), "__s64": (8, 8)}


def split_members(body):
    """Top level `;`-separated member declarations of a struct/union body."""
    out, buf, depth = [], "", 0
    for ch in body:
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
        if ch == ";" and depth == 0:
            if buf.strip():
                out.append(buf.strip())
            buf = ""
        else:
            buf += ch
    if buf.strip():
        out.append(buf.strip())
    return out


def inner_body(text):
    """Text between the first `{` and its matching `}`, plus whatever follows the `}`."""
    start = text.index("{")
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start + 1:i], text[i + 1:]
    raise ValueError("unbalanced braces")


def find_type(text, kind, name):
    """Body text of `<kind> <name> { ... }` in text."""
    m = re.search(r"\b" + kind + r"\s+" + name + r"\s*\{", text)
    if not m:
        raise KeyError(f"{kind} {name}")
    return inner_body(text[m.start():])[0]


ARRAY = re.compile(r"\[(\d+)\]")


def layout(decl_text, kind, source, fields, offset=0, prefix=""):
    """Lay out one struct/union body System V x86-64. Appends leaf primitives to `fields` as path -> (offset, size,
    fmt). Returns (size, align)."""
    size, align = 0, 1
    cursor = offset
    for member in split_members(decl_text):
        member = " ".join(member.split())
        m = re.match(r"^(struct|union)\s*\{", member)
        if m:
            body, rest = inner_body(member)
            name = rest.strip().rstrip(";").strip()
            sub_prefix = prefix + (name + "." if name else "")
            msize, malign = layout(body, m.group(1), source, fields, cursor, sub_prefix)
        else:
            m = re.match(r"^(struct|union)\s+(\w+)\s+(\w+)$", member)
            if m:
                body = find_type(source, m.group(1), m.group(2))
                msize, malign = layout(body, m.group(1), source, {}, 0, "")
                malign = max(malign, 1)
                cursor = (cursor + malign - 1) // malign * malign
                layout(body, m.group(1), source, fields, cursor, prefix + m.group(3) + ".")
            else:
                m = re.match(r"^(\w+)\s+(\w+)((?:\[\d+\])*)$", member)
                if not m or m.group(1) not in PRIM:
                    raise ValueError(f"cannot lay out member: {member!r}")
                psize, malign = PRIM[m.group(1)]
                count = 1
                for dim in ARRAY.findall(m.group(3)):
                    count *= int(dim)
                cursor = (cursor + malign - 1) // malign * malign
                msize = psize * count
                fmt = {1: "B", 2: "H", 4: "I", 8: "Q"}[psize]
                if m.group(1).startswith("__s"):
                    fmt = fmt.lower()
                fields[prefix + m.group(2)] = {"offset": cursor, "size": msize, "count": count, "fmt": fmt}
        align = max(align, malign)
        if kind == "union":
            size = max(size, msize)
            cursor = offset
        else:
            cursor += msize
            size = cursor - offset
    size = (size + align - 1) // align * align
    return size, align


def type_layout(source, kind, name):
    fields = {}
    size, align = layout(find_type(source, kind, name), kind, source, fields)
    return {"kind": kind, "name": name, "size": size, "align": align, "fields": fields}


# ---------------------------------------------------------------- libdrm extraction

def array_words(text, name):
    """The integer initialisers of `static const uint32_t NAME[] = { ... };` plus its 1-based line range."""
    m = re.search(r"static\s+const\s+uint32_t\s+" + name + r"\[\]\s*=\s*\{", text)
    if not m:
        raise KeyError(name)
    body, _ = inner_body(text[m.start():])
    first = text[:m.start()].count("\n") + 1
    last = first + body.count("\n") + 1
    words = [int(w, 0) for w in re.findall(r"0[xX][0-9a-fA-F]+|\b\d+\b", body)]
    return words, (first, last)


def reg_table(text, name):
    """The {offset, value} pairs of `static const struct reg_info NAME[] = { ... };` plus its line range."""
    m = re.search(r"static\s+const\s+struct\s+reg_info\s+" + name + r"\[\]\s*=\s*\{", text)
    if not m:
        raise KeyError(name)
    body, _ = inner_body(text[m.start():])
    first = text[:m.start()].count("\n") + 1
    pairs = [(int(a, 0), int(b, 0)) for a, b in re.findall(r"\{\s*(0[xX][0-9a-fA-F]+)\s*,\s*(0[xX][0-9a-fA-F]+)\s*\}", body)]
    return pairs, (first, first + body.count("\n") + 1)


def function_lines(lines, name):
    """(first, last) 1-based line numbers of `static void NAME(...)` including its closing brace."""
    for i, line in enumerate(lines):
        if re.match(r"^(static\s+)?\w+\s+" + name + r"\s*\(", line):
            for j in range(i, len(lines)):
                if lines[j].rstrip() == "{":
                    for k in range(j + 1, len(lines)):
                        if lines[k].rstrip() == "}":
                            return i + 1, k + 1
                    break
            break
    raise KeyError(name)


WRITE = re.compile(r"^ptr\[i\+\+\]\s*=\s*(.+?);\s*$")
ADVANCE = re.compile(r"^i\s*\+=\s*(\d+)\s*;\s*$")

# The C expressions of E that are not compile-time constants. Everything else must evaluate to an integer.
# Each capture group is an integer literal of the expression, carried into dispatch.json as "args" so that
# dispatch.py reproduces libdrm's arithmetic without retyping any of its numbers.
SYMBOLS = [
    # (shader_addr >> 8) and (shader_addr >> 40): COMPUTE_PGM_LO/HI take the program address shifted right by 8,
    # so the shader BO must be 256-byte aligned. Both go through one rule; the shift is the argument.
    (r"\(\s*shader_addr\s*>>\s*(\d+)\s*\)", "SHADER_ADDR_SHR"),
    (r"test_priv->dst\.mc_address", "DST_ADDR"),
    # (dst.mc_address >> 32) | 0x100000: the high half of the buffer V# with STRIDE = 16 OR-ed in.
    (r"\(\s*test_priv->dst\.mc_address\s*>>\s*(\d+)\s*\)\s*\|\s*(0[xX][0-9a-fA-F]+)", "DST_ADDR_SHR_OR"),
    (r"test_priv->dst\.size\s*/\s*(\d+)", "DST_NUM_RECORDS"),
    (r"\(\s*test_priv->dst\.size\s*/\s*(\d+)\s*\+\s*(0[xX][0-9a-fA-F]+)\s*-\s*1\s*\)\s*/\s*(0[xX][0-9a-fA-F]+)",
     "GROUPS_X"),
]


def statements(lines, first, last, consts, extra=None):
    """The ordered dword writes of a line range of E. Each is (lineno, value) where value is an int or a
    {"sym": NAME} placeholder. `i += N` becomes N zero dwords (the command buffer is memset to 0 at E:595)."""
    out = []
    for n in range(first, last + 1):
        text = re.sub(r"/\*.*?\*/", " ", re.sub(r"//.*$", "", lines[n - 1]))
        text = " ".join(text.split())
        m = ADVANCE.match(text)
        if m:
            out += [(n, 0)] * int(m.group(1))
            continue
        m = WRITE.match(text)
        if not m:
            continue
        out.append((n, evaluate(m.group(1), consts, extra)))
    return out


def evaluate(expr, consts, extra=None):
    expr = " ".join(expr.split())
    if extra and expr in extra:
        return extra[expr]
    m = re.fullmatch(r"PACKET3(_COMPUTE)?\(\s*(\w+)\s*,\s*(\d+)\s*\)", expr)
    if m:
        # E:22-25.  PACKET3(op, n) = (3 << 30) | ((op & 0xFF) << 8) | ((n & 0x3FFF) << 16);
        # PACKET3_COMPUTE additionally sets bit 1, the type-3 header's shader-type bit.
        op, count = consts[m.group(2)], int(m.group(3))
        word = (3 << 30) | ((op & 0xFF) << 8) | ((count & 0x3FFF) << 16)
        return word | (1 << 1) if m.group(1) else word
    for pattern, sym in SYMBOLS:
        m = re.fullmatch(pattern, expr)
        if m:
            return {"sym": sym, "args": [int(a, 0) for a in m.groups()], "c": expr}
    return const_eval(expr, consts)


def regroup(flat):
    """Split a flat dword list into PM4 packets by decoding each type-3 header. Returns a list of
    {header, op, count, shader_type, body, lines}."""
    packets, i = [], 0
    while i < len(flat):
        line, header = flat[i]
        if not isinstance(header, int) or (header >> 30) != 3:
            raise ValueError(f"line {line}: not a type-3 packet header: {header!r}")
        count = (header >> 16) & 0x3FFF
        body = flat[i + 1:i + 2 + count]
        if len(body) != count + 1:
            raise ValueError(f"line {line}: packet runs past the end of the extracted statements")
        packets.append({"header": header, "op": (header >> 8) & 0xFF, "count": count, "flat": i,
                        "shader_type": (header >> 1) & 1, "body": [v for _, v in body],
                        "lines": [line] + [n for n, _ in body]})
        i += 2 + count
    return packets


# ---------------------------------------------------------------- main

def main():
    drm_h = strip_comments(Path(UAPI / "drm.h").read_text(encoding="utf-8", errors="replace"))
    amd_h_raw = Path(UAPI / "amdgpu_drm.h").read_text(encoding="utf-8", errors="replace")
    amd_h = strip_comments(amd_h_raw)
    gfx10_h = Path(LIBDRM / "shader_code_gfx10.h").read_text(encoding="utf-8", errors="replace")
    gfx9_h = Path(LIBDRM / "shader_code_gfx9.h").read_text(encoding="utf-8", errors="replace")
    util_c = Path(LIBDRM / "shader_test_util.c").read_text(encoding="utf-8", errors="replace")
    util_lines = util_c.splitlines()

    # --- A/B: ioctl numbers.  _IOC(dir, type, nr, size) = (dir << 30) | (size << 16) | (type << 8) | nr
    # (asm-generic/ioctl.h, not shipped in drm-uapi; same encoding info.py already uses).
    base = re.search(r"#define\s+DRM_IOCTL_BASE\s+'(.)'", drm_h).group(1)
    command_base = const_eval(re.search(r"#define\s+DRM_COMMAND_BASE\s+(\S+)", drm_h).group(1))
    cmd = defines(UAPI / "amdgpu_drm.h", r"DRM_AMDGPU_\w+")

    types = {}
    for kind, name, source in [
        ("union", "drm_amdgpu_gem_create", amd_h), ("union", "drm_amdgpu_gem_mmap", amd_h),
        ("struct", "drm_amdgpu_gem_va", amd_h), ("union", "drm_amdgpu_ctx", amd_h),
        ("union", "drm_amdgpu_cs", amd_h), ("struct", "drm_amdgpu_cs_chunk", amd_h),
        ("struct", "drm_amdgpu_cs_chunk_ib", amd_h), ("struct", "drm_amdgpu_bo_list_in", amd_h),
        ("struct", "drm_amdgpu_bo_list_entry", amd_h), ("union", "drm_amdgpu_wait_cs", amd_h),
        ("struct", "drm_amdgpu_info", amd_h), ("struct", "drm_amdgpu_info_device", amd_h),
        ("struct", "drm_amdgpu_info_hw_ip", amd_h), ("struct", "drm_gem_close", drm_h),
    ]:
        types[name] = type_layout(source, kind, name)

    def request(nr, name, direction):
        size = types[name]["size"]
        return (direction << 30) | (size << 16) | (ord(base) << 8) | nr

    ioctls = {
        "GEM_CREATE": request(command_base + cmd["DRM_AMDGPU_GEM_CREATE"], "drm_amdgpu_gem_create", 3),
        "GEM_MMAP": request(command_base + cmd["DRM_AMDGPU_GEM_MMAP"], "drm_amdgpu_gem_mmap", 3),
        "CTX": request(command_base + cmd["DRM_AMDGPU_CTX"], "drm_amdgpu_ctx", 3),
        "CS": request(command_base + cmd["DRM_AMDGPU_CS"], "drm_amdgpu_cs", 3),
        "INFO": request(command_base + cmd["DRM_AMDGPU_INFO"], "drm_amdgpu_info", 1),
        "GEM_VA": request(command_base + cmd["DRM_AMDGPU_GEM_VA"], "drm_amdgpu_gem_va", 1),
        "WAIT_CS": request(command_base + cmd["DRM_AMDGPU_WAIT_CS"], "drm_amdgpu_wait_cs", 3),
        "GEM_CLOSE": request(const_eval(re.search(r"#define\s+DRM_IOCTL_GEM_CLOSE\s+DRM_IOW\s*\(\s*(0x[0-9a-fA-F]+)",
                                                  drm_h).group(1)), "drm_gem_close", 1),
    }

    consts = defines(UAPI / "amdgpu_drm.h",
                     r"AMDGPU_(GEM_DOMAIN|GEM_CREATE|VM_PAGE|VM_DELAY|VA_OP|CTX_OP|CHUNK_ID|HW_IP|IB_FLAG|INFO)\w*")

    # --- C/D: the shader and its register table
    shader, shader_lines = array_words(gfx10_h, "bufferclear_cs_shader_gfx10")
    sh_reg_base = const_eval(re.search(r"static\s+const\s+uint32_t\s+sh_reg_base_gfx10\s*=\s*(\S+?)\s*;", gfx10_h).group(1))
    table, table_lines = reg_table(gfx9_h, "bufferclear_cs_shader_registers_gfx9")

    nvd = defines(NVD, r"PACKET3_SET_(SH|UCONFIG)_REG_START")
    uconfig_base = nvd["PACKET3_SET_UCONFIG_REG_START"]
    if nvd["PACKET3_SET_SH_REG_START"] != sh_reg_base:
        raise SystemExit(f"SH reg base disagrees: libdrm 0x{sh_reg_base:x} vs nvd.h 0x{nvd['PACKET3_SET_SH_REG_START']:x}")

    # --- G: regcalc is the authority for every register offset (repo rule 1)
    rm = RegMap()
    by_dword = {}
    for name, (mm, idx) in rm.regs.items():                             # dict order is header order
        by_dword.setdefault(rm.segs.get(idx, 0) + mm, []).append(name)

    # gc_10_1_0_offset.h gives several registers two spellings at the same offset (mmCOMPUTE_STATIC_THREAD_MGMT_SE0
    # and mmCOMPUTE_DESTINATION_EN_SE0 are both 0x1bb6). Prefer the spelling libdrm itself writes in its comments,
    # so the dry output reads like the reference; otherwise take the header's first spelling.
    libdrm_names = set(re.findall(r"\bmm[A-Z0-9_]+\b", util_c))

    def resolve(dword):
        names = by_dword.get(dword, [])
        if not names:
            return None
        for name in names:
            if name in libdrm_names:
                return name
        return names[0]

    # --- E: opcodes, then the packet stream
    opcodes = defines(LIBDRM / "shader_test_util.c", r"PACKET3_\w+|PACKET_TYPE3")
    fn = {name: function_lines(util_lines, name) for name in
          ("amdgpu_dispatch_init_gfx9", "amdgpu_dispatch_init_gfx10", "amdgpu_dispatch_write_cumask",
           "amdgpu_dispatch_write2hw_gfx10", "amdgpu_dispatch_write_dispatch_cmd", "write_context_control")}

    def marker(name, needle, after=None):
        first, last = fn[name]
        for n in range(after or first, last + 1):
            if needle in util_lines[n - 1]:
                return n
        raise SystemExit(f"{name}: marker {needle!r} not found; libdrm source has moved")

    flat = []
    cites = []

    # 1. amdgpu_dispatch_init_gfx9 (E:206). init_gfx9 opens with write_context_control (E:130), whose three
    # dwords are behind `if (ip == AMDGPU_HW_IP_GFX)` and so emit nothing on a compute ring. Confirm that guard
    # is still there rather than assuming it, because dropping it silently would corrupt the stream.
    ctx_first, ctx_last = fn["write_context_control"]
    ctx_body = "\n".join(util_lines[ctx_first - 1:ctx_last])
    if "AMDGPU_HW_IP_GFX" not in ctx_body:
        raise SystemExit("write_context_control no longer skips compute rings; re-read shader_test_util.c")
    if statements(util_lines, ctx_first, ctx_last, consts=opcodes) == []:
        raise SystemExit("write_context_control has no packets at all; libdrm source has moved")
    flat += statements(util_lines, *fn["amdgpu_dispatch_init_gfx9"], consts=opcodes)
    cites.append(f"shader_test_util.c:{fn['amdgpu_dispatch_init_gfx9'][0]}-{fn['amdgpu_dispatch_init_gfx9'][1]}")

    # 2. the gfx10-only tail of amdgpu_dispatch_init_gfx10, after its call to init_gfx9
    call = marker("amdgpu_dispatch_init_gfx10", "amdgpu_dispatch_init_gfx9(test_priv);")
    flat += statements(util_lines, call, fn["amdgpu_dispatch_init_gfx10"][1], consts=opcodes)
    cites.append(f"shader_test_util.c:{call}-{fn['amdgpu_dispatch_init_gfx10'][1]}")

    # 3. the AMDGPU_TEST_GFX_V10 arm of amdgpu_dispatch_write_cumask
    case = marker("amdgpu_dispatch_write_cumask", "case AMDGPU_TEST_GFX_V10:")
    brk = marker("amdgpu_dispatch_write_cumask", "break;", after=case)
    flat += statements(util_lines, case, brk, consts=opcodes)
    cites.append(f"shader_test_util.c:{case}-{brk}")

    # 4. amdgpu_dispatch_write2hw_gfx10, with the sh_reg loop unrolled over D's table and the CS_BUFFERCLEAR arm
    w_first, w_last = fn["amdgpu_dispatch_write2hw_gfx10"]
    loop = marker("amdgpu_dispatch_write2hw_gfx10", "for (j = 0; j < cs_shader->num_sh_reg; j++)")
    loop_end = marker("amdgpu_dispatch_write2hw_gfx10", "}", after=loop + 1)
    if_line = marker("amdgpu_dispatch_write2hw_gfx10", "if (CS_BUFFERCLEAR ==", after=loop_end)
    else_line = marker("amdgpu_dispatch_write2hw_gfx10", "} else {", after=if_line)
    flat += statements(util_lines, w_first, loop - 1, consts=opcodes)
    header = evaluate("PACKET3_COMPUTE(PACKET3_SET_SH_REG, 1)", opcodes)
    unrolled = set()
    for mm_dword, value in table:                                       # E:418-423, one packet per table entry
        unrolled.add(len(flat))
        flat += [(loop, header), (loop + 3, mm_dword - sh_reg_base), (loop + 4, value)]
    flat += statements(util_lines, loop_end + 1, if_line - 1, consts=opcodes)
    flat += statements(util_lines, if_line, else_line - 1, consts=opcodes)
    cites.append(f"shader_test_util.c:{w_first}-{else_line - 1} (CS_BUFFERCLEAR arm, sh_reg loop unrolled)")

    # 5. amdgpu_dispatch_write_dispatch_cmd
    flat += statements(util_lines, *fn["amdgpu_dispatch_write_dispatch_cmd"], consts=opcodes)
    cites.append(f"shader_test_util.c:{fn['amdgpu_dispatch_write_dispatch_cmd'][0]}-{fn['amdgpu_dispatch_write_dispatch_cmd'][1]}")

    packets = regroup(flat)

    # name every register the stream writes, and prove libdrm's literal equals regcalc's offset
    op_name = {v: k for k, v in opcodes.items() if k.startswith("PACKET3_") and not k.endswith("_START")}
    table_cite = f"shader_code_gfx9.h:{table_lines[0]}-{table_lines[1]}"
    for p in packets:
        p["op_name"] = op_name.get(p["op"], f"0x{p['op']:02x}")
        lo, hi = min(p["lines"]), max(p["lines"])
        p["cite"] = f"shader_test_util.c:{lo}" if lo == hi else f"shader_test_util.c:{lo}-{hi}"
        if p["flat"] in unrolled:
            p["cite"] += f" over {table_cite}"
        del p["flat"]
        if p["op_name"] in ("PACKET3_SET_SH_REG", "PACKET3_SET_SH_REG_INDEX", "PACKET3_SET_UCONFIG_REG"):
            raw = p["body"][0]
            if not isinstance(raw, int):
                raise SystemExit("a register offset came out symbolic; libdrm source has moved")
            p["index"] = raw >> 28
            offset = raw & 0x0FFFFFFF
            p["offset"] = offset
            first = uconfig_base if p["op_name"] == "PACKET3_SET_UCONFIG_REG" else sh_reg_base
            p["reg_base"] = first
            p["regs"] = [resolve(first + offset + k) for k in range(len(p["body"]) - 1)]
            p["values"] = p["body"][1:]
        else:
            p["values"] = p["body"]
        del p["body"]

    named = {n: {"dword": p["reg_base"] + p["offset"] + k, "packet_offset": p["offset"] + k,
                 "reg_base": p["reg_base"]}
             for p in packets if "regs" in p for k, n in enumerate(p["regs"]) if n}
    for name, info in named.items():
        mm, idx = rm.regs[name]
        if rm.segs[idx] + mm != info["dword"]:
            raise SystemExit(f"{name}: libdrm says dword 0x{info['dword']:x}, regcalc says 0x{rm.segs[idx] + mm:x}")
        info["mm"] = mm
        info["aliases"] = [a for a in by_dword[info["dword"]] if a != name]

    # the trailing type-3 NOP the test pads the command buffer with (E:620-621)
    pad = re.search(r"while \(i & (\d+)\)\s*\n\s*ptr_cmd\[i\+\+\] = (0x[0-9a-fA-F]+);", util_c)
    pad_align, pad_word = int(pad.group(1)) + 1, int(pad.group(2), 0)
    pad_line = util_c[:pad.start()].count("\n") + 1

    dwords = sum(2 + p["count"] for p in packets)
    out = {
        "generated_by": "experiments/E13-linux-reference-2/gen_dispatch.py",
        "sources": {
            "uapi": str(UAPI).replace("\\", "/"),
            "libdrm": str(LIBDRM).replace("\\", "/"),
            "libdrm_tag": LIBDRM_TAG, "libdrm_commit": libdrm_commit(),
            "nvd_h": str(NVD).replace("\\", "/"),
            "regcalc_headers": str(REPO / "third_party" / "linux-amdgpu").replace("\\", "/"),
        },
        "ioctl": ioctls,
        "types": types,
        "const": consts,
        "shader": {"words": shader, "cite": f"shader_code_gfx10.h:{shader_lines[0]}-{shader_lines[1]}",
                   "symbol": "bufferclear_cs_shader_gfx10", "dwords": len(shader)},
        "reg_table": {"pairs": table, "cite": f"shader_code_gfx9.h:{table_lines[0]}-{table_lines[1]}",
                      "symbol": "bufferclear_cs_shader_registers_gfx9"},
        "pm4": {"sh_reg_base": sh_reg_base, "uconfig_reg_base": uconfig_base,
                "opcodes": {k: v for k, v in opcodes.items() if k.startswith("PACKET3_")},
                "pad_align_dw": pad_align, "pad_word": pad_word,
                "pad_cite": f"shader_test_util.c:{pad_line}-{pad_line + 1}"},
        "registers": named,
        "packets": packets,
        "ib_dwords": dwords,
        "cites": cites,
    }
    target = Path(__file__).resolve().parent / "dispatch.json"
    target.write_text(json.dumps(out, indent=1), encoding="utf-8", newline="\n")
    print(f"{len(packets)} packets, {dwords} dwords, {len(shader)} shader dwords, "
          f"{len(named)} named registers, {len(types)} struct layouts -> {target}")
    for name in sorted(named):
        info = named[name]
        print(f"  {name}: packet offset 0x{info['packet_offset']:03x} mm 0x{info['mm']:04x} "
              f"(base 0x{info['reg_base']:04x})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
