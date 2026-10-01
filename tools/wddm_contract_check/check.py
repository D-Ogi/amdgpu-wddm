#!/usr/bin/env python3
"""Static checker for the WDDM start-up contract of driver/kmd/wddm.c.

Four lab runs of E16 (2026-09-21) were spent finding start-up contract violations one at a time, each
costing a driver install, a device restart and a trip back to the display-only driver. Every one of them
was visible in the source: a cap declared without the DDIs that cap obliges, a DDI left NULL next to the
one dxgkrnl tests it with, a package without a user-mode driver name. This reads the tree instead.

What it extracts from driver/kmd/wddm.c:
  - the DXGK_QUERYADAPTERINFOTYPE values the QueryAdapterInfo switch handles, and what the default arm
    answers for everything else;
  - every DXGK_DRIVERCAPS member WddmDriverCaps() assigns, with its value;
  - the segment descriptor flags WddmQuerySegment4() sets;
  - the DXGK_GPUMMUCAPS and DXGK_PAGE_TABLE_LEVEL_DESC members;
  - every DRIVER_INITIALIZATION_DATA member WddmBuildTable() assigns, and by omission the ones it leaves
    NULL;
  - the buffer-size guard of each handler.
And from driver/kmd/bc250kmd.inf plus driver/kmd/build.ps1: whether a package with UserModeDriverName
can be produced at all.

Those facts are then run against rules.json, where every rule carries the source of its obligation: a
reading of the lab's own dxgkrnl, a Microsoft documentation page, or a file:line in a Microsoft sample
driver. A rule without a source does not belong in that file.

Usage:
    python tools/wddm_contract_check/check.py [--repo <path>] [--markdown] [--inventory] [--json]
    python tools/wddm_contract_check/check.py --wddm <file> --inf <file> --build <file>

Exit code: 0 when no rule is violated, 1 when one is, 2 when the tree could not be read.
"""

from __future__ import annotations

import argparse
import ast
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_REPO = os.path.abspath(os.path.join(HERE, "..", ".."))

# DXGK_QUERYADAPTERINFOTYPE, from the WDK's d3dkmddi.h. Only the names that can appear at or below
# WDDM 2.0 plus the two the lab's dxgkrnl really asked for (15 and 47, E16 run 003).
QAI_TYPES = {
    "DXGKQAITYPE_UMDRIVERPRIVATE": 0,
    "DXGKQAITYPE_DRIVERCAPS": 1,
    "DXGKQAITYPE_QUERYSEGMENT": 2,
    "DXGKQAITYPE_RESERVED": 3,
    "DXGKQAITYPE_QUERYSEGMENT2": 4,
    "DXGKQAITYPE_QUERYSEGMENT3": 5,
    "DXGKQAITYPE_NUMPOWERCOMPONENTS": 6,
    "DXGKQAITYPE_POWERCOMPONENTINFO": 7,
    "DXGKQAITYPE_PREFERREDGPUNODE": 8,
    "DXGKQAITYPE_POWERCOMPONENTPSTATEINFO": 9,
    "DXGKQAITYPE_HISTORYBUFFERPRECISION": 10,
    "DXGKQAITYPE_QUERYSEGMENT4": 11,
    "DXGKQAITYPE_SEGMENTMEMORYSTATE": 12,
    "DXGKQAITYPE_GPUMMUCAPS": 13,
    "DXGKQAITYPE_PAGETABLELEVELDESC": 14,
    "DXGKQAITYPE_PHYSICALADAPTERCAPS": 15,
    "DXGKQAITYPE_DISPLAY_DRIVERCAPS_EXTENSION": 16,
    "DXGKQAITYPE_64BITONLYCAPS": 47,
}

# DXGKDDI_INTERFACE_VERSION_* values that matter to the rules (d3dukmdt.h).
INTERFACE_VERSIONS = {
    "DXGKDDI_INTERFACE_VERSION_WIN8": 0x300E,
    "DXGKDDI_INTERFACE_VERSION_WDDM1_3": 0x4002,
    "DXGKDDI_INTERFACE_VERSION_WDDM2_0": 0x5023,
    "DXGKDDI_INTERFACE_VERSION_WDDM2_1": 0x6003,
    "DXGKDDI_INTERFACE_VERSION_WDDM2_2": 0x700A,
    "DXGKDDI_INTERFACE_VERSION_WDDM2_3": 0x8001,
    "DXGKDDI_INTERFACE_VERSION_WDDM2_4": 0x9006,
    "DXGKDDI_INTERFACE_VERSION_WDDM2_5": 0xA00B,
    "DXGKDDI_INTERFACE_VERSION_WDDM2_6": 0xB004,
    "DXGKDDI_INTERFACE_VERSION_WDDM2_7": 0xC004,
    "DXGKDDI_INTERFACE_VERSION_WDDM2_8": 0xD001,
    "DXGKDDI_INTERFACE_VERSION_WDDM2_9": 0xE003,
    "DXGKDDI_INTERFACE_VERSION_WDDM3_0": 0xF003,
    "DXGKDDI_INTERFACE_VERSION_WDDM3_1": 0x10004,
    "DXGKDDI_INTERFACE_VERSION_WDDM3_2": 0x11007,
}

WDDM_VERSIONS = {
    "DXGKDDI_WDDMv1_2": 0x1200,
    "DXGKDDI_WDDMv1_3": 0x1300,
    "DXGKDDI_WDDMv2": 0x2000,
    "DXGKDDI_WDDMv2_0": 0x2000,
}


# --------------------------------------------------------------------------------------------------
# reading the source
# --------------------------------------------------------------------------------------------------

def strip_comments(text: str) -> str:
    """Block and line comments out, newlines kept so that line numbers survive."""
    out = []
    i, n = 0, len(text)
    while i < n:
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.append("".join(c if c == "\n" else " " for c in text[i:end]))
            i = end
        elif text.startswith("//", i):
            end = text.find("\n", i)
            end = n if end < 0 else end
            out.append(" " * (end - i))
            i = end
        elif text[i] in "\"'":
            quote = text[i]
            j = i + 1
            while j < n and text[j] != quote:
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(text[i:j])
            i = j
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def function_body(text: str, name: str):
    """(body, first line number) of the function called `name`, found by brace matching.

    The declarations wddm.c puts in front of every DDI ("static DXGKDDI_PRESENT Bc250WddmPresent;") are
    skipped: a body is the first occurrence of the name followed by a parameter list and a brace.
    """
    for match in re.finditer(r"\b" + re.escape(name) + r"\s*\(", text):
        depth, i, n = 0, match.end() - 1, len(text)
        while i < n and text[i] != "{":
            if text[i] == ")" and depth == 0:
                pass
            if text[i] == ";":
                break
            i += 1
        if i >= n or text[i] != "{":
            continue
        start = i
        depth = 0
        while i < n:
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
                if depth == 0:
                    return text[start:i + 1], text.count("\n", 0, match.start()) + 1
            i += 1
    return None, None


def line_of(text: str, index: int) -> int:
    return text.count("\n", 0, index) + 1


class Constants:
    """The #defines of wddm.c and bc250kmd.h, folded so that a value like BC250_WDDM_VA_BITS prints as
    a number instead of as a name. Only integer arithmetic is folded; anything else stays a string."""

    def __init__(self, *texts: str):
        self.values = {}
        self.macros = {}
        for text in texts:
            if not text:
                continue
            for name, body in re.findall(r"^\s*#define\s+([A-Za-z_][A-Za-z0-9_]*)\s+(.+?)\s*$",
                                         text, re.MULTILINE):
                if "(" in name:
                    continue
                self.values[name] = body.strip()
            # Function-like macros, one level, no nesting of a call inside its own argument. Stage B's
            # BC250_WDDM_SEGMENT_SET(id) is one, and a rule that cannot fold it reads as "cannot tell".
            for name, params, body in re.findall(
                    r"^\s*#define\s+([A-Za-z_][A-Za-z0-9_]*)\(([^)]*)\)\s+(.+?)\s*$", text, re.MULTILINE):
                self.macros[name] = ([p.strip() for p in params.split(",") if p.strip()], body.strip())

    def expand_macros(self, expression: str):
        for _ in range(4):
            changed = False
            for name, (params, body) in self.macros.items():
                match = re.search(r"\b" + re.escape(name) + r"\(([^()]*(?:\([^()]*\)[^()]*)*)\)", expression)
                if not match:
                    continue
                args = [a.strip() for a in match.group(1).split(",")]
                if len(args) != len(params):
                    continue
                text = body
                for param, arg in zip(params, args):
                    text = re.sub(r"\b" + re.escape(param) + r"\b", "(" + arg + ")", text)
                expression = expression[:match.start()] + "(" + text + ")" + expression[match.end():]
                changed = True
            if not changed:
                break
        return expression

    def fold_all(self, expression: str):
        """Every value the expression can take, folded. A ternary answers one of two things and both have
        to be legal, so both arms are returned; anything that will not fold comes back as None."""
        expression = expression.strip().rstrip(";").strip()
        match = re.match(r"^[^?]*\?(.*):(.*)$", expression, re.DOTALL)
        if match:
            return [self.fold(match.group(1)), self.fold(match.group(2))]
        return [self.fold(expression)]

    def fold(self, expression: str):
        expression = self.expand_macros(expression.strip().rstrip(";").strip())
        seen = set()
        for _ in range(8):
            names = set(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", expression))
            todo = {n for n in names if n in self.values and n not in seen}
            if not todo:
                break
            for name in todo:
                seen.add(name)
                expression = re.sub(r"\b" + re.escape(name) + r"\b",
                                    "(" + self.values[name] + ")", expression)
        return evaluate_int(expression)


def evaluate_int(expression: str):
    """Integer value of a C constant expression, or None. ast.parse on a whitelist of nodes: no eval,
    no names, no calls."""
    cleaned = re.sub(r"\b(0[xX][0-9a-fA-F]+|\d+)[uUlL]+\b", lambda m: m.group(1), expression)
    cleaned = cleaned.replace("TRUE", "1").replace("FALSE", "0")
    if re.search(r"[A-Za-z_]", cleaned):
        return None
    try:
        tree = ast.parse(cleaned, mode="eval")
    except SyntaxError:
        return None
    allowed = (ast.Expression, ast.BinOp, ast.UnaryOp, ast.Constant, ast.Add, ast.Sub, ast.Mult,
               ast.Div, ast.FloorDiv, ast.Mod, ast.LShift, ast.RShift, ast.BitOr, ast.BitAnd,
               ast.BitXor, ast.USub, ast.UAdd, ast.Invert)
    for node in ast.walk(tree):
        if not isinstance(node, allowed):
            return None
    try:
        value = eval(compile(tree, "<const>", "eval"), {"__builtins__": {}}, {})
    except Exception:
        return None
    return value if isinstance(value, int) else None


def assignments(body: str, prefix: str, sep="->"):
    """{member: (raw value, line offset within the body)} for `prefix->member = value;` and
    `prefix->member.sub = value;`.

    sep is what joins the prefix to the member: "->" for a pointer (`caps->X`), "." for a structure
    reached through one (`pCreateContext->ContextInfo.X`, where the prefix already carries the arrow).

    The lookarounds around the `=` keep comparisons out: `if (out->pSegmentDescriptor == NULL) return
    STATUS_INVALID_PARAMETER;` is a guard, not an answer, and reading it as one put a member named
    "pSegmentDescriptor" with the value "= NULL) return STATUS_INVALID_PARAMETER" into the inventory.
    """
    found = {}
    pattern = re.compile(re.escape(prefix) + re.escape(sep) +
                         r"([A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*)"
                         r"\s*(?<![=!<>+\-*/&|^])=(?!=)\s*([^;]+);")
    for match in pattern.finditer(body):
        found[match.group(1)] = (match.group(2).strip(), line_of(body, match.start()))
    return found


class Facts:
    """Everything the rules are evaluated against."""

    def __init__(self):
        self.source = {}                # file -> path, for the report
        self.qai_handled = {}           # type value -> (name, handler, line)
        self.qai_default = None
        self.caps = {}                  # DXGK_DRIVERCAPS member -> {"raw", "value", "line"}
        self.segments = []              # one entry per DXGK_SEGMENTDESCRIPTOR4 filled, in declaration order
        self.segment_flags = {}         # segment 1's flags: the scanned-out one
        self.segment = {}               # segment 1's members other than Flags
        self.segment_out = {}           # DXGK_QUERYSEGMENTOUT4 members
        self.gpummu = {}
        self.pagetable = {}
        self.ddis = {}                  # DRIVER_INITIALIZATION_DATA member -> {"value", "line"}
        self.ctx = {}                   # DXGK_CONTEXTINFO member -> {"raw", "value", "line"}
        self.ctx_flags_read = set()     # DXGK_CREATECONTEXTFLAGS members the handler looks at
        self.ctx_node_checked = False   # NodeOrdinal is bounds-checked
        self.size_checks = {}           # handler -> the sizeof() it guards with
        self.interface_version = None
        self.table_version = None
        self.inf_umd_active = False
        self.inf_umd_marked = False
        self.build_generates_umd = False
        self.node_count = None
        self.notes = []
        self.wddm_lines = 0

    def cap(self, name):
        return self.caps.get(name)

    def cap_value(self, name):
        entry = self.caps.get(name)
        return None if entry is None else entry["value"]


def read_file(path):
    if not path or not os.path.exists(path):
        return ""
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        return handle.read()


def read_facts(wddm_path, header_path, inf_path, build_path):
    """The tree on disk. The work is done by read_facts_text, which the unit tests drive with fixture
    strings instead of files."""
    facts = read_facts_text(read_file(wddm_path), read_file(header_path),
                            read_file(inf_path), read_file(build_path))
    facts.source["wddm.c"] = wddm_path
    if header_path and os.path.exists(header_path):
        facts.source["bc250kmd.h"] = header_path
    if inf_path and os.path.exists(inf_path):
        facts.source["bc250kmd.inf"] = inf_path
    if build_path and os.path.exists(build_path):
        facts.source["build.ps1"] = build_path
    return facts


def read_facts_text(wddm_text, header_text="", inf_text="", build_text=""):
    facts = Facts()
    raw = wddm_text
    facts.wddm_lines = raw.count("\n") + 1
    text = strip_comments(raw)

    header = strip_comments(header_text) if header_text else ""
    constants = Constants(text, header)

    match = re.search(r"#define\s+DXGKDDI_INTERFACE_VERSION\s+(0[xX][0-9a-fA-F]+)", header or text)
    if match:
        facts.interface_version = int(match.group(1), 16)

    # (a) the QueryAdapterInfo switch
    body, base = function_body(text, "Bc250WddmQueryAdapterInfo")
    if body:
        for match in re.finditer(r"case\s+(DXGKQAITYPE_[A-Z0-9_]+)\s*:(.*?)(?=case\s+DXGKQAITYPE_|default\s*:)",
                                 body, re.DOTALL):
            name = match.group(1)
            arm = match.group(2)
            handler = re.search(r"status\s*=\s*(Wddm[A-Za-z0-9_]+)\s*\(", arm)
            facts.qai_handled[QAI_TYPES.get(name, -1)] = {
                "name": name,
                "handler": handler.group(1) if handler else "inline",
                "line": base + line_of(body, match.start()) - 1,
            }
        default = re.search(r"default\s*:(.*?)break\s*;", body, re.DOTALL)
        if default:
            status = re.search(r"status\s*=\s*(STATUS_[A-Z_]+)", default.group(1))
            facts.qai_default = status.group(1) if status else "unknown"

    # (b) DXGK_DRIVERCAPS
    body, base = function_body(text, "WddmDriverCaps")
    if body:
        for member, (value, line) in assignments(body, "caps").items():
            facts.caps[member] = {"raw": value, "value": constants.fold(value), "line": base + line - 1}
        match = re.search(r"OutputDataSize\s*<\s*sizeof\(([^)]*)\)", body)
        facts.size_checks["WddmDriverCaps"] = match.group(1) if match else None
        facts.node_count = facts.cap_value("GpuEngineTopology.NbAsymetricProcessingNodes")

    # (c) the segment descriptors. Stage B added a second one, so they are kept apart: the handler walks
    # a single `descriptor` pointer and re-aims it, and merging the two would make the aperture segment's
    # Flags.Aperture look like the scanned-out segment's. Each re-aiming starts a new segment; a chunk
    # that only reads (the logging tail does) contributes nothing and is dropped.
    body, base = function_body(text, "WddmQuerySegment4")
    if body:
        offset = 0
        for index, chunk in enumerate(re.split(r"descriptor\s*=\s*\(DXGK_SEGMENTDESCRIPTOR4", body)):
            if index > 0:
                members = assignments(chunk, "descriptor")
                if members:
                    segment = {"flags": {}, "members": {}, "id": len(facts.segments) + 1}
                    for member, (value, line) in members.items():
                        entry = {"raw": value, "value": constants.fold(value),
                                 "line": base + line_of(body, offset) + line - 2}
                        if member.startswith("Flags."):
                            segment["flags"][member[len("Flags."):]] = entry
                        else:
                            segment["members"][member] = entry
                    facts.segments.append(segment)
            offset += len(chunk)
        # The scanned-out segment is segment 1, and the rules that talk about "the segment" mean that one:
        # it is what DirectFlip, Agp and the CPU host aperture are about.
        if facts.segments:
            facts.segment_flags = facts.segments[0]["flags"]
            facts.segment = facts.segments[0]["members"]
        for member, (value, line) in assignments(body, "out").items():
            facts.segment_out[member] = {
                "raw": value, "value": constants.fold(value), "line": base + line - 1}
        match = re.search(r"OutputDataSize\s*<\s*sizeof\(([^)]*)\)", body)
        facts.size_checks["WddmQuerySegment4"] = match.group(1) if match else None

    for function, prefix, store in (("WddmGpuMmuCaps", "caps", "gpummu"),
                                    ("WddmPageTableLevelDesc", "desc", "pagetable")):
        body, base = function_body(text, function)
        if not body:
            continue
        target = getattr(facts, store)
        for member, (value, line) in assignments(body, prefix).items():
            target[member] = {"raw": value, "value": constants.fold(value), "line": base + line - 1}
        match = re.search(r"OutputDataSize\s*<\s*sizeof\(([^)]*)\)", body)
        facts.size_checks[function] = match.group(1) if match else None

    # (d) the table
    body, base = function_body(text, "WddmBuildTable")
    if body:
        for member, (value, line) in assignments(body, "Data").items():
            facts.ddis[member] = {"value": value, "line": base + line - 1}
        facts.table_version = facts.ddis.get("Version", {}).get("value")

    # (e) DXGK_CONTEXTINFO, the second answer with a contract of its own. E16 run 004 died one call
    # after CreateContext, so what it answers is read here the same way DRIVERCAPS is.
    body, base = function_body(text, "Bc250WddmCreateContext")
    if body:
        for member, (value, line) in assignments(body, "pCreateContext->ContextInfo", ".").items():
            facts.ctx[member] = {"raw": value, "value": constants.fold(value),
                                 "values": constants.fold_all(value), "line": base + line - 1}
        # Which of the incoming flags the answer is allowed to depend on: an answer that never reads
        # Flags.GdiContext cannot be keeping the GdiContext obligation, whatever it returns.
        facts.ctx_flags_read = set(re.findall(r"pCreateContext->Flags\.([A-Za-z_][A-Za-z0-9_]*)", body))
        facts.ctx_node_checked = bool(re.search(r"pCreateContext->NodeOrdinal\s*(!=|>=|>)", body))

    # BuildPagingBuffer may only ever answer three statuses (bugcheck 0x119 otherwise).
    body, _ = function_body(text, "Bc250WddmBuildPagingBuffer")
    if body:
        facts.notes.append(("BuildPagingBuffer returns", sorted(set(re.findall(r"return\s+(STATUS_[A-Z_]+)", body)))))

    # the INF and its generator
    if inf_text:
        facts.inf_umd_active = bool(re.search(r"(?m)^\s*HKR,,\s*UserModeDriverName\b", inf_text))
        facts.inf_umd_marked = bool(re.search(r"(?m)^\s*;@UMD\s+HKR,,\s*UserModeDriverName\b", inf_text))
    if build_text:
        facts.build_generates_umd = ("Write-UmdInf" in build_text
                                     and "UserModeDriverName" in build_text
                                     and "-UmdStub" in build_text.replace("$UmdStub", "-UmdStub"))
    return facts


# --------------------------------------------------------------------------------------------------
# evaluating the rules
# --------------------------------------------------------------------------------------------------

OK, VIOLATION, NA, UNKNOWN, WARNING = "OK", "VIOLATION", "n/a", "UNKNOWN", "WARNING"


def truthy(entry):
    if entry is None:
        return False
    if entry["value"] is not None:
        return entry["value"] != 0
    return entry["raw"].strip() not in ("0", "FALSE", "NULL")


def predicate(facts: Facts, pred: dict):
    """(result, explanation). result is True, False or None for 'cannot tell'."""
    if "any_of" in pred:
        results = [predicate(facts, p) for p in pred["any_of"]]
        if any(r[0] is True for r in results):
            return True, " or ".join(r[1] for r in results if r[0] is True)
        if any(r[0] is None for r in results):
            return None, " or ".join(r[1] for r in results)
        return False, " and ".join(r[1] for r in results)

    if "cap_true" in pred:
        name = pred["cap_true"]
        entry = facts.cap(name)
        return truthy(entry), "caps.%s = %s" % (name, entry["raw"] if entry else "not set (0)")
    if "cap_false" in pred:
        name = pred["cap_false"]
        entry = facts.cap(name)
        return not truthy(entry), "caps.%s = %s" % (name, entry["raw"] if entry else "not set (0)")
    if "cap_nonzero" in pred:
        name = pred["cap_nonzero"]
        entry = facts.cap(name)
        return truthy(entry), "caps.%s = %s" % (name, entry["raw"] if entry else "not set (0)")
    if "cap_equals" in pred:
        name, want = pred["cap_equals"]["name"], pred["cap_equals"]["value"]
        entry = facts.cap(name)
        got = entry["raw"] if entry else "not set"
        if entry is None:
            return False, "caps.%s not set, wanted %s" % (name, want)
        return entry["raw"] == want, "caps.%s = %s" % (name, got)
    # VIDMM_GLOBAL::VerifySegmentSet, as dxgmms2 codes it: a set of 0 passes unconditionally, and every
    # bit that is set must name a segment carrying Flags.Aperture. Bit N-1 is segment N.
    if "segment_set_aperture_only" in pred:
        where = pred["segment_set_aperture_only"]
        entry = facts.ctx.get(where) if where in facts.ctx else facts.segment_out.get(where)
        if entry is None:
            return None, "%s is never set" % where
        # A conditional answer has to be legal in both arms, so every value it can take is tested.
        values = entry.get("values") or [entry["value"]]
        if any(v is None for v in values):
            return None, "%s is %s, which this checker cannot fold to a number" % (where, entry["raw"])
        bad, good = [], []
        for value in values:
            if value == 0:
                good.append("0 (system memory, which VerifySegmentSet passes unconditionally)")
                continue
            for bit in range(32):
                if not (value >> bit) & 1:
                    continue
                segment_id = bit + 1
                if segment_id > len(facts.segments):
                    bad.append("0x%X names segment %d, which is not declared" % (value, segment_id))
                elif not truthy(facts.segments[segment_id - 1]["flags"].get("Aperture")):
                    bad.append("0x%X names segment %d, whose Flags.Aperture is clear" % (value, segment_id))
                else:
                    good.append("0x%X names segment %d, an aperture segment" % (value, segment_id))
        if bad:
            return False, "%s: %s" % (where, "; ".join(bad))
        return True, "%s: %s" % (where, "; ".join(good))

    # The same rule where the driver names one segment by id rather than by a set.
    if "segment_id_aperture" in pred:
        where = pred["segment_id_aperture"]
        entry = facts.segment_out.get(where)
        if entry is None or entry["value"] is None:
            return None, "%s is %s, which this checker cannot fold to a number" % (
                where, entry["raw"] if entry else "never set")
        segment_id = entry["value"]
        if segment_id == 0:
            return True, "%s = 0 (system memory)" % where
        if segment_id > len(facts.segments):
            return False, "%s = %d, which is not a declared segment" % (where, segment_id)
        flags = facts.segments[segment_id - 1]["flags"]
        return truthy(flags.get("Aperture")), "%s = %d, Flags.Aperture %s" % (
            where, segment_id, "set" if truthy(flags.get("Aperture")) else "clear")

    if "aperture_segment_declared" in pred:
        found = [s["id"] for s in facts.segments if truthy(s["flags"].get("Aperture"))]
        return bool(found), ("aperture segment(s): %s" % found) if found else "no aperture segment declared"

    # Every declared segment sits in exactly one budget group: memory segments in the local group, aperture
    # segments in the non-local group (d3dkmddi.h DXGK_SEGMENTFLAGS LocalBudgetGroup/NonLocalBudgetGroup).
    # With no segment in the non-local group dxgkrnl folded the shared system memory into the local budget
    # (trial 211, K48). The rule reads every segment, not only segment 1.
    if "segment_budget_groups" in pred:
        if not facts.segments:
            return False, "no segment descriptor found"
        bad, good = [], []
        for segment in facts.segments:
            flags = segment["flags"]
            aperture = truthy(flags.get("Aperture"))
            local, nonlocal_ = truthy(flags.get("LocalBudgetGroup")), truthy(flags.get("NonLocalBudgetGroup"))
            want = "non-local" if aperture else "local"
            line = "segment %d (%s): LocalBudgetGroup %d, NonLocalBudgetGroup %d" % (
                segment["id"], "aperture" if aperture else "memory", local, nonlocal_)
            if (nonlocal_ and not local) if aperture else (local and not nonlocal_):
                good.append(line)
            else:
                bad.append(line + ", wanted the %s group only" % want)
        if bad:
            return False, "; ".join(bad)
        return True, "; ".join(good)

    # DXGK_CONTEXTINFO, the CreateContext answer.
    if "ctx_equals" in pred:
        name, want = pred["ctx_equals"]["name"], pred["ctx_equals"]["value"]
        entry = facts.ctx.get(name)
        if entry is None:
            return False, "ContextInfo.%s never assigned, wanted %s" % (name, want)
        return entry["raw"] == want, "ContextInfo.%s = %s" % (name, entry["raw"])
    if "ctx_set" in pred:
        name = pred["ctx_set"]
        entry = facts.ctx.get(name)
        return entry is not None, "ContextInfo.%s %s" % (
            name, ("= " + entry["raw"]) if entry else "never assigned")
    if "ctx_flag_read" in pred:
        name = pred["ctx_flag_read"]
        return name in facts.ctx_flags_read, "CreateContext %s Flags.%s" % (
            "reads" if name in facts.ctx_flags_read else "never reads", name)
    if "ctx_node_checked" in pred:
        return facts.ctx_node_checked, "NodeOrdinal is %sbounds-checked" % (
            "" if facts.ctx_node_checked else "not ")

    if "ddi_set" in pred:
        name = pred["ddi_set"]
        entry = facts.ddis.get(name)
        return entry is not None, "%s = %s" % (name, entry["value"] if entry else "NULL")
    if "ddi_null" in pred:
        name = pred["ddi_null"]
        entry = facts.ddis.get(name)
        return entry is None, "%s = %s" % (name, entry["value"] if entry else "NULL")
    if "segment_flag_set" in pred:
        name = pred["segment_flag_set"]
        entry = facts.segment_flags.get(name)
        return truthy(entry), "segment Flags.%s = %s" % (name, entry["raw"] if entry else "not set (0)")
    if "segment_flag_clear" in pred:
        name = pred["segment_flag_clear"]
        entry = facts.segment_flags.get(name)
        return not truthy(entry), "segment Flags.%s = %s" % (name, entry["raw"] if entry else "not set (0)")
    if "qai_handled" in pred:
        value = QAI_TYPES.get(pred["qai_handled"], pred["qai_handled"]) if isinstance(pred["qai_handled"], str) \
            else pred["qai_handled"]
        entry = facts.qai_handled.get(value)
        return entry is not None, "QueryAdapterInfo type %s %s" % (
            value, "handled by %s()" % entry["handler"] if entry else "falls to the default arm")
    if "qai_refused" in pred:
        value = QAI_TYPES.get(pred["qai_refused"], pred["qai_refused"]) if isinstance(pred["qai_refused"], str) \
            else pred["qai_refused"]
        entry = facts.qai_handled.get(value)
        if entry is not None:
            return False, "QueryAdapterInfo type %s is handled by %s()" % (value, entry["handler"])
        return facts.qai_default == "STATUS_NOT_SUPPORTED", \
            "QueryAdapterInfo type %s falls to the default arm, which answers %s" % (value, facts.qai_default)
    if "gpummu_true" in pred:
        name = pred["gpummu_true"]
        entry = facts.gpummu.get(name)
        return truthy(entry), "GPUMMUCAPS.%s = %s" % (name, entry["raw"] if entry else "not set (0)")
    if "gpummu_equals" in pred:
        name, want = pred["gpummu_equals"]["name"], pred["gpummu_equals"]["value"]
        entry = facts.gpummu.get(name)
        if entry is None:
            return False, "GPUMMUCAPS.%s not set, wanted %s" % (name, want)
        return entry["raw"] == want, "GPUMMUCAPS.%s = %s" % (name, entry["raw"])
    if "gpummu_between" in pred:
        name = pred["gpummu_between"]["name"]
        low, high = pred["gpummu_between"]["min"], pred["gpummu_between"]["max"]
        entry = facts.gpummu.get(name)
        if entry is None or entry["value"] is None:
            return None, "GPUMMUCAPS.%s = %s, not a foldable constant" % (
                name, entry["raw"] if entry else "not set")
        return low <= entry["value"] <= high, "GPUMMUCAPS.%s = %d (wanted %d..%d)" % (
            name, entry["value"], low, high)
    if "segment_out_nonzero" in pred:
        name = pred["segment_out_nonzero"]
        entry = facts.segment_out.get(name)
        return truthy(entry), "QUERYSEGMENTOUT4.%s = %s" % (name, entry["raw"] if entry else "not set (0)")
    if "segment_out_equals" in pred:
        name, want = pred["segment_out_equals"]["name"], pred["segment_out_equals"]["value"]
        entry = facts.segment_out.get(name)
        if entry is None:
            return False, "QUERYSEGMENTOUT4.%s not set, wanted %s" % (name, want)
        return entry["raw"] == want, "QUERYSEGMENTOUT4.%s = %s" % (name, entry["raw"])
    if "pagetable_set" in pred:
        name = pred["pagetable_set"]
        entry = facts.pagetable.get(name)
        return entry is not None, "PAGETABLELEVELDESC.%s = %s" % (name, entry["raw"] if entry else "not set")
    if "table_version_is" in pred:
        want = pred["table_version_is"]
        return facts.table_version == want, "DRIVER_INITIALIZATION_DATA.Version = %s" % facts.table_version
    if "table_version_matches_interface" in pred:
        # The table must declare the version the binary is compiled at: dxgkrnl sizes its copies of the table,
        # DRIVERCAPS and DXGKRNL_INTERFACE by the declared version, our sizeof() follows the compiled one.
        raw = (facts.table_version or "").strip()
        declared = INTERFACE_VERSIONS.get(raw)
        if declared is None and re.fullmatch(r"0[xX][0-9a-fA-F]+", raw):
            declared = int(raw, 16)
        if declared is None or facts.interface_version is None:
            return None, "DRIVER_INITIALIZATION_DATA.Version = %s, DXGKDDI_INTERFACE_VERSION = %s" % (
                facts.table_version, "0x%04X" % facts.interface_version if facts.interface_version else "not found")
        return declared == facts.interface_version, "DRIVER_INITIALIZATION_DATA.Version = %s (0x%04X), compiled 0x%04X" % (
            raw, declared, facts.interface_version)
    if "interface_at_least" in pred:
        want = int(pred["interface_at_least"], 16)
        if facts.interface_version is None:
            return None, "DXGKDDI_INTERFACE_VERSION not found"
        return facts.interface_version >= want, "DXGKDDI_INTERFACE_VERSION = 0x%04X (>= 0x%04X)" % (
            facts.interface_version, want)
    if "wddm_version_is" in pred:
        entry = facts.cap("WDDMVersion")
        want = pred["wddm_version_is"]
        got = entry["raw"] if entry else "not set"
        return got == want, "caps.WDDMVersion = %s" % got
    if "wddm_version_at_least" in pred:
        entry = facts.cap("WDDMVersion")
        if entry is None:
            return False, "caps.WDDMVersion not set"
        value = WDDM_VERSIONS.get(entry["raw"].strip())
        if value is None:
            return None, "caps.WDDMVersion = %s, not a known constant" % entry["raw"]
        want = int(pred["wddm_version_at_least"], 16)
        return value >= want, "caps.WDDMVersion = %s (0x%04X)" % (entry["raw"], value)
    if "inf_umd_name" in pred:
        if facts.inf_umd_active:
            return True, "bc250kmd.inf writes UserModeDriverName in every package"
        if facts.inf_umd_marked and facts.build_generates_umd:
            return True, ("bc250kmd.inf carries the ;@UMD UserModeDriverName block and build.ps1 "
                          "-UmdStub generates the package that enables it")
        if facts.inf_umd_marked:
            return False, "bc250kmd.inf has the ;@UMD block but build.ps1 does not generate that package"
        return False, "no UserModeDriverName anywhere in bc250kmd.inf"
    if "always" in pred:
        return bool(pred["always"]), "unconditional"
    return None, "unknown predicate %s" % sorted(pred)


def evaluate(facts: Facts, rules: dict):
    results = []
    for rule in rules["rules"]:
        whens = [predicate(facts, p) for p in rule.get("when", [{"always": True}])]
        if any(w[0] is False for w in whens):
            status = NA
            why = "; ".join(w[1] for w in whens if w[0] is False)
            requires = []
        else:
            requires = [(p, predicate(facts, p)) for p in rule["require"]]
            if any(r[1][0] is False for r in requires):
                status = VIOLATION
            elif any(r[1][0] is None for r in requires) or any(w[0] is None for w in whens):
                status = UNKNOWN
            else:
                status = OK
            why = "; ".join(r[1][1] for r in requires)
        if status == VIOLATION:
            why = "; ".join(r[1][1] for r in requires if r[1][0] is not True)
            # A rule whose obligation is a recommendation rather than a refusal we can point at is
            # reported and does not fail the run; rules.json says which.
            if rule.get("severity", "violation") == "warning":
                status = WARNING
        results.append({
            "id": rule["id"],
            "declaration": rule["declaration"],
            "requires": rule["requires_text"],
            "source": rule["source"],
            "severity": rule.get("severity", "violation"),
            "status": status,
            "detail": why,
        })
    return results


# --------------------------------------------------------------------------------------------------
# output
# --------------------------------------------------------------------------------------------------

def print_inventory(facts: Facts, stream=sys.stdout):
    def line(*args):
        print(*args, file=stream)

    line("== driver/kmd/wddm.c, %d lines ==" % facts.wddm_lines)
    line("DXGKDDI_INTERFACE_VERSION: %s" % (
        "0x%04X" % facts.interface_version if facts.interface_version else "not found"))
    line("DRIVER_INITIALIZATION_DATA.Version: %s" % facts.table_version)
    line("")
    line("-- QueryAdapterInfo types handled --")
    for value in sorted(facts.qai_handled):
        entry = facts.qai_handled[value]
        line("  %3d  %-42s %-26s wddm.c:%d" % (value, entry["name"], entry["handler"] + "()", entry["line"]))
    line("  everything else -> %s" % facts.qai_default)
    line("")
    line("-- DXGK_DRIVERCAPS members set --")
    for name in sorted(facts.caps):
        entry = facts.caps[name]
        folded = "" if entry["value"] is None else "  = %d" % entry["value"]
        line("  %-46s %-34s%s  wddm.c:%d" % (name, entry["raw"], folded, entry["line"]))
    line("")
    line("-- DXGK_QUERYSEGMENTOUT4 members set --")
    for name in sorted(facts.segment_out):
        entry = facts.segment_out[name]
        line("  %-46s %-34s  wddm.c:%d" % (name, entry["raw"], entry["line"]))
    line("")
    line("-- DXGK_SEGMENTDESCRIPTOR4 members set (%d segment(s) declared) --"
         % len(facts.segments))
    for segment in facts.segments:
        line("  segment %d:" % segment["id"])
        for name in sorted(segment["flags"]):
            entry = segment["flags"][name]
            line("    Flags.%-38s %-34s  wddm.c:%d" % (name, entry["raw"], entry["line"]))
        for name in sorted(segment["members"]):
            entry = segment["members"][name]
            line("    %-44s %-34s  wddm.c:%d" % (name, entry["raw"], entry["line"]))
    line("")
    line("-- DXGK_GPUMMUCAPS members set --")
    for name in sorted(facts.gpummu):
        entry = facts.gpummu[name]
        folded = "" if entry["value"] is None else "  = %d" % entry["value"]
        line("  %-46s %-34s%s  wddm.c:%d" % (name, entry["raw"], folded, entry["line"]))
    line("")
    line("-- DXGK_PAGE_TABLE_LEVEL_DESC members set --")
    for name in sorted(facts.pagetable):
        entry = facts.pagetable[name]
        folded = "" if entry["value"] is None else "  = %d" % entry["value"]
        line("  %-46s %-34s%s  wddm.c:%d" % (name, entry["raw"], folded, entry["line"]))
    line("")
    line("-- DXGK_CONTEXTINFO members set (DxgkDdiCreateContext) --")
    for name in sorted(facts.ctx):
        entry = facts.ctx[name]
        folded = "" if entry["value"] is None else "  = %d" % entry["value"]
        line("  %-46s %-34s%s  wddm.c:%d" % (name, entry["raw"], folded, entry["line"]))
    line("  %-46s %s" % ("(reads DXGK_CREATECONTEXTFLAGS)",
                         ", ".join(sorted(facts.ctx_flags_read)) or "none"))
    line("  %-46s %s" % ("(NodeOrdinal bounds-checked)", "yes" if facts.ctx_node_checked else "NO"))
    line("")
    line("-- buffer-size guards --")
    for name in sorted(facts.size_checks):
        line("  %-46s OutputDataSize < sizeof(%s)" % (name, facts.size_checks[name]))
    line("")
    line("-- DRIVER_INITIALIZATION_DATA members assigned (%d) --" % len(facts.ddis))
    for name in sorted(facts.ddis):
        line("  %-46s %-34s  wddm.c:%d" % (name, facts.ddis[name]["value"], facts.ddis[name]["line"]))
    line("")
    line("-- package --")
    line("  bc250kmd.inf UserModeDriverName active in every package: %s" % facts.inf_umd_active)
    line("  bc250kmd.inf ;@UMD UserModeDriverName block present:     %s" % facts.inf_umd_marked)
    line("  build.ps1 -UmdStub generates that package:               %s" % facts.build_generates_umd)
    for note in facts.notes:
        line("  %s: %s" % note)


def print_text(results, stream=sys.stdout):
    width = max(len(r["id"]) for r in results)
    for result in results:
        print("%-9s %-*s  %s" % (result["status"], width, result["id"], result["declaration"]),
              file=stream)
        if result["status"] in (VIOLATION, UNKNOWN, WARNING):
            print("%-9s %-*s  needs: %s" % ("", width, "", result["requires"]), file=stream)
            print("%-9s %-*s  found: %s" % ("", width, "", result["detail"]), file=stream)
            print("%-9s %-*s  source: %s" % ("", width, "", result["source"]), file=stream)


def print_markdown(results, facts: Facts, stream=sys.stdout):
    print("| # | Declaration | Required element | Source of the rule | Status |", file=stream)
    print("|---|---|---|---|---|", file=stream)
    for result in results:
        print("| %s | %s | %s | %s | **%s** |" % (
            result["id"], result["declaration"].replace("|", "\\|"),
            result["requires"].replace("|", "\\|"), result["source"].replace("|", "\\|"),
            result["status"] if result["status"] != NA else "n/a (not declared)"), file=stream)
    print("", file=stream)
    for result in results:
        if result["status"] in (VIOLATION, UNKNOWN, WARNING):
            print("- **%s %s**: %s" % (result["status"], result["id"], result["detail"]), file=stream)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo", default=DEFAULT_REPO)
    parser.add_argument("--wddm")
    parser.add_argument("--header")
    parser.add_argument("--inf")
    parser.add_argument("--build")
    parser.add_argument("--rules", default=os.path.join(HERE, "rules.json"))
    parser.add_argument("--markdown", action="store_true")
    parser.add_argument("--inventory", action="store_true")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)

    kmd = os.path.join(args.repo, "driver", "kmd")
    wddm = args.wddm or os.path.join(kmd, "wddm.c")
    header = args.header or os.path.join(kmd, "bc250kmd.h")
    inf = args.inf or os.path.join(kmd, "bc250kmd.inf")
    build = args.build or os.path.join(kmd, "build.ps1")
    if not os.path.exists(wddm):
        print("cannot read %s" % wddm, file=sys.stderr)
        return 2

    facts = read_facts(wddm, header, inf, build)
    with open(args.rules, "r", encoding="utf-8") as handle:
        rules = json.load(handle)
    results = evaluate(facts, rules)

    if args.inventory:
        print_inventory(facts)
        print("")
    if args.json:
        print(json.dumps({"results": results}, indent=2))
    elif args.markdown:
        print_markdown(results, facts)
    else:
        print_text(results)
        counts = {}
        for result in results:
            counts[result["status"]] = counts.get(result["status"], 0) + 1
        print("")
        print("%d rules: %s" % (len(results), ", ".join(
            "%d %s" % (counts[k], k) for k in (OK, VIOLATION, WARNING, UNKNOWN, NA) if k in counts)))

    return 1 if any(r["status"] == VIOLATION for r in results) else 0


if __name__ == "__main__":
    sys.exit(main())
