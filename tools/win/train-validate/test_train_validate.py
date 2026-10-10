"""Host tests of the train validation suite.

    python -m unittest discover -s tools/win/train-validate

Nothing here reaches the lab, starts a process or sleeps: the whole sequence runs against a fake target
(`FakeShell`) and a fake clock. The cases are the ones that cost a round in a real train: an arm that
overruns its bound, a thermal stop, the overlay STOP flag, a failed health gate and a leftover attempt
directory. The shipped `arms.json` is checked against the owner's bounds and against the lab scripts it
calls, so a renamed parameter fails here and not in the middle of a validation.
"""
from __future__ import annotations

import json
import re
import unittest
import unittest.mock
from pathlib import Path

import manifest
import promote
import runner as runner_module
import summary as summary_module
from manifest import GAME_KINDS, HOST_KINDS, TRIAL_KINDS, ManifestError, Package
from runner import BOUND, Completed, FAIL, OPERATOR, PASS, SKIPPED, THERMAL, WARN, Runner

HERE = Path(__file__).resolve().parent
LAB = HERE / "lab"
GATE_OK = """label vk-smoke utc 2026-10-10T08:00:00.0000000Z
boot 2026-10-10T07:47:40Z uptime_s 740
installroot C:\\Program Files\\amdgpu-wddm
health: health abi=1 version=0x000700D8 flags=15 generation=1 epoch=5 completed=9 age_ms=2 ready_ms=5
log 212 lines
counters: faults=0 fence_timeouts=0 reset_engine=0 hang_recovery=0
display: events=0 id4101=0
bugchecks: 0
appcrashes: 0
hangrecord: absent
dpm: dpm 500 MHz 820 mV (SMU 500 MHz VID 116) 63.4 C busy 0.0%
fan: state=curve controlling=1 rpm=2100
tctl: 63.4
tdr: TdrDelay=10 TdrDdiDelay= TdrLevel=
adapter: status=OK name=BC-250 GPU (amdgpu-wddm)
gate end
"""


class FakeClock:
    def __init__(self):
        self.t, self.slept = 0.0, []

    def now(self) -> float:
        self.t += 1.0
        return self.t

    def sleep(self, seconds: float) -> None:
        self.slept.append(seconds)
        self.t += seconds


class FakeShell:
    """The fake ssh layer: it answers by the words in the command line and remembers every call.

    With a `clock` and a `cost` the answer also takes time on the fake clock, which is how a slow or
    stalling lab is written down here: the runner's own budgets have to come out of the same clock.
    """

    def __init__(self, replies=None, default=None, clock=None, cost=0.0):
        self.replies = list(replies or ())
        self.default = default or Completed(0, "ok")
        self.calls: list[list[str]] = []
        self.clock, self.cost = clock, cost

    def run(self, step, env=None) -> Completed:
        self.calls.append(list(step.argv))
        if self.clock is not None and self.cost:
            # The call cannot take longer than the host backstop the runner gave it.
            self.clock.t += min(self.cost, step.timeout_s)
        joined = " ".join(step.argv)
        for needle, reply in self.replies:
            if needle in joined:
                return reply(step) if callable(reply) else reply
        return self.default

    def said(self, needle: str) -> list[list[str]]:
        return [call for call in self.calls if needle in " ".join(call)]


def small_manifest(**overrides) -> dict:
    data = {
        "schema": 1,
        "config": {"target": "{repo}/t.py", "mon": "{repo}/mon.py", "temp": "{repo}/temp.py",
                   "kit": "{repo}/lab", "caps": "{ws}/caps", "pathtrace": "{ws}/pt",
                   "plug": "{ws}/plug.py", "lab_root": "C:\\L\\{release}",
                   "lab_pkg": "C:\\L\\{release}\\{pkg_name}", "lab_zip": "C:\\L\\{release}\\{pkg_name}.zip"},
        "limits": {"trial_bound_s": 170, "game_bound_s": 1200, "tctl_start_max_c": 80.0,
                   "tctl_poll_s": 20, "tctl_cool_max_s": 60, "host_grace_s": 45,
                   "min_poll_s": 10},
        "sets": {"standard": ["smoke", "demo", "game"]},
        "arms": [
            {"id": "smoke", "title": "smoke", "kind": "lab", "bound_s": 170,
             "run": [["python", "{target}", "ps", "{kit}/vk-smoke.ps1"]], "expect": "smoke ok"},
            {"id": "demo", "title": "demo", "kind": "lab", "bound_s": 170, "gate": "q2rtx",
             "run": [["python", "{target}", "ps", "{pathtrace}/lab/pt-run.ps1", "-Seconds", "150"]],
             "result": {"regex": r"([\d.]+) fps", "name": "demo", "unit": "fps"},
             "baseline": {"value": 60.0, "tolerance_pct": 10, "source": "the previous train"},
             "depends_on": ["smoke"]},
            {"id": "game", "title": "game", "kind": "game", "bound_s": 1200, "gate": "rottr",
             "attempt": "native-caps",
             "pre": [["python", "{target}", "ps", "{caps}/lab/vsync.ps1", "-Value", "0"]],
             "run": [["bash", "{caps}/run-game.sh", "{attempt_n}", "rottr", "{bound}"]],
             "post": [["python", "{target}", "ps", "{caps}/lab/vsync.ps1", "-Value", "1"]],
             "result": {"operator": True, "name": "overall", "unit": "fps", "how": "the result screen"},
             "baseline": {"value": 55.15, "tolerance_pct": 10, "source": "b26 517"},
             "depends_on": ["demo"]},
        ],
        "failure_classes": [{"id": "BD-102", "symptom": "Quake II RTX hangs the GPU.",
                             "match": ["HARDWARE FENCE TIMEOUT"], "needs_any": True}],
    }
    data.update(overrides)
    return data


def fake_package(tmp: Path, with_zip: bool = True) -> Package:
    directory = tmp / "amdgpu-wddm-tester-0.7.0-tester.1"
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "manifest.json").write_text(json.dumps({
        "name": directory.name, "release": "0.7.0-tester.1", "version": "0.7.0-tester.1",
        "kmd_build": "0.7.0.1", "kmd_abi": "0x000700D8", "built_utc": "2026-10-10T07:45:30Z"}),
        encoding="utf-8")
    # A real package has its zip beside the directory, and the arms that install it name the zip and its
    # hash. The tests keep one, a few bytes long, so that the hashing path runs as it does in a train.
    if with_zip:
        (tmp / (directory.name + ".zip")).write_bytes(b"PK\x03\x04 a fake package zip")
    return Package.read(directory)


def step_argvs(arm: dict) -> list[list[str]]:
    """Every command line of an arm. A step is a list, or an object with `argv` when it is optional."""
    out = []
    for entry in arm.get("pre", []) + arm.get("run", []) + arm.get("post", []):
        out.append(entry["argv"] if isinstance(entry, dict) else entry)
    return out


def small_plan(tmp: Path, data=None, attempt_base=600):
    """A plan whose workspace is the test's own temporary directory: the tests write nowhere else."""
    return manifest.build(fake_package(tmp), data or small_manifest(), out_dir=tmp / "out",
                          roots=(HERE.parents[2], tmp), attempt_base=attempt_base, python="python")


class ShippedManifest(unittest.TestCase):
    """The file the operator runs with, against the owner's rules and the scripts on disk."""

    def setUp(self):
        self.data = manifest.load_manifest()

    def test_bounds_per_kind(self):
        for arm in self.data["arms"]:
            if arm["kind"] in TRIAL_KINDS:
                self.assertLessEqual(arm["bound_s"], 170, arm["id"])
            if arm["kind"] in GAME_KINDS:
                self.assertLessEqual(arm["bound_s"], 1200, arm["id"])
            if arm["kind"] in HOST_KINDS and arm["kind"] != "restart":
                self.assertIn("why_not_a_trial", arm, arm["id"])

    def test_both_owner_gates_are_in_the_standard_set(self):
        standard = set(self.data["sets"]["standard"])
        gates = {arm["gate"] for arm in self.data["arms"] if arm.get("gate") and arm["id"] in standard}
        self.assertEqual(gates, {"rottr", "q2rtx"})

    def test_the_interactive_witcher_session_stays_with_the_operator(self):
        arm = next(a for a in self.data["arms"] if a["id"] == "w3-high-rt")
        self.assertEqual(arm["kind"], "operator")

    def test_every_lab_script_the_manifest_calls_exists(self):
        for arm in self.data["arms"]:
            for argv in step_argvs(arm):
                for part in argv:
                    if part.startswith("{kit}/"):
                        self.assertTrue((LAB / part.split("/", 1)[1]).is_file(), part)

    def test_the_kill_script_the_runner_uses_after_a_failed_arm_exists(self):
        self.assertTrue((LAB / "kill-clients.ps1").is_file())
        declared = (LAB / "kill-clients.ps1").read_text(encoding="utf-8")
        self.assertIn("$Names", declared)
        for arm in self.data["arms"]:
            self.assertIsInstance(arm.get("kill", []), list, arm["id"])

    def test_every_parameter_the_manifest_passes_is_declared_by_its_lab_script(self):
        for arm in self.data["arms"]:
            for argv in step_argvs(arm):
                script = next((part for part in argv if part.startswith("{kit}/")), None)
                if not script:
                    continue
                path = LAB / script.split("/", 1)[1]
                declared = set(re.findall(r"\[(?:\w+\[?\]?\s*)?\$(\w+)", path.read_text(encoding="utf-8")))
                declared |= set(re.findall(r"\$(\w+)\s*=", path.read_text(encoding="utf-8")))
                for part in argv:
                    if part.startswith("-") and not part[1:].isdigit():
                        self.assertIn(part[1:], declared, f"{path.name} has no {part}")

    def test_the_gate_script_is_there_and_the_runner_names_it(self):
        self.assertTrue((LAB / "gate.ps1").is_file())
        plan = manifest.build(Package(directory=Path("."), release="r", name="n", kmd_build="", kmd_abi="",
                                      built_utc="", zip_path=None, zip_sha256="", zip_bytes=0), self.data,
                              arm_ids=["vk-smoke"], python="python")
        self.assertIn("gate.ps1", " ".join(plan.gate_step.argv))

    def test_a_manifest_over_the_trial_bound_is_refused(self):
        data = small_manifest()
        data["arms"][0]["bound_s"] = 200
        with self.assertRaises(ManifestError):
            manifest.check_manifest(data)

    def test_a_baseline_without_a_tolerance_is_refused(self):
        data = small_manifest()
        data["arms"][1]["baseline"] = {"value": 1, "source": "nowhere"}
        with self.assertRaises(ManifestError):
            manifest.check_manifest(data)

    def test_an_unknown_placeholder_is_refused(self):
        with self.assertRaises(ManifestError):
            manifest.expand("{nothing}", {"repo": "x"})


class GateParsing(unittest.TestCase):
    def test_a_clean_gate_passes(self):
        gate = runner_module.parse_gate(GATE_OK)
        self.assertTrue(gate.ok, gate.violations)
        self.assertEqual(gate.facts["flags"], 15)
        self.assertEqual(gate.facts["tctl"], 63.4)

    def test_a_bugcheck_is_critical(self):
        gate = runner_module.parse_gate(GATE_OK.replace("bugchecks: 0", "bugchecks: 1"))
        self.assertFalse(gate.ok)
        self.assertTrue(gate.critical)

    def test_a_tdr_event_fails_the_gate(self):
        gate = runner_module.parse_gate(GATE_OK.replace("id4101=0", "id4101=2"))
        self.assertFalse(gate.ok)
        self.assertFalse(gate.critical)
        self.assertTrue(any("id4101=2" in v for v in gate.violations))

    def test_a_fence_timeout_fails_the_gate(self):
        gate = runner_module.parse_gate(GATE_OK.replace("fence_timeouts=0", "fence_timeouts=1"))
        self.assertFalse(gate.ok)

    def test_a_tdrdelay_other_than_ten_is_a_warning(self):
        gate = runner_module.parse_gate(GATE_OK.replace("TdrDelay=10", "TdrDelay=2"))
        self.assertFalse(gate.ok)
        self.assertFalse(gate.critical)
        self.assertTrue(all(v.startswith("warn") for v in gate.violations), gate.violations)

    def test_a_fan_off_our_curve_is_a_warning(self):
        gate = runner_module.parse_gate(GATE_OK.replace("state=curve controlling=1", "state=auto controlling=0"))
        self.assertTrue(any("fan line" in v for v in gate.violations))

    def test_a_gate_that_ends_early_is_critical(self):
        gate = runner_module.parse_gate("label x\nhealth: flags=15\n")
        self.assertTrue(gate.critical)

    def test_a_stage_one_recovery_is_a_warning_and_is_read(self):
        text = GATE_OK.replace("hang_recovery=0", "hang_recovery=2").replace(
            "hangrecord: absent", "hangrecord: attempts=2 recovered=1 not_drained=1 refused=0 last_verdict=2")
        gate = runner_module.parse_gate(text)
        self.assertEqual(gate.facts["hang_recovery"], 2)
        self.assertEqual(gate.facts["hangrecord"],
                         "attempts=2 recovered=1 not_drained=1 refused=0 last_verdict=2")
        self.assertFalse(gate.ok)
        self.assertFalse(gate.critical)
        self.assertTrue(any(v.startswith("warn") and "hang_recovery=2" in v for v in gate.violations),
                        gate.violations)

    def test_a_clean_gate_reads_no_recovery(self):
        gate = runner_module.parse_gate(GATE_OK)
        self.assertEqual(gate.facts["hang_recovery"], 0)
        self.assertEqual(gate.facts["hangrecord"], "absent")


class HangRecoveryCount(unittest.TestCase):
    """The pattern gate.ps1 counts stage-1 hang recoveries with, read out of the script itself.

    The gate counted every log line that held the words "hang recovery" or "HangRecovery". Every boot writes
    'wddm: HangRecoveryMode 1: a node-0 ResetEngine tries stage-1 soft recovery, verdicts in
    Parameters\\HangRecovery' (driver/kmd/wddm.c), so the gate reported one attempt on a machine that had never
    hung, and it reported one again when two real attempts had happened: the lines a real attempt writes say
    "soft recovery", not "hang recovery", so the old pattern never matched one of them. The fixtures below are
    the driver's own format strings. Select-String matches without regard to case, so the test does too.
    """

    # driver/kmd/wddm.c, the start of every device: the switch, not an attempt.
    MODE_ON = (r"  37      1.158 wddm: HangRecoveryMode 1: a node-0 ResetEngine tries stage-1 soft recovery, "
               r"verdicts in Parameters\HangRecovery")
    MODE_OFF = "  37      1.158 wddm: HangRecoveryMode 0: ResetEngine refuses every engine reset, as before 0.7.216.13"
    # One stage-1 attempt that drained, with the line that says why, and the record guard.c flushed for it.
    RECOVERED = ("  412     88.700 wddm: *** ResetEngine node 0: SOFT RECOVERED, aborted fence 1186, "
                 "node 0 reopened ***")
    RECOVERED_WHY = "  413     88.701 wddm: soft recovery: verdict 1 seq 4211 vmid 3, 7 kill(s) in 902 us"
    RECORD = "  414     88.702 hang record: verdict 1 seq 4211 fence 1186 kills 7 902 us, persisted 0x00000000"
    # One stage-1 attempt that did not drain: refused, then the adapter-wide reset and 0x116.
    REFUSED = "  520    120.300 wddm: ResetEngine node 0: soft recovery verdict 2, refusing as before"
    REFUSED_WHY = ("  521    120.301 wddm: soft recovery refused: seq 4300 fence 1190 vmid 3, "
                   "95 kill(s) in 10002 us")
    # The answer of a node this stage never touches: not a stage-1 attempt.
    NO_ENGINE_RESET = ("  522    120.400 wddm: *** ResetEngine node 1 engine 0: refused, this part has no "
                       "engine reset ***")

    @staticmethod
    def pattern() -> str:
        text = (LAB / "gate.ps1").read_text(encoding="utf-8")
        found = re.search(r"^\$HangRecoveryLine = '(.+)'$", text, re.M)
        assert found, "gate.ps1 no longer declares $HangRecoveryLine on a line of its own"
        return found.group(1)

    def count(self, lines) -> int:
        rule = re.compile(self.pattern(), re.I)
        return sum(1 for line in lines if rule.search(line))

    def test_the_old_pattern_is_gone(self):
        self.assertNotIn("hang recovery|HangRecovery", (LAB / "gate.ps1").read_text(encoding="utf-8"))

    def test_the_start_up_line_is_not_an_attempt(self):
        self.assertEqual(self.count([self.MODE_ON]), 0)
        self.assertEqual(self.count([self.MODE_OFF]), 0)

    def test_one_recovery_counts_once(self):
        self.assertEqual(self.count([self.MODE_ON, self.RECOVERED, self.RECOVERED_WHY, self.RECORD]), 1)

    def test_one_refusal_counts_once(self):
        self.assertEqual(self.count([self.MODE_ON, self.REFUSED, self.REFUSED_WHY]), 1)

    def test_two_attempts_count_twice(self):
        self.assertEqual(self.count([self.MODE_ON, self.RECOVERED, self.RECOVERED_WHY, self.RECORD,
                                     self.REFUSED, self.REFUSED_WHY]), 2)

    def test_the_node_one_refusal_is_not_an_attempt(self):
        self.assertEqual(self.count([self.MODE_ON, self.NO_ENGINE_RESET]), 0)

    def test_the_gate_reads_the_drivers_own_record(self):
        text = (LAB / "gate.ps1").read_text(encoding="utf-8")
        self.assertIn(r"Services\bc250kmd\Parameters\HangRecovery", text)
        self.assertIn("hangrecord:", text)


class ArmLoop(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(self.enterContext(__import__("tempfile").TemporaryDirectory()))
        self.plan = small_plan(self.tmp)
        self.clock = FakeClock()

    def run_with(self, replies, arms=None):
        shell = FakeShell(replies)
        run = Runner(self.plan, shell, self.tmp / "out", clock=self.clock, writer=lambda *a: None,
                     python="python")
        records = run.run(arms or self.plan.arms)
        return run, shell, {record.id: record for record in records}

    def test_the_whole_sequence_passes_and_the_game_arm_keeps_its_restore(self):
        run, shell, records = self.run_with([
            ("mon.py stop?", Completed(0, "no stop request")),
            ("temp.py", Completed(0, "Tctl 61.5 C")),
            ("vk-smoke.ps1", Completed(0, "smoke ok")),
            ("pt-run.ps1", Completed(0, "631 frames, 10.45 seconds: 60.40 fps")),
            ("run-game.sh", Completed(0, "status ok")),
            ("gate.ps1", Completed(0, GATE_OK)),
        ])
        self.assertEqual(records["smoke"].verdict, PASS)
        self.assertEqual(records["demo"].verdict, PASS)
        self.assertEqual(records["demo"].value, 60.40)
        self.assertEqual(records["game"].verdict, PASS)
        self.assertEqual(records["game"].attempt, "native-caps600")
        self.assertEqual(len(shell.said("vsync.ps1")), 2, "the pre and the post step both ran")
        self.assertEqual(len(shell.said("gate.ps1")), 3, "a gate after each of the three arms")
        self.assertFalse(run.halted)

    def test_an_arm_that_overruns_its_bound_is_bound_and_the_rest_still_runs(self):
        run, shell, records = self.run_with([
            ("mon.py stop?", Completed(0, "no stop request")),
            ("temp.py", Completed(0, "Tctl 61.5 C")),
            ("vk-smoke.ps1", Completed(124, "", "no end", timed_out=True)),
            ("pt-run.ps1", Completed(0, "631 frames, 10.45 seconds: 60.40 fps")),
            ("gate.ps1", Completed(0, GATE_OK)),
        ])
        self.assertEqual(records["smoke"].verdict, BOUND)
        self.assertIn("no end within", records["smoke"].reason)
        self.assertEqual(records["demo"].verdict, SKIPPED, "it depends on the arm that overran")
        self.assertIn("depends on smoke", records["demo"].reason)
        self.assertFalse(run.halted, "one failed arm does not stop the run")

    def test_a_subset_runs_although_its_dependency_is_not_in_it(self):
        _, shell, records = self.run_with([
            ("mon.py stop?", Completed(0, "no stop request")),
            ("temp.py", Completed(0, "Tctl 61.5 C")),
            ("pt-run.ps1", Completed(0, "631 frames, 10.45 seconds: 60.40 fps")),
            ("gate.ps1", Completed(0, GATE_OK)),
        ], arms=[self.plan.by_id("demo")])
        self.assertEqual(records["demo"].verdict, PASS, "demo depends on smoke, which was not asked for")
        self.assertEqual(shell.said("vk-smoke.ps1"), [], "the dependency was not run either")

    def test_a_thermal_stop_is_its_own_verdict(self):
        _, _, records = self.run_with([
            ("mon.py stop?", Completed(0, "no stop request")),
            ("temp.py", Completed(0, "Tctl 61.5 C")),
            ("vk-smoke.ps1", Completed(0, "smoke ok")),
            ("pt-run.ps1", Completed(0, "t=40 tctl 88.1\nend: reason thermal, elapsed 44.2 s")),
            ("gate.ps1", Completed(0, GATE_OK)),
        ])
        self.assertEqual(records["demo"].verdict, THERMAL)
        self.assertIn("87 C", records["demo"].reason)

    def test_the_arm_waits_while_the_machine_cools_and_never_polls_faster_than_the_minimum(self):
        temperatures = iter(["Tctl 88.0 C", "Tctl 84.0 C", "Tctl 70.0 C"])
        _, shell, records = self.run_with([
            ("mon.py stop?", Completed(0, "no stop request")),
            ("temp.py", lambda step: Completed(0, next(temperatures, "Tctl 70.0 C"))),
            ("vk-smoke.ps1", Completed(0, "smoke ok")),
            ("gate.ps1", Completed(0, GATE_OK)),
        ], arms=[self.plan.by_id("smoke")])
        self.assertEqual(records["smoke"].verdict, PASS)
        self.assertEqual(self.clock.slept, [20, 20])
        self.assertTrue(all(wait >= self.plan.limits["min_poll_s"] for wait in self.clock.slept))

    def test_a_machine_that_stays_hot_skips_the_arm(self):
        _, _, records = self.run_with([
            ("mon.py stop?", Completed(0, "no stop request")),
            ("temp.py", Completed(0, "Tctl 88.0 C")),
            ("vk-smoke.ps1", Completed(0, "smoke ok")),
        ], arms=[self.plan.by_id("smoke")])
        self.assertEqual(records["smoke"].verdict, SKIPPED)
        self.assertIn("cool-down", records["smoke"].reason)

    def test_the_stop_flag_ends_the_run_before_the_arm_starts(self):
        run, shell, records = self.run_with([
            ("mon.py stop?", Completed(1, "STOP requested by the owner")),
            ("temp.py", Completed(0, "Tctl 61.5 C")),
        ])
        self.assertEqual(records["smoke"].verdict, SKIPPED)
        self.assertIn("STOP", run.halted)
        self.assertEqual(shell.said("vk-smoke.ps1"), [], "no arm ran")
        self.assertEqual(records["game"].verdict, SKIPPED)

    def test_a_failed_health_gate_fails_the_arm_that_came_before_it(self):
        run, _, records = self.run_with([
            ("mon.py stop?", Completed(0, "no stop request")),
            ("temp.py", Completed(0, "Tctl 61.5 C")),
            ("vk-smoke.ps1", Completed(0, "smoke ok")),
            ("gate.ps1", Completed(0, GATE_OK.replace("id4101=0", "id4101=1"))),
        ], arms=[self.plan.by_id("smoke")])
        self.assertEqual(records["smoke"].verdict, FAIL)
        self.assertIn("health gate", records["smoke"].reason)
        self.assertFalse(run.halted, "a TDR event is not a reason to stop the whole run")

    def test_a_bugcheck_after_an_arm_halts_the_run(self):
        run, _, records = self.run_with([
            ("mon.py stop?", Completed(0, "no stop request")),
            ("temp.py", Completed(0, "Tctl 61.5 C")),
            ("vk-smoke.ps1", Completed(0, "smoke ok")),
            ("gate.ps1", Completed(0, GATE_OK.replace("bugchecks: 0", "bugchecks: 1"))),
        ])
        self.assertEqual(records["smoke"].verdict, FAIL)
        self.assertIn("critical", run.halted)
        self.assertEqual(records["demo"].verdict, SKIPPED)

    def test_a_value_under_its_baseline_is_a_warning_and_not_a_failure(self):
        _, _, records = self.run_with([
            ("mon.py stop?", Completed(0, "no stop request")),
            ("temp.py", Completed(0, "Tctl 61.5 C")),
            ("vk-smoke.ps1", Completed(0, "smoke ok")),
            ("pt-run.ps1", Completed(0, "631 frames, 20.0 seconds: 31.50 fps")),
            ("gate.ps1", Completed(0, GATE_OK)),
        ])
        self.assertEqual(records["demo"].verdict, WARN)
        self.assertIn("under", records["demo"].reason)

    def test_two_sshd_refusals_stop_the_run_instead_of_feeding_the_penalty(self):
        data = small_manifest()
        data["arms"][1].pop("depends_on")  # two arms that do not wait on each other
        self.plan = small_plan(self.tmp, data)
        run, shell, records = self.run_with([
            ("mon.py stop?", Completed(0, "no stop request")),
            ("temp.py", Completed(0, "Tctl 61.5 C")),
            ("vk-smoke.ps1", Completed(255, "", "kex_exchange_identification: Connection closed")),
            ("pt-run.ps1", Completed(255, "", "banner exchange: Connection timed out")),
            ("gate.ps1", Completed(0, GATE_OK)),
        ], arms=[self.plan.by_id("smoke"), self.plan.by_id("demo"), self.plan.by_id("game")])
        self.assertEqual(records["smoke"].verdict, FAIL)
        self.assertEqual(records["demo"].verdict, FAIL)
        self.assertIn("BD-051", run.halted)
        self.assertEqual(records["game"].verdict, SKIPPED, "the run stopped before the game arm")

    def test_the_raw_logs_are_written_for_every_step(self):
        _, _, records = self.run_with([
            ("mon.py stop?", Completed(0, "no stop request")),
            ("temp.py", Completed(0, "Tctl 61.5 C")),
            ("vk-smoke.ps1", Completed(0, "smoke ok")),
            ("gate.ps1", Completed(0, GATE_OK)),
        ], arms=[self.plan.by_id("smoke")])
        record = records["smoke"]
        self.assertTrue((self.tmp / "out" / "smoke" / "arm.txt").is_file())
        self.assertTrue((self.tmp / "out" / "smoke" / "gate.txt").is_file())
        self.assertTrue((self.tmp / "out" / "smoke" / "record.json").is_file())
        self.assertTrue(all(Path(path).is_file() for path in record.raw_paths if path))


class Restart(unittest.TestCase):
    """A planned restart is over when the boot time differs, not when port 22 answers."""

    def setUp(self):
        self.tmp = Path(self.enterContext(__import__("tempfile").TemporaryDirectory()))
        data = small_manifest()
        data["arms"] = [{"id": "restart-1", "title": "restart", "kind": "restart", "bound_s": 600,
                         "gate_after": True,
                         "run": [["python", "{target}", "ps", "{kit}/restart-now.ps1", "-Reason", "test"]]}]
        data["sets"]["standard"] = ["restart-1"]
        self.plan = small_plan(self.tmp, data)
        self.clock = FakeClock()

    def run_with(self, replies):
        shell = FakeShell(replies)
        run = Runner(self.plan, shell, self.tmp / "out", clock=self.clock, writer=lambda *a: None,
                     python="python")
        return run, shell, {record.id: record for record in run.run(self.plan.arms)}

    def test_the_wait_ends_on_a_new_boot_time(self):
        boots = iter([GATE_OK, GATE_OK, GATE_OK.replace("2026-10-10T07:47:40Z", "2026-10-10T09:00:00Z")])
        run, shell, records = self.run_with([
            ("mon.py stop?", Completed(0, "no stop request")),
            ("restart-now.ps1", Completed(0, "boot 2026-10-10T07:47:40Z\nrestart in 5 s: test")),
            ("gate.ps1", lambda step: Completed(0, next(boots, GATE_OK))),
        ])
        self.assertEqual(records["restart-1"].verdict, PASS)
        self.assertEqual(self.clock.slept[0], 30, "it waits before the first probe")
        self.assertTrue(all(wait >= 10 for wait in self.clock.slept))
        self.assertEqual(len(shell.said("gate.ps1")), 3, "it probed until the boot time changed")
        self.assertTrue((self.tmp / "out" / "restart-1" / "after-restart.txt").is_file())
        self.assertFalse(run.halted)

    def test_the_wait_holds_until_the_driver_reports_flags_15(self):
        # The arm's gate is judged, so an early probe must not fail it: the boot is new 35 s in, and the
        # driver needs about 80 s to report flags=15.
        texts = iter([GATE_OK.replace("2026-10-10T07:47:40Z", "2026-10-10T09:00:00Z").replace("flags=15",
                                                                                              "flags=7"),
                      GATE_OK.replace("2026-10-10T07:47:40Z", "2026-10-10T09:00:00Z")])
        run, shell, records = self.run_with([
            ("mon.py stop?", Completed(0, "no stop request")),
            ("restart-now.ps1", Completed(0, "boot 2026-10-10T07:47:40Z\nrestart in 5 s: test")),
            ("gate.ps1", lambda step: Completed(0, next(texts, GATE_OK))),
        ])
        self.assertEqual(records["restart-1"].verdict, PASS)
        self.assertEqual(len(shell.said("gate.ps1")), 2)
        self.assertFalse(run.halted, "an early flags=7 is not a critical gate")

    def test_a_machine_that_never_comes_back_fails_the_arm(self):
        run, _, records = self.run_with([
            ("mon.py stop?", Completed(0, "no stop request")),
            ("restart-now.ps1", Completed(0, "boot 2026-10-10T07:47:40Z\nrestart in 5 s: test")),
            ("gate.ps1", Completed(255, "", "Connection refused")),
        ])
        self.assertEqual(records["restart-1"].verdict, FAIL)
        self.assertIn("no new boot time", records["restart-1"].reason)


class Promotion(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(self.enterContext(__import__("tempfile").TemporaryDirectory()))
        data = small_manifest()
        data["arms"].insert(2, {"id": "promote", "title": "promote", "kind": "promote", "bound_s": 170,
                                "attempt": "native-caps", "attempt_count": 2,
                                "promote": {"shell": "{pkg}/payload/d3d12/amdgpu_wddm_d3d12.dll",
                                            "engine": "{pkg}/payload/d3d12/amdgpu_wddm_vkd3d.dll",
                                            "icd": "{pkg}/payload/d3d12/amdgpu_wddm_radv.dll"}})
        data["sets"]["standard"].append("promote")
        self.plan = small_plan(self.tmp, data)
        caps = Path(self.plan.values["caps"])
        (caps / "attempts").mkdir(parents=True, exist_ok=True)
        self.caps = caps
        payload = Path(self.plan.values["pkg"]) / "payload" / "d3d12"
        payload.mkdir(parents=True, exist_ok=True)
        for name in promote.NAMES:
            (payload / name).write_bytes(name.encode())

    def accepted(self, same: bool) -> None:
        block = {name: manifest.sha256(Path(self.plan.values["pkg"]) / "payload" / "d3d12" / name)
                 for name in promote.NAMES} if same else {name: "00" for name in promote.NAMES}
        (self.caps / "lab-baseline.json").write_text(json.dumps({"d3d12": {"accepted": block}}),
                                                     encoding="utf-8")

    def test_the_plan_gives_the_promotion_two_numbers_and_the_game_the_next_one(self):
        arm = self.plan.by_id("promote")
        self.assertEqual(arm.arm["attempt_name"], "native-caps600")
        self.assertEqual(arm.arm["check_attempt_name"], "native-caps601")
        self.assertEqual(self.plan.by_id("game").arm["attempt_name"], "native-caps602")

    def test_the_next_free_attempt_number_is_used_when_none_is_asked_for(self):
        for name in ("native-caps520", "native-caps521"):
            (self.caps / "attempts" / name).mkdir()
        arm = dict(self.plan.by_id("promote").arm)
        arm.pop("attempt_name"), arm.pop("check_attempt_name")
        prepared = promote.prepare(self.plan, arm)
        self.assertEqual(prepared.attempt, "native-caps522")
        self.assertEqual(prepared.check_attempt, "native-caps523")

    def test_a_leftover_attempt_directory_is_moved_aside_and_not_deleted(self):
        leftover = self.caps / "attempts" / "native-caps600"
        (leftover / "package").mkdir(parents=True)
        (leftover / "package" / "config.json").write_text("{}", encoding="utf-8")
        prepared = promote.prepare(self.plan, self.plan.by_id("promote").arm)
        self.assertEqual(prepared.attempt, "native-caps600", "the asked number is kept")
        self.assertTrue(prepared.moved_aside, "the leftover must be moved aside")
        moved = Path(prepared.moved_aside[0])
        self.assertTrue((moved / "package" / "config.json").is_file(), "nothing is deleted")
        self.assertFalse(leftover.exists())

    def test_a_dry_run_moves_nothing(self):
        leftover = self.caps / "attempts" / "native-caps600"
        (leftover / "package").mkdir(parents=True)
        prepared = promote.prepare(self.plan, self.plan.by_id("promote").arm, clean=False)
        self.assertEqual(prepared.moved_aside, [])
        self.assertTrue(leftover.exists())
        self.assertIn("a run would move it aside", prepared.note)

    def test_an_attempt_directory_with_evidence_is_never_touched(self):
        attempt = self.caps / "attempts" / "native-caps600"
        (attempt / "pull").mkdir(parents=True)
        (attempt / "pull" / "result.json").write_text("{}", encoding="utf-8")
        prepared = promote.prepare(self.plan, self.plan.by_id("promote").arm)
        self.assertEqual(prepared.moved_aside, [])
        self.assertTrue((attempt / "pull" / "result.json").is_file())
        self.assertEqual(prepared.attempt, "native-caps601", "the step steps over the evidence")
        self.assertIn("holds lab evidence", prepared.note)

    def test_a_triplet_that_is_already_accepted_skips_the_promotion_session(self):
        self.accepted(same=True)
        arm = self.plan.by_id("promote")
        prepared = promote.attach(self.plan, arm, "python", clean=False)
        self.assertTrue(prepared.already_accepted)
        text = " ".join(step.text() for step in arm.steps)
        self.assertNotIn("promote-d3d12.py stage ", text)
        self.assertIn("stage-check", text)
        self.assertNotIn("promoted-retained", arm.arm["expect_all"])

    def test_a_new_triplet_runs_the_whole_sequence_inside_the_bounds(self):
        self.accepted(same=False)
        arm = self.plan.by_id("promote")
        promote.attach(self.plan, arm, "python", clean=False)
        text = [step.text() for step in arm.steps]
        self.assertTrue(any("promote-d3d12.py stage " in line for line in text))
        self.assertTrue(any("run-slot.py" in line for line in text))
        self.assertTrue(any("accept --attempt" in line and "--apply" in line for line in text))
        self.assertTrue(any("release-baseline.py" in line and "--keep-accepted-d3d12" in line
                            for line in text))
        for step in arm.steps:
            if "run-slot.py" in step.text():
                self.assertLessEqual(step.timeout_s, 170 + self.plan.limits["host_grace_s"])
        self.assertIn("promoted-retained", arm.arm["expect_all"])


class Summary(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(self.enterContext(__import__("tempfile").TemporaryDirectory()))
        self.plan = small_plan(self.tmp)
        self.clock = FakeClock()

    def records(self, replies):
        shell = FakeShell(replies)
        run = Runner(self.plan, shell, self.tmp / "out", clock=self.clock, writer=lambda *a: None,
                     python="python")
        return run.run(self.plan.arms)

    def test_a_clean_run_states_both_owner_gates_as_met(self):
        records = self.records([
            ("mon.py stop?", Completed(0, "no stop request")),
            ("temp.py", Completed(0, "Tctl 61.5 C")),
            ("vk-smoke.ps1", Completed(0, "smoke ok")),
            ("pt-run.ps1", Completed(0, "631 frames, 10.45 seconds: 60.40 fps")),
            ("run-game.sh", Completed(0, "status ok")),
            ("gate.ps1", Completed(0, GATE_OK)),
        ])
        text = summary_module.write(self.plan, records, self.tmp / "out", train="b99")
        self.assertIn("# b99 lab validation: 0.7.0-tester.1 on unit A", text)
        self.assertIn("**Quake II RTX with ray tracing**: MET", text)
        # The game arm ran well, and its score is still in a person's eyes: the owner's gate stays open
        # until that number is filled in. A met gate on an unread value is what blocked round one.
        self.assertIn("**Rise of the Tomb Raider**: NOT MET", text)
        self.assertIn("the operator still reads it from the screen", text)
        self.assertIn("--set game=<overall>", text)
        self.assertIn("| 1 | smoke (`smoke`) |", text)
        self.assertIn("60.4 fps", text)
        self.assertIn("60.0 fps (-10%)", text)

    def test_the_rottr_gate_is_met_once_the_operator_fills_the_score_in(self):
        records = self.records([
            ("mon.py stop?", Completed(0, "no stop request")),
            ("temp.py", Completed(0, "Tctl 61.5 C")),
            ("vk-smoke.ps1", Completed(0, "smoke ok")),
            ("pt-run.ps1", Completed(0, "631 frames, 10.45 seconds: 60.40 fps")),
            ("run-game.sh", Completed(0, "status ok")),
            ("gate.ps1", Completed(0, GATE_OK)),
        ])
        text = summary_module.write(self.plan, records, self.tmp / "out", train="b99",
                                    operator_values={"game": 56.4})
        self.assertIn("**Rise of the Tomb Raider**: MET", text)
        self.assertIn("56.4 fps", text)

    def test_a_number_given_on_the_command_line_for_a_parsed_arm_says_where_it_came_from(self):
        records = self.records([
            ("mon.py stop?", Completed(0, "no stop request")),
            ("temp.py", Completed(0, "Tctl 61.5 C")),
            ("vk-smoke.ps1", Completed(0, "smoke ok")),
            ("pt-run.ps1", Completed(0, "the demo said nothing this time")),
            ("run-game.sh", Completed(0, "status ok")),
            ("gate.ps1", Completed(0, GATE_OK)),
        ])
        self.assertEqual({r.id: r.verdict for r in records}["demo"], runner_module.UNREAD)
        text = summary_module.write(self.plan, records, self.tmp / "out", train="b99",
                                    operator_values={"demo": 60.1})
        self.assertIn("60.1 fps (given on the command line)", text)
        self.assertIn("60.1 fps", text)

    def test_a_reading_far_under_the_baseline_does_not_meet_the_gate(self):
        records = self.records([
            ("mon.py stop?", Completed(0, "no stop request")),
            ("temp.py", Completed(0, "Tctl 61.5 C")),
            ("vk-smoke.ps1", Completed(0, "smoke ok")),
            ("pt-run.ps1", Completed(0, "631 frames, 10.45 seconds: 60.40 fps")),
            ("run-game.sh", Completed(0, "status ok")),
            ("gate.ps1", Completed(0, GATE_OK)),
        ])
        text = summary_module.write(self.plan, records, self.tmp / "out", train="b99",
                                    operator_values={"game": 12.0})
        self.assertIn("**Rise of the Tomb Raider**: NOT MET", text)
        self.assertIn("12.0 is under 49.63", text)
        self.assertNotIn("| 12.0 fps | 55.15 fps (-10%) | PASS", text)

    def test_a_failed_gate_arm_is_not_met_and_its_symptom_shape_is_named(self):
        records = self.records([
            ("mon.py stop?", Completed(0, "no stop request")),
            ("temp.py", Completed(0, "Tctl 61.5 C")),
            ("vk-smoke.ps1", Completed(0, "smoke ok")),
            ("pt-run.ps1", Completed(1, "HARDWARE FENCE TIMEOUT on node 0\nno frame rate")),
            ("gate.ps1", Completed(0, GATE_OK)),
        ])
        text = summary_module.write(self.plan, records, self.tmp / "out", train="b99")
        self.assertIn("**Quake II RTX with ray tracing**: NOT MET", text)
        self.assertIn("symptom shape matches **BD-102**", text)
        self.assertIn("not a diagnosis", " ".join(text.split()))

    def test_an_operator_value_can_be_filled_in_later(self):
        records = self.records([
            ("mon.py stop?", Completed(0, "no stop request")),
            ("temp.py", Completed(0, "Tctl 61.5 C")),
            ("vk-smoke.ps1", Completed(0, "smoke ok")),
            ("pt-run.ps1", Completed(0, "631 frames, 10.45 seconds: 60.40 fps")),
            ("run-game.sh", Completed(0, "status ok")),
            ("gate.ps1", Completed(0, GATE_OK)),
        ])
        text = summary_module.write(self.plan, records, self.tmp / "out", train="b99",
                                    operator_values={"game": 55.9})
        self.assertIn("55.9 fps", text)


class OneCommand(unittest.TestCase):
    """`validate.py run` end to end against the fake target: the files it leaves and the exit code."""

    def setUp(self):
        import os
        import validate
        self.validate = validate
        self.tmp = Path(self.enterContext(__import__("tempfile").TemporaryDirectory()))
        self.enterContext(unittest.mock.patch.dict(os.environ, {"BC250_ROOT": str(self.tmp)}))
        self.manifest_path = self.tmp / "arms.json"
        self.manifest_path.write_text(json.dumps(small_manifest()), encoding="utf-8")
        self.package = fake_package(self.tmp)
        self.replies = [
            ("mon.py stop?", Completed(0, "no stop request")),
            ("temp.py", Completed(0, "Tctl 61.5 C")),
            ("vk-smoke.ps1", Completed(0, "smoke ok")),
            ("pt-run.ps1", Completed(0, "631 frames, 10.45 seconds: 60.40 fps")),
            ("run-game.sh", Completed(0, "status ok")),
            ("gate.ps1", Completed(0, GATE_OK)),
        ]
        self.shells = []

        def factory(cwd, base_env=None, writer=print):
            shell = FakeShell(self.replies)
            self.shells.append(shell)
            return shell

        self.enterContext(unittest.mock.patch.object(validate, "make_shell", factory))
        self.enterContext(unittest.mock.patch.object(validate, "make_clock", FakeClock))

    def test_the_one_command_writes_the_plan_the_records_and_the_results(self):
        out = self.tmp / "out"
        code = self.validate.main(["--manifest", str(self.manifest_path), "run",
                                   "--package", str(self.package.directory), "--out", str(out),
                                   "--train", "b99", "--attempt-base", "600", "--no-hash"])
        self.assertEqual(code, 0)
        self.assertTrue((out / "plan.txt").is_file())
        self.assertTrue((out / "records.json").is_file())
        results = (out / "RESULTS.md").read_text(encoding="utf-8")
        self.assertIn("# b99 lab validation", results)
        self.assertIn("MET", results)
        records = json.loads((out / "records.json").read_text(encoding="utf-8"))
        self.assertEqual([row["verdict"] for row in records], ["PASS", "PASS", "PASS"])
        self.assertTrue((Path(self.tmp) / "caps" / "plans" / "plan-600.md").is_file(),
                        "the game arm got its plan file")

    def test_the_dry_run_writes_nothing(self):
        out = self.tmp / "out-dry"
        code = self.validate.main(["--manifest", str(self.manifest_path), "run",
                                   "--package", str(self.package.directory), "--out", str(out),
                                   "--dry-run", "--attempt-base", "600", "--no-hash"])
        self.assertEqual(code, 0)
        self.assertFalse(out.exists())
        self.assertEqual(self.shells, [], "the dry run opened no shell")

    def test_a_failed_arm_makes_the_command_exit_non_zero(self):
        self.replies = [*self.replies[:2], ("vk-smoke.ps1", Completed(1, "nothing")),
                        ("gate.ps1", Completed(0, GATE_OK))]
        code = self.validate.main(["--manifest", str(self.manifest_path), "run",
                                   "--package", str(self.package.directory),
                                   "--out", str(self.tmp / "out-bad"), "--attempt-base", "600",
                                   "--no-hash"])
        self.assertEqual(code, 1)


class ShippedSetEndToEnd(unittest.TestCase):
    """The shipped arms.json, every arm of it, against the fake target."""

    def setUp(self):
        import os
        import validate
        self.validate = validate
        self.tmp = Path(self.enterContext(__import__("tempfile").TemporaryDirectory()))
        self.enterContext(unittest.mock.patch.dict(os.environ, {"BC250_ROOT": str(self.tmp)}))
        self.package = fake_package(self.tmp)
        payload = self.package.directory / "payload" / "d3d12"
        payload.mkdir(parents=True, exist_ok=True)
        for name in promote.NAMES:
            (payload / name).write_bytes(name.encode())
        self.booted = ["2026-10-10T07:47:40Z"]

        def gate(step):
            return Completed(0, GATE_OK.replace("2026-10-10T07:47:40Z", self.booted[-1]))

        def restart(step):
            self.booted.append(f"2026-10-10T0{len(self.booted) + 7}:00:00Z")
            return Completed(0, f"boot {self.booted[-2]}\nrestart in 5 s")

        replies = [
            ("mon.py", Completed(0, "no stop request")),
            ("temp.py", Completed(0, "Tctl 58.0 C")),
            ("target.py forget", Completed(0, "")),
            ("gate.ps1", gate),
            ("restart-now.ps1", restart),
            ("target.py run", Completed(0, "ready")),
            ("target.py push", Completed(0, "pushed 95244264 bytes")),
            ("zipcheck.ps1", Completed(0, "zip OK (matches the development PC)")),
            # The real step prints the inventory twice. The arm's number is the one after the uninstaller.
            ("clean-slate.ps1 -Step uninstall",
             Completed(0, "installed release 0.7.0-tester.1 (kmd_build 0.7.0.1), uninstaller abcdef12\n"
                          "--- before\ndriver store bc250kmd packages: 1 oem42.inf 0.7.0.1 10-09-2026\n"
                          "--- uninstall\nuninstall exit 0\n"
                          "--- after\ndriver store bc250kmd packages: 0 \n")),
            ("clean-slate.ps1 -Step install", Completed(0, "install exit 0")),
            ("slots.ps1", Completed(0, "slot check: 36 match, 0 differ, 1 absent (of 37 payload files)")),
            ("kmdver.ps1", Completed(0, "install root C:\\Program Files\\amdgpu-wddm")),
            ("preflight.ps1", Completed(0, "preflight: ok")),
            ("pin-baseline.py", Completed(0, "release 0.7.0-tester.1: 85 files verified")),
            ("release-baseline.py", Completed(0, "release 0.7.0-tester.1: 85 files verified")),
            ("vk-smoke.ps1", Completed(0, "driverInfo = Mesa 26.3.0-devel (git-18e0f56be7)")),
            ("x86-d3d11-smoke.ps1", Completed(0, "exit 0 after 12.0 s")),
            ("pt-run.ps1 -Demo q2rtx-timedemo -RtApi pipeline",
             Completed(0, "Using VK_KHR_ray_tracing_pipeline\n631 frames, 10.45 seconds: 60.40 fps")),
            ("pt-run.ps1 -Demo q2rtx-timedemo -RtApi query",
             Completed(0, "Using VK_KHR_ray_query\n631 frames, 10.37 seconds: 60.86 fps")),
            ("pt-run.ps1 -Demo q2rtx-loop", Completed(0, "end: reason bound, elapsed 163.5 s")),
            ("pt-run.ps1 -Demo dxrpt", Completed(0, "436 frames in 30.0 s, 19.13 fps, 126.9 Mrays/s")),
            ("mesacache.ps1", Completed(0, "cache C:\\Users\\bc250\\AppData\\Local\\mesa_shader_cache: 541 "
                                           "files, 2775706 bytes, newest 2026-10-10T07:30:37Z")),
            ("run-batch.ps1", Completed(0, "list-rt-k97 run=trainr route=direct icd=1D4DAD41 exit=1 "
                                           "complete=True done=140/140 attempts=1 elapsed=8.7s\n"
                                           "counts: Pass=137 NotRun=3")),
            ("llm-dense.ps1", Completed(0, "llm-dense result verdict PASS arm_exit 0 pp512 207.38 t/s "
                                           "tg128 32.94 t/s")),
            ("promote-d3d12.py stage ", Completed(0, "plan written")),
            ("promote-d3d12.py stage-check", Completed(0, "plan written")),
            ("run-slot.py", Completed(0, "Start Running")),
            ("close-attempt.py native-caps001", Completed(0, '"status": "promoted-retained"')),
            ("close-attempt.py", Completed(0, '"status": "functional-restored"')),
            ("promote-d3d12.py accept", Completed(0, "d3d12 block written")),
            ("run-game.sh", Completed(0, '"status": "ok"')),
        ]
        self.shell = FakeShell(replies, default=Completed(0, "ok"))
        self.enterContext(unittest.mock.patch.object(
            validate, "make_shell", lambda cwd, base_env=None, writer=print: self.shell))
        self.enterContext(unittest.mock.patch.object(validate, "make_clock", FakeClock))

    def test_every_arm_of_the_standard_set_passes_against_the_fake_target(self):
        out = self.tmp / "out"
        code = self.validate.main(["run", "--package", str(self.package.directory), "--out", str(out),
                                   "--train", "b99", "--attempt-base", "1"])
        records = json.loads((out / "records.json").read_text(encoding="utf-8"))
        bad = [(row["id"], row["verdict"], row["reason"]) for row in records
               if row["verdict"] not in ("PASS", "OPERATOR")]
        self.assertEqual(bad, [])
        self.assertEqual(code, 0)
        results = (out / "RESULTS.md").read_text(encoding="utf-8")
        self.assertIn("**Quake II RTX with ray tracing**: MET", results)
        # Rise of the Tomb Raider ran, and its score is read by a person: the gate waits for that number.
        self.assertIn("**Rise of the Tomb Raider**: NOT MET", results)
        self.assertIn("`w3-high-rt`", results)
        self.assertEqual(len(records), 25)
        self.assertEqual([row["value"] for row in records if row["id"] == "uninstall"], [0.0],
                         "the number comes from the inventory after the uninstaller, not before it")
        plans = Path(self.tmp) / "scratch" / "m15" / "native-caps001" / "plans"
        self.assertTrue((plans / "plan-003.md").is_file(), "the plan file of the rottr arm")
        self.assertTrue((plans / "plan-004.md").is_file(),
                        "the operator's own game arm got its plan file too: run-m157.sh refuses without it")

    def test_the_push_arm_hands_target_py_the_lab_directory_after_the_to_flag(self):
        out = self.tmp / "out-push"
        self.validate.main(["run", "--package", str(self.package.directory), "--out", str(out),
                            "--train", "b99", "--attempt-base", "1", "--arms", "push-zip,zipcheck"])
        push = self.shell.said("target.py push")
        self.assertEqual(len(push), 1)
        self.assertEqual(push[0][-2], "--to", "target.py push takes the destination after --to only")
        self.assertTrue(push[0][-1].startswith("C:\\BC250\\tmp\\train-"), push[0])
        self.assertTrue(push[0][-3].endswith(".zip"), push[0])
        want = next(call for call in self.shell.said("zipcheck.ps1"))
        self.assertEqual(len(want[want.index("-Want") + 1]), 64, "the lab compares the real SHA-256")

    def test_a_quake_arm_that_runs_and_says_nothing_leaves_the_owner_gate_not_met(self):
        # rc 0, the extension line printed, the run to its bound and no frame rate: the shape BD-102 leaves.
        # The shipped manifest used to report PASS here, and the owner's release gate read MET.
        self.shell.replies.insert(0, ("pt-run.ps1 -Demo q2rtx-timedemo -RtApi pipeline",
                                      Completed(0, "Using VK_KHR_ray_tracing_pipeline\n"
                                                   "end: reason bound, elapsed 170.4 s")))
        out = self.tmp / "out-q2"
        code = self.validate.main(["run", "--package", str(self.package.directory), "--out", str(out),
                                   "--train", "b99", "--attempt-base", "1", "--arms", "q2rtx-pipeline"])
        self.assertEqual(code, 1)
        records = json.loads((out / "records.json").read_text(encoding="utf-8"))
        self.assertEqual([row["verdict"] for row in records], ["UNREAD"])
        results = (out / "RESULTS.md").read_text(encoding="utf-8")
        self.assertIn("**Quake II RTX with ray tracing**: NOT MET", results)
        self.assertIn("No known failure class matches this shape", results)

    def test_a_benchmark_reading_far_under_the_baseline_leaves_the_rottr_gate_not_met(self):
        out = self.tmp / "out"
        self.validate.main(["run", "--package", str(self.package.directory), "--out", str(out),
                            "--train", "b99", "--attempt-base", "1"])
        self.validate.main(["summary", "--out", str(out), "--package", str(self.package.directory),
                            "--train", "b99", "--attempt-base", "1", "--set", "rottr=12.0"])
        results = (out / "RESULTS.md").read_text(encoding="utf-8")
        self.assertIn("**Rise of the Tomb Raider**: NOT MET", results)
        self.assertIn("12.0 is under 49.63", results)

    def test_the_whole_set_is_met_once_the_operator_fills_the_benchmark_score_in(self):
        out = self.tmp / "out"
        self.validate.main(["run", "--package", str(self.package.directory), "--out", str(out),
                            "--train", "b99", "--attempt-base", "1"])
        code = self.validate.main(["summary", "--out", str(out), "--package", str(self.package.directory),
                                   "--train", "b99", "--attempt-base", "1", "--set", "rottr=57.2"])
        self.assertEqual(code, 0)
        results = (out / "RESULTS.md").read_text(encoding="utf-8")
        self.assertIn("**Rise of the Tomb Raider**: MET", results)
        self.assertIn("57.2 fps", results)


class PlanText(unittest.TestCase):
    def test_the_dry_run_prints_every_command_and_its_bound(self):
        tmp = Path(self.enterContext(__import__("tempfile").TemporaryDirectory()))
        plan = small_plan(tmp)
        import validate
        text = validate.plan_text(plan, tmp / "out", "b99")
        self.assertIn("pt-run.ps1", text)
        self.assertIn("bound 170 s", text)
        self.assertIn("OWNER GATE rottr", text)
        self.assertIn("nothing above has run", text)
        self.assertIn("the operator's own session fits after the arms above", text)

    def test_the_shipped_manifest_builds_a_plan_for_a_package(self):
        tmp = Path(self.enterContext(__import__("tempfile").TemporaryDirectory()))
        plan = manifest.build(fake_package(tmp), manifest.load_manifest(), attempt_base=700, python="python")
        self.assertEqual([arm.id for arm in plan.arms][:3], ["push-zip", "zipcheck", "uninstall"])
        self.assertTrue(any(arm.arm.get("gate") == "q2rtx" for arm in plan.arms))

    def test_the_plan_states_the_worst_case_of_every_arm_and_not_only_its_bound(self):
        tmp = Path(self.enterContext(__import__("tempfile").TemporaryDirectory()))
        data = small_manifest()
        data["arms"].insert(2, {"id": "promote", "title": "promote", "kind": "promote", "bound_s": 170,
                                "attempt": "native-caps", "attempt_count": 2,
                                "promote": {"shell": "{pkg}/a", "engine": "{pkg}/b", "icd": "{pkg}/c"}})
        data["sets"]["standard"].append("promote")
        plan = small_plan(tmp, data)
        import validate
        arm = plan.by_id("promote")
        promote.attach(plan, arm, "python", clean=False)
        text = validate.plan_text(plan, tmp / "out", "b99")
        worst = sum(step.timeout_s for step in arm.steps)
        self.assertGreater(worst, 170 * 5, "nine steps of 215 s and 300 s are not a 170 s arm")
        self.assertIn(f"host worst case {worst} s", text)


class PackageAndPush(unittest.TestCase):
    """The first arm of the one command: the zip, its hash and the form target.py's push takes."""

    def setUp(self):
        self.tmp = Path(self.enterContext(__import__("tempfile").TemporaryDirectory()))
        self.data = manifest.load_manifest()

    def test_the_push_arm_names_the_destination_after_the_to_flag(self):
        argv = next(arm for arm in self.data["arms"] if arm["id"] == "push-zip")["run"][1]
        self.assertEqual(argv[-3:], ["{zip}", "--to", "{lab_root}"],
                         "without --to, target.py reads the lab path as a second local file")

    def test_a_package_with_no_zip_beside_it_is_refused_before_anything_runs(self):
        package = fake_package(self.tmp, with_zip=False)
        with self.assertRaises(ManifestError) as refused:
            manifest.build(package, self.data, attempt_base=700, python="python")
        self.assertIn("push-zip", str(refused.exception))

    def test_a_package_that_was_not_hashed_is_refused_for_the_arms_that_compare_the_hash(self):
        package = Package.read(fake_package(self.tmp).directory, hash_zip=False)
        with self.assertRaises(ManifestError) as refused:
            manifest.build(package, self.data, arm_ids=["zipcheck"], python="python")
        self.assertIn("--no-hash", str(refused.exception))

    def test_a_package_with_no_zip_still_builds_the_arms_that_do_not_install_it(self):
        package = fake_package(self.tmp, with_zip=False)
        plan = manifest.build(package, self.data, arm_ids=["vk-smoke", "x86-smoke"], python="python")
        self.assertEqual([arm.id for arm in plan.arms], ["vk-smoke", "x86-smoke"])


class DestructiveArm(unittest.TestCase):
    """The uninstall removes the display driver. It waits for the proof that the package is on the lab."""

    def setUp(self):
        self.tmp = Path(self.enterContext(__import__("tempfile").TemporaryDirectory()))
        self.data = manifest.load_manifest()
        self.clock = FakeClock()

    def test_the_manifest_makes_the_uninstall_wait_for_the_zip_check(self):
        arm = next(a for a in self.data["arms"] if a["id"] == "uninstall")
        self.assertEqual(arm["needs_verified"], "zipcheck")
        self.assertIn("zipcheck", arm["depends_on"])

    def test_an_uninstall_asked_for_on_its_own_does_not_run(self):
        plan = manifest.build(fake_package(self.tmp), self.data, arm_ids=["uninstall"], python="python")
        shell = FakeShell([("mon.py stop?", Completed(0, "no stop request")),
                           ("temp.py", Completed(0, "Tctl 58.0 C"))])
        run = Runner(plan, shell, self.tmp / "out", clock=self.clock, writer=lambda *a: None,
                     python="python")
        records = {r.id: r for r in run.run(plan.arms)}
        self.assertEqual(records["uninstall"].verdict, SKIPPED)
        self.assertIn("zipcheck", records["uninstall"].reason)
        self.assertEqual(shell.said("clean-slate.ps1"), [], "nothing was removed from the lab")

    def test_a_failed_push_leaves_the_driver_installed(self):
        ids = ["push-zip", "zipcheck", "uninstall", "restart-1", "install"]
        plan = manifest.build(fake_package(self.tmp), self.data, arm_ids=ids, python="python")
        shell = FakeShell([("mon.py stop?", Completed(0, "no stop request")),
                           ("temp.py", Completed(0, "Tctl 58.0 C")),
                           ("target.py run", Completed(0, "ready")),
                           ("target.py push", Completed(1, "", "push failed: no space left on device")),
                           ("gate.ps1", Completed(0, GATE_OK))])
        run = Runner(plan, shell, self.tmp / "out", clock=self.clock, writer=lambda *a: None,
                     python="python")
        records = {r.id: r for r in run.run(plan.arms)}
        self.assertEqual(records["push-zip"].verdict, FAIL)
        self.assertEqual(records["uninstall"].verdict, SKIPPED)
        self.assertEqual(records["install"].verdict, SKIPPED)
        self.assertEqual(shell.said("clean-slate.ps1"), [])
        self.assertEqual(shell.said("restart-now.ps1"), [], "and the machine was not restarted either")

    def test_a_resumed_run_keeps_the_proof_of_an_earlier_session(self):
        ids = ["uninstall"]
        plan = manifest.build(fake_package(self.tmp), self.data, arm_ids=ids, python="python")
        shell = FakeShell([("mon.py stop?", Completed(0, "no stop request")),
                           ("temp.py", Completed(0, "Tctl 58.0 C")),
                           ("clean-slate.ps1", Completed(0, "--- after\ndriver store bc250kmd packages: 0\n"
                                                            "uninstall exit 0")),
                           ("gate.ps1", Completed(0, GATE_OK))])
        run = Runner(plan, shell, self.tmp / "out", clock=self.clock, writer=lambda *a: None,
                     python="python", already_good={"zipcheck"})
        records = {r.id: r for r in run.run(plan.arms)}
        self.assertEqual(records["uninstall"].verdict, PASS)


class ValueJudgement(unittest.TestCase):
    """A baselined arm that produced no number is UNREAD, and an UNREAD gate arm is not met."""

    def setUp(self):
        self.tmp = Path(self.enterContext(__import__("tempfile").TemporaryDirectory()))
        self.plan = small_plan(self.tmp)
        self.clock = FakeClock()

    def records(self, demo_reply):
        shell = FakeShell([
            ("mon.py stop?", Completed(0, "no stop request")),
            ("temp.py", Completed(0, "Tctl 61.5 C")),
            ("vk-smoke.ps1", Completed(0, "smoke ok")),
            ("pt-run.ps1", demo_reply),
            ("run-game.sh", Completed(0, "status ok")),
            ("gate.ps1", Completed(0, GATE_OK)),
        ])
        run = Runner(self.plan, shell, self.tmp / "out", clock=self.clock, writer=lambda *a: None,
                     python="python")
        return run.run(self.plan.arms)

    def test_an_arm_that_ran_well_and_said_no_number_is_unread(self):
        # The shape of BD-102 on the q2rtx arms: the extension line is there, the run ends on its bound and
        # the frame rate line is absent. rc 0 and PASS would have called the owner's gate met.
        records = {r.id: r for r in self.records(Completed(0, "Using VK_KHR_ray_tracing_pipeline\n"
                                                              "end: reason bound, elapsed 170.2 s"))}
        self.assertEqual(records["demo"].verdict, runner_module.UNREAD)
        self.assertIsNone(records["demo"].value)
        self.assertFalse(records["demo"].value_read)
        self.assertIs(records["demo"].baseline_ok, False)
        self.assertIn("no demo was read", records["demo"].reason)

    def test_an_unread_arm_leaves_its_owner_gate_not_met_and_gets_its_symptom_shape(self):
        records = self.records(Completed(0, "Using VK_KHR_ray_tracing_pipeline\n"
                                            "HARDWARE FENCE TIMEOUT on node 0\nend: reason bound"))
        text = summary_module.write(self.plan, records, self.tmp / "out", train="b99")
        self.assertIn("**Quake II RTX with ray tracing**: NOT MET", text)
        self.assertIn("symptom shape matches **BD-102**", text)
        self.assertIn("**unread**", text)

    def test_an_unread_arm_holds_back_the_arms_that_depend_on_it(self):
        records = {r.id: r for r in self.records(Completed(0, "nothing to say"))}
        self.assertEqual(records["demo"].verdict, runner_module.UNREAD)
        self.assertEqual(records["game"].verdict, SKIPPED)

    def test_a_value_that_is_not_a_number_is_not_compared(self):
        records = {r.id: r for r in self.records(Completed(0, "a lot of fps"))}
        self.assertEqual(records["demo"].verdict, runner_module.UNREAD)

    def test_the_last_match_is_the_arms_value_and_a_section_marker_cuts_the_text(self):
        data = manifest.load_manifest()
        plan = manifest.build(fake_package(self.tmp), data, arm_ids=["uninstall"], python="python")
        shell = FakeShell([
            ("mon.py stop?", Completed(0, "no stop request")),
            ("temp.py", Completed(0, "Tctl 61.5 C")),
            ("clean-slate.ps1", Completed(0, "--- before\ndriver store bc250kmd packages: 1 oem42.inf\n"
                                             "--- uninstall\nuninstall exit 0\n"
                                             "--- after\ndriver store bc250kmd packages: 0 \n")),
            ("gate.ps1", Completed(0, GATE_OK)),
        ])
        run = Runner(plan, shell, self.tmp / "out", clock=self.clock, writer=lambda *a: None,
                     python="python", already_good={"zipcheck"})
        record = run.run(plan.arms)[0]
        self.assertEqual(record.value, 0.0, "the inventory before the uninstaller is not the arm's answer")
        self.assertEqual(record.verdict, PASS)

    def test_a_dirty_uninstall_is_a_warning_against_its_baseline(self):
        data = manifest.load_manifest()
        plan = manifest.build(fake_package(self.tmp), data, arm_ids=["uninstall"], python="python")
        shell = FakeShell([
            ("mon.py stop?", Completed(0, "no stop request")),
            ("temp.py", Completed(0, "Tctl 61.5 C")),
            ("clean-slate.ps1", Completed(0, "--- before\ndriver store bc250kmd packages: 1 oem42.inf\n"
                                             "--- uninstall\nuninstall exit 0\n"
                                             "--- after\ndriver store bc250kmd packages: 1 oem42.inf\n")),
            ("gate.ps1", Completed(0, GATE_OK)),
        ])
        run = Runner(plan, shell, self.tmp / "out", clock=self.clock, writer=lambda *a: None,
                     python="python", already_good={"zipcheck"})
        record = run.run(plan.arms)[0]
        self.assertEqual(record.value, 1.0)
        self.assertEqual(record.verdict, WARN)
        self.assertIs(record.baseline_ok, False)


class Budgets(unittest.TestCase):
    """A slow or stalling lab cannot hold the runner past the budget of a gate between two arms."""

    def setUp(self):
        self.tmp = Path(self.enterContext(__import__("tempfile").TemporaryDirectory()))
        self.plan = small_plan(self.tmp)
        self.clock = FakeClock()

    def runner_with(self, replies, cost=0.0, plan=None):
        shell = FakeShell(replies, clock=self.clock, cost=cost)
        return Runner(plan or self.plan, shell, self.tmp / "out", clock=self.clock,
                      writer=lambda *a: None, python="python"), shell

    def test_a_cool_down_whose_reads_take_ninety_seconds_still_ends_on_its_budget(self):
        # The lab answers every temperature read after 90 s and reports 85 C. The budget is 60 s here (420 s
        # in the shipped manifest): the gate must end on it, and not make twenty reads of the same lab.
        run, shell = self.runner_with([("temp.py", Completed(0, "Tctl 85.0 C"))], cost=90)
        ok, why = run.cool_down()
        self.assertFalse(ok)
        self.assertIn("cool-down budget ran out", why)
        self.assertEqual(len(shell.said("temp.py")), 1)
        self.assertLess(self.clock.t, 60 + 90 + 30, f"the gate took {self.clock.t} s of wall time")

    def test_a_temperature_that_never_parses_does_not_read_the_lab_nine_times(self):
        run, shell = self.runner_with([("temp.py", Completed(0, "the driver does not answer"))], cost=90)
        ok, why = run.cool_down()
        self.assertFalse(ok)
        self.assertIn("unreadable", why)
        self.assertLessEqual(len(shell.said("temp.py")), 2, "three 90 s reads do not fit a 60 s budget")

    def test_a_restart_wait_ends_on_the_arms_bound_although_every_probe_is_slow(self):
        data = small_manifest()
        data["arms"] = [{"id": "restart-1", "title": "restart", "kind": "restart", "bound_s": 600,
                         "run": [["python", "{target}", "ps", "{kit}/restart-now.ps1", "-Reason", "t"]]}]
        data["sets"]["standard"] = ["restart-1"]
        plan = small_plan(self.tmp, data)
        run, shell = self.runner_with([("restart-now.ps1", Completed(0, "boot 2026-10-10T07:47:40Z")),
                                       ("gate.ps1", Completed(255, "", "Connection timed out"))],
                                      cost=120, plan=plan)
        came_back, _, why = run.wait_for_boot(plan.by_id("restart-1"), "boot 2026-10-10T07:47:40Z")
        self.assertFalse(came_back)
        self.assertIn("no new boot time", why)
        self.assertLess(self.clock.t, 600 + 180, f"the wait took {self.clock.t} s of wall time")
        self.assertFalse(run.halted, "a refused connection while the machine reboots is not BD-051")

    def test_two_refusals_in_the_gates_between_the_arms_halt_the_run(self):
        # The arms themselves would answer well. The refusals are in the calls between them, which is where
        # the accept stall of 220 and 221 bit, and they were not counted at all before.
        data = small_manifest()
        data["arms"][1].pop("depends_on")
        data["arms"][2].pop("depends_on")
        self.plan = small_plan(self.tmp, data)
        run, shell = self.runner_with([
            ("mon.py stop?", Completed(0, "no stop request")),
            ("temp.py", Completed(255, "", "kex_exchange_identification: Connection closed by remote host")),
            ("vk-smoke.ps1", Completed(0, "smoke ok")),
            ("gate.ps1", Completed(0, GATE_OK)),
        ])
        records = {r.id: r for r in run.run(self.plan.arms)}
        self.assertIn("BD-051", run.halted)
        self.assertEqual(records["smoke"].verdict, SKIPPED)
        self.assertEqual(records["game"].verdict, SKIPPED)
        self.assertIn("halted", records["game"].reason)
        self.assertEqual(shell.said("vk-smoke.ps1"), [], "no GPU arm started after the second refusal")
        self.assertLessEqual(len(shell.said("temp.py")), 2, "the run stopped feeding the penalty")


class StopFlagReading(unittest.TestCase):
    """`mon.py stop?` answers with one of two lines. Anything else is 'I could not ask'."""

    def setUp(self):
        self.tmp = Path(self.enterContext(__import__("tempfile").TemporaryDirectory()))
        self.plan = small_plan(self.tmp)
        self.clock = FakeClock()

    def run_with(self, mon_reply):
        shell = FakeShell([("mon.py stop?", mon_reply),
                           ("temp.py", Completed(0, "Tctl 61.5 C")),
                           ("vk-smoke.ps1", Completed(0, "smoke ok")),
                           ("gate.ps1", Completed(0, GATE_OK))])
        run = Runner(self.plan, shell, self.tmp / "out", clock=self.clock, writer=lambda *a: None,
                     python="python")
        records = {r.id: r for r in run.run([self.plan.by_id("smoke")])}
        return run, shell, records

    def test_the_printed_line_is_what_lets_an_arm_run(self):
        run, shell, records = self.run_with(Completed(0, "no stop request"))
        self.assertEqual(records["smoke"].verdict, PASS)
        self.assertFalse(run.halted)

    def test_an_answer_without_the_flag_halts_the_run_instead_of_running_the_arm(self):
        # The overlay API answered and the flags payload had no stop key: mon.py then printed nothing and
        # exited 0. Reading that as 'no stop request' ran every arm with the owner's flag unread.
        run, shell, records = self.run_with(Completed(0, ""))
        self.assertEqual(records["smoke"].verdict, SKIPPED)
        self.assertIn("could not be read", run.halted)
        self.assertEqual(shell.said("vk-smoke.ps1"), [])

    def test_an_error_of_the_monitor_is_not_reported_as_the_owners_stop(self):
        run, _, _ = self.run_with(Completed(2, "", "TargetError: no answer from the monitor"))
        self.assertIn("could not be read", run.halted)
        self.assertNotIn("is set", run.halted)

    def test_a_set_flag_says_so(self):
        run, shell, records = self.run_with(Completed(1, "STOP requested"))
        self.assertIn("STOP flag is set", run.halted)
        self.assertEqual(shell.said("vk-smoke.ps1"), [])


class StepsAndCleanup(unittest.TestCase):
    """A pre step that failed, an optional step that failed, and the client of a failed arm."""

    def setUp(self):
        self.tmp = Path(self.enterContext(__import__("tempfile").TemporaryDirectory()))
        self.clock = FakeClock()
        self.data = small_manifest()
        self.data["arms"][0]["pre"] = [["python", "{target}", "ps", "{caps}/lab/vsync.ps1", "-Value", "0"]]
        self.data["arms"][0]["post"] = [{"argv": ["python", "{mon}", "action", "overlay.show"],
                                         "optional": True}]
        self.data["arms"][0]["kill"] = ["q2rtx"]
        self.plan = small_plan(self.tmp, self.data)

    def run_with(self, replies):
        shell = FakeShell([("mon.py stop?", Completed(0, "no stop request")),
                           ("temp.py", Completed(0, "Tctl 61.5 C")),
                           ("gate.ps1", Completed(0, GATE_OK)), *replies])
        run = Runner(self.plan, shell, self.tmp / "out", clock=self.clock, writer=lambda *a: None,
                     python="python")
        records = {r.id: r for r in run.run([self.plan.by_id("smoke")])}
        return run, shell, records

    def test_a_failed_pre_step_fails_the_arm_and_the_run_command_never_starts(self):
        run, shell, records = self.run_with([("vsync.ps1", Completed(2, "cannot write the profile")),
                                             ("vk-smoke.ps1", Completed(0, "smoke ok"))])
        self.assertEqual(records["smoke"].verdict, FAIL)
        self.assertIn("pre step", records["smoke"].reason)
        self.assertEqual(shell.said("vk-smoke.ps1"), [], "the arm's own run did not start")

    def test_an_optional_step_that_fails_is_recorded_and_fails_nothing(self):
        run, shell, records = self.run_with([("vsync.ps1", Completed(0, "ok")),
                                             ("vk-smoke.ps1", Completed(0, "smoke ok")),
                                             ("mon.py action", Completed(1, "", "no answer from the overlay"))])
        self.assertEqual(records["smoke"].verdict, PASS)
        self.assertIn("optional post step", records["smoke"].reason)

    def test_a_failed_restore_step_is_a_warning_on_a_good_arm(self):
        self.data["arms"][0]["post"] = [["python", "{target}", "ps", "{caps}/lab/vsync.ps1", "-Value", "1"]]
        self.plan = small_plan(self.tmp, self.data)
        run, shell, records = self.run_with([("vsync.ps1 -Value 0", Completed(0, "ok")),
                                             ("vk-smoke.ps1", Completed(0, "smoke ok")),
                                             ("vsync.ps1 -Value 1", Completed(5, "", "the game holds it"))])
        self.assertEqual(records["smoke"].verdict, WARN)
        self.assertIn("restore step", records["smoke"].reason)

    def test_the_client_of_a_bound_arm_is_ended_on_the_lab_before_the_next_arm(self):
        run, shell, records = self.run_with([("vsync.ps1", Completed(0, "ok")),
                                             ("vk-smoke.ps1", Completed(124, "", "no end", timed_out=True)),
                                             ("kill-clients.ps1", Completed(0, "ended process q2rtx\n"
                                                                               "kill-clients end"))])
        self.assertEqual(records["smoke"].verdict, BOUND)
        killed = shell.said("kill-clients.ps1")
        self.assertEqual(len(killed), 1)
        self.assertIn("q2rtx", killed[0])
        self.assertTrue((self.tmp / "out" / "smoke" / "kill-clients.txt").is_file())

    def test_a_good_arm_kills_nothing(self):
        run, shell, records = self.run_with([("vsync.ps1", Completed(0, "ok")),
                                             ("vk-smoke.ps1", Completed(0, "smoke ok"))])
        self.assertEqual(records["smoke"].verdict, PASS)
        self.assertEqual(shell.said("kill-clients.ps1"), [])


class GateTextReachesTheMatcher(unittest.TestCase):
    """The failure class of a bugcheck is matched on the gate's own words, as gate.ps1 prints them."""

    def setUp(self):
        self.tmp = Path(self.enterContext(__import__("tempfile").TemporaryDirectory()))
        self.clock = FakeClock()
        data = small_manifest()
        data["failure_classes"] = manifest.load_manifest()["failure_classes"]
        self.plan = small_plan(self.tmp, data)

    def test_bd114_is_named_when_the_gate_counts_a_bugcheck_record(self):
        shell = FakeShell([
            ("mon.py stop?", Completed(0, "no stop request")),
            ("temp.py", Completed(0, "Tctl 61.5 C")),
            ("vk-smoke.ps1", Completed(0, "smoke ok")),
            ("gate.ps1", Completed(0, GATE_OK.replace("bugchecks: 0", "bugchecks: 1"))),
        ])
        run = Runner(self.plan, shell, self.tmp / "out", clock=self.clock, writer=lambda *a: None,
                     python="python")
        records = run.run([self.plan.by_id("smoke")])
        text = summary_module.write(self.plan, records, self.tmp / "out", train="b99")
        self.assertIn("symptom shape matches **BD-114**", text)
        self.assertIn("bugchecks", text)


class AttemptNumbers(unittest.TestCase):
    """Two arms must never write into one attempt directory."""

    def setUp(self):
        self.tmp = Path(self.enterContext(__import__("tempfile").TemporaryDirectory()))
        data = small_manifest()
        data["arms"].insert(2, {"id": "promote", "title": "promote", "kind": "promote", "bound_s": 170,
                                "attempt": "native-caps", "attempt_count": 2,
                                "promote": {"shell": "{pkg}/payload/d3d12/amdgpu_wddm_d3d12.dll",
                                            "engine": "{pkg}/payload/d3d12/amdgpu_wddm_vkd3d.dll",
                                            "icd": "{pkg}/payload/d3d12/amdgpu_wddm_radv.dll"}})
        data["sets"]["standard"].append("promote")
        self.plan = small_plan(self.tmp, data)
        self.caps = Path(self.plan.values["caps"])
        (self.caps / "attempts").mkdir(parents=True, exist_ok=True)
        payload = Path(self.plan.values["pkg"]) / "payload" / "d3d12"
        payload.mkdir(parents=True, exist_ok=True)
        for name in promote.NAMES:
            (payload / name).write_bytes(name.encode())

    def test_the_game_arm_moves_up_when_the_promotion_has_to_step_over_evidence(self):
        import validate
        for name in ("native-caps600", "native-caps601"):
            (self.caps / "attempts" / name / "pull").mkdir(parents=True)
            (self.caps / "attempts" / name / "pull" / "result.json").write_text("{}", encoding="utf-8")
        game = self.plan.by_id("game")
        self.assertEqual(game.arm["attempt_name"], "native-caps602")
        prepared = promote.attach(self.plan, self.plan.by_id("promote"), "python", clean=False)
        self.assertEqual([prepared.attempt, prepared.check_attempt],
                         ["native-caps602", "native-caps603"], "the promotion stepped over the evidence")
        moved = validate.reserve_game_attempts(self.plan, "python", writer=lambda *a: None)
        self.assertEqual(game.arm["attempt_name"], "native-caps604")
        self.assertIn("run-game.sh 604 ", " ".join(step.text() for step in game.steps),
                      "the number is inside the command line, so the steps are built again")
        self.assertEqual(moved, ["game: native-caps602 -> native-caps604"])

    def test_nothing_moves_when_the_promotion_keeps_the_pair_it_was_handed(self):
        import validate
        promote.attach(self.plan, self.plan.by_id("promote"), "python", clean=False)
        self.assertEqual(validate.reserve_game_attempts(self.plan, "python", writer=lambda *a: None), [])
        self.assertEqual(self.plan.by_id("game").arm["attempt_name"], "native-caps602")


if __name__ == "__main__":
    unittest.main()
