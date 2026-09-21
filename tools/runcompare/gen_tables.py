#!/usr/bin/env python3
"""gen_tables - build runcompare's name tables from the headers, never from memory.

What decides how a log line is named:

    DXGK_QUERYADAPTERINFOTYPE   shared/d3dkmddi.h   (WDK)   type 15 -> PHYSICALADAPTERCAPS
    NTSTATUS                    shared/ntstatus.h   (SDK)   0xC00000BB -> STATUS_NOT_SUPPORTED
    CM_PROB_*                   shared/cfg.h        (SDK)   Code 43 -> CM_PROB_FAILED_POST_START
    BC250_STAGE                 driver/kmd/bc250kmd.h       stage 39 -> StageStartDone

and the bit fields the driver logs as raw words, which get decoded bit by bit:

    DXGK_CREATECONTEXTFLAGS     shared/d3dkmddi.h   flags 0x5 -> SystemContext | VirtualAddressing
    DXGK_CONTEXTINFO_CAPS       shared/d3dkmddi.h   caps 0x1  -> NoPatchingRequired
    DXGK_CREATEDEVICEFLAGS      shared/d3dkmddi.h   flags 0x1 -> SystemDevice
    DXGK_CREATEPROCESSFLAGS     shared/d3dkmddi.h   flags 0x1 -> SystemProcess

Typing any of those by hand is how the previous attempt got its "certainties", so
this script reads them out of the headers and writes tables_generated.py, which is
checked in with the Kit version it came from (the repo spells generated files
`*.generated.*`; Python needs an importable module name, so the dot becomes an
underscore). Run it again after a Kit or driver header change:

    python tools/runcompare/gen_tables.py
    python tools/runcompare/gen_tables.py --kits P:\\BC-250\\toolchain\\nuget --check

--check exits non-zero when the checked-in table no longer matches the headers.

NTSTATUS is not copied whole (ntstatus.h has thousands of values): only the codes a
display miniport hands back or receives, listed in NTSTATUS_WANTED below. Their
values still come from the header. A status the tool meets that is not in the table
is printed as its raw hex, never guessed at.
"""

import argparse
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
DEFAULT_KITS = REPO.parent / "toolchain" / "nuget"
OUT = HERE / "tables_generated.py"

KMD_HEADER = REPO / "driver" / "kmd" / "bc250kmd.h"
KMD_WDDM = REPO / "driver" / "kmd" / "wddm.c"

# The statuses that occur in these logs, plus the neighbours a stage A DDI can return
# (wddm.c returns STATUS_NOT_SUPPORTED, STATUS_INVALID_PARAMETER, STATUS_NO_MEMORY,
# STATUS_BUFFER_TOO_SMALL) and the graphics codes dxgkrnl answers a miniport with.
NTSTATUS_WANTED = [
    "STATUS_SUCCESS",
    "STATUS_BUFFER_OVERFLOW",
    "STATUS_UNSUCCESSFUL",
    "STATUS_NOT_IMPLEMENTED",
    "STATUS_INFO_LENGTH_MISMATCH",
    "STATUS_INVALID_PARAMETER",
    "STATUS_INVALID_DEVICE_REQUEST",
    "STATUS_NO_MEMORY",
    "STATUS_BUFFER_TOO_SMALL",
    "STATUS_INSUFFICIENT_RESOURCES",
    "STATUS_NOT_SUPPORTED",
    "STATUS_INTERNAL_ERROR",
    "STATUS_DEVICE_REMOVED",
    "STATUS_GRAPHICS_INVALID_DRIVER_MODEL",
    "STATUS_GRAPHICS_DRIVER_MISMATCH",
    "STATUS_GRAPHICS_ADAPTER_WAS_RESET",
    "STATUS_GRAPHICS_NO_VIDEO_MEMORY",
]

# The bit-field structures the driver logs as a raw .Value, and which runcompare decodes.
BITFIELDS = [
    ("CREATECONTEXT_FLAGS", "DXGK_CREATECONTEXTFLAGS"),
    ("CONTEXTINFO_CAPS", "DXGK_CONTEXTINFO_CAPS"),
    ("CREATEDEVICE_FLAGS", "DXGK_CREATEDEVICEFLAGS"),
    ("CREATEPROCESS_FLAGS", "DXGK_CREATEPROCESSFLAGS"),
]

_QAI_RE = re.compile(r"^\s*DXGKQAITYPE_(\w+)\s*=\s*(\d+)\s*,")
_TYPEDEF_OPEN_RE = re.compile(r"^\s*typedef\s+(?:struct|union)\s+(_\w+)")
_TYPEDEF_CLOSE_RE = re.compile(r"^\}\s*(\w+)\s*;")
_BITFIELD_RE = re.compile(r"^\s*UINT\s+(\w+)\s*:\s*(\d+)\s*;")
_DEFINE_INT_RE = r"^#define\s+%s\s+(\d+)\b"
_NTSTATUS_RE = re.compile(r"^#define\s+(STATUS_\w+)\s+\(\(NTSTATUS\)(0x[0-9A-Fa-f]+)L?\)")
_CMPROB_RE = re.compile(r"^#define\s+(CM_PROB_\w+)\s+\((0x[0-9A-Fa-f]+)\)")
_STAGE_RE = re.compile(r"^\s*(Stage\w+)\s*=\s*(\d+)\s*,")


def find_header(kits, relative):
    """The newest copy of shared/<name> under the NuGet Kits, with its Kit version.

    Returns (path, label, version). The label is the path relative to the Kits root,
    so that the generated table names its source without writing this machine's
    directory layout into the repository.
    """
    matches = sorted(kits.rglob(relative))
    if not matches:
        raise SystemExit("gen_tables: %s not found under %s" % (relative, kits))
    best = matches[-1]
    version = "unknown"
    for part in best.parts:
        if re.fullmatch(r"10\.\d+\.\d+\.\d+", part):
            version = part
    return best, best.relative_to(kits).as_posix(), version


def read(path):
    return path.read_text(encoding="utf-8", errors="replace").splitlines()


def parse_qai_types(path):
    """{value: NAME} from the DXGK_QUERYADAPTERINFOTYPE enum block only."""
    out, inside = {}, False
    for line in read(path):
        if "_DXGK_QUERYADAPTERINFOTYPE" in line:
            inside = True
            continue
        if inside:
            if line.startswith("}"):
                break
            m = _QAI_RE.match(line)
            if m:
                out[int(m.group(2))] = m.group(1)
    if not out:
        raise SystemExit("gen_tables: no DXGKQAITYPE_* found in %s" % path)
    return out


def parse_ntstatus(path, wanted):
    found = {}
    for line in read(path):
        m = _NTSTATUS_RE.match(line)
        if m and m.group(1) in wanted:
            found.setdefault(int(m.group(2), 16), m.group(1))
    missing = set(wanted) - set(found.values())
    if missing:
        raise SystemExit("gen_tables: not in %s: %s" % (path, ", ".join(sorted(missing))))
    return found


def parse_cm_prob(path):
    out = {}
    for line in read(path):
        m = _CMPROB_RE.match(line)
        if m:
            out.setdefault(int(m.group(2), 16), m.group(1))
    if not out:
        raise SystemExit("gen_tables: no CM_PROB_* found in %s" % path)
    out.setdefault(0, "CM_PROB_NONE")  # cfg.h has no define for "no problem"; the state files print it
    return out


def parse_bitfields(path, typedef_name):
    """{bit: FieldName} for one `union { struct { UINT X : 1; ... }; UINT Value; }`.

    MSVC lays a bit field out from the low bit up, which is what makes the union with
    `UINT Value` work at all, and which d3dkmddi.h states for itself next to
    DXGK_CREATEDEVICEFLAGS (`SystemDevice : 1;  // 0x00000001`).

    Where the header brackets a field with #if DXGKDDI_INTERFACE_VERSION, the first
    branch - the newest interface - is taken, because the value being decoded is the one
    dxgkrnl passed and dxgkrnl is the newest. `bc250kmd.h` compiles the driver at
    DXGKDDI_INTERFACE_VERSION_WDDM2_0, where the later fields are part of Reserved, so a
    bit above the driver's own version is real but invisible to the driver. Reserved
    fields advance the bit position and are not named.
    """
    fields, bit, block, skipping = {}, 0, False, []
    for line in read(path):
        if _TYPEDEF_OPEN_RE.match(line):
            fields, bit, block, skipping = {}, 0, True, []
            continue
        if not block:
            continue
        closed = _TYPEDEF_CLOSE_RE.match(line)
        if closed:
            if closed.group(1) == typedef_name:
                return fields
            block = False
            continue
        stripped = line.strip()
        if stripped.startswith("#if"):
            skipping.append(False)
            continue
        if stripped.startswith("#else"):
            if skipping:
                skipping[-1] = True
            continue
        if stripped.startswith("#endif"):
            if skipping:
                skipping.pop()
            continue
        if any(skipping):
            continue
        m = _BITFIELD_RE.match(line)
        if m:
            name, width = m.group(1), int(m.group(2))
            if name != "Reserved":
                fields[bit] = name
            bit += width
    raise SystemExit("gen_tables: %s not found in %s" % (typedef_name, path))


def parse_define_int(path, name):
    for line in read(path):
        m = re.match(_DEFINE_INT_RE % name, line)
        if m:
            return int(m.group(1))
    raise SystemExit("gen_tables: #define %s not found in %s" % (name, path))


def parse_stages(path):
    out = {}
    for line in read(path):
        m = _STAGE_RE.match(line)
        if m:
            out.setdefault(int(m.group(2)), m.group(1))
    if not out:
        raise SystemExit("gen_tables: no BC250_STAGE values found in %s" % path)
    return out


def render_dict(name, table, formatter):
    lines = ["%s = {" % name]
    for key in sorted(table):
        lines.append("    %s: %r," % (formatter(key), table[key]))
    lines.append("}")
    return "\n".join(lines)


def render(qai, status, prob, stages, bitfields, log_calls, sources):
    head = [
        '"""Generated by tools/runcompare/gen_tables.py - do not edit by hand.',
        "",
        "Regenerate with `python tools/runcompare/gen_tables.py`; `--check` fails when this",
        "file and the headers have drifted apart.",
        "",
        "Sources (paths relative to the NuGet Kits root, resp. to the repository):",
    ]
    for name, where, version in sources:
        head.append("    %-12s %s  (%s)" % (name, where, version))
    head += ['"""', "", "HEADER_SOURCES = ("]
    for name, where, version in sources:
        head.append("    (%r, %r, %r)," % (name, where, version))
    head.append(")")
    body = [
        render_dict("QAI_TYPES", qai, lambda k: str(k)),
        render_dict("NTSTATUS_NAMES", status, lambda k: "0x%08X" % k),
        render_dict("CM_PROB_NAMES", prob, lambda k: str(k)),
        render_dict("STAGE_NAMES", stages, lambda k: str(k)),
    ]
    for name, table in bitfields:
        body.append(render_dict(name, table, lambda k: str(k)))
    body.append("# driver/kmd/wddm.c: how many first calls of each DDI reach the guard log.\n"
                "LOG_CALLS = %d" % log_calls)
    return "\n".join(head) + "\n\n" + "\n\n".join(body) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description="regenerate runcompare's name tables")
    ap.add_argument("--kits", type=Path, default=DEFAULT_KITS, help="NuGet WDK/SDK root")
    ap.add_argument("--check", action="store_true", help="only compare with the checked-in table")
    args = ap.parse_args(argv)

    ddi, ddi_at, ddi_ver = find_header(args.kits, "shared/d3dkmddi.h")
    nts, nts_at, nts_ver = find_header(args.kits, "shared/ntstatus.h")
    cfg, cfg_at, cfg_ver = find_header(args.kits, "shared/cfg.h")

    text = render(
        parse_qai_types(ddi),
        parse_ntstatus(nts, NTSTATUS_WANTED),
        parse_cm_prob(cfg),
        parse_stages(KMD_HEADER),
        [(name, parse_bitfields(ddi, typedef)) for name, typedef in BITFIELDS],
        parse_define_int(KMD_WDDM, "BC250_WDDM_LOG_CALLS"),
        [
            ("d3dkmddi.h", ddi_at, "Kit " + ddi_ver),
            ("ntstatus.h", nts_at, "Kit " + nts_ver),
            ("cfg.h", cfg_at, "Kit " + cfg_ver),
            ("bc250kmd.h", KMD_HEADER.relative_to(REPO).as_posix(), "this repository"),
            ("wddm.c", KMD_WDDM.relative_to(REPO).as_posix(), "this repository"),
        ],
    )

    if args.check:
        current = OUT.read_text(encoding="utf-8") if OUT.exists() else ""
        if current != text:
            print("gen_tables: %s is out of date, rerun without --check" % OUT.name)
            return 1
        print("gen_tables: %s matches the headers" % OUT.name)
        return 0

    OUT.write_text(text, encoding="utf-8", newline="\n")
    print("gen_tables: wrote %s" % OUT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
