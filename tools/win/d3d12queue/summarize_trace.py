# SPDX-License-Identifier: MIT
"""Bounded offline summary of one process's mixed runtime.err and optional API trace.

Only named event fields survive. DDI S_OK denotes normal return, including void
DDIs; neither a returned DDI nor a hosted callback proves GPU completion.
"""
import argparse
from collections import Counter
import json
import re
from pathlib import Path

MAX_LINE = 8192
MAX_IDS = 50000
MAX_BUCKETS = 64
MAX_EXAMPLES = 8
U64 = (1 << 64) - 1
# Fixed schemas keep observations bounded and exclude payloads or private pointers.
STATUS_OBSERVATIONS = frozenset(("ddi-caps", "ddi-layout-set"))
OBSERVATIONS = {
    "ddi-caps": ("last_caps", ("type", "data_size", "info_present"), ("info_present",), None),
    "ddi-layout-set": ("last_layout_set", ("layout", "unit"), (), None),
    "ddi-caps-memory": (
        "last_caps_memory",
        ("type", "node", "uma", "io_coherent", "cache_coherent", "heap_serialization", "resource_serialization"),
        ("uma", "io_coherent", "cache_coherent"), 1002),
    "ddi-caps-layout": (
        "last_caps_layout",
        ("type", "layouts", "swizzles", "standard64k", "row_major", "indexable"),
        ("standard64k", "row_major", "indexable"), 1060),
    "ddi-node-map": ("last_node_map", ("count", "first", "lost"), ("lost",), None),
}
API_NAMES = frozenset((
    "create-device", "create-queue", "copy", "status", "exit", "abort",
    "CreateDXGIFactory1", "EnumAdapters1", "EnumWarpAdapter", "GetDesc1", "GetDeviceRemovedReason",
    "D3D12CreateDevice FL11_0", "CreateCommandQueue DIRECT", "CreateCommittedResource UPLOAD",
    "CreateCommittedResource READBACK", "Map UPLOAD", "Unmap UPLOAD",
    "CreateCommandAllocator", "CreateCommandList", "CopyBufferRegion 4096",
    "Close CommandList", "CreateFence", "ExecuteCommandLists", "Queue Signal 1",
    "SetEventOnCompletion 1", "WaitForFence bounded", "GetCompletedValue",
    "Map READBACK", "Unmap READBACK", "Compare 4096 exact bytes",
))


def integer(value, maximum=U64):
    return value if type(value) is int and 0 <= value <= maximum else None


def status(value):
    if isinstance(value, str) and re.fullmatch(r"(?:0x)?[0-9a-fA-F]{8}", value):
        return value.removeprefix("0x").lower()
    return None


def ddi_name(value):
    return value if isinstance(value, str) and re.fullmatch(r"pfn[A-Za-z0-9_]{1,90}", value) else None


def bump(counter, key):
    # A separate overflow bucket bounds adversarial or unexpected cardinality.
    if key not in counter and len(counter) >= MAX_BUCKETS:
        key = "other"
    counter[key] += 1


def records(path, counts):
    with Path(path).open("rb") as source:
        while True:
            line = source.readline(MAX_LINE + 1)
            if not line:
                return
            counts["lines"] += 1
            if len(line) > MAX_LINE:
                while line and not line.endswith(b"\n"):
                    line = source.readline(MAX_LINE + 1)
                counts["oversized_lines"] += 1
                continue
            try:
                text = line.decode("utf-8-sig").strip()
            except UnicodeDecodeError:
                counts["invalid_encoding"] += 1
                continue
            if not text.startswith("{"):
                counts["text_lines"] += 1
                continue
            try:
                obj = json.loads(text)
            except (ValueError, RecursionError):
                counts["invalid_json"] += 1
                continue
            if not isinstance(obj, dict):
                counts["invalid_schema"] += 1
                continue
            counts["json_records"] += 1
            yield obj


def summarize(runtime_err, api_trace=None):
    counts = Counter()
    ddi_counts, ddi_status, names = Counter(), Counter(), Counter()
    hosted_counts, hosted_status, hosted_ops = Counter(), Counter(), Counter()
    sequences = {}
    frequencies = set()
    clock_conflict = False
    last_ddi = last_format = last_msaa = None
    observations = {schema[0]: None for schema in OBSERVATIONS.values()}
    for obj in records(runtime_err, counts):
        event = obj.get("event")
        if event == "clock":
            frequency = integer(obj.get("frequency"))
            if not frequency:
                counts["invalid_schema"] += 1
            elif frequencies and frequency not in frequencies:
                clock_conflict = True
            else:
                frequencies.add(frequency)
        elif event == "ddi":
            edge, seq = obj.get("edge"), integer(obj.get("sequence"))
            name, thread = ddi_name(obj.get("name")), integer(obj.get("thread"), 0xffffffff)
            tick = integer(obj.get("qpc"))
            code = status(obj.get("status")) if edge == "end" else None
            if edge not in ("begin", "end") or not seq or not name or thread is None or tick is None or (edge == "end" and code is None):
                counts["invalid_schema"] += 1
                continue
            ddi_counts[edge] += 1
            bump(names, name)
            last_ddi = dict(sequence=seq, name=name, edge=edge)
            if code is not None:
                bump(ddi_status, code)
                last_ddi.update(status=code, outcome="normal_return" if code == "00000000" else "reported_status")
            if seq not in sequences:
                if len(sequences) >= MAX_IDS:
                    ddi_counts["untracked_records"] += 1
                    continue
                sequences[seq] = {"begin_count": 0, "end_count": 0}
            record = sequences[seq]
            record[edge + "_count"] += 1
            if edge not in record:
                record[edge] = (name, thread, tick, counts["lines"])
            else:
                ddi_counts["duplicate_" + edge] += 1
        elif event == "hosted-callback":
            edge, op = obj.get("edge"), integer(obj.get("op"), 0xffffffff)
            code = status(obj.get("status")) if edge == "end" else None
            if edge not in ("begin", "end") or op is None or (edge == "end" and code is None):
                counts["invalid_schema"] += 1
                continue
            hosted_counts[edge] += 1
            bump(hosted_ops, str(op))
            if code is not None:
                bump(hosted_status, code)
        elif isinstance(event, str) and event in OBSERVATIONS:
            key, fields, flags, expected_type = OBSERVATIONS[event]
            selected = {field: integer(obj.get(field), 0xffffffff) for field in fields}
            if (any(value is None for value in selected.values()) or
                    any(selected[flag] not in (0, 1) for flag in flags) or
                    (expected_type is not None and selected["type"] != expected_type)):
                counts["invalid_schema"] += 1
                continue
            if event in STATUS_OBSERVATIONS:
                code = obj.get("status")
                if not isinstance(code, str) or not re.fullmatch(r"[0-9a-fA-F]{8}", code):
                    counts["invalid_schema"] += 1
                    continue
                selected["status"] = code.lower()
            observations[key] = selected
            counts[event] += 1
        elif event in ("ddi-format", "ddi-msaa"):
            fields = ("format", "output_present", "support") if event == "ddi-format" else ("format", "samples", "flags", "output_present", "levels")
            selected = {key: integer(obj.get(key), 0xffffffff) for key in fields}
            if any(value is None for value in selected.values()) or selected["output_present"] not in (0, 1):
                counts["invalid_schema"] += 1
                continue
            if event == "ddi-format":
                last_format = selected
            else:
                last_msaa = selected
            counts[event] += 1
        else:
            counts["ignored_json"] += 1

    anomalies = []
    duration_ticks = []
    paired = 0
    for seq, record in sequences.items():
        issues = []
        if not record["begin_count"]:
            ddi_counts["unmatched_end"] += record["end_count"]
            issues.append("end_without_begin")
        if not record["end_count"]:
            ddi_counts["unmatched_begin"] += record["begin_count"]
            issues.append("begin_without_end")
        if record["begin_count"] > 1 or record["end_count"] > 1:
            issues.append("duplicate_sequence")
        if record["begin_count"] == record["end_count"] == 1:
            begin, end = record["begin"], record["end"]
            if begin[:2] != end[:2]:
                ddi_counts["identity_mismatch"] += 1
                issues.append("identity_mismatch")
            elif end[2] < begin[2] or end[3] < begin[3]:
                ddi_counts["order_mismatch"] += 1
                issues.append("order_mismatch")
            else:
                paired += 1
                duration_ticks.append(end[2] - begin[2])
        if issues and len(anomalies) < MAX_EXAMPLES:
            anomalies.append(dict(sequence=seq, issues=issues))
    frequency = next(iter(frequencies)) if frequencies and not clock_conflict else None
    ddi_counts["paired"] = paired
    result = dict(schema=1, interpretation="DDI normal_return is not GPU completion or API success proof",
                  input=dict(counts), clock=dict(frequency=frequency, conflicting=clock_conflict),
                  ddi=dict(counts=dict(ddi_counts), statuses=dict(ddi_status), name_edges=dict(names),
                           last=last_ddi, anomaly_examples=anomalies,
                           pairing_complete=bool(sequences) and not any(counts[k] for k in ("invalid_json", "invalid_encoding", "oversized_lines", "invalid_schema")) and not any(ddi_counts[k] for k in ("untracked_records", "duplicate_begin", "duplicate_end", "unmatched_begin", "unmatched_end", "identity_mismatch", "order_mismatch"))),
                  hosted=dict(counts=dict(hosted_counts), statuses=dict(hosted_status), operation_edges=dict(hosted_ops),
                              pairing="not_attempted_device_local_ids"),
                  last_format=last_format, last_msaa=last_msaa)
    result.update(observations)
    if frequency and duration_ticks:
        result["ddi"]["elapsed_ms"] = dict(paired_total=round(sum(duration_ticks) * 1000 / frequency, 6),
                                              paired_max=round(max(duration_ticks) * 1000 / frequency, 6))
    if api_trace is not None:
        api_counts, api_statuses, api_names, read_counts = Counter(), Counter(), Counter(), Counter()
        last = None
        for obj in records(api_trace, read_counts):
            phase, name = obj.get("phase"), obj.get("api")
            code, seq, elapsed = status(obj.get("hr")), integer(obj.get("sequence")), integer(obj.get("elapsed_ms"))
            if phase not in ("before", "after") or not isinstance(name, str) or name not in API_NAMES or code is None or seq is None or elapsed is None:
                read_counts["invalid_schema"] += 1
                continue
            api_counts[phase] += 1
            bump(api_names, name)
            if phase == "after":
                bump(api_statuses, code)
            last = dict(sequence=seq, api=name, phase=phase, hr=code, elapsed_ms=elapsed)
        result["api"] = dict(input=dict(read_counts), counts=dict(api_counts), statuses=dict(api_statuses), name_edges=dict(api_names), last=last)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime-err", required=True, type=Path)
    parser.add_argument("--trace", type=Path, help="Optional interactive trace.jsonl")
    args = parser.parse_args()
    try:
        result = summarize(args.runtime_err, args.trace)
    except OSError:
        parser.exit(2, "Cannot read an explicit input file.\n")
    print(json.dumps(result, separators=(",", ":"), sort_keys=True))


if __name__ == "__main__":
    main()
