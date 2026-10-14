#!/usr/bin/env python3
"""Build driver/kmd/gdi_kernels.cl for gfx1013 and generate driver/kmd/gdi_kernels.generated.h.

The six GDI compute kernels (docs/design/gdi-hw-accel.md, increment 2) ship inside the miniport as
machine code, because DxgkDdiRenderGdi has no compiler and no time for one. This script is the only
way that code enters the driver image, and the header it writes carries three kinds of number:

  the code      the .text bytes of each kernel, as dwords, with each kernel's offset and length
  the kernel    COMPUTE_PGM_RSRC1/2/3 and the thread counts, read out of the AMDHSA kernel
                descriptor the compiler emitted, never chosen here
  the packets   the SET_SH_REG offsets of every COMPUTE_* register the dispatch programs, from
                tools/regcalc, and the DISPATCH_DIRECT initiator bits from AMD's own field masks

Nothing in the header is hand-typed (CLAUDE.md hard rules 1 and 2), and nothing in the driver has to
be: gdi_dispatch.c reads this file and only this file.

The gate (--check) runs with or without the compiler:

  with clang     the kernels are compiled again and the whole header must come out byte for byte
                 the same, which is also the reproducibility check (-Brepro's equivalent here is
                 that two builds of one source give one blob: no path, time or counter enters it)
  without clang  the header must still describe itself - the recorded hash of the source it was
                 built from must be the hash of the source in the tree, and the recorded hash of
                 the code must be the hash of the dwords in the header - so an edited kernel with a
                 stale header fails the gate on any machine

Run:
  python tools/gpu/gdi_kernels.py                 # write the header
  python tools/gpu/gdi_kernels.py --check         # the gate
  python tools/gpu/gdi_kernels.py --clang <path>  # a compiler other than the default search
"""

import argparse
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import textwrap
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]                      # the repository
sys.path.insert(0, str(ROOT / "tools" / "regcalc"))
from regcalc import RegMap                  # noqa: E402

SOURCE = ROOT / "driver/kmd/gdi_kernels.cl"
SHARED = ROOT / "driver/kmd/gdi_pixel.h"    # the kernels' only include, and part of their identity
ENUM = ROOT / "driver/kmd/gdi_render.h"     # the enum that fixes the order of the table below
NVD = ROOT / "driver/amdgpu-import/nvd.h"   # PACKET3_SET_SH_REG_START
MASKS = ROOT / "third_party/linux-amdgpu/gc_10_1_0_sh_mask.h"
ENUMS = ROOT / "third_party/linux-amdgpu/navi10_enum.h"     # VGT_EVENT_TYPE
OUTPUT = ROOT / "driver/kmd/gdi_kernels.generated.h"

# The compiler, and the flags that are part of the artifact's identity. -cl-std=CL1.2 is deliberate
# and not a leftover: OpenCL C 1.2 has no generic address space, so a pointer that should be
# __global cannot silently become a flat one (the arm guard in gdi_pixel.h explains the accident it
# prevents). -nogpulib keeps the device libraries out: these kernels call nothing.
TARGET = "amdgcn-amd-amdhsa"
MCPU = "gfx1013"
CLANG_FLAGS = ["-x", "cl", "-cl-std=CL1.2", "-nogpulib", "-Xclang", "-finclude-default-header",
               f"--target={TARGET}", f"-mcpu={MCPU}", "-O2", "-Wall", "-Werror",
               "-Wl,-m,elf64_amdgpu"]

# What the kernels must be, after compilation. Every one of these is a condition the dispatch
# emitter depends on, and the comment says which part of it.
WAVEFRONT = 32          # the initiator sets CS_W32_EN, and the code uses vcc_lo and a 32-lane exec
GROUP = (8, 8, 1)       # BC250_GDI_GROUP_X/Y of gdi_pixel.h; the emitter programs NUM_THREAD_X/Y/Z
KERNARG_BYTES = 104     # the emitter's kernarg block, which gdi_dispatch.c builds field by field
USER_SGPRS = 6          # s[0:3] private segment buffer (unused, zero), s[4:5] the kernarg pointer
PROPERTIES = 0x0409     # private_segment_buffer | kernarg_segment_ptr | wavefront_size32
METADATA_VERSION = [1, 2]       # code object v5

# The kernarg layout, which is the one contract between gdi_kernels.cl's GDI_ARGS and
# gdi_dispatch.c's BC250_GDI_KERNARG. The compiler's own metadata is held against it, so a reordered
# argument fails the gate instead of painting the screen wrong.
KERNARG = [
    ("dst", 0, 8, "global_buffer"), ("src", 8, 8, "global_buffer"),
    ("alpha", 16, 8, "global_buffer"), ("gamma", 24, 8, "global_buffer"),
    ("dstPitch", 32, 4, "by_value"), ("srcPitch", 36, 4, "by_value"),
    ("alphaPitch", 40, 4, "by_value"), ("width", 44, 4, "by_value"),
    ("height", 48, 4, "by_value"), ("color", 52, 4, "by_value"),
    ("color2", 56, 4, "by_value"), ("rop", 60, 4, "by_value"),
    ("flags", 64, 4, "by_value"), ("sca", 68, 4, "by_value"),
    ("srcLeft", 72, 4, "by_value"), ("srcTop", 76, 4, "by_value"),
    ("dx0", 80, 4, "by_value"), ("dy0", 84, 4, "by_value"),
    ("ws", 88, 4, "by_value"), ("wd", 92, 4, "by_value"),
    ("hs", 96, 4, "by_value"), ("hd", 100, 4, "by_value"),
]

# The COMPUTE_* registers the dispatch programs, by name, in the order gdi_dispatch.c writes them.
# A register that is written as a run of consecutive registers names the length of the run.
#
# COMPUTE_START_X/Y/Z is deliberately absent: the initiator below carries FORCE_START_AT_000, which
# is what RADV does when the grid has no offset (ref/mesa/src/amd/vulkan/radv_cmd_buffer.c, the
# DISPATCH_DIRECT emitter), and a bit in a packet we write cannot be left over from another context.
SH_REGISTERS = [
    ("TMPRING_SIZE", "mmCOMPUTE_TMPRING_SIZE", 1),  # no scratch: the kernels have no private segment
    ("REQ_CTRL", "mmCOMPUTE_REQ_CTRL", 6),
    ("STATIC_THREAD_MGMT_SE0", "mmCOMPUTE_STATIC_THREAD_MGMT_SE0", 2),
    ("STATIC_THREAD_MGMT_SE2", "mmCOMPUTE_STATIC_THREAD_MGMT_SE2", 2),
    ("PGM_LO", "mmCOMPUTE_PGM_LO", 2),              # PGM_LO, PGM_HI: the program address
    ("PGM_RSRC1", "mmCOMPUTE_PGM_RSRC1", 2),        # RSRC1, RSRC2 are consecutive
    ("PGM_RSRC3", "mmCOMPUTE_PGM_RSRC3", 1),
    ("NUM_THREAD_X", "mmCOMPUTE_NUM_THREAD_X", 3),  # X, Y, Z: the work-group shape
    ("USER_DATA_0", "mmCOMPUTE_USER_DATA_0", 6),    # 0..3 zero, 4..5 the kernarg pointer
    ("RESOURCE_LIMITS", "mmCOMPUTE_RESOURCE_LIMITS", 1),
]

# The instructions no kernel may contain, because the dispatch programs neither a scratch
# descriptor (COMPUTE_TMPRING_SIZE is zero and USER_DATA_0..3 are zeros) nor an LDS size, and
# because a call would leave the slice of .text that is copied into the DMA buffer.
FORBIDDEN = re.compile(r"\b(scratch_[a-z0-9_]+|ds_[a-z0-9_]+|flat_[a-z0-9_]+|s_swappc_b64|"
                       r"s_setpc_b64|s_call_b64)\b")


def fail(message):
    sys.exit(f"gdi_kernels: {message}")


# ---------------------------------------------------------------------------------------------
# The compiler
# ---------------------------------------------------------------------------------------------

def find_clang(given):
    """The device compiler: what was asked for, then the environment, then the workspace."""
    if given:
        return Path(given)
    env = os.environ.get("BC250_AMDGPU_CLANG")
    if env:
        return Path(env)
    # The workspace layout the build scripts use: the repository is a directory inside it, and the
    # portable toolchains sit next to it. Newest version first, so a fresher toolchain wins.
    workspace = Path(os.environ.get("BC250_ROOT", ROOT.parent))
    found = sorted((workspace / "toolchain").glob("llvm-amdgpu-*/mingw64/bin/clang.exe"),
                   reverse=True)
    if found:
        return found[0]
    found = sorted((workspace / "toolchain").glob("llvm-amdgpu-*/bin/clang"), reverse=True)
    return found[0] if found else None


def tool(clang, name):
    """An LLVM tool next to clang, by its own name."""
    candidate = clang.with_name(name + clang.suffix)
    if not candidate.exists():
        fail(f"{candidate} is missing: the LLVM tools must sit next to clang")
    return candidate


def run(argv, **kwargs):
    done = subprocess.run([str(a) for a in argv], capture_output=True, text=True, **kwargs)
    if done.returncode != 0:
        fail(f"{Path(str(argv[0])).name} failed ({done.returncode})\n{done.stderr.strip()}")
    return done.stdout


def compile_kernels(clang, work):
    """Compile the kernels and return the code object's path. Nothing is written in the repo."""
    work.mkdir(parents=True, exist_ok=True)
    # clang writes its temporary objects into TEMP, and the project rule is that nothing heavy
    # lands on drive C. The work directory is below the workspace, so point TEMP at it as well.
    env = dict(os.environ, TEMP=str(work), TMP=str(work))
    out = work / "gdi_kernels.elf"
    run([clang] + CLANG_FLAGS + ["-o", out, SOURCE], env=env, cwd=str(work))
    return out


def clang_version(clang):
    first = run([clang, "--version"]).splitlines()[0].strip()
    # "clang version 22.1.8 (https://github.com/... <commit>)": the whole line identifies the build,
    # and the URL in it is upstream's, not this machine's.
    return first


# ---------------------------------------------------------------------------------------------
# The code object
# ---------------------------------------------------------------------------------------------

SHT_RELA, SHT_REL, SHT_SYMTAB = 4, 9, 2


def read_elf(path):
    """Sections and symbols of a 64-bit little-endian ELF. Only what this script needs."""
    data = path.read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        fail(f"{path} is not a 64-bit little-endian ELF")
    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    raw = []
    for i in range(shnum):
        fields = struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize)
        raw.append(fields)
    names = raw[shstrndx]
    strtab = data[names[4]:names[4] + names[5]]

    def name_at(table, offset):
        end = table.index(b"\0", offset)
        return table[offset:end].decode("utf-8")

    sections = {}
    symbols = {}
    for fields in raw:
        nameoff, stype, flags, addr, offset, size = fields[0], fields[1], fields[2], fields[3], fields[4], fields[5]
        section = dict(name=name_at(strtab, nameoff), type=stype, addr=addr, offset=offset, size=size)
        sections[section["name"]] = section
        if stype in (SHT_RELA, SHT_REL) and size:
            fail(f"the code object has relocations in {section['name']}: it cannot be copied as bytes")
        if stype == SHT_SYMTAB:
            link = raw[fields[6]]
            names2 = data[link[4]:link[4] + link[5]]
            for j in range(0, size, fields[9]):
                st_name, st_info, _, st_shndx, st_value, st_size = struct.unpack_from("<IBBHQQ", data, offset + j)
                if st_name:
                    symbols[name_at(names2, st_name)] = dict(value=st_value, size=st_size,
                                                             shndx=st_shndx, info=st_info)
    for want in (".text", ".rodata"):
        if want not in sections:
            fail(f"the code object has no {want}")
    return data, sections, symbols


def metadata(clang, elf):
    """The AMDGPU metadata note, as llvm-readelf prints it: the compiler's own description."""
    import yaml
    text = run([tool(clang, "llvm-readelf"), "--notes", elf])
    marker = "\n        ---\n"
    if marker not in text:
        fail("the code object carries no AMDGPU metadata note")
    body = text[text.index(marker) + len(marker):]
    parsed = yaml.safe_load(body)
    if not isinstance(parsed, dict) or "amdhsa.kernels" not in parsed:
        fail("the AMDGPU metadata note did not parse as a kernel list")
    return parsed


def kernel_descriptor(data, sections, symbols, name):
    """The AMDHSA kernel descriptor of one kernel: the fields the dispatch programs.

    The layout is the one amdhsa.kernel_descriptor of LLVM's AMDGPUUsage.rst documents and the
    assembler emits: 64 bytes, with compute_pgm_rsrc3 at 44, rsrc1 at 48, rsrc2 at 52 and
    kernel_code_properties at 56. The three sizes in front of them are read as well, because a
    kernel that grew a private or group segment must fail the gate and not the lab.
    """
    symbol = symbols.get(name + ".kd")
    if symbol is None:
        fail(f"{name}.kd is not in the code object")
    rodata = sections[".rodata"]
    offset = rodata["offset"] + (symbol["value"] - rodata["addr"])
    if symbol["size"] != 64 or offset + 64 > len(data):
        fail(f"{name}.kd is {symbol['size']} bytes, not the 64 of a kernel descriptor")
    blob = data[offset:offset + 64]
    group, private, kernarg = struct.unpack_from("<III", blob, 0)
    entry, = struct.unpack_from("<q", blob, 16)
    rsrc3, rsrc1, rsrc2 = struct.unpack_from("<III", blob, 44)
    properties, = struct.unpack_from("<H", blob, 56)
    return dict(group=group, private=private, kernarg=kernarg,
                entry=symbol["value"] + entry, rsrc1=rsrc1, rsrc2=rsrc2, rsrc3=rsrc3,
                properties=properties)


def disassembly(clang, elf, names):
    """The six kernels' instructions, by kernel, for the forbidden-instruction check."""
    argv = [tool(clang, "llvm-objdump"), "-d", "--no-show-raw-insn"]
    argv += [f"--disassemble-symbols={n}" for n in names]
    return run(argv + [elf])


# ---------------------------------------------------------------------------------------------
# The derived numbers
# ---------------------------------------------------------------------------------------------

def entry_points():
    """The kernel entry points, in the order of enum bc250_gdi_kernel, read from gdi_render.h.

    The enum is the shipping header and the comment beside each value already names the entry point
    in gdi_kernels.cl, so the order is taken from there rather than repeated here: a new kernel is
    declared once.
    """
    text = ENUM.read_text(encoding="utf-8")
    block = re.search(r"enum bc250_gdi_kernel \{(.*?)\};", text, re.S)
    if not block:
        fail("gdi_render.h has no enum bc250_gdi_kernel")
    names = []
    for line in block.group(1).splitlines():
        found = re.match(r"\s*Bc250GdiKernel(\w+),?\s*(?:/\*\s*(gdi_\w+))?", line)
        if not found or found.group(1) in ("None", "s"):
            continue
        if not found.group(2):
            fail(f"the enum value Bc250GdiKernel{found.group(1)} names no gdi_* entry point")
        names.append(found.group(2))
    if not names:
        fail("no kernel entry points in enum bc250_gdi_kernel")
    return names


def define(path, name):
    """One #define of a C header, as an integer."""
    found = re.search(rf"^#define\s+{re.escape(name)}\s+(0x[0-9a-fA-F]+|\d+)L?\b", path.read_text(encoding="utf-8"),
                      re.M)
    if not found:
        fail(f"{name} is not defined in {path.name}")
    return int(found.group(1), 0)


def enum_value(path, name):
    """One enumerator of an AMD enum header, as an integer."""
    found = re.search(rf"^{re.escape(name)}\s*=\s*(0x[0-9a-fA-F]+|\d+),", path.read_text(encoding="utf-8"),
                      re.M)
    if not found:
        fail(f"{name} is not an enumerator of {path.name}")
    return int(found.group(1), 0)


def sh_offsets():
    """The SET_SH_REG packet offset of every register the dispatch programs.

    A SET_SH_REG names a register by its distance from the packet's own base, which is the
    arithmetic driver/shim/bc250_dispatch.c follows: offset = register dword index -
    PACKET3_SET_SH_REG_START. The dword index comes from tools/regcalc, so no address is typed.
    """
    base = define(NVD, "PACKET3_SET_SH_REG_START")
    regs = RegMap()
    out = []
    for label, register, count in SH_REGISTERS:
        if register not in regs.regs:
            fail(f"regcalc does not know {register}")
        byte = regs.byte_offset(register)
        if byte % 4:
            fail(f"{register} is not dword aligned")
        out.append((label, register, byte // 4 - base, count))
    return out


# ---------------------------------------------------------------------------------------------
# The header
# ---------------------------------------------------------------------------------------------

def source_hash():
    """What the code object was built from: the kernels and the shared arithmetic, in that order."""
    digest = hashlib.sha256()
    for path in (SOURCE, SHARED):
        digest.update(path.read_bytes())
    return digest.hexdigest()


def build(clang, work):
    """Compile and read back everything the header holds. Returns the table and the code dwords."""
    elf = compile_kernels(clang, work)
    data, sections, symbols = read_elf(elf)
    meta = metadata(clang, elf)
    if meta.get("amdhsa.target") != f"{TARGET}--{MCPU}":
        fail(f"the code object is for {meta.get('amdhsa.target')}, not {TARGET}--{MCPU}")
    if list(meta.get("amdhsa.version", [])) != METADATA_VERSION:
        fail(f"metadata version {meta.get('amdhsa.version')}, not {METADATA_VERSION} (code object v5)")

    wanted = entry_points()
    described = {k[".name"]: k for k in meta["amdhsa.kernels"]}
    if sorted(described) != sorted(wanted):
        fail(f"the code object holds {sorted(described)}, the enum names {sorted(wanted)}")

    text = sections[".text"]
    listing = disassembly(clang, elf, wanted)
    found = FORBIDDEN.findall(listing)
    if found:
        fail(f"a kernel uses {sorted(set(found))}: the dispatch programs no scratch, no LDS and no call")

    kernels = []
    code = bytearray()
    for name in wanted:
        described_one = described[name]
        kd = kernel_descriptor(data, sections, symbols, name)
        symbol = symbols.get(name)
        if symbol is None or symbol["shndx"] == 0:
            fail(f"{name} is not a defined symbol")
        if kd["entry"] != symbol["value"]:
            fail(f"{name}: the descriptor's entry 0x{kd['entry']:x} is not the symbol 0x{symbol['value']:x}")
        for label, value, want in (
                ("group segment", kd["group"], 0),
                ("private segment", kd["private"], 0),
                ("kernarg size", kd["kernarg"], KERNARG_BYTES),
                ("compute_pgm_rsrc3", kd["rsrc3"], 0),
                ("kernel_code_properties", kd["properties"], PROPERTIES),
                ("group segment (metadata)", described_one[".group_segment_fixed_size"], 0),
                ("private segment (metadata)", described_one[".private_segment_fixed_size"], 0),
                ("kernarg size (metadata)", described_one[".kernarg_segment_size"], KERNARG_BYTES),
                ("wavefront size", described_one[".wavefront_size"], WAVEFRONT),
                ("sgpr spills", described_one[".sgpr_spill_count"], 0),
                ("vgpr spills", described_one[".vgpr_spill_count"], 0),
                ("dynamic stack", 1 if described_one[".uses_dynamic_stack"] else 0, 0)):
            if value != want:
                fail(f"{name}: {label} is {value}, not {want}")
        if list(described_one.get(".reqd_workgroup_size", [])) != list(GROUP):
            fail(f"{name}: the work-group shape is {described_one.get('.reqd_workgroup_size')}, not {list(GROUP)}")
        # COMPUTE_PGM_RSRC2.USER_SGPR, the field that decides where the kernarg pointer lands.
        user = (kd["rsrc2"] >> 1) & 0x1F
        if user != USER_SGPRS:
            fail(f"{name}: {user} user SGPRs, not {USER_SGPRS}: the kernarg pointer would not be s[4:5]")
        args = described_one.get(".args", [])
        if len(args) != len(KERNARG):
            fail(f"{name}: {len(args)} arguments, not {len(KERNARG)}")
        for arg, (label, offset, size, kind) in zip(args, KERNARG):
            if arg[".offset"] != offset or arg[".size"] != size or arg[".value_kind"] != kind:
                fail(f"{name}: argument {label} is {arg['.value_kind']} {arg['.size']}@{arg['.offset']}, "
                     f"not {kind} {size}@{offset}")

        start = text["offset"] + (symbol["value"] - text["addr"])
        length = symbol["size"]
        if symbol["value"] % 256 or length == 0 or length % 4:
            fail(f"{name}: {length} bytes at 0x{symbol['value']:x}, which is not a whole dword count "
                 f"at a 256-byte boundary")
        if len(code) % 256:
            fail("internal: a kernel would not start at a 256-byte boundary")
        kernels.append(dict(name=name, offset=len(code), bytes=length,
                            rsrc1=kd["rsrc1"], rsrc2=kd["rsrc2"], properties=kd["properties"],
                            vgprs=described_one[".vgpr_count"], sgprs=described_one[".sgpr_count"]))
        code += data[start:start + length]
        # COMPUTE_PGM_LO is the program address shifted right by 8, so every kernel must begin at a
        # 256-byte boundary of the DMA buffer. Padding each slice keeps that true for all six from
        # one aligned base, and the padding is never executed.
        while len(code) % 256:
            code += b"\0"
    return kernels, bytes(code), clang_version(clang)


def render(kernels, code, compiler):
    """The generated header, as text. Deterministic: nothing but the inputs enters it."""
    dwords = struct.unpack(f"<{len(code) // 4}I", code)
    lines = [
        "/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */",
        "#ifndef BC250_GDI_KERNELS_GENERATED_H",
        "#define BC250_GDI_KERNELS_GENERATED_H",
        "/*",
        " * GENERATED by tools/gpu/gdi_kernels.py from driver/kmd/gdi_kernels.cl. Do not edit: the gate",
        " * builds it again and compares, and without a compiler it still checks the two hashes below.",
        " *",
        " * The code is machine code for gfx1013, one slice for each kernel, each slice starting at a",
        " * 256-byte boundary because COMPUTE_PGM_LO carries the program address shifted right by 8.",
        " * COMPUTE_PGM_RSRC1 and RSRC2 are the compiler's own, read out of the kernel descriptor it",
        " * emitted; the SET_SH_REG offsets are from tools/regcalc and the initiator bits from AMD's",
        " * field masks, so nothing here was typed by hand (CLAUDE.md hard rules 1 and 2).",
        " */",
        "",
        f"#define BC250_GDI_CODE_TARGET \"{TARGET}--{MCPU}\"",
        f"#define BC250_GDI_CODE_COMPILER \"{compiler}\"",
        f"#define BC250_GDI_CODE_FLAGS \"{' '.join(CLANG_FLAGS)}\"",
        f"#define BC250_GDI_CODE_SOURCE_SHA256 \"{source_hash()}\"",
        f"#define BC250_GDI_CODE_SHA256 \"{hashlib.sha256(code).hexdigest()}\"",
        "",
        "/* The whole blob, and the alignment every slice inside it needs. */",
        f"#define BC250_GDI_CODE_DWORDS {len(dwords)}u",
        "#define BC250_GDI_CODE_ALIGN 256u",
        f"#define BC250_GDI_KERNARG_BYTES {KERNARG_BYTES}u",
        f"#define BC250_GDI_KERNEL_COUNT {len(kernels)}u",
        "",
    ]

    lines += ["/* The kernel argument block, in dwords, as the compiler laid it out: this is the one",
              " * contract between gdi_kernels.cl's GDI_ARGS and the block gdi_dispatch.c writes, and the",
              " * build holds both against the code object's own metadata. */"]
    width = max(len(label) for label, _, _, _ in KERNARG)
    for label, offset, size, kind in KERNARG:
        lines.append(f"#define BC250_GDI_ARG_{label.upper():<{width}} {offset // 4}u"
                     f"  /* {size} bytes, {kind} */")
    lines.append("")

    lines += ["/* The SET_SH_REG packet offsets: register dword index - PACKET3_SET_SH_REG_START. */"]
    width = max(len(label) for label, _, _, _ in sh_offsets())
    for label, register, offset, count in sh_offsets():
        run_of = f", {count} consecutive" if count > 1 else ""
        lines.append(f"#define BC250_GDI_SH_{label:<{width}} 0x{offset:04X}u"
                     f"  /* {register}{run_of} */")
    lines.append("")

    initiator = 0
    for field in ("COMPUTE_SHADER_EN", "CS_W32_EN", "FORCE_START_AT_000"):
        initiator |= define(MASKS, f"COMPUTE_DISPATCH_INITIATOR__{field}_MASK")
    event = enum_value(ENUMS, "CS_PARTIAL_FLUSH")
    lines += [
        "/* COMPUTE_DISPATCH_INITIATOR, from gc_10_1_0_sh_mask.h: start the shader, launch it in",
        " * wave32 because that is the wavefront size the compiler built these kernels for, and start",
        " * at (0,0,0) rather than depend on COMPUTE_START_X/Y/Z, which this driver never writes. */",
        f"#define BC250_GDI_DISPATCH_INITIATOR 0x{initiator:08X}u",
        "",
        "/* VGT_EVENT_TYPE of the flush behind the dispatch, from navi10_enum.h. The index that goes",
        " * with it is PACKET3_EVENT_WRITE__EVENT_INDEX__CS_PARTIAL_FLUSH of nvd.h, which C code can",
        " * use as it stands. */",
        f"#define BC250_GDI_EVENT_CS_PARTIAL_FLUSH {event}u",
        "",
        "/* One kernel of the blob. The index is the value of enum bc250_gdi_kernel, so slot 0 (None)",
        " * is an empty record and no caller has to subtract one. */",
        "typedef struct BC250_GDI_CODE_KERNEL {",
        "    unsigned int Offset;        /* dwords into g_Bc250GdiCode */",
        "    unsigned int Dwords;",
        "    unsigned int Rsrc1, Rsrc2;  /* COMPUTE_PGM_RSRC1 and RSRC2, from the kernel descriptor */",
        "    unsigned int Properties;    /* kernel_code_properties, which places the kernarg pointer */",
        "} BC250_GDI_CODE_KERNEL;",
        "",
        f"static const BC250_GDI_CODE_KERNEL g_Bc250GdiCodeKernels[BC250_GDI_KERNEL_COUNT + 1u] = {{",
        "    { 0u, 0u, 0u, 0u, 0u },      /* Bc250GdiKernelNone */",
    ]
    for kernel in kernels:
        lines.append(f"    {{ {kernel['offset'] // 4}u, {kernel['bytes'] // 4}u, "
                     f"0x{kernel['rsrc1']:08X}u, 0x{kernel['rsrc2']:08X}u, 0x{kernel['properties']:04X}u }},"
                     f"  /* {kernel['name']}: {kernel['bytes']} bytes, "
                     f"{kernel['vgprs']} VGPRs, {kernel['sgprs']} SGPRs */")
    lines += ["};", "",
              "/* The entry points, in the same order, for a test that has to agree with it. A macro and",
              " * not an array, so the driver image carries no string it never prints. */",
              "#define BC250_GDI_CODE_NAME_LIST { 0, " +
              ", ".join(f"\"{k['name']}\"" for k in kernels) + " }",
              "",
              "static const unsigned int g_Bc250GdiCode[BC250_GDI_CODE_DWORDS] = {"]
    for i in range(0, len(dwords), 6):
        lines.append("    " + " ".join(f"0x{d:08X}u," for d in dwords[i:i + 6]))
    lines += ["};", "", "#endif", ""]
    return "\n".join(lines)


def self_check(text):
    """The offline arm of the gate: the header must describe the source and the code it carries."""
    def one(name):
        found = re.search(rf'#define {name} "([0-9a-f]{{64}})"', text)
        if not found:
            fail(f"the generated header has no {name}")
        return found.group(1)

    want_source = source_hash()
    if one("BC250_GDI_CODE_SOURCE_SHA256") != want_source:
        fail("the generated header was built from a different gdi_kernels.cl or gdi_pixel.h "
             f"(the source in the tree hashes to {want_source}): run tools/gpu/gdi_kernels.py")
    body = re.search(r"g_Bc250GdiCode\[BC250_GDI_CODE_DWORDS\] = \{(.*?)\n\};", text, re.S)
    if not body:
        fail("the generated header has no g_Bc250GdiCode table")
    dwords = [int(d, 16) for d in re.findall(r"0x([0-9A-F]{8})u,", body.group(1))]
    count = re.search(r"#define BC250_GDI_CODE_DWORDS (\d+)u", text)
    if not count or int(count.group(1)) != len(dwords):
        fail("the generated header's dword count does not match its own table")
    code = struct.pack(f"<{len(dwords)}I", *dwords)
    if one("BC250_GDI_CODE_SHA256") != hashlib.sha256(code).hexdigest():
        fail("the generated header's code does not hash to the value it records")
    return len(dwords)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--clang", help="the device compiler (default: the workspace toolchain)")
    parser.add_argument("--work", help="where to build (default: <workspace>/scratch/gdi-kernels)")
    parser.add_argument("--check", action="store_true", help="the gate: do not write anything")
    parser.add_argument("--out", default=str(OUTPUT))
    args = parser.parse_args()

    out = Path(args.out)
    clang = find_clang(args.clang)
    work = Path(args.work) if args.work else \
        Path(os.environ.get("BC250_ROOT", ROOT.parent)) / "scratch/gdi-kernels"

    if args.check:
        if not out.exists():
            fail(f"{out} is missing: run tools/gpu/gdi_kernels.py")
        text = out.read_text(encoding="utf-8")
        dwords = self_check(text)
        if clang is None or not Path(clang).exists():
            print(f"gdi_kernels: header self-check PASS ({dwords} dwords, no device compiler here)")
            return
        kernels, code, compiler = build(Path(clang), work)
        again = render(kernels, code, compiler)
        if again != text:
            fail(f"{out} is not what the compiler produces now: regenerate it "
                 f"(a differing compiler version is enough, and then the version must be recorded)")
        print(f"gdi_kernels: PASS, {len(code)} bytes of gfx1013 code in {len(kernels)} kernels, "
              f"rebuilt byte for byte")
        return

    if clang is None or not Path(clang).exists():
        fail("no device compiler: pass --clang, or set BC250_AMDGPU_CLANG")
    kernels, code, compiler = build(Path(clang), work)
    text = render(kernels, code, compiler)
    out.write_text(text, encoding="utf-8", newline="\n")
    self_check(text)
    print(f"{out.name}: {len(code)} bytes of gfx1013 code, " +
          ", ".join(f"{k['name']} {k['bytes']}B" for k in kernels))


if __name__ == "__main__":
    main()
