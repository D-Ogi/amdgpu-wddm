"""The D3D12 triplet promotion of a train, as one idempotent step.

The promotion itself belongs to `scratch/m15/native-caps001/promote-d3d12.py`, which owns the accepted
triplet, the witness and the rollback. This module only drives that tool in the right order and removes the
two things that cost a round in train b27:

* a leftover attempt directory. `run.py Stage` makes `attempts/<name>/package` with `exist_ok=False`, so a
  second stage of the same attempt name fails. Here an attempt directory that holds no lab evidence is moved
  aside under `attempts/.leftover/`, and one that does hold evidence is left alone and the next free number
  is used. Evidence is never deleted.
* a promotion that is already done. When the package triplet is the accepted triplet, `stage` refuses. That
  is not a failure of the train: the step reports that the registered triplet is already this package's and
  runs the acceptance session only.

After the acceptance the lab baseline must name this package. `release-baseline.py --keep-accepted-d3d12`
re-pins it without touching the triplet block the promotion just wrote.
"""
from __future__ import annotations

import json
import re
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

from manifest import Plan, PlannedStep, sha256

ATTEMPT = re.compile(r"^native-caps(\d{3})$")
# What makes an attempt directory evidence rather than a leftover of a refused stage.
EVIDENCE = ("pull", "analysis", "live", "drive", "collected.tar", "result.json")
NAMES = ("amdgpu_wddm_d3d12.dll", "amdgpu_wddm_vkd3d.dll", "amdgpu_wddm_radv.dll")


@dataclass
class Prepared:
    attempt: str
    check_attempt: str
    moved_aside: list[str]
    already_accepted: bool
    accepted: dict
    candidate: dict
    note: str


def attempts_dir(plan: Plan) -> Path:
    return Path(plan.values["caps"]) / "attempts"


def baseline_path(plan: Plan) -> Path:
    return Path(plan.values["caps"]) / "lab-baseline.json"


def next_attempt(directory: Path, taken: int = 2) -> list[str]:
    numbers = [int(found.group(1)) for found in
               (ATTEMPT.match(child.name) for child in
                (directory.iterdir() if directory.is_dir() else ()) if child.is_dir()) if found]
    start = (max(numbers) if numbers else 0) + 1
    return [f"native-caps{start + offset:03d}" for offset in range(taken)]


def has_evidence(directory: Path) -> bool:
    return any((directory / name).exists() for name in EVIDENCE)


def prepare(plan: Plan, arm: dict, clean: bool = True) -> Prepared:
    """Choose the attempt names, move a leftover aside and say whether the triplet is already accepted."""
    directory = attempts_dir(plan)
    if clean:
        directory.mkdir(parents=True, exist_ok=True)
    moved: list[str] = []
    candidate = {}
    for name, key in zip(NAMES, ("shell", "engine", "icd")):
        path = Path(plan.values["pkg"]) / "payload" / "d3d12" / name
        template = arm.get("promote", {}).get(key, "")
        if template:
            path = Path(template)
        candidate[name] = sha256(path) if path.is_file() else ""
    accepted = {}
    if baseline_path(plan).is_file():
        block = json.loads(baseline_path(plan).read_text(encoding="utf-8")).get("d3d12") or {}
        accepted = dict(block.get("accepted") or {})
    already = bool(accepted) and all(accepted.get(name) == candidate.get(name) for name in NAMES)
    wanted = [arm.get("attempt_name"), arm.get("check_attempt_name")]
    if not all(wanted):
        wanted = next_attempt(directory)
    notes = []
    if any((directory / name).exists() and has_evidence(directory / name) for name in wanted):
        held = [name for name in wanted if (directory / name).exists() and has_evidence(directory / name)]
        wanted = next_attempt(directory)
        notes.append("the asked attempt " + ", ".join(held) + " holds lab evidence and stays untouched; "
                     "this step uses " + " and ".join(wanted))
    for name in wanted:
        path = directory / name
        if not path.exists():
            continue
        # A directory with no lab evidence is the leftover of a refused stage: run.py Stage makes
        # attempts/<name>/package with exist_ok=False, so it would refuse again. It is moved, never deleted.
        if not clean:
            notes.append(f"{name} exists with no lab evidence; a run would move it aside")
            continue
        spare = directory / ".leftover"
        spare.mkdir(exist_ok=True)
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        path.rename(spare / f"{name}-{stamp}")
        moved.append((spare / f"{name}-{stamp}").as_posix())
    notes.insert(0, "the package triplet is the accepted triplet: only the acceptance session runs"
                 if already else "the package triplet differs from the accepted one: stage, session, accept")
    return Prepared(attempt=wanted[0], check_attempt=wanted[1], moved_aside=moved, already_accepted=already,
                    accepted=accepted, candidate=candidate, note="; ".join(notes))


def steps(plan: Plan, arm: dict, prepared: Prepared, python: str) -> list[PlannedStep]:
    """The exact sequence, each lab step inside the trial bound."""
    caps = plan.values["caps"]
    bound = int(arm["bound_s"])
    grace = int(plan.limits["host_grace_s"])
    out = []

    def host(argv, timeout=300):
        out.append(PlannedStep([python, *argv], timeout, "run"))

    def lab(argv, timeout=bound + grace):
        out.append(PlannedStep([python, *argv], timeout, "run"))

    if not prepared.already_accepted:
        triplet = []
        for key in ("shell", "engine", "icd"):
            value = arm.get("promote", {}).get(key)
            if value:
                triplet += [f"--{key}", value]
        host([f"{caps}/promote-d3d12.py", "stage", "--attempt", prepared.attempt,
              "--check-attempt", prepared.check_attempt, *triplet])
        lab([f"{caps}/run-slot.py", prepared.attempt])
        lab([f"{caps}/close-attempt.py", prepared.attempt], 300)
        host([f"{caps}/promote-d3d12.py", "accept", "--attempt", prepared.attempt])
        host([f"{caps}/promote-d3d12.py", "accept", "--attempt", prepared.attempt, "--apply"])
    host([f"{caps}/promote-d3d12.py", "stage-check", "--attempt", prepared.check_attempt])
    lab([f"{caps}/run-slot.py", prepared.check_attempt])
    lab([f"{caps}/close-attempt.py", prepared.check_attempt], 300)
    # Through `pin-baseline.py`, for the reason that script carries: `release-baseline.py --apply` refuses a
    # second apply of one release, and a repeated or resumed validation run must answer as the first one did.
    host([f"{plan.values['repo']}/tools/win/train-validate/pin-baseline.py", caps, plan.values["pkg"],
          "--keep-accepted-d3d12"])
    return out


def attach(plan: Plan, arm, python: str, clean: bool = True) -> Prepared:
    """Fill a promote arm's steps and the lines its output must carry. Offline; `clean=False` moves nothing."""
    prepared = prepare(plan, arm.arm, clean=clean)
    arm.steps = steps(plan, arm.arm, prepared, python)
    arm.arm["prepared"] = {"attempt": prepared.attempt, "check_attempt": prepared.check_attempt,
                           "already_accepted": prepared.already_accepted, "moved_aside": prepared.moved_aside,
                           "note": prepared.note, "candidate": prepared.candidate,
                           "accepted": prepared.accepted}
    arm.arm["attempt_name"] = prepared.attempt
    wanted = ["functional-restored", "files verified"]
    if not prepared.already_accepted:
        wanted = ["promoted-retained", *wanted]
    arm.arm["expect_all"] = wanted
    return prepared
