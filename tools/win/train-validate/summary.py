"""The RESULTS.md skeleton of one train validation.

One row per arm with its value, its baseline, its verdict and the path of its raw log, the two owner release
gates stated as met or not met, and, for a failed arm, the known failure class whose symptom shape its logs
match. A symptom shape is not a diagnosis: the suite names the shape and stops there, so that the reader
goes to the raw log instead of trusting a label.

The rule the owner's gates stand on: a gate is met only when every arm of it passed **and** its number was
read and compared with its baseline. An arm that ran, said nothing and exited 0 leaves its gate NOT MET.
"""
from __future__ import annotations

import re
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

from runner import (BOUND, FAIL, GOOD, OPERATOR, PASS, SKIPPED, THERMAL, UNREAD, WARN, ArmRecord, judge)

GATE_RULES = {
    "rottr": ("Rise of the Tomb Raider", "owner, 2026-10-04: no tester release without a working Rise of the "
                                         "Tomb Raider"),
    "q2rtx": ("Quake II RTX with ray tracing", "owner, 2026-10-08: no package ships until Quake II RTX runs, "
                                               "and it ships with support for it"),
}
# Verdicts whose raw logs are read again for a symptom shape. UNREAD belongs here: an arm that ran and
# produced no number is exactly the shape of BD-102 (the GPU hangs and the run ends with no frame rate).
BAD = (FAIL, BOUND, THERMAL, UNREAD)


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


def gate_text(record: ArmRecord) -> str:
    """The health gate's own output of one arm, from the file the runner wrote next to the arm's log.

    The record keeps the gate's facts and violations, not its text, so the failure-class matcher reads the
    file. Without this the matcher saw an empty string and no gate symptom could ever match.
    """
    gate = record.gate or {}
    raw = (gate.get("facts") or {}).get("raw") or ""
    text = "\n".join(gate.get("violations") or ())
    path = Path(raw) if raw else None
    if path and path.is_file():
        text += "\n" + path.read_text(encoding="utf-8", errors="replace")
    return text


def gates(records: list[ArmRecord], plan) -> list[Gate]:
    out = []
    for name, (title, rule) in GATE_RULES.items():
        arms = [r for r in records if r.gate_name == name]
        if not arms:
            out.append(Gate(name, title, rule, False, "no arm of this gate ran"))
            continue
        blocking = []
        for record in arms:
            if record.verdict not in GOOD:
                blocking.append(f"{record.id} {record.verdict}"
                                + (f" ({record.reason})" if record.reason else ""))
            elif record.value_name and not record.value_read:
                blocking.append(f"{record.id} has no {record.value_name} yet: "
                                + ("the operator still reads it from the screen" if record.value_operator
                                   else "nothing in its raw log matched the result line of the arm"))
            elif record.baseline_ok is False:
                blocking.append(f"{record.id} {record.value} {record.unit} against its baseline"
                                + (f": {record.reason}" if record.reason else ""))
        if blocking:
            out.append(Gate(name, title, rule, False, "; ".join(blocking)))
            continue
        out.append(Gate(name, title, rule, True,
                        "; ".join(f"{r.id} {r.verdict}"
                                  + (f" {r.value} {r.unit}" if r.value is not None else "") for r in arms)))
    return out


def _value_cell(record: ArmRecord) -> str:
    if record.value is None:
        if not record.value_name:
            return "-"
        return "the operator reads it" if record.value_operator else "**unread**"
    unit = f" {record.unit}" if record.unit else ""
    given = "" if record.value_operator or not record.value_given else " (given on the command line)"
    return f"{record.value}{unit}{given}"


def _baseline_cell(record: ArmRecord) -> str:
    if not record.baseline:
        return "-"
    base = record.baseline
    tolerance = (f"+-{base['tolerance_abs']}" if "tolerance_abs" in base
                 else f"-{base['tolerance_pct']:g}%")
    unit = f" {record.unit}" if record.unit else ""
    return f"{base['value']}{unit} ({tolerance})"


def fill_operator_values(records: list[ArmRecord], operator_values: dict) -> list[str]:
    """Put the readings of a person into their arms, and hold each one to its baseline.

    `summary --set rottr=12.0` used to print PASS and a met gate. A number a person read is compared with
    the baseline exactly as a parsed one is, so a reading far under it is a WARN and its gate is not met.
    """
    notes = []
    for record in records:
        if record.id not in operator_values:
            continue
        record.value, record.value_given = operator_values[record.id], True
        verdict, why, record.baseline_ok, record.value_read = judge(record)
        if verdict != PASS and record.verdict in (PASS, OPERATOR, WARN, UNREAD):
            record.verdict, record.reason = verdict, why
        elif verdict == PASS and record.verdict == UNREAD:
            record.verdict, record.reason = PASS, ""
        if not record.value_operator:
            # This arm's value was meant to come from its own log. A number given on the command line is
            # kept, and it says where it came from, so that a parse miss is never laundered into a value.
            record.reason = (record.reason + "; " if record.reason else "") + \
                "this value was given on the command line, not read from the arm's log"
        notes.append(f"{record.id}={record.value}")
    return notes


def latest_attempts(records: list[ArmRecord]) -> tuple[list[ArmRecord], list[ArmRecord]]:
    """Split the records into the last attempt of each arm and the attempts it superseded.

    A `--resume` run appends a new record for every arm it runs again, so an arm that failed, was repaired
    and then passed holds two records. The last one is the arm's verdict; the earlier ones are its history
    and are reported as such. Without this, a repaired arm kept its old FAIL in the table, in the gates and
    in the exit code (b28 validation, 2026-10-10).
    """
    last: dict[str, int] = {}
    for index, record in enumerate(records):
        last[record.id] = index
    latest = [r for i, r in enumerate(records) if last[r.id] == i]
    earlier = [r for i, r in enumerate(records) if last[r.id] != i]
    return latest, earlier


def write(plan, records: list[ArmRecord], out_dir: Path, texts: dict | None = None,
          operator_values: dict | None = None, train: str = "") -> str:
    texts = texts or {}
    records, superseded = latest_attempts(records)
    fill_operator_values(records, operator_values or {})
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
    unresolved = [r.id for r in records if r.verdict == OPERATOR or (r.value_name and not r.value_read)]
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
    failed = [r for r in records if r.verdict in BAD]
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
        hits = match_classes("\n".join([text, gate_text(record), record.reason]), plan.failure_classes)
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
    owed = [r for r in records if r.verdict in (OPERATOR, SKIPPED) or (r.value_name and not r.value_read)]
    if not owed:
        lines.append("Nothing: every arm ran and every value is read.")
    for record in owed:
        how = (record.value_spec or {}).get("how", "")
        lines.append(f"- `{record.id}`: {record.reason or how or 'the value is read from the screen'}."
                     + ("" if record.value_read or not record.value_name else
                        f" Fill it in with `validate.py summary --out <dir> --package <dir> "
                        f"--set {record.id}=<{record.value_name}>`."))
    if superseded:
        lines += ["", "## Earlier attempts of an arm that ran again", "",
                  "Each row is a record of this same directory that a later attempt of its arm superseded. "
                  "They are the history of the round, not its verdict. The raw logs of each one are kept "
                  "next to the arm's own directory as `<arm>-attempt-N`, in the order they ran.", "",
                  "| Arm | Verdict | What ended it |", "|---|---|---|"]
        for record in superseded:
            lines.append(f"| `{record.id}` | {record.verdict} | {record.reason or 'see the raw log'} |")
    lines += ["", "## Evidence", "",
              f"All raw logs are under `{out_dir.as_posix()}`, one directory per arm, with the arm's own "
              f"output in `arm.txt`, each step in `step-NN-<phase>.txt`, the health gate in `gate.txt` and "
              f"the plug samples in `plug.jsonl` where the arm sampled them.", ""]
    return "\n".join(lines)
