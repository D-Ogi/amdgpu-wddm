"""The arm loop of a train validation: the bounds, the gates and the raw records.

Every command goes through `Shell`, and every wait through `Clock`, so the whole sequence runs in the host
tests against a fake target with no lab, no sleep and no subprocess. The owner's rules live here and not in
the operator's head:

* a trial arm is bounded (the lab script ends its own run; this side keeps a backstop of bound + grace),
* the overlay STOP flag is read between arms, and a set flag ends the run,
* the temperature is read between arms, and the next arm waits until Tctl is under the cool-down line,
* one ssh call at a time: no session is held open while a trial runs, and the only sampler is the smart
  plug over the LAN (BD-051: a held session before the trial ran coincided with an sshd accept stall),
* the temperature poll never runs faster than the manifest's minimum (the lab sshd penalises fast probes),
* a health gate runs after an arm that touched the GPU, and its raw text is kept next to the arm's own,
* a failed arm keeps its logs, runs its restore steps and does not stop the arms that do not depend on it.

The runner never diagnoses. It records what happened and names the symptom shape in the summary.
"""
from __future__ import annotations

import json
import re
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field, asdict
from datetime import datetime, timezone
from pathlib import Path

from manifest import GAME_KINDS, Plan, PlannedArm, PlannedStep, TRIAL_KINDS

PASS, WARN, FAIL, BOUND, THERMAL, SKIPPED, OPERATOR = "PASS", "WARN", "FAIL", "BOUND", "THERMAL", "SKIPPED", "OPERATOR"
# A verdict that lets the arms which depend on this one run.
GOOD = (PASS, WARN)


def utc_iso() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


@dataclass
class Completed:
    rc: int
    out: str = ""
    err: str = ""
    seconds: float = 0.0
    timed_out: bool = False

    @property
    def text(self) -> str:
        return self.out + ("\n--- stderr\n" + self.err if self.err.strip() else "")


class Shell:
    """Runs one command at a time on the development PC, and keeps its raw output.

    `base_env` carries what every step needs. `BC250_ROOT` is the one that matters: `target.py` reads the
    lab configuration from `<BC250_ROOT>/secrets/client/target.json`, and a worktree copy of the tool would
    otherwise look for it beside the worktree.
    """

    def __init__(self, cwd: Path, base_env: dict | None = None, writer=print):
        self.cwd, self.writer = Path(cwd), writer
        self.base_env = dict(base_env or {})

    def run(self, step: PlannedStep, env: dict | None = None) -> Completed:
        started = time.monotonic()
        merged = None
        if self.base_env or env:
            import os
            merged = dict(os.environ, **self.base_env, **(env or {}))
        try:
            done = subprocess.run(step.argv, capture_output=True, text=True, timeout=step.timeout_s,
                                  cwd=str(self.cwd), env=merged)
            return Completed(done.returncode, done.stdout or "", done.stderr or "", time.monotonic() - started)
        except subprocess.TimeoutExpired as expired:
            out = expired.stdout or ""
            err = expired.stderr or ""
            return Completed(124, out if isinstance(out, str) else out.decode("utf-8", "replace"),
                             (err if isinstance(err, str) else err.decode("utf-8", "replace"))
                             + f"\n[host backstop] no end within {step.timeout_s} s",
                             time.monotonic() - started, timed_out=True)


class Clock:
    def now(self) -> float:
        return time.monotonic()

    def sleep(self, seconds: float) -> None:
        time.sleep(seconds)


class PlugSampler(threading.Thread):
    """Wall power every 15 s over the LAN, never over ssh. The PSU is 300 W and this is the witness."""

    def __init__(self, shell: Shell, plug: str, path: Path, python: str, clock: Clock):
        super().__init__(daemon=True)
        self.shell, self.plug, self.path, self.python, self.clock = shell, plug, path, python, clock
        self.halt = threading.Event()

    def run(self) -> None:  # pragma: no cover - the tests drive the sampler through its own step
        with open(self.path, "a", encoding="utf-8") as handle:
            while not self.halt.is_set():
                step = PlannedStep([self.python, self.plug, "telemetry"], 30, "sample")
                done = self.shell.run(step)
                handle.write(json.dumps({"local_utc": utc_iso(), "rc": done.rc,
                                         "out": done.out.strip()[-400:]}) + "\n")
                handle.flush()
                self.halt.wait(15)


GATE_RULES = {
    # key in the gate output -> (what must hold, how bad it is when it does not)
    "flags": ("health flags=15", "critical"),
    "faults": ("0 GPU faults", "fail"),
    "fence_timeouts": ("0 hardware fence timeouts", "fail"),
    "id4101": ("0 Display 4101 events (TDR)", "fail"),
    "bugchecks": ("0 bugcheck records since the boot", "critical"),
    "TdrDelay": ("TdrDelay 10", "warn"),
    "fan": ("fan state=curve controlling=1", "warn"),
    "dpm": ("a readable DPM line", "warn"),
}


@dataclass
class GateResult:
    ok: bool = True
    critical: bool = False
    violations: list[str] = field(default_factory=list)
    facts: dict = field(default_factory=dict)
    raw: str = ""


def parse_gate(text: str) -> GateResult:
    """Read gate.ps1's output. A section it could not read is a violation, never a silent pass."""
    result = GateResult(raw=text)
    facts = result.facts
    for pattern, key, cast in (
        (r"flags=(\d+)", "flags", int),
        (r"faults=(\d+)", "faults", int),
        (r"fence_timeouts=(\d+)", "fence_timeouts", int),
        (r"reset_engine=(\d+)", "reset_engine", int),
        (r"id4101=(\d+)", "id4101", int),
        (r"^bugchecks:\s*(\d+)", "bugchecks", int),
        (r"^appcrashes:\s*(\d+)", "appcrashes", int),
        (r"TdrDelay=(\d+)", "TdrDelay", int),
        (r"^tctl:\s*([-\d.]+)", "tctl", float),
    ):
        found = re.search(pattern, text, re.M)
        if found:
            facts[key] = cast(found.group(1))
    fan = re.search(r"^fan:\s*(.+)$", text, re.M)
    facts["fan"] = fan.group(1).strip() if fan else ""
    dpm = re.search(r"^dpm:\s*(.+)$", text, re.M)
    facts["dpm"] = dpm.group(1).strip() if dpm else ""

    def violate(message: str, severity: str) -> None:
        result.violations.append(f"{severity}: {message}")
        result.ok = False
        if severity == "critical":
            result.critical = True

    if "gate end" not in text:
        violate("the gate did not finish: its output ends early", "critical")
    if facts.get("flags") != 15:
        violate(f"health flags={facts.get('flags', 'unreadable')}, wanted 15", "critical")
    for key in ("faults", "fence_timeouts", "id4101", "bugchecks"):
        value = facts.get(key)
        if value is None:
            violate(f"{key} is unreadable", "fail")
        elif value > 0:
            violate(f"{key}={value}, wanted 0", "critical" if key == "bugchecks" else "fail")
    if facts.get("TdrDelay") != 10:
        violate(f"TdrDelay={facts.get('TdrDelay', 'unreadable')}, wanted 10", "warn")
    if not re.search(r"state=curve\b.*controlling=1", facts.get("fan", "")):
        violate(f"fan line is {facts.get('fan') or 'unreadable'!r}, wanted state=curve controlling=1", "warn")
    if "MHz" not in facts.get("dpm", ""):
        violate("the DPM line is unreadable", "warn")
    return result


THERMAL_MARKS = (
    re.compile(r"end:\s*reason\s*thermal", re.I),
    re.compile(r"thermal stop", re.I),
    re.compile(r"tctl[^\n]*>=\s*8[79]", re.I),
)
STALL_MARKS = (
    re.compile(r"kex_exchange_identification", re.I),
    re.compile(r"banner exchange", re.I),
    re.compile(r"Connection timed out", re.I),
)


@dataclass
class ArmRecord:
    id: str
    kind: str
    title: str = ""
    verdict: str = SKIPPED
    reason: str = ""
    bound_s: int = 0
    seconds: float = 0.0
    value: float | str | None = None
    value_name: str = ""
    unit: str = ""
    baseline: dict | None = None
    gate: dict | None = None
    raw_paths: list[str] = field(default_factory=list)
    steps: list[dict] = field(default_factory=list)
    started_utc: str = ""
    ended_utc: str = ""
    gate_name: str = ""
    attempt: str = ""


class Runner:
    def __init__(self, plan: Plan, shell: Shell, out_dir: Path, clock: Clock | None = None,
                 writer=print, python: str = sys.executable):
        self.plan, self.shell, self.out, self.clock = plan, shell, Path(out_dir), clock or Clock()
        self.writer, self.python = writer, python
        self.records: list[ArmRecord] = []
        self.halted = ""
        self._stalls = 0

    # -- gates -------------------------------------------------------------------------------------
    def stop_flag_set(self) -> tuple[bool, str]:
        step = PlannedStep([self.python, self.plan.values["mon"], "stop?"], 90, "gate")
        done = self.shell.run(step)
        return (done.rc != 0, done.text.strip()[-200:])

    def read_tctl(self) -> tuple[float | None, str]:
        """Tctl now, with the retry rule of run-slot.py: a banner timeout waits a minute, not ten seconds."""
        limits = self.plan.limits
        text = ""
        for attempt in range(3):
            done = self.shell.run(PlannedStep([self.python, self.plan.values["temp"]], 90, "gate"))
            text = done.text.strip()
            found = re.search(r"Tctl\s+([0-9.]+)", text)
            if found:
                return float(found.group(1)), text
            if attempt < 2:
                self.clock.sleep(60 if any(mark.search(text) for mark in STALL_MARKS) else
                                 max(int(limits["min_poll_s"]), 10))
        return None, text

    def cool_down(self) -> tuple[bool, str]:
        """Wait until Tctl is under the start line. The owner's rule is to cool below 87 C between trials;
        a trial does not start above the manifest's start line either (80 C, as every native trial slot)."""
        limits = self.plan.limits
        start_max = float(limits["tctl_start_max_c"])
        poll = max(int(limits["tctl_poll_s"]), int(limits["min_poll_s"]))
        waited = 0
        while True:
            tctl, text = self.read_tctl()
            if tctl is None:
                return False, "the temperature is unreadable: " + text[-160:]
            if tctl < start_max:
                return True, f"Tctl {tctl} C"
            if waited >= int(limits["tctl_cool_max_s"]):
                return False, f"Tctl {tctl} C after {waited} s of cool-down"
            self.writer(f"  cool-down: Tctl {tctl} C, waiting {poll} s")
            self.clock.sleep(poll)
            waited += poll

    def _gate_text(self, label: str) -> str:
        argv = [part.replace("{label}", label) for part in self.plan.gate_step.argv]
        return self.shell.run(PlannedStep(argv, self.plan.gate_step.timeout_s, "gate")).text

    def wait_for_boot(self, arm: PlannedArm, before: str, need_health: bool = False) -> tuple[bool, str, str]:
        """After a planned restart, wait for a boot time that differs from the one before it.

        A restart is not over when port 22 answers: the machine is still up for a few seconds after
        `shutdown /r` and answers the first probe. The gate prints the boot time, so the wait is over the
        boot time itself and not over a guess (BD-059 is the other side of this: a planned restart must not
        be read as an unclean one). With `need_health` the wait also holds until the driver reports
        `flags=15`, because a gate read 35 s into a boot would otherwise fail an arm for being early. The
        poll is slow on purpose, because the lab sshd penalises fast probes.
        """
        limits = self.plan.limits
        was = re.search(r"^boot (\S+)", before, re.M)
        settle = int(limits.get("restart_settle_s", 30))
        poll = max(int(limits.get("restart_poll_s", 30)), int(limits["min_poll_s"]))
        self.writer(f"  restart: waiting {settle} s before the first probe")
        self.clock.sleep(settle)
        waited, text, new_boot = settle, "", False
        while waited < arm.bound_s:
            self.shell.run(PlannedStep([self.python, self.plan.values["target"], "forget"], 60, "gate"))
            text = self._gate_text("after-restart")
            now = re.search(r"^boot (\S+)", text, re.M)
            new_boot = bool("gate end" in text and now and (was is None or now.group(1) != was.group(1)))
            if new_boot and (not need_health or "flags=15" in text):
                return True, text, f"the machine is back after {waited} s"
            self.clock.sleep(poll)
            waited += poll
        if new_boot:
            return False, text, (f"the machine came back but the driver did not report flags=15 within "
                                 f"{arm.bound_s} s")
        return False, text, f"no new boot time within {arm.bound_s} s of the restart"

    def health_gate(self, arm_id: str, directory: Path) -> GateResult:
        argv = [part.replace("{label}", arm_id) for part in self.plan.gate_step.argv]
        done = self.shell.run(PlannedStep(argv, self.plan.gate_step.timeout_s, "gate"))
        gate = parse_gate(done.text)
        path = directory / "gate.txt"
        self._write(path, done.text)
        gate.facts["raw"] = path.as_posix()
        return gate

    # -- arms --------------------------------------------------------------------------------------
    def _write(self, path: Path, text: str) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8", newline="\n")

    def _parse_value(self, arm: PlannedArm, text: str):
        spec = arm.arm.get("result")
        if not spec or spec.get("operator"):
            return None
        found = re.search(spec["regex"], text, re.M)
        if not found:
            return None
        raw = found.group(int(spec.get("group", 1)))
        if spec.get("kind") == "text":
            return raw.strip()
        try:
            return float(raw)
        except ValueError:
            return raw.strip()

    def _judge_value(self, arm: PlannedArm, value) -> tuple[str, str]:
        baseline = arm.arm.get("baseline")
        if baseline is None or value is None or isinstance(value, str):
            return PASS, ""
        want = float(baseline["value"])
        if "tolerance_abs" in baseline:
            allowed = float(baseline["tolerance_abs"])
            if abs(value - want) > allowed:
                return WARN, f"{value} against {want} (allowed {allowed:+})"
            return PASS, ""
        pct = float(baseline["tolerance_pct"])
        floor = want * (1 - pct / 100.0)
        if value < floor:
            return WARN, f"{value} is under {floor:.2f} ({pct:g}% below the {want} of {baseline['source']})"
        return PASS, ""

    def run_arm(self, arm: PlannedArm) -> ArmRecord:
        record = ArmRecord(id=arm.id, kind=arm.kind, title=arm.arm.get("title", ""), bound_s=arm.bound_s,
                           baseline=arm.arm.get("baseline"), gate_name=arm.arm.get("gate", ""),
                           attempt=arm.arm.get("attempt_name", ""), started_utc=utc_iso())
        directory = self.out / arm.id
        directory.mkdir(parents=True, exist_ok=True)
        if arm.kind == "operator":
            record.verdict = OPERATOR
            record.reason = arm.arm.get("note", "the operator runs this arm")
            record.ended_utc = utc_iso()
            return record

        texts: list[str] = []
        sampler = None
        if arm.arm.get("plug"):
            sampler = PlugSampler(self.shell, self.plan.values["plug"], directory / "plug.jsonl",
                                  self.python, self.clock)
            sampler.start()
        started = self.clock.now()
        failed = False
        try:
            for number, step in enumerate(arm.steps, start=1):
                if failed and step.phase == "run":
                    # The arm is already lost. The post steps are the restore and still run; another run
                    # command would only add noise on a machine in an unknown state.
                    record.steps.append({"phase": step.phase, "argv": step.argv, "rc": None,
                                         "seconds": 0, "skipped": "an earlier run step failed"})
                    continue
                done = self.shell.run(step, env=self._env(arm) if step.phase == "run" else None)
                path = directory / f"step-{number:02d}-{step.phase}.txt"
                self._write(path, f"$ {step.text()}\n(rc {done.rc}, {done.seconds:.1f} s)\n\n{done.text}")
                record.raw_paths.append(path.as_posix())
                record.steps.append({"phase": step.phase, "argv": step.argv, "rc": done.rc,
                                     "seconds": round(done.seconds, 1), "timed_out": done.timed_out,
                                     "raw": path.as_posix()})
                texts.append(done.text)
                if any(mark.search(done.text) for mark in STALL_MARKS):
                    self._stalls += 1
                if step.phase == "run" and (done.timed_out or done.rc != 0):
                    record.verdict = BOUND if done.timed_out else FAIL
                    record.reason = (f"no end within {step.timeout_s} s (bound {arm.bound_s} s)"
                                     if done.timed_out else f"rc {done.rc}")
                    failed = True
        finally:
            if sampler:
                sampler.halt.set()
                sampler.join(timeout=40)
        gate_done = False
        if arm.kind == "restart" and not failed:
            came_back, boot_text, why = self.wait_for_boot(arm, "\n".join(texts), need_health=arm.gate_after)
            self._write(directory / "after-restart.txt", boot_text)
            record.raw_paths.append((directory / "after-restart.txt").as_posix())
            texts.append(boot_text)
            if not came_back:
                record.verdict = FAIL
                record.reason = why
            elif arm.gate_after:
                # The gate that proved the new boot is the health gate of this arm: it does not run twice.
                gate = parse_gate(boot_text)
                record.gate = {"ok": gate.ok, "critical": gate.critical, "violations": gate.violations,
                               "facts": gate.facts}
                gate_done = True
                if not gate.ok:
                    failing = [v for v in gate.violations if not v.startswith("warn")]
                    record.verdict = FAIL if failing else WARN
                    record.reason = "the health gate after the restart: " + "; ".join(gate.violations)
                if gate.critical:
                    self.halted = f"the health gate after {arm.id} is critical: " + "; ".join(gate.violations)
        record.seconds = round(self.clock.now() - started, 1)
        text = "\n".join(texts)
        self._write(directory / "arm.txt", text)
        record.raw_paths.insert(0, (directory / "arm.txt").as_posix())

        if any(mark.search(text) for mark in THERMAL_MARKS) and record.verdict in (SKIPPED, FAIL):
            record.verdict = THERMAL
            record.reason = "the arm ended on the thermal stop (Tctl 87 C held 10 s, or 89 C at once)"
        wanted = [arm.arm["expect"]] if arm.arm.get("expect") else []
        wanted += list(arm.arm.get("expect_all", ()))
        if record.verdict == SKIPPED:
            absent = [line for line in wanted if line not in text]
            if absent:
                record.verdict = FAIL
                record.reason = "the output does not carry " + ", ".join(repr(line) for line in absent)
            else:
                record.verdict = PASS
        spec = arm.arm.get("result") or {}
        record.value = self._parse_value(arm, text)
        record.value_name, record.unit = spec.get("name", ""), spec.get("unit", "")
        if record.verdict == PASS:
            verdict, why = self._judge_value(arm, record.value)
            if verdict != PASS:
                record.verdict, record.reason = verdict, why
        if spec.get("operator"):
            record.reason = (record.reason + "; " if record.reason else "") + \
                "the value is the operator's reading: " + spec.get("how", "")
        if arm.gate_after and not gate_done:
            gate = self.health_gate(arm.id, directory)
            record.gate = {"ok": gate.ok, "critical": gate.critical, "violations": gate.violations,
                           "facts": gate.facts}
            record.raw_paths.append(gate.facts.get("raw", ""))
            if not gate.ok:
                failing = [v for v in gate.violations if not v.startswith("warn")]
                if failing and record.verdict in GOOD:
                    record.verdict = FAIL
                    record.reason = (record.reason + "; " if record.reason else "") + \
                        "the health gate after it: " + "; ".join(failing)
                elif not failing and record.verdict == PASS:
                    record.verdict = WARN
                    record.reason = "the health gate: " + "; ".join(gate.violations)
            if gate.critical:
                self.halted = f"the health gate after {arm.id} is critical: " + "; ".join(gate.violations)
        record.ended_utc = utc_iso()
        self._write(directory / "record.json", json.dumps(asdict(record), indent=2))
        return record

    def _env(self, arm: PlannedArm) -> dict | None:
        return dict(arm.env) if arm.env else None

    def run(self, arms: list[PlannedArm] | None = None) -> list[ArmRecord]:
        arms = arms or self.plan.arms
        # Only a dependency inside this run can hold an arm back. A subset (--arms gates) or a resumed run
        # names the arms it wants, and an arm that ran in an earlier session is not run again to satisfy a
        # dependency on it.
        selected = {arm.id for arm in arms}
        done_well: set[str] = set()
        for arm in arms:
            if self.halted:
                record = ArmRecord(id=arm.id, kind=arm.kind, title=arm.arm.get("title", ""),
                                   verdict=SKIPPED, reason="the run halted: " + self.halted)
                self.records.append(record)
                continue
            missing = [d for d in arm.depends_on if d in selected and d not in done_well]
            if missing:
                self.writer(f"[skip] {arm.id}: waits on {', '.join(missing)}")
                self.records.append(ArmRecord(id=arm.id, kind=arm.kind, title=arm.arm.get("title", ""),
                                              verdict=SKIPPED,
                                              reason="depends on " + ", ".join(missing)))
                continue
            if arm.kind != "operator":
                stopped, text = self.stop_flag_set()
                if stopped:
                    self.halted = "the overlay STOP flag is set: " + text
                    self.writer("[halt] " + self.halted)
                    self.records.append(ArmRecord(id=arm.id, kind=arm.kind, title=arm.arm.get("title", ""),
                                                  verdict=SKIPPED, reason=self.halted))
                    continue
                if arm.kind in TRIAL_KINDS or arm.kind in GAME_KINDS:
                    ok, why = self.cool_down()
                    if not ok:
                        self.writer(f"[skip] {arm.id}: {why}")
                        self.records.append(ArmRecord(id=arm.id, kind=arm.kind,
                                                      title=arm.arm.get("title", ""), verdict=SKIPPED,
                                                      reason="the temperature gate: " + why))
                        continue
                    self.writer(f"[arm ] {arm.id} ({arm.bound_s} s bound, {why})")
                else:
                    self.writer(f"[arm ] {arm.id} ({arm.bound_s} s bound)")
            record = self.run_arm(arm)
            self.records.append(record)
            self.writer(f"[{record.verdict:7}] {arm.id}"
                        + (f": {record.reason}" if record.reason else "")
                        + (f" ({record.value} {record.unit})" if record.value is not None else ""))
            if record.verdict in GOOD:
                done_well.add(arm.id)
            if self._stalls >= 2 and not self.halted:
                self.halted = ("two arms saw the lab's sshd refuse a connection (symptom shape of BD-051): "
                               "the run stops rather than feed the connection penalty")
        return self.records
