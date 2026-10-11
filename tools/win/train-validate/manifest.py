"""The arm manifest of a release train validation, and the package it is run against.

Nothing here starts a process or reaches the lab: this module reads `arms.json`, reads the package's own
`manifest.json`, resolves the placeholders of every command and hands back a plan. `validate.py --dry-run`
prints exactly what this module built, so a plan can be read before anything runs.
"""
from __future__ import annotations

import hashlib
import json
import os
import re
from dataclasses import dataclass, field
from pathlib import Path

HERE = Path(__file__).resolve().parent
PLACEHOLDER = re.compile(r"\{([a-z_0-9]+)\}")
# Arms that run a workload on the lab. The owner's three-minute trial bound applies to these.
TRIAL_KINDS = ("lab", "lab-read", "promote")
# Arms that run on the development PC, move a file or wait for a restart. They touch no GPU, so the trial
# bound does not apply; each one still carries a bound of its own.
HOST_KINDS = ("host", "transfer", "restart")
GAME_KINDS = ("game", "operator")
# An arm that changes the board's memory carve-out says which way: `set` writes the size the games run at, and
# `restore` puts back the size before the change. `game_window` orders the game arms between the two.
BOARD_MEMORY_MARKS = ("set", "restore")


class ManifestError(Exception):
    """The manifest, the package or the request is wrong. It is raised before anything runs."""


def find_bash() -> str:
    """The POSIX shell that runs a session script, named by its own path.

    On Windows `bash` on PATH is `C:\\Windows\\System32\\bash.exe`, the WSL launcher: it tries to mount this
    machine's WSL disk and ends rc 1 in two seconds without starting anything. The game arms need the Git
    shell, so this returns the first Git `bash.exe` that exists and falls back to the bare name.
    """
    for candidate in (os.environ.get("TRAIN_VALIDATE_BASH", ""),
                      r"C:\Program Files\Git\bin\bash.exe",
                      r"C:\Program Files\Git\usr\bin\bash.exe",
                      r"C:\Program Files (x86)\Git\bin\bash.exe"):
        if candidate and Path(candidate).is_file():
            return candidate
    return "bash"


def find_roots(tool: Path | None = None) -> tuple[Path, Path]:
    """(repo root, workspace root) from this file's place on disk.

    Works in the repository checkout and in a worktree under `scratch/`: the repo is the directory that
    holds `tools/win/train-validate`, and the workspace is the first parent above it that holds both
    `scratch` and a directory with the repo's own `tools/win/target.py`.
    """
    here = (tool or HERE).resolve()
    repo = None
    for parent in [here, *here.parents]:
        if (parent / "tools" / "win" / "train-validate").is_dir():
            repo = parent
            break
    if repo is None:  # pragma: no cover - only if the tool is moved out of its directory
        raise ManifestError(f"no repository root above {here}")
    forced = os.environ.get("BC250_ROOT")
    if forced:
        return repo, Path(forced).resolve()
    for parent in repo.parents:
        # A worktree under scratch/ holds tools/win/target.py too, and scratch/train holds a scratch/
        # directory of its own, so a candidate inside a scratch tree is never the workspace root.
        if any(part.lower() == "scratch" for part in parent.parts):
            continue
        if (parent / "scratch").is_dir() and any(
            (child / "tools" / "win" / "target.py").is_file() for child in parent.iterdir() if child.is_dir()
        ):
            return repo, parent
    # A clone with no workspace around it (a build machine): the parent stands in, so that the host tests
    # and `validate.py arms` still run. Only a lab run needs the real workspace, and it names it.
    return repo, repo.parent


def next_attempt_base(caps_dir: Path) -> int | None:
    """The first free native-caps number of the trial harness, so the one command needs no number typed."""
    attempts = Path(caps_dir) / "attempts"
    if not attempts.is_dir():
        return None
    numbers = [int(name[-3:]) for name in (child.name for child in attempts.iterdir() if child.is_dir())
               if re.fullmatch(r"native-caps\d{3}", name)]
    return (max(numbers) + 1) if numbers else 1


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest().upper()


@dataclass
class Package:
    """The release package under test, read from its own manifest."""

    directory: Path
    release: str
    name: str
    kmd_build: str
    kmd_abi: str
    built_utc: str
    zip_path: Path | None
    zip_sha256: str
    zip_bytes: int

    @classmethod
    def read(cls, directory: Path, hash_zip: bool = True) -> "Package":
        directory = Path(directory).resolve()
        manifest = directory / "manifest.json"
        if not manifest.is_file():
            raise ManifestError(f"no manifest.json in {directory}: that is not a release package")
        data = json.loads(manifest.read_text(encoding="utf-8"))
        name = data.get("name") or directory.name
        candidate = directory.parent / (name + ".zip")
        zip_path = candidate if candidate.is_file() else None
        digest, size = "", 0
        if zip_path and hash_zip:
            digest, size = sha256(zip_path), zip_path.stat().st_size
        return cls(directory=directory, release=data.get("release") or data.get("version") or name, name=name,
                   kmd_build=data.get("kmd_build", ""), kmd_abi=data.get("kmd_abi", ""),
                   built_utc=data.get("built_utc", ""), zip_path=zip_path, zip_sha256=digest, zip_bytes=size)


@dataclass
class PlannedStep:
    argv: list[str]
    timeout_s: int
    phase: str = "run"  # pre | run | post | gate
    # A step the manifest marked as not load-bearing for the arm's question (hiding the overlay, say). It is
    # recorded when it fails and it fails no arm. Every other pre or run step failing fails the arm.
    optional: bool = False

    def text(self) -> str:
        return " ".join(part if " " not in part else f'"{part}"' for part in self.argv)


@dataclass
class PlannedArm:
    arm: dict
    steps: list[PlannedStep]
    env: dict = field(default_factory=dict)

    @property
    def id(self) -> str:
        return self.arm["id"]

    @property
    def kind(self) -> str:
        return self.arm["kind"]

    @property
    def bound_s(self) -> int:
        return int(self.arm["bound_s"])

    @property
    def gate_after(self) -> bool:
        if "gate_after" in self.arm:
            return bool(self.arm["gate_after"])
        return self.kind in ("lab", "promote", "game")

    @property
    def depends_on(self) -> list[str]:
        return list(self.arm.get("depends_on", ()))

    @property
    def allow_rc(self) -> list[int]:
        """Exit codes of a run step that this arm's own question treats as an answer, not a failure.

        One harness on the lab says 'some of my cases did not run' with an exit code and says which ones in
        its output. The manifest names those codes, and the arm's `expect` lines then decide.
        """
        return [int(code) for code in self.arm.get("allow_rc", ())]


@dataclass
class Plan:
    package: Package
    arms: list[PlannedArm]
    limits: dict
    values: dict
    failure_classes: list[dict]
    gate_step: PlannedStep
    attempt_base: int | None = None

    def by_id(self, arm_id: str) -> PlannedArm:
        for arm in self.arms:
            if arm.id == arm_id:
                return arm
        raise ManifestError(f"no arm {arm_id} in the plan")


def load_manifest(path: Path | None = None) -> dict:
    data = json.loads((path or (HERE / "arms.json")).read_text(encoding="utf-8"))
    check_manifest(data)
    return data


def check_manifest(data: dict) -> None:
    """Invariants the manifest must hold. The host tests run this against the shipped file."""
    limits = data["limits"]
    seen = set()
    for arm in data["arms"]:
        arm_id = arm["id"]
        if arm_id in seen:
            raise ManifestError(f"two arms share the id {arm_id}")
        seen.add(arm_id)
        kind = arm["kind"]
        if kind not in TRIAL_KINDS + HOST_KINDS + GAME_KINDS:
            raise ManifestError(f"{arm_id}: unknown kind {kind}")
        bound = int(arm["bound_s"])
        if kind in TRIAL_KINDS and bound > int(limits["trial_bound_s"]):
            raise ManifestError(f"{arm_id}: {bound} s is over the trial bound of {limits['trial_bound_s']} s")
        if kind in GAME_KINDS and bound > int(limits["game_bound_s"]):
            raise ManifestError(f"{arm_id}: {bound} s is over the game bound of {limits['game_bound_s']} s")
        if kind in HOST_KINDS and "why_not_a_trial" not in arm and kind != "restart":
            raise ManifestError(f"{arm_id}: a {kind} arm must say why the trial bound does not apply")
        if kind != "promote" and not arm.get("run"):
            raise ManifestError(f"{arm_id}: no run command")
        if kind == "promote" and not arm.get("promote"):
            raise ManifestError(f"{arm_id}: a promote arm needs its triplet")
        for dependency in arm.get("depends_on", ()):
            if dependency not in seen:
                raise ManifestError(f"{arm_id}: depends on {dependency}, which does not come before it")
        guard = arm.get("needs_verified")
        if guard and guard not in seen:
            raise ManifestError(f"{arm_id}: it needs {guard} to verify the package, and {guard} does not "
                                f"come before it")
        result = arm.get("result") or {}
        if result.get("pick") not in (None, "first", "last"):
            raise ManifestError(f"{arm_id}: a result picks the first or the last match, not "
                                f"{result['pick']!r}")
        if not isinstance(arm.get("kill", []), list):
            raise ManifestError(f"{arm_id}: kill is the list of client process names to end")
        if arm.get("board_memory") not in (None, *BOARD_MEMORY_MARKS):
            raise ManifestError(f"{arm_id}: board_memory is one of {', '.join(BOARD_MEMORY_MARKS)}, not "
                                f"{arm['board_memory']!r}")
        baseline = arm.get("baseline")
        if baseline and not ("tolerance_pct" in baseline or "tolerance_abs" in baseline):
            raise ManifestError(f"{arm_id}: a baseline needs a tolerance")
        if baseline and "source" not in baseline:
            raise ManifestError(f"{arm_id}: a baseline must name the train it comes from")
        if baseline and not result:
            raise ManifestError(f"{arm_id}: a baseline with no result line can never be compared")
    for name, members in data["sets"].items():
        for arm_id in members:
            if arm_id not in seen:
                raise ManifestError(f"set {name} names {arm_id}, which is not an arm")
    if int(limits["tctl_poll_s"]) < int(limits["min_poll_s"]):
        raise ManifestError("the temperature poll interval is under the minimum: the lab sshd penalises that")


def expand(template: str, values: dict) -> str:
    def replace(match: re.Match) -> str:
        key = match.group(1)
        if key not in values:
            raise ManifestError(f"unknown placeholder {{{key}}} in {template!r}")
        return str(values[key])

    return PLACEHOLDER.sub(replace, template)


def plan_steps(arm: dict, values: dict, limits: dict, python: str) -> list[PlannedStep]:
    """The steps of one arm, in order, with the host backstop of each.

    A step is a command line, or an object `{"argv": [...], "optional": true}` for a step that fails nothing
    (the overlay calls of a game arm). The host timeout of a run step is the arm's bound plus the grace of
    the manifest; the lab script itself owns the bound.
    """
    grace = int(limits["host_grace_s"]) if arm["kind"] in TRIAL_KINDS + GAME_KINDS else 0
    if arm["kind"] in GAME_KINDS:
        # A game arm's bound is the session's own, which the session script holds. The host step around it
        # also stages the trial and pulls its evidence afterwards (the deferred ICD logs, the ETW files, the
        # closure), and in the b28 run those pulls were still going 10 minutes after the game had closed and
        # the session had restored the lab. That is not the game running over its bound, so the host backstop
        # of a game run step is the bound plus this margin.
        grace += int(limits.get("game_evidence_s", 900))
    steps: list[PlannedStep] = []
    for phase in ("pre", "run", "post"):
        entries = list(arm.get(phase, ()))
        if phase == "pre" and arm["kind"] in GAME_KINDS:
            entries.insert(0, game_memory_step(limits))
        for entry in entries:
            argv = entry["argv"] if isinstance(entry, dict) else entry
            optional = bool(entry.get("optional")) if isinstance(entry, dict) else False
            resolved = [expand(str(part), values) for part in argv]
            if resolved and resolved[0] == "python":
                resolved[0] = python
            timeout = int(arm["bound_s"]) + grace if phase == "run" else min(180, int(arm["bound_s"]) + grace)
            steps.append(PlannedStep(argv=resolved, timeout_s=timeout, phase=phase, optional=optional))
    return steps


def game_memory_step(limits: dict) -> list[str]:
    """The first pre step of every game arm: the session's memory admission, read before anything starts.

    The lab's game harness (`game-runtime.ps1`) admits a session only with `game_available_mb` available, and on
    this board the carve-out takes its share of the RAM before Windows starts. At 12288 MiB Windows had 1302 MB
    available, and the b29 RotTR arm (native-caps547) did not start the game but held the runner on its host
    backstop until it was stopped by hand. `lab/game-memory.ps1` reads the same counter and fails the arm at
    once, with the reason.
    """
    return ["python", "{target}", "ps", "{kit}/game-memory.ps1", "-NeedMB",
            str(int(limits.get("game_available_mb", 3500)))]


def game_window(order: list[str], index: dict) -> list[str]:
    """The running order, with every game arm inside the board memory window when the plan has one.

    A game runs at the smaller carve-out (`game_memory_step`). A plan that sets the board memory therefore runs
    its game arms after the last `set` arm and before the first `restore` arm after it, which is after the
    restart and the reads at the new size. Game arms already in the window keep their places; the others move to
    the end of it, or to the end of the plan when it has no restore. A plan with no `set` arm keeps its order,
    and the memory step still stops a game that the carve-out cannot admit.
    """
    marks = [index[arm_id].get("board_memory") for arm_id in order]
    if "set" not in marks:
        return order
    last_set = len(marks) - 1 - marks[::-1].index("set")
    restore = next((i for i in range(last_set + 1, len(order)) if marks[i] == "restore"), len(order))
    moving = [arm_id for i, arm_id in enumerate(order)
              if index[arm_id]["kind"] in GAME_KINDS and not last_set < i < restore]
    if not moving:
        return order
    rest = [arm_id for arm_id in order if arm_id not in moving]
    at = rest.index(order[restore]) if restore < len(order) else len(rest)
    moved = rest[:at] + moving + rest[at:]
    position = {arm_id: i for i, arm_id in enumerate(moved)}
    for arm_id in moved:
        for dependency in index[arm_id].get("depends_on", ()):
            if position.get(dependency, -1) > position[arm_id]:
                raise ManifestError(f"{arm_id} depends on {dependency}, and the game arms move after the board "
                                    f"memory set: {dependency} would run after {arm_id}")
    return moved


def arm_values(values: dict, arm: dict, number: int | None) -> tuple[dict, list[str]]:
    """(the placeholder values of one arm, the attempt names it takes). `number` is its first free one."""
    out = dict(values, bound=arm["bound_s"], attempt_n="", attempt="")
    if not arm.get("attempt"):
        return out, []
    if number is None:
        raise ManifestError(f"{arm['id']} needs an attempt number: pass --attempt-base")
    names = [f"{arm['attempt']}{number + offset:03d}" for offset in range(int(arm.get("attempt_count", 1)))]
    out["attempt_n"], out["attempt"] = f"{number:03d}", names[0]
    return out, names


def reassign_attempt(plan: "Plan", arm: PlannedArm, number: int, python: str = "python") -> str:
    """Hand one attempt arm another number, and build its steps again from the manifest templates.

    `promote.prepare` may step over an attempt directory that holds lab evidence, and the pair it then uses
    can be the pair this plan already handed a later game arm. An attempt number is inside the game arm's
    command line, so the steps and the environment are expanded again here and never patched.
    """
    values, names = arm_values(plan.values, arm.arm, number)
    arm.arm["attempt_name"] = names[0]
    if len(names) > 1:
        arm.arm["check_attempt_name"] = names[1]
    arm.steps = plan_steps(arm.arm, values, plan.limits, python)
    arm.env = {key: expand(value, values) for key, value in arm.arm.get("env", {}).items()}
    return names[0]


def build(package: Package, data: dict | None = None, arm_ids: list[str] | None = None,
          out_dir: Path | None = None, roots: tuple[Path, Path] | None = None,
          attempt_base: int | None = None, python: str = "python", for_run: bool = True) -> Plan:
    """Resolve the manifest against one package into a plan of arms to run, in order.

    `for_run=False` is for a plan built to write a summary of a run that already happened: the arms are not
    going to start, so the package's zip and its hash are not required.
    """
    data = data or load_manifest()
    repo, workspace = roots or find_roots()
    values = {
        "repo": repo.as_posix(), "ws": workspace.as_posix(), "python": python,
        "pkg": package.directory.as_posix(), "pkg_name": package.name, "release": package.release,
        "zip": package.zip_path.as_posix() if package.zip_path else "", "zip_sha256": package.zip_sha256,
        "kmd_build": package.kmd_build, "out": (out_dir or Path(".")).as_posix(),
        "bash": find_bash(),
    }
    for key, template in data["config"].items():
        values[key] = expand(template, values)
    limits = data["limits"]
    wanted = arm_ids or data["sets"]["standard"]
    index = {arm["id"]: arm for arm in data["arms"]}
    unknown = [arm_id for arm_id in wanted if arm_id not in index]
    if unknown:
        raise ManifestError("no such arm: " + ", ".join(unknown))
    order = game_window([arm["id"] for arm in data["arms"] if arm["id"] in set(wanted)], index)
    # An arm that names the package zip or its hash cannot run without them. The push arm would otherwise
    # hand target.py an empty path, and zipcheck would compare the lab's hash against nothing.
    needs_zip = [arm_id for arm_id in order if "{zip}" in json.dumps(index[arm_id])] if for_run else []
    needs_hash = [arm_id for arm_id in order if "{zip_sha256}" in json.dumps(index[arm_id])] if for_run else []
    if needs_zip and not package.zip_path:
        raise ManifestError(f"no {package.name}.zip beside {package.directory.parent.as_posix()}, and "
                            f"{', '.join(needs_zip)} needs it: build the package zip, or ask for arms that "
                            f"do not install it")
    if needs_hash and not package.zip_sha256:
        raise ManifestError(f"{', '.join(needs_hash)} compares the zip's SHA-256, so the zip must be "
                            f"hashed: drop --no-hash")
    planned: list[PlannedArm] = []
    attempt = attempt_base
    for arm_id in order:
        arm = dict(index[arm_id])
        per_arm, names = arm_values(values, arm, attempt)
        if names:
            arm["attempt_name"] = names[0]
            if len(names) > 1:
                arm["check_attempt_name"] = names[1]
            attempt += len(names)
        if arm.get("promote"):
            arm["promote"] = {key: expand(value, per_arm) for key, value in arm["promote"].items()}
        if arm.get("operator_line"):
            arm["operator_line"] = expand(arm["operator_line"], per_arm)
        planned.append(PlannedArm(arm=arm, steps=plan_steps(arm, per_arm, limits, python),
                                  env={k: expand(v, per_arm) for k, v in arm.get("env", {}).items()}))
    gate = PlannedStep(argv=[python, values["target"], "ps", values["kit"] + "/gate.ps1", "-Label", "{label}"],
                       timeout_s=180, phase="gate")
    return Plan(package=package, arms=planned, limits=limits, values=values,
                failure_classes=data.get("failure_classes", []), gate_step=gate, attempt_base=attempt_base)
