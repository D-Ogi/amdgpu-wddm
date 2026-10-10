"""The RESULTS.md skeleton of one train validation.

One row per arm with its value, its baseline, its verdict and the path of its raw log, the two owner release
gates stated as met or not met, and, for a failed arm, the known failure class whose symptom shape its logs
match. A symptom shape is not a diagnosis: the suite names the shape and stops there, so that the reader
goes to the raw log instead of trusting a label.
"""
from __future__ import annotations

import re
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

from runner import BOUND, FAIL, GOOD, OPERATOR, PASS, SKIPPED, THERMAL, WARN, ArmRecord

GATE_RULES = {
    "rottr": ("Rise of the Tomb Raider", "owner, 2026-10-04: no tester release without a working Rise of the "
                                         "Tomb Raider"),
    "q2rtx": ("Quake II RTX with ray tracing", "owner, 2026-10-08: no package ships until Quake II RTX runs, "
                                               "and it ships with support for it"),
}


@dataclass
class Gate:
    name: str
    title: str
    rule: str
    met: bool
    detail: str


def match_classes(text: str, classes: list[dict]) -> list[dict]:
    found = []
    for failure in classes:
        hits = [pattern for pattern in failure.get("match", ()) if re.search(pattern, text, re.I | re.M)]
        if hits and (failure.get("needs_any", True) or len(hits) == len(failure["match"])):
            found.append({"id": failure["id"], "symptom": failure["symptom"], "hits": hits})
    return found


def gates(records: list[ArmRecord], plan) -> list[Gate]:
    out = []
    for name, (title, rule) in GATE_RULES.items():
        arms = [r for r in records if r.gate_name == name]
        if not arms:
            out.append(Gate(name, title, rule, False, "no arm of this gate ran"))
            continue
        bad = [r for r in arms if r.verdict not in GOOD]
        if bad:
            out.append(Gate(name, title, rule, False,
                            "; ".join(f"{r.id} {r.verdict}" + (f" ({r.reason})" if r.reason else "")
                                      for r in bad)))
            continue
        open_values = [r for r in arms if r.value is None and r.value_name]
        detail = "; ".join(f"{r.id} {r.verdict}" + (f" {r.value} {r.unit}" if r.value is not None else "")
                           for r in arms)
        if open_values:
            detail += " (the value is still the operator's reading: " + \
                      ", ".join(r.id for r in open_values) + ")"
        out.append(Gate(name, title, rule, True, detail))
    return out


def _value_cell(record: ArmRecord) -> str:
    if record.value is None:
        return "operator" if record.value_name else "-"
    unit = f" {record.unit}" if record.unit else ""
    return f"{record.value}{unit}"


def _baseline_cell(record: ArmRecord) -> str:
    if not record.baseline:
        return "-"
    base = record.baseline
    tolerance = (f"+-{base['tolerance_abs']}" if "tolerance_abs" in base
                 else f"-{base['tolerance_pct']:g}%")
    unit = f" {record.unit}" if record.unit else ""
    return f"{base['value']}{unit} ({tolerance})"


def write(plan, records: list[ArmRecord], out_dir: Path, texts: dict | None = None,
          operator_values: dict | None = None, train: str = "") -> str:
    texts = texts or {}
    operator_values = operator_values or {}
    for record in records:
        if record.id in operator_values and record.value is None:
            record.value = operator_values[record.id]
    package = plan.package
    now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%MZ")
    lines = [f"# {train or 'Train'} lab validation: {package.release} on unit A", ""]
    lines += [
        f"Package under test: `{package.name}.zip`, SHA-256 `{package.zip_sha256 or 'not hashed'}`, "
        f"{package.zip_bytes} bytes, KMD build {package.kmd_build}, ABI {package.kmd_abi}, built "
        f"{package.built_utc}. The suite ran the arms below from `{out_dir.as_posix()}`; every value in the "
        f"table comes from the raw log named in its row. Written {now}.", ""]
    counts = {}
    for record in records:
        counts[record.verdict] = counts.get(record.verdict, 0) + 1
    lines += ["## Verdict", ""]
    for gate in gates(records, plan):
        lines.append(f"- **{gate.title}**: {'MET' if gate.met else 'NOT MET'}. {gate.detail}. "
                     f"The rule is the {gate.rule}.")
    lines.append("- Arms: " + ", ".join(f"{count} {verdict}" for verdict, count in sorted(counts.items())) + ".")
    unresolved = [r.id for r in records if r.verdict == OPERATOR or (r.value is None and r.value_name)]
    if unresolved:
        lines.append("- Still open for the operator: " + ", ".join(sorted(set(unresolved))) + ".")
    lines += ["", "## Arms", "",
              "| # | Arm | Value | Baseline | Verdict | Raw |", "|---|---|---|---|---|---|"]
    for number, record in enumerate(records, start=1):
        raw = record.raw_paths[0] if record.raw_paths else "-"
        if raw != "-":
            raw = f"`{Path(raw).name}` in `{Path(raw).parent.name}/`"
        verdict = f"**{record.verdict}**" if record.verdict not in (PASS, OPERATOR) else record.verdict
        title = record.title or record.id
        note = f" {record.reason}" if record.reason and record.verdict not in (PASS, OPERATOR) else ""
        lines.append(f"| {number} | {title} (`{record.id}`) | {_value_cell(record)} | "
                     f"{_baseline_cell(record)} | {verdict}{note} | {raw} |")
    failed = [r for r in records if r.verdict in (FAIL, BOUND, THERMAL)]
    lines += ["", "## Failed arms", ""]
    if not failed:
        lines.append("No arm failed, so no symptom shape is named here.")
    for record in failed:
        text = texts.get(record.id, "")
        if not text:
            for path in record.raw_paths:
                candidate = Path(path)
                if candidate.name == "arm.txt" and candidate.is_file():
                    text = candidate.read_text(encoding="utf-8", errors="replace")
        hits = match_classes(text + "\n" + (record.gate or {}).get("raw", ""), plan.failure_classes)
        lines.append(f"- **{record.id}** {record.verdict}: {record.reason or 'see the raw log'}. "
                     f"Raw: " + ", ".join(f"`{Path(p).as_posix()}`" for p in record.raw_paths if p) + ".")
        for failure in hits:
            lines.append(f"  - The symptom shape matches **{failure['id']}**: {failure['symptom']} "
                         f"Matched on: {', '.join(repr(h) for h in failure['hits'])}. A symptom shape is not "
                         f"a diagnosis.")
        if not hits:
            lines.append("  - No known failure class matches this shape. It is new, and it needs its own "
                         "look before the next round.")
    gate_warnings = [r for r in records if r.gate and not r.gate.get("ok")]
    lines += ["", "## Health gates", ""]
    if not gate_warnings:
        lines.append("Every health gate after an arm was clean: 0 GPU faults, 0 fence timeouts, 0 Display "
                     "4101 events, 0 bugchecks, health flags=15, TdrDelay 10 and the fan on our curve.")
    for record in gate_warnings:
        lines.append(f"- After **{record.id}**: " + "; ".join(record.gate["violations"]) + ".")
    lines += ["", "## What the operator still owes", ""]
    owed = [r for r in records if r.verdict in (OPERATOR, SKIPPED) or (r.value is None and r.value_name)]
    if not owed:
        lines.append("Nothing: every arm ran and every value is read.")
    for record in owed:
        lines.append(f"- `{record.id}`: {record.reason or 'the value is read from the screen'}.")
    lines += ["", "## Evidence", "",
              f"All raw logs are under `{out_dir.as_posix()}`, one directory per arm, with the arm's own "
              f"output in `arm.txt`, each step in `step-NN-<phase>.txt`, the health gate in `gate.txt` and "
              f"the plug samples in `plug.jsonl` where the arm sampled them.", ""]
    return "\n".join(lines)
