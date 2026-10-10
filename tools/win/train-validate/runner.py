"""The arm loop of a train validation: the bounds, the gates and the raw records.

Every command goes through `Shell`, and every wait through `Clock`, so the whole sequence runs in the host
tests against a fake target with no lab, no sleep and no subprocess. The owner's rules live here and not in
the operator's head:

* a trial arm is bounded (the lab script ends its own run; this side keeps a backstop of bound + grace),
* every gate between two arms has one deadline of its own, taken from `Clock`, so a lab that answers slowly
  cannot hold the runner past the budget: the ssh calls come out of the same budget as the waits,
* the overlay STOP flag is read between arms, and a set flag ends the run. A flag that cannot be read ends
  the run as well: an unread STOP flag is never a licence to start a GPU arm,
* the temperature is read between arms, and the next arm waits until Tctl is under the cool-down line,
* one ssh call at a time: no session is held open while a trial runs, and the only sampler is the smart
  plug over the LAN (BD-051: a held session before the trial ran coincided with an sshd accept stall),
* the temperature poll never runs faster than the manifest's minimum (the lab sshd penalises fast probes),
* two refused connections stop the run, counted over every ssh call the runner makes, and not only over the
  arms (the gates between the arms are where the accept stall bit in 220 and 221),
* a health gate runs after an arm that touched the GPU, and its raw text is kept next to the arm's own,
* a destructive arm does not run until the arm that verifies the package on the lab has passed in this run,
* a failed arm keeps its logs, runs its restore steps, ends its client on the lab and does not stop the arms
  that do not depend on it,
* a baselined arm whose number was never read is UNREAD, which is a failure and never a pass: a gate of the
  owner's is met only on a number that was read and compared.

The runner never diagnoses. It records what happened and names the symptom shape in the summary.
"""
from __future__ import annotations

import json
import re
import shutil
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field, asdict
from datetime import datetime, timezone
from pathlib import Path

from manifest import GAME_KINDS, Plan, PlannedArm, PlannedStep, TRIAL_KINDS

PASS, WARN, FAIL, BOUND, THERMAL, SKIPPED, OPERATOR = "PASS", "WARN", "FAIL", "BOUND", "THERMAL", "SKIPPED", "OPERATOR"
# A baselined arm that produced no number to compare. It is not a pass: the arm ran and said nothing.
UNREAD = "UNREAD"
# A verdict that lets the arms which depend on this one run.
GOOD = (PASS, WARN)
# No ssh call is issued with less room than this, so that a call always has time to answer. A gate can
# therefore overrun its budget by one step, and by no more than that.
MIN_STEP_S = 20


def utc_iso() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def keep_earlier_attempt(directory: Path) -> Path | None:
    """Keep the raw logs of an attempt that is about to be written over, as `<arm>-attempt-N`.

    An arm that runs again writes its steps into the same directory. In the b28 round that erased the
    logs of the Rise of the Tomb Raider session whose score the release gate stands on. The copy is made
    before the new attempt writes anything, and the arm's own directory always holds the last attempt.
    """
    if not (directory / "record.json").is_file():
        return None
    number = 1
    while (kept := directory.with_name(f"{directory.name}-attempt-{number}")).exists():
        number += 1
    shutil.copytree(directory, kept)
    return kept


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


@dataclass
class GateResult:
    ok: bool = True
    critical: bool = False
    violations: list[str] = field(default_factory=list)
    facts: dict = field(default_factory=dict)
    raw: str = ""


def parse_gate(text: str) -> GateResult:
    """Read gate.ps1's output. A section it could not read is a violation, never a silent pass.

    The rules are here and nowhere else: health flags=15 and no bugcheck record are critical (the machine
    needs a person, not another arm), a GPU fault, a fence timeout or a Display 4101 event fails the arm,
    and TdrDelay, the fan line and the DPM line are warnings.
    """
    result = GateResult(raw=text)
    facts = result.facts
    for pattern, key, cast in (
        (r"flags=(\d+)", "flags", int),
        (r"faults=(\d+)", "faults", int),
        (r"fence_timeouts=(\d+)", "fence_timeouts", int),
        (r"reset_engine=(\d+)", "reset_engine", int),
        (r"hang_recovery=(\d+)", "hang_recovery", int),
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
    record = re.search(r"^hangrecord:\s*(.+)$", text, re.M)
    facts["hangrecord"] = record.group(1).strip() if record else ""

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
    # A stage-1 soft recovery is a recovery, so an arm that took one can still be a pass; an operator has to
    # see it all the same. The count is of the ResetEngine verdict lines alone, never of the start-up line that
    # names the HangRecoveryMode switch, which every boot writes.
    if facts.get("hang_recovery"):
        violate(f"hang_recovery={facts['hang_recovery']}: the driver took a stage-1 soft recovery in this arm "
                f"(record: {facts.get('hangrecord') or 'unread'})", "warn")
    if facts.get("TdrDelay") != 10:
        violate(f"TdrDelay={facts.get('TdrDelay', 'unreadable')}, wanted 10", "warn")
    if not re.search(r"state=curve\b.*controlling=1", facts.get("fan", "")):
        violate(f"fan line is {facts.get('fan') or 'unreadable'!r}, wanted state=curve controlling=1", "warn")
    if "MHz" not in facts.get("dpm", ""):
        violate("the DPM line is unreadable", "warn")
    return result


# What a session script prints when another session holds the lab's one-session-at-a-time lock. The arm did
# not start, so it has nothing to restore.
REFUSED_MARKS = (
    "is still running: wait for its end",
    "a session holds the lock",
)
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
# mon.py prints one of these two lines and nothing else. Anything else means the question did not reach the
# overlay, and an unread STOP flag is treated as a stop.
STOP_SET = re.compile(r"^STOP requested", re.M)
STOP_CLEAR = re.compile(r"^no stop request", re.M)


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
    # Was a number (or text) read where this arm declares one? An arm with no result line reads True.
    value_read: bool = False
    # True or False from the comparison with the baseline, None when there is no baseline to compare.
    baseline_ok: bool | None = None
    # True when only a person can read this arm's value, so that an open value is not read as a parse miss.
    value_operator: bool = False
    # True when the number came from `summary --set` instead of the arm's own log.
    value_given: bool = False
    value_spec: dict | None = None
    baseline: dict | None = None
    gate: dict | None = None
    raw_paths: list[str] = field(default_factory=list)
    steps: list[dict] = field(default_factory=list)
    started_utc: str = ""
    ended_utc: str = ""
    gate_name: str = ""
    attempt: str = ""


def judge(record: ArmRecord) -> tuple[str, str, bool | None, bool]:
    """(verdict, why, baseline ok, value read) of one arm's value against its baseline.

    The runner calls it when an arm ends and the summary calls it again when a person fills in a value only
    they could read, so a number from the screen is held to the same line as one the suite parsed. The case
    that cost this suite a review is the third one: a baselined arm with no number is UNREAD, never PASS.
    """
    baseline, spec = record.baseline, record.value_spec or {}
    name = spec.get("name") or "value"
    if record.value is None:
        if not spec:
            return PASS, "", None, True          # the arm declares no value of its own
        if spec.get("operator"):
            return PASS, "", None, False         # a person reads it; the summary keeps the gate open
        if baseline is None:
            return WARN, f"no {name} was read from the output", None, False
        return UNREAD, (f"no {name} was read from the output, so the {baseline['value']} of "
                        f"{baseline['source']} is not compared"), False, False
    if baseline is None:
        return PASS, "", None, True
    if isinstance(record.value, str):
        return UNREAD, (f"the {name} {record.value!r} is not a number, so the {baseline['value']} of "
                        f"{baseline['source']} is not compared"), False, True
    value, want = float(record.value), float(baseline["value"])
    if "tolerance_abs" in baseline:
        allowed = float(baseline["tolerance_abs"])
        if abs(value - want) > allowed:
            return WARN, f"{value} against {want} (allowed {allowed:+})", False, True
        return PASS, "", True, True
    pct = float(baseline["tolerance_pct"])
    floor = want * (1 - pct / 100.0)
    if value < floor:
        return (WARN, f"{value} is under {floor:.2f} ({pct:g}% below the {want} of {baseline['source']})",
                False, True)
    return PASS, "", True, True


class Runner:
    def __init__(self, plan: Plan, shell: Shell, out_dir: Path, clock: Clock | None = None,
                 writer=print, python: str = sys.executable, already_good: set[str] | None = None):
        self.plan, self.shell, self.out, self.clock = plan, shell, Path(out_dir), clock or Clock()
        self.writer, self.python = writer, python
        self.records: list[ArmRecord] = []
        self.halted = ""
        self._stalls = 0
        # Arms a resumed run already has a good record for. They are not run again, and they still satisfy a
        # dependency and the verified-package guard of a destructive arm.
        self.already_good = set(already_good or ())

    # -- every ssh call of the runner goes through here, so that a refusal is counted wherever it happens --
    def _shell(self, step: PlannedStep, env: dict | None = None, count_stalls: bool = True) -> Completed:
        done = self.shell.run(step, env=env)
        if count_stalls and any(mark.search(done.text) for mark in STALL_MARKS):
            self._stalls += 1
        return done

    def _stalled(self) -> bool:
        """Two refused connections stop the run rather than feed the lab sshd's connection penalty."""
        if self._stalls >= 2 and not self.halted:
            self.halted = ("two ssh calls were refused by the lab (symptom shape of BD-051): the run stops "
                           "rather than feed the connection penalty")
        return bool(self.halted)

    def _budget(self, deadline: float | None) -> float | None:
        return None if deadline is None else deadline - self.clock.now()

    def _timeout(self, deadline: float | None, most: int) -> int | None:
        """The host timeout of one call inside a gate's budget, or None when the budget has run out."""
        left = self._budget(deadline)
        if left is None:
            return most
        if left <= 0:
            return None
        return max(MIN_STEP_S, min(most, int(left)))

    # -- gates -------------------------------------------------------------------------------------
    def stop_flag_set(self) -> tuple[bool, str]:
        """(stop, why). `mon.py stop?` answers with one of two lines; anything else is 'I could not ask'.

        The exit code alone is not enough in either direction: the overlay API can answer without the flag,
        and then mon.py prints no stop request and exits 0, while any error of its own exits non-zero. An
        unread flag stops the run, because the owner's STOP must never be read by guesswork.
        """
        done = self._shell(PlannedStep([self.python, self.plan.values["mon"], "stop?"], 90, "gate"))
        text = done.text.strip()[-200:] or "no output"
        if STOP_SET.search(done.text):
            return True, "the overlay STOP flag is set: " + text
        if STOP_CLEAR.search(done.text) and done.rc == 0:
            return False, ""
        return True, f"the overlay STOP flag could not be read (rc {done.rc}): {text}"

    def read_tctl(self, deadline: float | None = None) -> tuple[float | None, str]:
        """Tctl now, with the retry rule of run-slot.py: a banner timeout waits a minute, not ten seconds.

        Every call and every wait comes out of `deadline`, so three slow reads cannot outlast the budget of
        the gate that asked for the temperature.
        """
        limits = self.plan.limits
        text = "no temperature was read"
        for attempt in range(3):
            timeout = self._timeout(deadline, 90)
            if timeout is None:
                return None, text + " (the budget of this gate ran out)"
            done = self._shell(PlannedStep([self.python, self.plan.values["temp"]], timeout, "gate"))
            text = done.text.strip()
            found = re.search(r"Tctl\s+([0-9.]+)", text)
            if found:
                return float(found.group(1)), text
            if attempt < 2:
                wait = (60 if any(mark.search(text) for mark in STALL_MARKS)
                        else max(int(limits["min_poll_s"]), 10))
                left = self._budget(deadline)
                if left is not None and wait >= left:
                    return None, text + " (no room left in the budget for another read)"
                self.clock.sleep(wait)
        return None, text

    def cool_down(self) -> tuple[bool, str]:
        """Wait until Tctl is under the start line. The owner's rule is to cool below 87 C between trials;
        a trial does not start above the manifest's start line either (80 C, as every native trial slot).

        The budget is one deadline over the whole gate, the ssh calls included, so a lab that takes 90 s to
        answer each read cannot turn a 420 s cool-down into forty minutes.
        """
        limits = self.plan.limits
        start_max = float(limits["tctl_start_max_c"])
        poll = max(int(limits["tctl_poll_s"]), int(limits["min_poll_s"]))
        budget = int(limits["tctl_cool_max_s"])
        deadline = self.clock.now() + budget
        while True:
            tctl, text = self.read_tctl(deadline)
            if tctl is None:
                return False, "the temperature is unreadable: " + text[-160:]
            if tctl < start_max:
                return True, f"Tctl {tctl} C"
            if (self._budget(deadline) or 0) <= poll:
                return False, (f"Tctl {tctl} C, still over {start_max} C when the {budget} s cool-down "
                               f"budget ran out")
            self.writer(f"  cool-down: Tctl {tctl} C, waiting {poll} s")
            self.clock.sleep(poll)

    def _gate_text(self, label: str, timeout: int | None = None, count_stalls: bool = True) -> str:
        argv = [part.replace("{label}", label) for part in self.plan.gate_step.argv]
        step = PlannedStep(argv, timeout or self.plan.gate_step.timeout_s, "gate")
        return self._shell(step, count_stalls=count_stalls).text

    def wait_for_boot(self, arm: PlannedArm, before: str, need_health: bool = False) -> tuple[bool, str, str]:
        """After a planned restart, wait for a boot time that differs from the one before it.

        A restart is not over when port 22 answers: the machine is still up for a few seconds after
        `shutdown /r` and answers the first probe. The gate prints the boot time, so the wait is over the
        boot time itself and not over a guess (BD-059 is the other side of this: a planned restart must not
        be read as an unclean one). With `need_health` the wait also holds until the driver reports
        `flags=15`, because a gate read 35 s into a boot would otherwise fail an arm for being early. The
        poll is slow on purpose, because the lab sshd penalises fast probes, and the arm's bound is one
        deadline over the probes and the waits together.

        This is the one place where a refused connection is not counted towards the BD-051 halt: while the
        machine is down, a refusal is the expected answer.
        """
        limits = self.plan.limits
        was = re.search(r"^boot (\S+)", before, re.M)
        settle = int(limits.get("restart_settle_s", 30))
        poll = max(int(limits.get("restart_poll_s", 30)), int(limits["min_poll_s"]))
        started = self.clock.now()
        deadline = started + arm.bound_s
        self.writer(f"  restart: waiting {settle} s before the first probe")
        self.clock.sleep(settle)
        text, new_boot = "", False
        while True:
            timeout = self._timeout(deadline, 60)
            if timeout is None:
                break
            self._shell(PlannedStep([self.python, self.plan.values["target"], "forget"], timeout, "gate"),
                        count_stalls=False)
            timeout = self._timeout(deadline, self.plan.gate_step.timeout_s)
            if timeout is None:
                break
            text = self._gate_text("after-restart", timeout, count_stalls=False)
            now = re.search(r"^boot (\S+)", text, re.M)
            new_boot = bool("gate end" in text and now and (was is None or now.group(1) != was.group(1)))
            if new_boot and (not need_health or "flags=15" in text):
                return True, text, f"the machine is back after {round(self.clock.now() - started)} s"
            if (self._budget(deadline) or 0) <= poll:
                break
            self.clock.sleep(poll)
        if new_boot:
            return False, text, (f"the machine came back but the driver did not report flags=15 within "
                                 f"{arm.bound_s} s")
        return False, text, f"no new boot time within {arm.bound_s} s of the restart"

    def health_gate(self, arm_id: str, directory: Path) -> GateResult:
        text = self._gate_text(arm_id)
        gate = parse_gate(text)
        path = directory / "gate.txt"
        self._write(path, text)
        gate.facts["raw"] = path.as_posix()
        return gate

    # -- arms --------------------------------------------------------------------------------------
    def _write(self, path: Path, text: str) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8", newline="\n")

    def _parse_value(self, arm: PlannedArm, text: str):
        """The arm's own number, from the last match of its result line.

        The last match and not the first: `clean-slate.ps1 -Step uninstall` prints its inventory twice, once
        before the uninstaller and once after it, and the number that matters is the one after. A spec may
        also name a `section` marker, and then only the text after its last occurrence is read.
        """
        spec = arm.arm.get("result")
        if not spec or spec.get("operator"):
            return None
        section = spec.get("section")
        if section:
            at = text.rfind(section)
            text = text[at + len(section):] if at >= 0 else text
        found = list(re.finditer(spec["regex"], text, re.M))
        if not found:
            return None
        pick = found[0] if spec.get("pick") == "first" else found[-1]
        raw = pick.group(int(spec.get("group", 1)))
        if spec.get("kind") == "text":
            return raw.strip()
        try:
            return float(raw)
        except ValueError:
            return raw.strip()

    def _kill_clients(self, arm: PlannedArm, record: ArmRecord, directory: Path) -> None:
        """End this arm's client on the lab after it failed or hit its bound.

        The lab scripts bound their own GPU work (`pt-run.ps1` registers its client with an
        ExecutionTimeLimit), so this is a backstop rather than the bound itself: when the host gave up on the
        ssh call, nothing on this side had asked the client to go, and the next arm would start beside it.
        """
        names = list(arm.arm.get("kill") or ())
        if not names:
            return
        argv = [self.python, self.plan.values["target"], "ps", self.plan.values["kit"] + "/kill-clients.ps1",
                "-Names", ",".join(names)]
        done = self._shell(PlannedStep(argv, 90, "post"))
        path = directory / "kill-clients.txt"
        self._write(path, f"$ {' '.join(argv)}\n(rc {done.rc})\n\n{done.text}")
        record.raw_paths.append(path.as_posix())
        record.steps.append({"phase": "post", "argv": argv, "rc": done.rc, "seconds": round(done.seconds, 1),
                             "raw": path.as_posix(), "note": "the client of a failed arm is ended"})

    def run_arm(self, arm: PlannedArm) -> ArmRecord:
        spec = arm.arm.get("result") or {}
        record = ArmRecord(id=arm.id, kind=arm.kind, title=arm.arm.get("title", ""), bound_s=arm.bound_s,
                           baseline=arm.arm.get("baseline"), gate_name=arm.arm.get("gate", ""),
                           attempt=arm.arm.get("attempt_name", ""), started_utc=utc_iso(),
                           value_name=spec.get("name", ""), unit=spec.get("unit", ""),
                           value_operator=bool(spec.get("operator")), value_spec=dict(spec) or None)
        directory = self.out / arm.id
        directory.mkdir(parents=True, exist_ok=True)
        kept = keep_earlier_attempt(directory)
        if kept:
            self.writer(f"[keep] {arm.id}: the earlier attempt's raw logs are kept as {kept.name}/")
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
        restore_failed: list[str] = []
        try:
            for number, step in enumerate(arm.steps, start=1):
                if failed and step.phase in ("pre", "run"):
                    # The arm is already lost. The post steps are the restore and still run; another run
                    # command would only add noise on a machine in an unknown state.
                    record.steps.append({"phase": step.phase, "argv": step.argv, "rc": None,
                                         "seconds": 0, "skipped": "an earlier step of this arm failed"})
                    continue
                done = self._shell(step, env=self._env(arm) if step.phase == "run" else None)
                path = directory / f"step-{number:02d}-{step.phase}.txt"
                self._write(path, f"$ {step.text()}\n(rc {done.rc}, {done.seconds:.1f} s)\n\n{done.text}")
                record.raw_paths.append(path.as_posix())
                record.steps.append({"phase": step.phase, "argv": step.argv, "rc": done.rc,
                                     "seconds": round(done.seconds, 1), "timed_out": done.timed_out,
                                     "optional": step.optional, "raw": path.as_posix()})
                texts.append(done.text)
                allowed = step.phase == "run" and done.rc in arm.allow_rc
                bad = done.timed_out or (done.rc != 0 and not allowed)
                if not bad:
                    continue
                if step.optional:
                    # An optional step is one the manifest marked as not load-bearing for the arm's own
                    # question, such as hiding the overlay. It is recorded and it fails nothing.
                    restore_failed.append(f"the optional {step.phase} step ({step.text()}) ended rc {done.rc}")
                elif step.phase == "post":
                    # A restore that did not run leaves the lab carrying this arm's setting. The arm's own
                    # answer stands, and the reader is told.
                    restore_failed.append(f"the restore step ({step.text()}) ended rc {done.rc}")
                elif any(mark in done.text for mark in REFUSED_MARKS):
                    # The session script refused the arm because another session holds its lock: nothing of
                    # this arm ran. Its post steps are restores of shared game settings, and in the b28 run
                    # one of them (RotTR vsync back to 1) landed on a session another run had just started.
                    # An arm that did not start restores nothing.
                    record.verdict, record.reason = FAIL, (
                        "another session holds the session lock, so this arm did not start and its restore "
                        "steps did not run: " + done.text.strip().splitlines()[-1])
                    failed = True
                    break
                else:
                    record.verdict = BOUND if done.timed_out else FAIL
                    where = "" if step.phase == "run" else f" of the {step.phase} step"
                    record.reason = (f"no end within {step.timeout_s} s (bound {arm.bound_s} s){where}"
                                     if done.timed_out else f"rc {done.rc}{where}")
                    failed = True
        finally:
            if sampler:
                sampler.halt.set()
                sampler.join(timeout=40)
        gate_done = False
        if arm.kind == "restart" and not failed:
            came_back, boot_text, why = self.wait_for_boot(arm, "\n".join(texts), need_health=arm.gate_after)
            boot_path = directory / "after-restart.txt"
            self._write(boot_path, boot_text)
            record.raw_paths.append(boot_path.as_posix())
            texts.append(boot_text)
            if not came_back:
                record.verdict = FAIL
                record.reason = why
            elif arm.gate_after:
                # The gate that proved the new boot is the health gate of this arm: it does not run twice.
                gate = parse_gate(boot_text)
                gate.facts["raw"] = boot_path.as_posix()
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
        record.value = self._parse_value(arm, text)
        verdict, why, record.baseline_ok, record.value_read = judge(record)
        if record.verdict == PASS and verdict != PASS:
            record.verdict, record.reason = verdict, why
        if record.value_operator:
            record.reason = (record.reason + "; " if record.reason else "") + \
                "the value is the operator's reading: " + spec.get("how", "")
        if restore_failed:
            record.reason = (record.reason + "; " if record.reason else "") + "; ".join(restore_failed)
            if record.verdict == PASS and any(line.startswith("the restore") for line in restore_failed):
                record.verdict = WARN
        if record.verdict not in GOOD:
            self._kill_clients(arm, record, directory)
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

    def _skip(self, arm: PlannedArm, reason: str) -> ArmRecord:
        spec = arm.arm.get("result") or {}
        return ArmRecord(id=arm.id, kind=arm.kind, title=arm.arm.get("title", ""), verdict=SKIPPED,
                         reason=reason, bound_s=arm.bound_s, baseline=arm.arm.get("baseline"),
                         gate_name=arm.arm.get("gate", ""), value_name=spec.get("name", ""),
                         unit=spec.get("unit", ""), value_operator=bool(spec.get("operator")),
                         value_spec=dict(spec) or None, started_utc=utc_iso(), ended_utc=utc_iso())

    def run(self, arms: list[PlannedArm] | None = None) -> list[ArmRecord]:
        # An empty list is a run with nothing left to do, which a resumed run reaches when every arm it
        # names has already passed. Only `None` means "the whole plan".
        arms = self.plan.arms if arms is None else arms
        # Only a dependency inside this run can hold an arm back. A subset (--arms gates) or a resumed run
        # names the arms it wants, and an arm that ran in an earlier session is not run again to satisfy a
        # dependency on it.
        selected = {arm.id for arm in arms}
        done_well: set[str] = set(self.already_good)
        for arm in arms:
            # `_stalled` sets `halted` itself when two ssh calls were refused, wherever they were refused:
            # the gates between the arms are calls too, and that is where the accept stall bit in 220.
            if self._stalled():
                self.records.append(self._skip(arm, "the run halted: " + self.halted))
                continue
            missing = [d for d in arm.depends_on if d in selected and d not in done_well]
            if missing:
                self.writer(f"[skip] {arm.id}: waits on {', '.join(missing)}")
                self.records.append(self._skip(arm, "depends on " + ", ".join(missing)))
                continue
            # A destructive arm (the uninstall of the owner's installer test) does not run on a package that
            # was not verified on the lab in this run. Without the guard, a failed push leaves the machine
            # with no display driver and a person has to put it back.
            guard = arm.arm.get("needs_verified")
            if guard and guard not in done_well:
                reason = (f"{arm.id} removes the installed driver, and {guard} has not verified the package "
                          f"on the lab in this run: ask for {guard} as well, or resume a run that passed it")
                self.writer(f"[skip] {arm.id}: {reason}")
                self.records.append(self._skip(arm, reason))
                continue
            if arm.kind != "operator":
                stopped, why = self.stop_flag_set()
                if stopped:
                    self.halted = why
                    self.writer("[halt] " + self.halted)
                    self.records.append(self._skip(arm, self.halted))
                    continue
                if self._stalled():
                    self.records.append(self._skip(arm, "the run halted: " + self.halted))
                    continue
                if arm.kind in TRIAL_KINDS or arm.kind in GAME_KINDS:
                    ok, why = self.cool_down()
                    if not ok:
                        self.writer(f"[skip] {arm.id}: {why}")
                        self.records.append(self._skip(arm, "the temperature gate: " + why))
                        self._stalled()
                        continue
                    self.writer(f"[arm ] {arm.id} ({arm.bound_s} s bound, {why})")
                else:
                    self.writer(f"[arm ] {arm.id} ({arm.bound_s} s bound)")
                if self._stalled():
                    self.records.append(self._skip(arm, "the run halted: " + self.halted))
                    continue
            record = self.run_arm(arm)
            self.records.append(record)
            self.writer(f"[{record.verdict:7}] {arm.id}"
                        + (f": {record.reason}" if record.reason else "")
                        + (f" ({record.value} {record.unit})" if record.value is not None else ""))
            if record.verdict in GOOD:
                done_well.add(arm.id)
            self._stalled()
        return self.records
