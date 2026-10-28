#!/usr/bin/env python3
"""DXIL operation coverage of the D3D12 shader translation, per shader model.

The script reads three sources at exact commits and writes one Markdown page:

  DXC         utils/hct/hctdb.py, the table of every DXIL operation (opcode, class, category, shader model,
              stages, function attributes). The script imports it the way DXC's hctdb_instrhelp.py does.
  dxil-spirv  dxil.hpp (the DXIL::Op enumeration), opcodes/opcodes_dxil_builtins.cpp (the dispatch table of
              emit_dxil_instruction and the analysis passes) and the handler sources under opcodes/.
  this repo   driver/umd/d3d12/engine-ddi/caps.cpp, state-objects.cpp and adapter-caps.cpp (the shader model
              and the feature caps that the installed D3D12 driver reports), and three evidence files with the
              lab answers.

Status of an operation:
  translated          the dispatch table has a handler for the opcode.
  translated-partial  the handler, or a helper that it calls, refuses some valid inputs (the note names the
                      file:line of each refusal).
  refused             the handler exists and returns false on every path.
  missing             the opcode has no handler: emit_dxil_instruction stops with an error, or the opcode number
                      falls on an internal opcode of dxil-spirv (the note names it).

Reach says if an application can give the operation to the installed driver today, once for the Agility SDK
route and once for the System32 runtime of build 22621: the shader model that the shell reports on that route
admits the operation, and the feature cap that the operation needs is reported.
This is a static reading. A translated operation can still be wrong; the page lists candidates for runtime tests.

  python -I tools/docs/dxil_op_coverage.py --dxc <dxc> --dxil-spirv <repo> --dxil-spirv-rev <rev> \\
      --vkd3d <repo> --vkd3d-rev <rev> [--check]

Standard library only. The DXC tree must be a git checkout; the two forks are read with git show at the given
revision, so their working trees can be on any branch.
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
OUT = REPO / "docs" / "status" / "dxil-op-coverage.md"

CAPS_CPP = "driver/umd/d3d12/engine-ddi/caps.cpp"
STATE_OBJECTS_CPP = "driver/umd/d3d12/engine-ddi/state-objects.cpp"
ADAPTER_CAPS_CPP = "driver/umd/d3d12/adapter-caps.cpp"
DEVICE_C = "libs/vkd3d/device.c"

# Lab answers of the engine that the shell passes through. Each value is read from the file, not typed here.
EVIDENCE = {
    "conformance": ("evidence/windows/2026-10-13-E59-m153-conformance-tester30/run-20261013T150946Z.txt",
                    r"RaytracingTier=(\S+) MaxSupportedFeatureLevel=\S+ HighestShaderModel=(\S+)",
                    r"ConservativeRasterizationTier=(\d+)"),
    "mesh": ("evidence/windows/2026-10-24-gfx1013-mesh-r3-sync/lab/slot-stage1-20261023T233118Z.log",
             r"OPTIONS7: MeshShaderTier (\d+)", None),
    "sm68": ("evidence/windows/2026-10-08-sm68-lab-unit-a/sm68-agility-driver-log.txt",
             r"QueryAdapterCaps: shader model (\d+);.*SampleCmpGradientAndBias (\d), ExtendedCommandInfo (\d)", None),
}

# Optional: evidence of the engine's shader model 6.9 answer ("engine_sm69") and work graph tier ("engine_wg"),
# as (path, pattern). The engine log line "QueryAdapterCaps: shader model 69" and "work graphs: engine tier 10" are
# the patterns to use.
ENGINE_EVIDENCE: dict[str, tuple[str, str]] = {}

SHADER_MODELS = [(6, m) for m in range(0, 12)]

# Error logs that read as a refusal. Each one that a handler can reach must be in REFUSES_VALID_INPUT or in
# NOT_AN_INPUT_LIMIT, so that a new refusal in dxil-spirv stops the run until somebody classifies it.
REFUSAL_WORDING = re.compile(r"not supported|only supported|[Uu]nsupported|Cannot |cannot |no constant offset|"
                             r"must provide|Need to set|must be UBO|must be BDA|not yet implemented")
# A valid D3D12 input that the translation refuses: the operation is translated-partial.
REFUSES_VALID_INPUT = re.compile(r"only supported for SSBOs|Cannot use BDA and sparse feedback|"
                                 r"Cannot use SSBOs and sparse feedback|Cannot dynamically index into push constants|"
                                 r"no constant offset can be applied")
# Not a limit on D3D12 input, with the reason that the page gives.
NOT_AN_INPUT_LIMIT = [
    (re.compile(r"with input attachment|InputAttachment does not support"),
     "Input attachments. They are an engine-internal binding, and a D3D12 shader cannot name one"),
    (re.compile(r"Cannot find ray query flags mapping|Cannot find representative ray query flags"),
     "The internal consistency checks of the ray query analysis pass"),
    (re.compile(r"introspection variable for UAV counter|dynamic view instancing|view instance to viewport|"
                r"DynamicViewMasking"),
     "The checks of the engine's own options"),
    (re.compile(r"Lowering for dxbc"), "The DXBC input path"),
]
# Vendor extension paths (AMD AGS, NVAPI) ride on DXIL operations but are not DXIL semantics.
VENDOR_SOURCES = ("opcodes/dxil/dxil_ags.cpp", "opcodes/dxil/dxil_nvapi.cpp")


def sm_text(sm):
    return f"{sm[0]}.{sm[1]}"


def git(path, *args):
    return subprocess.run(["git", "-C", str(path), *args], check=True, capture_output=True,
                          text=True, encoding="utf-8", errors="replace").stdout


# ---------------------------------------------------------------------------------------------------- DXC table

def load_hctdb(dxc: Path):
    hct = dxc / "utils" / "hct"
    if git(dxc, "status", "--porcelain", "--", "utils/hct").strip():
        sys.exit(f"{hct} has local changes; the page must name an exact DXC commit")
    sys.path.insert(0, str(hct))
    import hctdb                                                  # noqa: E402  (the DXC table)
    db = hctdb.db_dxil()
    ops = []
    for i in db.get_dxil_ops():
        ret = i.ops[0].llvm_type if i.ops else ""
        vector_only = i.oload_types.startswith("<")
        sm = tuple(i.shader_model)
        # A vector-only overload exists only as a native vector, which DXIL has from shader model 6.9.
        eff = max(sm, (6, 9)) if vector_only else sm
        ops.append(dict(table=i.dxil_table, opid=i.dxil_opid, index=i.dxil_opid & 0xFFFF, name=i.dxil_op,
                        cls=i.dxil_class, category=i.category, sm=sm, eff_sm=eff, vector_only=vector_only,
                        stages=tuple(i.shader_stages), fn_attr=i.fn_attr, ret=ret, reserved=i.is_reserved))
    return ops


def dxc_lowering_refs(dxc: Path):
    """Opcode names that DXC sources outside the generated table and the validator refer to."""
    out = git(dxc, "grep", "-o", "-h", "-E", r"OpCode::[A-Za-z_0-9]+", "--", "lib", "tools/clang/lib",
              ":!lib/DXIL/DxilOperations.cpp", ":!lib/DxilValidation")
    return {line.split("::", 1)[1] for line in out.splitlines() if "::" in line}


# ---------------------------------------------------------------------------------------------------- dxil-spirv

class Source:
    def __init__(self, path, text):
        self.path, self.text = path, text
        self.lines = text.splitlines()

    def line_of(self, offset):
        return self.text.count("\n", 0, offset) + 1

    def find(self, pattern, flags=0):
        hits = [m for m in re.finditer(pattern, self.text, flags)]
        if len(hits) != 1:
            sys.exit(f"{self.path}: pattern {pattern!r} matches {len(hits)} times, expected 1")
        return hits[0]


def read_fork(repo: Path, rev: str, paths):
    files = git(repo, "ls-tree", "-r", "--name-only", rev, "--", *paths).split()
    return {p: Source(p, git(repo, "show", f"{rev}:{p}")) for p in files if p.endswith((".cpp", ".hpp"))}


def parse_op_enum(src: Source):
    body = src.find(r"enum class Op : unsigned\s*\{(.*?)\n\};", re.S).group(1)
    values, nxt = {}, 0
    for m in re.finditer(r"^\s*([A-Za-z_]\w*)\s*(?:=\s*(\d+))?\s*,?", body, re.M):
        name, val = m.group(1), m.group(2)
        nxt = int(val) if val is not None else nxt
        values[name] = nxt
        nxt += 1
    count = values.pop("Count")
    return values, count


def strip_code(text):
    """Blank comments and string literals, keep offsets."""
    def blank(m):
        return re.sub(r"[^\n]", " ", m.group(0))
    return re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', blank, text, flags=re.S)


def index_functions(sources):
    """Every bool/void function definition under opcodes/: name -> list of (source, start, end)."""
    index = {}
    for src in sources.values():
        code = strip_code(src.text)
        for m in re.finditer(r"^(?:template\s*<[^>]*>\s*)?(?:static\s+)?(?:inline\s+)?(?:bool|void|spv::Id)\s+"
                             r"(\w+)\s*\(", code, re.M):
            depth, i = 1, m.end()
            while i < len(code) and depth:
                depth += {"(": 1, ")": -1}.get(code[i], 0)
                i += 1
            j = i
            while j < len(code) and code[j] in " \t\r\nconst":
                j += 1
            if j >= len(code) or code[j] != "{":
                continue
            depth, k = 1, j + 1
            while k < len(code) and depth:
                depth += {"{": 1, "}": -1}.get(code[k], 0)
                k += 1
            index.setdefault(m.group(1), []).append((src, m.start(), k))
    return index


def handler_refusals(name, index, depth=2, seen=None):
    """file:line and message of each refusal log in the handler and in the helpers it calls."""
    seen = set() if seen is None else seen
    if name in seen or name not in index:
        return []
    seen.add(name)
    found = []
    for src, start, end in index[name]:
        if src.path in VENDOR_SOURCES:
            continue
        body = src.text[start:end]
        for m in re.finditer(r'LOGE\("([^"]*)', body):
            msg = m.group(1).replace("\\n", "")
            if not REFUSAL_WORDING.search(msg):
                continue
            where = f"{src.path}:{src.line_of(start + m.start())}"
            if REFUSES_VALID_INPUT.search(msg):
                found.append((where, msg))
            elif not any(p.search(msg) for p, _ in NOT_AN_INPUT_LIMIT):
                sys.exit(f"{where}: unclassified refusal {msg!r}; add it to REFUSES_VALID_INPUT or NOT_AN_INPUT_LIMIT")
        if depth:
            code = strip_code(body)
            for callee in sorted(set(re.findall(r"\b([a-z_]\w*)\s*[<(]", code))):
                if callee != name and callee in index:
                    found += handler_refusals(callee, index, depth - 1, seen)
    return sorted(set(found))


def handler_always_fails(name, index):
    if name not in index:
        return False
    rets = []
    for src, start, end in index[name]:
        rets += re.findall(r"\breturn\s+([^;]+);", strip_code(src.text[start:end]))
    return bool(rets) and all(r.strip() == "false" for r in rets)


def parse_dispatch(src: Source):
    body = src.find(r"DXILDispatcher\(\) noexcept\s*\{(.*?)\n\t\}", re.S)
    table = {}
    for m in re.finditer(r"OP\((\w+)\)\s*=\s*([A-Za-z_]\w*)", body.group(1)):
        table[m.group(1)] = (m.group(2), src.line_of(body.start(1) + m.start()))
    side = src.find(r"bool dxil_instruction_has_side_effects\(.*?\n\}", re.S).group(0)
    side_effects = set(re.findall(r"case DXIL::Op::(\w+):", side))
    out_of_range = src.line_of(src.find(r"opcode >= unsigned\(DXIL::Op::Count\)").start())
    no_handler = src.line_of(src.find(r'LOGE\("Unimplemented DXIL opcode').start())
    return table, side_effects, out_of_range, no_handler


# ---------------------------------------------------------------------------------------------------- reported caps

def read_caps():
    caps = Source(CAPS_CPP, (REPO / CAPS_CPP).read_text(encoding="utf-8"))
    so = Source(STATE_OBJECTS_CPP, (REPO / STATE_OBJECTS_CPP).read_text(encoding="utf-8"))

    def at(src, pattern):
        m = src.find(pattern)
        return m, f"{src.path}:{src.line_of(m.start())}"

    shell = Source(ADAPTER_CAPS_CPP, (REPO / ADAPTER_CAPS_CPP).read_text(encoding="utf-8"))
    m, sm_line = at(caps, r"constexpr D3D_SHADER_MODEL kHighestShaderModel = D3D_SHADER_MODEL_(\d+)_(\d+);")
    r = dict(sm=(int(m.group(1)), int(m.group(2))), sm_line=sm_line)
    m, r["tier12_ddi_line"] = at(caps, r"constexpr uint32_t kTier12DdiVersion = (\d+);")
    r["tier12_ddi"] = int(m.group(1))
    _, r["sm69_gate_line"] = at(caps, r"if \(m >= D3D_SHADER_MODEL_6_9 && !\(c\.report_raytracing_tier12 && sm69_floor\(c\)\)\)")
    _, r["tier12_switch_line"] = at(shell, r"engine_ddi::set_raytracing_tier12_reporting\(owner\.caps,!off\)")
    _, r["rt_line"] = at(caps, r"if \(tier12_reported\(c, ddi_version\)\) o\.RaytracingTier =")
    _, r["sfb_line"] = at(caps, r"o\.SamplerFeedbackTier = D3D12DDI_SAMPLER_FEEDBACK_TIER_NOT_SUPPORTED;")
    _, r["ato_line"] = at(caps, r"d->AdvancedTextureOpsSupported = FALSE;")
    _, r["msaa_line"] = at(caps, r"d->WriteableMSAATexturesSupported = FALSE;")
    _, r["mesh_line"] = at(caps, r"o\.MeshShaderTier = mesh_shaders\(c\)")
    _, r["bary_line"] = at(caps, r"o\.BarycentricsSupported = c\.options3\.BarycentricsSupported;")
    _, r["vi_line"] = at(caps, r"o\.ViewInstancingTier = ")
    _, r["sm68opt_line"] = at(caps, r"d->SampleCmpGradientAndBiasSupported = sm68 &&")
    _, r["eci_line"] = at(caps, r"d->ExtendedCommandInfoSupported = sm68 &&")
    m, r["wg_line"] = at(caps, r"d->WorkGraphsTier = ddi_version >= (\d+) && c\.options21\.WorkGraphsTier")
    r["wg_ddi"] = int(m.group(1))
    _, r["wg_so_line"] = at(so, r"case D3D12DDI_STATE_SUBOBJECT_TYPE_WORK_GRAPH:")
    m, r["ddi_max_line"] = at(caps, r"static_assert\(kDdiVersionMax == (\d+),")
    r["ddi_max"] = int(m.group(1))

    lab = {}
    for key, (path, pattern, extra) in EVIDENCE.items():
        text = (REPO / path).read_text(encoding="utf-8", errors="replace")
        m = re.search(pattern, text)
        if not m:
            sys.exit(f"{path}: no match for {pattern!r}")
        lab[key] = m.groups()
        if extra:
            lab[key + "_extra"] = re.search(extra, text).groups()
    r["lab"] = lab
    r["rt_tier"], r["sys_sm"] = lab["conformance"]
    r["crt"] = int(lab["conformance_extra"][0])
    r["mesh_tier"] = int(lab["mesh"][0])
    r["sm68_engine"], r["scgb"], r["eci"] = lab["sm68"]
    # The engine answers that a 6.9 program and a work graph need on the Agility route. No committed evidence gives
    # them yet. An entry here (path, pattern) turns the cond rows into yes rows.
    for key in ("engine_sm69", "engine_wg"):
        r[key] = False
        if key in ENGINE_EVIDENCE:
            path, pattern = ENGINE_EVIDENCE[key]
            r[key] = bool(re.search(pattern, (REPO / path).read_text(encoding="utf-8", errors="replace")))
    return r


def routes(caps):
    """The two runtime routes of the installed driver: what each one lets an application use."""
    agility = dict(name="Agility", ddi=caps["ddi_max"], sm=caps["sm"], wg=caps["ddi_max"] >= caps["wg_ddi"])
    if caps["ddi_max"] < caps["tier12_ddi"]:
        agility["sm"] = min(caps["sm"], (6, 8))
    system = dict(name="System32 22621", ddi=92, sm=tuple(int(x) for x in caps["sys_sm"].split("_")), wg=False)
    return agility, system


def reach(op, caps, route):
    """yes / no / cond, and the reason, for one route of the installed driver."""
    if op["reserved"]:
        return "-", ""
    if op["table"] != "CoreOps":
        return "no", "The experimental table needs an experimental shader model."
    if op["eff_sm"] > route["sm"]:
        where = caps["sm_line"] if route["name"] == "Agility" else EVIDENCE["conformance"][0]
        return "no", f"SM {sm_text(op['eff_sm'])} is above {sm_text(route['sm'])} ({where})."
    cat, name = op["category"], op["name"]
    if op["stages"] and set(op["stages"]) <= {"node"}:
        if not route["wg"]:
            return "no", (f"Node shaders only. Work graphs need DDI build {caps['wg_ddi']}, and this route "
                          f"negotiates {route['ddi']} ({caps['wg_line']}).")
        if not caps["engine_wg"]:
            return "cond", f"WorkGraphsTier is the engine answer ({caps['wg_line']}). The evidence has no value."
        return "yes", ""
    if op["eff_sm"] >= (6, 9) and not caps["engine_sm69"]:
        return "cond", (f"6.9 needs the engine answer and the shell floor ({caps['sm69_gate_line']}). "
                        "The evidence has no value.")
    if cat == "Sampler Feedback":
        return "no", f"SamplerFeedbackTier is not reported ({caps['sfb_line']})."
    if name in ("TextureGatherRaw", "SampleCmpLevel"):
        return "no", f"AdvancedTextureOps is FALSE ({caps['ato_line']})."
    if name == "TextureStoreSample":
        return "no", f"WriteableMSAATextures is FALSE ({caps['msaa_line']})."
    if name in ("SampleCmpGrad", "SampleCmpBias"):
        return ("yes" if caps["scgb"] == "1" else "no"), f"SampleCmpGradientAndBias {caps['scgb']} ({caps['sm68opt_line']})."
    if name in ("StartVertexLocation", "StartInstanceLocation"):
        return ("yes" if caps["eci"] == "1" else "no"), f"ExtendedCommandInfo {caps['eci']} ({caps['eci_line']})."
    if cat.startswith("Mesh shader") or cat.startswith("Amplification shader"):
        return ("yes" if caps["mesh_tier"] >= 10 else "no"), f"MeshShaderTier {caps['mesh_tier']} ({caps['mesh_line']})."
    if name == "AttributeAtVertex":
        return "cond", f"BarycentricsSupported is an engine answer ({caps['bary_line']}). The evidence has no value."
    if name == "ViewID":
        return "cond", f"ViewInstancingTier needs the engine contract ({caps['vi_line']}). The evidence has no value."
    if name == "InnerCoverage":
        return ("yes" if caps["crt"] >= 3 else "no"), f"ConservativeRasterizationTier {caps['crt']}."
    rt_stages = {"raygeneration", "intersection", "anyhit", "closesthit", "miss", "callable"}
    if cat == "Inline Ray Query" or "aytracing" in cat or "Ray " in cat or name in ("TraceRay", "ReportHit",
            "CallShader", "AcceptHitAndEndSearch", "IgnoreHit") or (op["stages"] and set(op["stages"]) - {"library"} <= rt_stages):
        return ("yes" if caps["rt_tier"] in ("1.1", "1.2") else "no"), f"RaytracingTier {caps['rt_tier']} or higher."
    return "yes", ""


# ---------------------------------------------------------------------------------------------------- candidates

# Operations whose handler exists but whose semantics look doubtful. Each anchor must match once in its file.
CURATED = [
    ("BarrierByMemoryHandle", "partial", "opcodes/dxil/dxil_workgraph.cpp",
     r"if \(value_is_dx_op_instrinsic\(inst->getOperand\(1\), DXIL::Op::AnnotateHandle\)\)",
     "The handler returns false when the handle does not come directly from AnnotateHandle. An example is a "
     "select or a phi of two handles. With GROUP_SCOPE, the analysis pass refuses the same case "
     "(opcodes/opcodes_dxil_builtins.cpp, case BarrierByMemoryHandle)."),
    ("BarrierByMemoryType", "doubtful", "opcodes/dxil/dxil_workgraph.cpp",
     r"if \(needs_device_memory_scope && impl\.execution_mode_meta\.memory_model == spv::MemoryModelGLSL450\)",
     "Under the Vulkan memory model, DEVICE_SCOPE gives a workgroup memory scope. The REORDER_SCOPE bit (0x8, "
     "DXC DxilConstants.h) is not in the barrier semantics of dxil.hpp, so emit_barrier ignores it. Test "
     "cross-group UAV visibility and SER reorder barriers."),
    ("BarrierByMemoryHandle", "doubtful", "opcodes/dxil/dxil_workgraph.cpp",
     r"static bool emit_barrier\(Converter::Impl &impl, uint32_t memory_flags, uint32_t semantic_flags\)",
     "It uses emit_barrier, as BarrierByMemoryType does. The scope and REORDER_SCOPE questions are the same."),
    ("AllocateRayQuery2", "doubtful", "opcodes/opcodes_dxil_builtins.cpp",
     r"if \(!value_is_dx_op_instrinsic\(object, DXIL::Op::AllocateRayQuery\)\)",
     "The TraceRayInline analysis looks only for AllocateRayQuery. Thus every SM 6.9 ray query object counts as "
     "indirectly indexed (uses_non_direct_indexing). That path is slower. Test that the results are equal."),
]


# ---------------------------------------------------------------------------------------------------- build

def build(args):
    dxc, fork, vk = Path(args.dxc), Path(args.dxil_spirv), Path(args.vkd3d)
    ops = load_hctdb(dxc)
    lowered = dxc_lowering_refs(dxc)
    sources = read_fork(fork, args.dxil_spirv_rev, ["dxil.hpp", "opcodes"])
    enum, enum_count = parse_op_enum(sources["dxil.hpp"])
    by_value = {v: k for k, v in enum.items()}
    builtins = sources["opcodes/opcodes_dxil_builtins.cpp"]
    table, side_effects, out_of_range_line, no_handler_line = parse_dispatch(builtins)
    index = index_functions(sources)
    caps = read_caps()
    agility, system = routes(caps)
    caps["agility"], caps["system"] = agility, system
    device =Source(DEVICE_C, git(vk, "show", f"{args.vkd3d_rev}:{DEVICE_C}"))
    sm69_line = device.line_of(device.find(r"Enabling experimental support for SM 6\.9").start())
    first_internal = min(v for k, v in enum.items() if k.startswith("Extended"))

    curated = []
    for name, kind, path, anchor, note in CURATED:
        src = sources[path]
        curated.append((name, kind, f"{path}:{src.line_of(src.find(anchor).start())}", note))

    rows, cands, collisions, dce = [], [], [], []
    for op in ops:
        row = dict(op)
        row["reach"], row["reach_why"] = reach(op, caps, agility)
        row["reach_sys"], row["reach_sys_why"] = reach(op, caps, system)
        row["note"] = ""
        if op["reserved"]:
            row["status"] = "reserved"
            rows.append(row)
            continue
        n = op["index"] if op["table"] == "CoreOps" else None
        spv_name = by_value.get(n) if n is not None else None
        bc = builtins.path
        if op["table"] != "CoreOps":
            row["status"] = "missing"
            row["note"] = (f"Opcode {op['opid']:#x} is above DXIL::Op::Count. "
                           f"The converter stops at {bc}:{out_of_range_line}.")
        elif n >= enum_count:
            row["status"] = "missing"
            row["note"] = (f"The opcode is above DXIL::Op::Count ({enum_count}). "
                           f"The converter stops at {bc}:{out_of_range_line}.")
        elif spv_name and spv_name.startswith("Extended"):
            handler = table.get(spv_name)
            row["status"] = "missing"
            row["note"] = (f"The internal DXIL::Op::{spv_name} has this number."
                           + (f" The converter runs {handler[0]} ({bc}:{handler[1]})." if handler else ""))
            collisions.append(n)
        elif spv_name not in table:
            row["status"] = "missing"
            row["note"] = f"No handler. The converter stops at {bc}:{no_handler_line}."
        else:
            handler, line = table[spv_name]
            refusals = handler_refusals(handler, index)
            notes = []
            if handler_always_fails(handler, index):
                row["status"] = "refused"
                notes.append(f"{handler} returns false on every path.")
            elif refusals:
                row["status"] = "translated-partial"
                notes += [f"{where}: {msg.rstrip('. ')}." for where, msg in refusals]
            else:
                row["status"] = "translated"
            row["handler"] = f"{handler} ({bc}:{line})"
            for cname, kind, where, note in curated:
                if cname == op["name"]:
                    if kind == "partial":
                        row["status"] = "translated-partial"
                        notes.append(f"{where}: {note}")
                    cands.append((op["name"], where, note))
            if spv_name != op["name"] and spv_name.lower() != op["name"].lower():
                notes.append(f"The dxil-spirv name is {spv_name}.")
            row["note"] = " ".join(notes)
            # An operation with side effects that returns a value can be removed as dead code when the value
            # is unused, unless dxil_instruction_has_side_effects lists it.
            # A ray query allocation without a use has no effect, so dropping it is safe.
            if (op["fn_attr"] in ("", "nd", "amo") and op["ret"] not in ("v", "") and spv_name not in side_effects
                    and not op["category"].startswith(("Wave", "Quad")) and op["category"] != "Group Wave Ops"
                    and not op["name"].startswith("AllocateRayQuery")):
                dce.append(op["name"])
        if row["status"] == "missing" and op["table"] == "CoreOps" and op["name"] not in lowered \
                and op["name"][0].lower() + op["name"][1:] not in lowered:
            row["note"] += " No DXC HLSL lowering refers to it."
        rows.append(row)

    if collisions:
        cands.insert(0, (f"opcodes {min(collisions)}-{max(collisions)}", f"dxil.hpp (DXIL::Op::Extended*), {builtins.path}",
                         f"These are {len(collisions)} SM 6.10 operations. emit_dxil_instruction and both analysis "
                         "passes read each one as the internal opcode of the same number. They run that handler "
                         "and do not refuse the operation. Today the shader model ceiling stops these programs. "
                         "Before SM 6.10 work, move the internal opcodes."))
    impl_src = sources["opcodes/converter_impl.hpp"]
    dce_line = f"{impl_src.path}:{impl_src.line_of(impl_src.find(r'bool eliminate_dead_code = false;').start())}"
    vk_dce = bool(subprocess.run(["git", "-C", str(vk), "grep", "-q", "DEAD_CODE_ELIMINATE", args.vkd3d_rev, "--", "libs"],
                                 capture_output=True).returncode == 0)

    meta = dict(
        dxc=git(dxc, "rev-parse", "HEAD").strip(),
        dxc_date=git(dxc, "log", "-1", "--format=%cs").strip(),
        fork=git(fork, "rev-parse", args.dxil_spirv_rev).strip(),
        vk=git(vk, "rev-parse", args.vkd3d_rev).strip(),
        enum_count=enum_count, first_internal=first_internal, sm69_line=sm69_line, caps=caps,
        out_of_range=f"{builtins.path}:{out_of_range_line}", no_handler=f"{builtins.path}:{no_handler_line}",
        dce=dce, dce_line=dce_line, vk_dce=vk_dce)
    return render(rows, cands, meta)


# ---------------------------------------------------------------------------------------------------- render

STATUSES = ("translated", "translated-partial", "refused", "missing")


def render(rows, cands, meta):
    caps = meta["caps"]
    live = [r for r in rows if r["status"] != "reserved"]
    reserved = [r for r in rows if r["status"] == "reserved"]
    core = [r for r in live if r["table"] == "CoreOps"]
    out = []
    w = out.append
    w("# DXIL operation coverage")
    w("")
    w("Generated by `tools/docs/dxil_op_coverage.py` from the sources below. Do not edit.")
    w("")
    w("This page gives each DXIL operation of the DXC table and the status of its translation in dxil-spirv. It also "
      "tells if an application can give the operation to the installed driver today. The script reads the source. "
      "It does not compile or run a shader.")
    w("")
    w("## Inputs")
    w("")
    w(f"- DXC `utils/hct/hctdb.py` at `{meta['dxc']}` ({meta['dxc_date']}).")
    w(f"- dxil-spirv at `{meta['fork']}`: `dxil.hpp`, `opcodes/opcodes_dxil_builtins.cpp` and the handlers under "
      "`opcodes/`.")
    w(f"- vkd3d-proton at `{meta['vk']}`: `{DEVICE_C}`, the shader model gate of the engine.")
    w(f"- This repository: `{CAPS_CPP}`, `{ADAPTER_CAPS_CPP}`, `{STATE_OBJECTS_CPP}` and the evidence files that "
      "the reach columns name.")
    w("")
    w("PROVENANCE: the opcode, class, category and shader model columns come from DXC hctdb.py "
      "(University of Illinois/NCSA Open Source License).")
    w("")
    w("## Rules")
    w("")
    w("- **translated**: the dispatch table of `emit_dxil_instruction` has a handler for the opcode.")
    w("- **translated-partial**: the handler has an error path for a valid D3D12 input. A helper that the handler "
      "calls counts too, to two levels. The note gives the file:line of each path. These error paths do not count:")
    for _, why in NOT_AN_INPUT_LIMIT:
        w(f"  - {why}.")
    w("  - The AMD AGS and NVAPI extension paths. They are not DXIL semantics.")
    w("- **refused**: the handler returns false on every path.")
    w(f"- **missing**: the opcode has no handler. An opcode at or above `DXIL::Op::Count` ({meta['enum_count']}) "
      f"stops at `{meta['out_of_range']}`. An opcode without a handler stops at `{meta['no_handler']}`.")
    w(f"- dxil-spirv puts its internal opcodes (`Extended*`) after its last DXIL opcode, from {meta['first_internal']}. "
      "DXC now uses these numbers for shader model 6.10. Such an opcode is **missing**. The note names the internal "
      "handler that it runs.")
    w("- The SM column is the hctdb value. Some operations have only vector overloads. Such an operation exists only "
      "on a native vector, and DXIL has native vectors from shader model 6.9. The SM* column gives 6.9 for these "
      "operations. The totals and the reach columns use SM*.")
    w("- **Reach** is for the installed driver: the D3D12 shell and the engine behind it. There is one column for "
      "each runtime route. **Agility** is an application that loads an Agility SDK core. **22621** is an "
      "application on the System32 runtime of build 22621. `yes`: the reported shader model and the reported caps "
      "admit the operation. `no`: they do not admit it. `cond`: the cap is an engine answer, and the evidence here "
      "does not give its value.")
    w("")
    w("A static matrix finds a missing case. It cannot find a wrong translation. The candidate list names the "
      "operations that have a case but doubtful semantics. Use it to select runtime tests.")
    w("")
    w("## Reported shader model and caps")
    w("")
    ag, sy = caps["agility"], caps["system"]
    w(f"- The shell reports the engine's shader model, at most {sm_text(caps['sm'])} (`{caps['sm_line']}`). On unit "
      f"A the engine gave {caps['sm68_engine'][0]}.{caps['sm68_engine'][1:]} to a 6.8 request "
      f"(`{EVIDENCE['sm68'][0]}`).")
    w(f"- The shell lists 6.9 only at a negotiated DDI build of {caps['tier12_ddi']} or higher. It also needs the "
      f"raytracing tier 1.2 switch and the 6.9 floor of the engine (`{caps['sm69_gate_line']}`). The release "
      f"shell sets the switch (`{caps['tier12_switch_line']}`).")
    w(f"- The engine can give 6.9 (`{DEVICE_C}:{meta['sm69_line']}`). No committed evidence gives that answer. "
      "Thus the 6.9 rows of the Agility column are `cond`.")
    w(f"- The Agility route negotiates DDI build {ag['ddi']} (`{caps['ddi_max_line']}`). The shell reports "
      f"WorkGraphsTier from DDI build {caps['wg_ddi']} with the engine tier (`{caps['wg_line']}`). It admits work "
      f"graph state objects (`{caps['wg_so_line']}`). No committed evidence gives the engine tier. Thus the node "
      "rows are `cond`.")
    w(f"- The System32 runtime 22621 negotiates DDI build {sy['ddi']} and maps shader models only to 6.6. The "
      f"conformance run reports HighestShaderModel {caps['sys_sm'].replace('_', '.')} "
      f"(`{EVIDENCE['conformance'][0]}`). On that runtime, 6.7 and higher need an Agility SDK core (facts M840, "
      f"M841). Work graphs need DDI build {caps['wg_ddi']}, so that route has no work graphs.")
    w(f"- RaytracingTier {caps['rt_tier']}, ConservativeRasterizationTier {caps['crt']}: `{EVIDENCE['conformance'][0]}`. "
      f"On the Agility route, the shell can report tier 1.2 (`{caps['rt_line']}`).")
    w(f"- MeshShaderTier {caps['mesh_tier']}: `{EVIDENCE['mesh'][0]}`.")
    w(f"- SampleCmpGradientAndBias {caps['scgb']}, ExtendedCommandInfo {caps['eci']}: `{EVIDENCE['sm68'][0]}`.")
    w(f"- Not reported: sampler feedback (`{caps['sfb_line']}`), AdvancedTextureOps (`{caps['ato_line']}`), "
      f"WriteableMSAATextures (`{caps['msaa_line']}`).")
    w("")

    w("## Totals per shader model")
    w("")
    w("Counts of the core table, by the SM* column. Reserved opcodes are not counted.")
    w("")
    w("| SM | Operations | Translated | Partial | Refused | Missing | Agility yes | Agility cond | 22621 yes |")
    w("|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    tot = [0] * 8
    for sm in SHADER_MODELS:
        group = [r for r in core if r["eff_sm"] == sm]
        if not group:
            continue
        vals = [len(group)] + [sum(r["status"] == s for r in group) for s in STATUSES] + \
               [sum(r["reach"] == "yes" for r in group), sum(r["reach"] == "cond" for r in group),
                sum(r["reach_sys"] == "yes" for r in group)]
        tot = [a + b for a, b in zip(tot, vals)]
        w(f"| {sm_text(sm)} | " + " | ".join(str(v) for v in vals) + " |")
    w("| all | " + " | ".join(str(v) for v in tot) + " |")
    w("")
    exp = [r for r in live if r["table"] != "CoreOps"]
    w(f"Experimental table: {len(exp)} operation ({', '.join(r['name'] for r in exp)}), missing. "
      f"Reserved opcodes: {len(reserved)} ({sum(r['table'] == 'CoreOps' for r in reserved)} core, "
      f"{sum(r['table'] != 'CoreOps' for r in reserved)} experimental).")
    w("")

    w("## Gaps to full shader model support")
    w("")
    w("Each list gives the operations that are not **translated**, up to and including the shader model. A list "
      "after the first gives only the operations that are not in the list before it.")
    w("")
    below = (6, -1)
    for target in ((6, 8), (6, 9), (6, 10)):
        total = [r for r in core if r["eff_sm"] <= target and r["status"] != "translated"]
        gap = [r for r in total if r["eff_sm"] > below]
        head = f"### SM {sm_text(target)}: {len(total)} operations"
        if below[1] >= 0:
            head += f", the SM {sm_text(below)} list and {len(gap)} more"
        w(head)
        w("")
        below = target
        if not gap:
            w("None.")
        for r in gap:
            w(f"- `{r['name']}` ({r['index']}, SM {sm_text(r['eff_sm'])}): {r['status']}. {r['note']}".rstrip())
        w("")

    w("## Candidates for runtime tests")
    w("")
    w("These operations have a case, or their opcode reaches a handler, but their semantics look doubtful.")
    w("")
    w("| Operation | Where | Why |")
    w("|---|---|---|")
    seen = set()
    for name, where, why in cands:
        if (name, why) in seen:
            continue
        seen.add((name, why))
        w(f"| `{name}` | `{where}` | {why} |")
    if meta["dce"]:
        names = ", ".join(f"`{n}`" for n in meta["dce"])
        row = ("These operations return a value and have side effects (hctdb). "
               "`dxil_instruction_has_side_effects` does not list them. "
               "Thus the dead-code option removes them when the value has no use. "
               f"The option is off by default (`{meta['dce_line']}`).")
        row += (" vkd3d-proton sets it." if meta["vk_dce"] else
                " vkd3d-proton does not set it, so the risk is latent on our route.")
        w(f"| {names} | `opcodes/opcodes_dxil_builtins.cpp` | {row} |")
    w("")

    w("## Matrix")
    w("")
    w("The note gives the reason for an Agility value that is not `yes`. A 22621 value that differs from the "
      "Agility value has its reason too.")
    w("")
    w("| Opcode | Name | Class | SM | SM* | Status | Agility | 22621 | Note |")
    w("|---:|---|---|---|---|---|---|---|---|")
    for r in live:
        opc = r["index"] if r["table"] == "CoreOps" else f"{r['opid']:#x}"
        parts = [r["note"], r["reach_why"] if r["reach"] != "yes" else ""]
        if r["reach_sys"] != r["reach"]:
            parts.append(f"22621: {r['reach_sys_why']}")
        note = " ".join(x for x in parts if x)
        w(f"| {opc} | `{r['name']}` | {r['cls']} | {sm_text(r['sm'])} | {sm_text(r['eff_sm'])} | {r['status']} | "
          f"{r['reach']} | {r['reach_sys']} | {note.replace('|', '/')} |")
    w("")
    return "\n".join(out)


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--dxc", required=True, help="DXC git checkout")
    p.add_argument("--dxil-spirv", required=True, help="dxil-spirv git repository")
    p.add_argument("--dxil-spirv-rev", required=True)
    p.add_argument("--vkd3d", required=True, help="vkd3d-proton git repository")
    p.add_argument("--vkd3d-rev", required=True)
    p.add_argument("--check", action="store_true", help="exit 1 when the committed page differs from a fresh run")
    args = p.parse_args()
    text = build(args)
    if args.check:
        old = OUT.read_text(encoding="utf-8") if OUT.exists() else ""
        if old != text:
            print(f"{OUT.relative_to(REPO)} differs from a fresh run")
            return 1
        print(f"{OUT.relative_to(REPO)} is current")
        return 0
    OUT.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {OUT.relative_to(REPO)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
