#!/usr/bin/env python3
"""One command for the standard validation set of a release train.

    python tools/win/train-validate/validate.py run --package <package dir> [--arms all] [--out <dir>]
    python tools/win/train-validate/validate.py run --package <package dir> --dry-run
    python tools/win/train-validate/validate.py summary --out <dir> [--set rottr=55.15]
    python tools/win/train-validate/validate.py arms

`run` works through the arms of `arms.json` in order, with the owner's bounds, the STOP flag, the
temperature gate and a health gate after every arm that touched the GPU, and writes a RESULTS.md skeleton
at the end. Nothing between the arms needs an operator, so the operator starts the run, drives the one
interactive game session the owner reserved for a person, and reads one summary.

`--dry-run` prints the whole plan, every command with its bound, and touches nothing.
"""
from __future__ import annotations

import argparse
import json
import sys
from dataclasses import asdict
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import manifest  # noqa: E402
import promote  # noqa: E402
import summary as summary_module  # noqa: E402
from runner import ArmRecord, Clock, GOOD, Runner, Shell  # noqa: E402

HERE = Path(__file__).resolve().parent
# The host tests replace these two with a fake target and a fake clock, so the whole command runs with no
# lab and no waiting.
make_shell = Shell
make_clock = Clock


def utc_tag() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")


def default_out(plan: manifest.Plan, train: str) -> Path:
    return Path(plan.values["ws"]) / "scratch" / "train" / (train or "unnamed") / "validation" / \
        f"suite-{utc_tag()}"


def plan_text(plan: manifest.Plan, out_dir: Path, train: str) -> str:
    package = plan.package
    limits = plan.limits
    lines = [f"train validation plan ({train or 'train not named'})", ""]
    lines += [f"package : {package.name}",
              f"          release {package.release}, KMD build {package.kmd_build}, ABI {package.kmd_abi}, "
              f"built {package.built_utc}",
              f"          {package.directory.as_posix()}",
              f"zip     : {package.zip_path.as_posix() if package.zip_path else 'ABSENT'}",
              f"          sha256 {package.zip_sha256 or '-'}, {package.zip_bytes} bytes",
              f"out     : {out_dir.as_posix()}",
              f"limits  : trial {limits['trial_bound_s']} s, game {limits['game_bound_s']} s, an arm starts "
              f"below {limits['tctl_start_max_c']} C, cool-down poll {limits['tctl_poll_s']} s (never under "
              f"{limits['min_poll_s']} s), host backstop bound + {limits['host_grace_s']} s", ""]
    worst = 0
    for number, arm in enumerate(plan.arms, start=1):
        # The arm's own worst case is the sum of its host timeouts, not its bound: a promote arm issues nine
        # steps of its own, and a plan line that said 'bound 170 s' understated it by a quarter of an hour.
        host_worst = sum(step.timeout_s for step in arm.steps) or arm.bound_s
        worst += host_worst
        head = f"{number:2d} {arm.id} [{arm.kind}, bound {arm.bound_s} s, host worst case {host_worst} s]"
        if arm.arm.get("gate"):
            head += f" OWNER GATE {arm.arm['gate']}"
        lines.append(head)
        lines.append(f"     {arm.arm.get('title', '')}")
        if arm.arm.get("why_not_a_trial"):
            lines.append(f"     not a trial: {arm.arm['why_not_a_trial']}")
        if arm.arm.get("prepared"):
            prepared = arm.arm["prepared"]
            lines.append(f"     attempt {prepared['attempt']}, acceptance {prepared['check_attempt']}; "
                         f"{prepared['note']}")
            if prepared["moved_aside"]:
                lines.append("     leftover attempt moved aside: " + ", ".join(prepared["moved_aside"]))
        if arm.env:
            lines.append("     env " + " ".join(f"{k}={v}" for k, v in arm.env.items()))
        for step in arm.steps:
            lines.append(f"     $ {step.text()}      [{step.phase}, host timeout {step.timeout_s} s"
                         + (", optional" if step.optional else "") + "]")
        if arm.arm.get("needs_verified"):
            lines.append(f"     it removes the installed driver: it does not run unless "
                         f"{arm.arm['needs_verified']} verified the package on the lab in this run")
        if arm.arm.get("kill"):
            lines.append("     after a failed or bound run, its client is ended on the lab: "
                         + ", ".join(arm.arm["kill"]))
        if arm.kind == "restart":
            lines.append(f"     then the runner waits for a boot time that differs from the one before the "
                         f"restart: a first wait of {limits.get('restart_settle_s', 30)} s, then a probe "
                         f"every {limits.get('restart_poll_s', 30)} s, up to the bound")
        if arm.kind == "operator":
            lines.append("     the suite never starts this arm: " + arm.arm.get("note", ""))
        wanted = ([arm.arm["expect"]] if arm.arm.get("expect") else []) + list(arm.arm.get("expect_all", ()))
        if wanted:
            lines.append("     expect " + ", ".join(repr(line) for line in wanted))
        result = arm.arm.get("result")
        if result:
            lines.append("     value  " + (f"operator reads it: {result.get('how', '')}"
                                           if result.get("operator")
                                           else f"/{result['regex']}/ group {result.get('group', 1)}, "
                                                f"{result.get('pick', 'last')} match"
                                                + (f" after {result['section']!r}" if result.get("section")
                                                   else "")
                                                + f", as {result.get('name', '')} {result.get('unit', '')}"))
        baseline = arm.arm.get("baseline")
        if baseline:
            tolerance = (f"+-{baseline['tolerance_abs']}" if "tolerance_abs" in baseline
                         else f"-{baseline['tolerance_pct']:g}%")
            lines.append(f"     base   {baseline['value']} ({tolerance}) from {baseline['source']}")
        lines.append(f"     gate after: {'yes' if arm.gate_after else 'no'}"
                     + (f"; depends on {', '.join(arm.depends_on)}" if arm.depends_on else ""))
        lines.append("")
    operator = [arm.id for arm in plan.arms if arm.kind == "operator"]
    lines += [f"worst case: {worst} s of host timeouts ({worst / 60:.0f} min) plus a cool-down of up to "
              f"{limits['tctl_cool_max_s']} s before each trial and game arm",
              "the operator's own session fits after the arms above: " + (", ".join(operator) or "none"),
              "nothing above has run: this is the plan only"]
    return "\n".join(lines)


def reserve_game_attempts(plan: manifest.Plan, python: str, writer=print) -> list[str]:
    """Keep the attempt numbers of the game arms clear of the promotion's own pair.

    The manifest hands out the numbers in order, and the promotion usually keeps the pair it was handed. When
    it has to step over an attempt directory that holds lab evidence, it moves up, and the pair it then takes
    can be the number a game arm already has in its command line. Two arms writing into one attempt directory
    is how evidence is lost, so every attempt arm after the promotion is given a number above it.
    """
    taken = 0
    moved = []
    for arm in plan.arms:
        prepared = arm.arm.get("prepared")
        if prepared:
            for name in (prepared["attempt"], prepared["check_attempt"]):
                taken = max(taken, int(name[-3:]))
            continue
        if not taken or not arm.arm.get("attempt_name"):
            continue
        if int(arm.arm["attempt_name"][-3:]) > taken:
            taken = int(arm.arm["attempt_name"][-3:]) + int(arm.arm.get("attempt_count", 1)) - 1
            continue
        was = arm.arm["attempt_name"]
        now = manifest.reassign_attempt(plan, arm, taken + 1, python)
        taken += int(arm.arm.get("attempt_count", 1))
        moved.append(f"{arm.id}: {was} -> {now}")
        writer(f"[attempt] {arm.id} moves from {was} to {now}: the promotion took that number")
    return moved


def ensure_game_plan(plan: manifest.Plan, arm: manifest.PlannedArm, writer=print) -> Path | None:
    """A game session of the native harness needs its own plan file. Write the facts if it is absent.

    It is written for the operator's own session as well (`w3-high-rt`): `run-m157.sh` refuses to start
    without `plans/plan-NNN.md`, and the operator should not have to find that out by hand. The conjecture
    and the reading of the result stay with the person.
    """
    number = arm.arm.get("attempt_name", "")[len("native-caps"):]
    if not number:
        return None
    path = Path(plan.values["caps"]) / "plans" / f"plan-{number}.md"
    if path.is_file():
        return path
    baseline = arm.arm.get("baseline") or {}
    package = plan.package
    text = [f"# Trial {number}: {arm.arm.get('title', arm.id)}",
            "",
            f"Package {package.release} (KMD build {package.kmd_build}, ABI {package.kmd_abi}), installed by "
            f"its own installer in this validation run. Nothing is swapped for this trial: the files under "
            f"test are the installed ones.",
            "",
            f"- Question: does this arm complete on {package.release} within its bound of {arm.bound_s} s?",
            f"- Reference: {baseline.get('value', 'none')} {arm.arm.get('result', {}).get('unit', '')} "
            f"from {baseline.get('source', 'no earlier run')}.",
            f"- Method: the train validation suite runs "
            f"`{next((step.text() for step in arm.steps if step.phase == 'run'), arm.id)}`. "
            f"The operator drives the game itself from half-scale shots, with no upscaler and no dynamic "
            f"resolution, and the session ends on its own bound or on the thermal stop.",
            "- Conjecture and the reading of the result screen belong to the session record, next to this "
            "file.",
            ""]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(text), encoding="utf-8", newline="\n")
    writer(f"[plan] wrote {path.as_posix()} for {arm.id}: fill in the conjecture before the session if it is "
           f"new work")
    return path


def load_records(out_dir: Path) -> list[ArmRecord]:
    """The records of the earlier runs in this directory.

    `records.json` is written when a run ends, so a run that was stopped in the middle leaves none. Each arm
    writes its own `<arm>/record.json` as it ends, and those are read here for every arm that `records.json`
    does not name, so that `--resume` after an interrupted run does not install the package a second time.
    """
    records, seen = [], set()
    path = out_dir / "records.json"
    if path.is_file():
        for row in json.loads(path.read_text(encoding="utf-8")):
            records.append(ArmRecord(**row))
            seen.add(records[-1].id)
    loose = []
    for child in sorted(out_dir.iterdir()) if out_dir.is_dir() else ():
        record = child / "record.json"
        if not child.is_dir() or not record.is_file():
            continue
        try:
            row = json.loads(record.read_text(encoding="utf-8"))
            arm = ArmRecord(**row)
        except (ValueError, TypeError):
            continue
        if arm.id not in seen:
            loose.append(arm)
            seen.add(arm.id)
    loose.sort(key=lambda arm: arm.started_utc or "")
    return records + loose


def attempt_base(args: argparse.Namespace, data: dict) -> int | None:
    """The first native-caps number the game and promotion arms may use, from the harness when not given."""
    if args.attempt_base:
        return args.attempt_base
    _, workspace = manifest.find_roots()
    return manifest.next_attempt_base(Path(manifest.expand(data["config"]["caps"],
                                                           {"ws": workspace.as_posix()})))


def command_run(args: argparse.Namespace) -> int:
    data = manifest.load_manifest(args.manifest)
    package = manifest.Package.read(Path(args.package), hash_zip=not args.no_hash)
    arm_ids = None
    if args.arms and args.arms != "all":
        arm_ids = data["sets"].get(args.arms) or [part.strip() for part in args.arms.split(",") if part.strip()]
    plan = manifest.build(package, data, arm_ids=arm_ids, out_dir=Path(args.out) if args.out else None,
                          attempt_base=attempt_base(args, data), python=sys.executable)
    out_dir = Path(args.out) if args.out else default_out(plan, args.train)
    for arm in plan.arms:
        if arm.kind == "promote":
            promote.attach(plan, arm, sys.executable, clean=not args.dry_run)
    reserve_game_attempts(plan, sys.executable, writer=(lambda *a: None) if args.dry_run else print)
    if args.dry_run:
        print(plan_text(plan, out_dir, args.train))
        return 0
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "plan.txt").write_text(plan_text(plan, out_dir, args.train), encoding="utf-8", newline="\n")
    for arm in plan.arms:
        if arm.kind in manifest.GAME_KINDS:
            ensure_game_plan(plan, arm)
    done = {record.id for record in load_records(out_dir) if record.verdict in GOOD} if args.resume else set()
    todo = [arm for arm in plan.arms if arm.id not in done]
    if done:
        print("[resume] already good: " + ", ".join(sorted(done)))
    shell = make_shell(Path(plan.values["ws"]), base_env={"BC250_ROOT": plan.values["ws"]})
    runner = Runner(plan, shell, out_dir, clock=make_clock(), python=sys.executable, already_good=done)
    records = runner.run(todo)
    records = load_records(out_dir) + records if args.resume else records
    (out_dir / "records.json").write_text(json.dumps([asdict(r) for r in records], indent=2),
                                          encoding="utf-8", newline="\n")
    text = summary_module.write(plan, records, out_dir, train=args.train)
    (out_dir / "RESULTS.md").write_text(text, encoding="utf-8", newline="\n")
    print("")
    print(text)
    print(f"[out ] {out_dir.as_posix()}")
    for gate in summary_module.gates(records, plan):
        if not gate.met:
            print(f"[gate] {gate.title}: NOT MET. {gate.detail}")
    for arm in plan.arms:
        if arm.kind == "operator":
            print(f"[next] the operator's own session: {arm.steps[-1].text() if arm.steps else arm.id}")
    if runner.halted:
        print("[halt] " + runner.halted)
        return 2
    return 0 if all(record.verdict in GOOD or record.kind == "operator" for record in records) else 1


def command_summary(args: argparse.Namespace) -> int:
    out_dir = Path(args.out)
    data = manifest.load_manifest(args.manifest)
    package = manifest.Package.read(Path(args.package), hash_zip=not args.no_hash) if args.package else None
    records = load_records(out_dir)
    if not records:
        print(f"no records.json in {out_dir}", file=sys.stderr)
        return 2
    if package is None:
        print("--package is needed to name the package in the summary", file=sys.stderr)
        return 2
    plan = manifest.build(package, data, out_dir=out_dir, attempt_base=attempt_base(args, data),
                          python=sys.executable, for_run=False)
    values = {}
    for pair in args.set or ():
        key, _, value = pair.partition("=")
        try:
            values[key] = float(value)
        except ValueError:
            values[key] = value
    text = summary_module.write(plan, records, out_dir, operator_values=values, train=args.train)
    (out_dir / "RESULTS.md").write_text(text, encoding="utf-8", newline="\n")
    print(text)
    return 0


def command_arms(args: argparse.Namespace) -> int:
    data = manifest.load_manifest(args.manifest)
    for name, members in data["sets"].items():
        print(f"set {name}: {', '.join(members)}")
    print("")
    for arm in data["arms"]:
        print(f"{arm['id']:16} {arm['kind']:9} {arm['bound_s']:5} s  {arm.get('title', '')}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--manifest", type=Path, default=None, help="another arms.json")
    sub = parser.add_subparsers(dest="command", required=True)
    run = sub.add_parser("run", help="run the arms, or print the plan with --dry-run")
    run.add_argument("--package", required=True, help="the release package directory (it holds manifest.json)")
    run.add_argument("--arms", default="all", help="all, a set name (standard, install, gates, smoke) or ids")
    run.add_argument("--out", default=None, help="where the raw logs and RESULTS.md go")
    run.add_argument("--train", default="", help="the train name, for the title and the default out directory")
    run.add_argument("--attempt-base", type=int, default=None,
                     help="the first native-caps attempt number the game and promotion arms may use")
    run.add_argument("--dry-run", action="store_true", help="print the plan and touch nothing")
    run.add_argument("--resume", action="store_true", help="keep the arms of --out that already passed")
    run.add_argument("--no-hash", action="store_true", help="do not hash the package zip")
    run.set_defaults(func=command_run)
    report = sub.add_parser("summary", help="write RESULTS.md again, with the operator's readings")
    report.add_argument("--out", required=True)
    report.add_argument("--package", default=None)
    report.add_argument("--train", default="")
    report.add_argument("--attempt-base", type=int, default=None)
    report.add_argument("--no-hash", action="store_true")
    report.add_argument("--set", action="append", help="arm=value, for a value only a person can read")
    report.set_defaults(func=command_summary)
    listing = sub.add_parser("arms", help="the arms and the sets of the manifest")
    listing.set_defaults(func=command_arms)
    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except manifest.ManifestError as wrong:
        print("REFUSED: " + str(wrong), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
