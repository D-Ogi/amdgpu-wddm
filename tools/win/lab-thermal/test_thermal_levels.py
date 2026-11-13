"""Host tests of the lab thermal levels: the definition, its readers, and the gate against a second copy.

    python -m unittest discover -s tools/win/lab-thermal

Nothing here reaches the lab. The gate reads every script and data file under tools/ and fails on a line that
compares a temperature with a number of its own, or gives a temperature parameter a number of its own.
"""
from __future__ import annotations

import json
import re
import subprocess
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOLS = HERE.parents[1]
REPO = TOOLS.parent
sys.path.insert(0, str(HERE))
import thermal_levels  # noqa: E402

SCHEMA_LINE = "'bc250.lab-thermal-levels.v1'"
# Every runner and tool that refuses a start or stops a run on temperature, and how it reads the definition.
CONSUMERS = {
    "win/lab-runner/game-runtime.ps1": ("thermal-levels.json", SCHEMA_LINE),
    "win/gui-trials/gui-trial-common.ps1": ("thermal-levels.json", SCHEMA_LINE),
    "win/gui-trials/run-trial.py": ("lab-thermal/thermal-levels.json",),
    "win/cts/run-batch.ps1": ("thermal-levels.json", SCHEMA_LINE),
    "win/frameloop/lab/frameloop-task.ps1": ("thermal-levels.json", SCHEMA_LINE),
    "win/train-validate/lab/preflight.ps1": ("thermal-levels.json", SCHEMA_LINE),
    "win/train-validate/manifest.py": ("import thermal_levels",),
    "win/train-validate/runner.py": ("thermal_levels",),
    "win/kmd-deploy/stage.py": ("lab-thermal/thermal-levels.json",),
    "win/kmd-deploy/template/kmd-transition/preflight.ps1": ("thermal-levels.json", SCHEMA_LINE),
    "win/kmd-deploy/template/kmd-transition/postflight.ps1": ("thermal-levels.json", SCHEMA_LINE),
    "win/lab-emerg/scripts/guard-http.sh": ("lab-thermal/thermal_levels.py",),
    "win/target.py": ('"lab-thermal" / "thermal-levels.json"',),
}

SCANNED = {".ps1", ".psm1", ".py", ".sh", ".json", ".cmd"}
# Tests hold sample readings, not levels; the definition and this directory hold the levels.
NOT_A_TOOL = re.compile(r"^(test_.*\.py|test-.*\.ps1|validate-.*\.ps1|host-checks\.ps1|.*-check\.ps1|"
                        r"run-tests\.sh|run-host-ref\.sh)$")
# A temperature word, also inside an identifier ($tempNow, TempCapC, temp_cap_c), but not ATTEMPTS or $env:TEMP.
TEMPERATURE = re.compile(r"[Tt]ctl|TCTL|[Tt]hermal|[Tt]emperat|\b[Tt]emp|[a-z]Temp|_temp|\b[Hh]ot|[Hh]ot[A-Z]|"
                         r"\b[Cc]ool")
# A number from 70 to 109 after a comparison, an assignment, a JSON key or an opening brace (a PowerShell fallback
# such as "else { 87.0 }"): a level of its own.
LEVEL = re.compile(r"(?:-ge|-gt|-le|-lt|>=|<=|>|<|=|:|\{)\s*\(?\s*(?:7|8|9|10)\d(?:\.\d+)?\b(?!\s*(?:%|mhz|mv|ms|s\b))",
                   re.I)
COMMENT = {".ps1": "#", ".psm1": "#", ".py": "#", ".sh": "#", ".cmd": "::"}
# Lines that hold such a number for another quantity, each with its reason. Keep this list short.
NOT_A_LEVEL = {
}


def code_part(line: str, suffix: str) -> str:
    marker = COMMENT.get(suffix)
    if marker and marker in line:
        before = line.split(marker, 1)[0]
        # A '#' inside a quoted string is not a comment; keep the line when the quotes are open.
        if before.count("'") % 2 == 0 and before.count('"') % 2 == 0:
            return before
    return line


def second_copies() -> list[str]:
    found = []
    for path in sorted(TOOLS.rglob("*")):
        if not path.is_file() or path.suffix.lower() not in SCANNED or HERE in path.parents:
            continue
        if NOT_A_TOOL.match(path.name) or {"test", "tests"} & {part.lower() for part in path.parent.parts}:
            continue
        rel = path.relative_to(TOOLS).as_posix()
        try:
            lines = path.read_text(encoding="utf-8-sig").splitlines()
        except (UnicodeDecodeError, OSError):
            continue
        for number, line in enumerate(lines, 1):
            code = code_part(line, path.suffix.lower())
            if TEMPERATURE.search(code) and LEVEL.search(code) and (rel, number) not in NOT_A_LEVEL:
                found.append(f"tools/{rel}:{number}: {line.strip()[:160]}")
    return found


class Definition(unittest.TestCase):
    def test_the_definition_loads_and_keeps_its_order(self):
        levels = thermal_levels.load()
        self.assertLessEqual(levels.measured_start_below_c, levels.start_below_c)
        self.assertLess(levels.start_below_c, levels.stop_held_c)
        self.assertLess(levels.stop_held_c, levels.stop_at_once_c)
        self.assertLessEqual(levels.driver_throttle_c, levels.stop_held_c,
                             "a run that the driver throttle holds must not stop")

    def test_a_definition_out_of_order_is_refused(self):
        data = json.loads(thermal_levels.PATH.read_text(encoding="utf-8"))
        for name, value in (("stop_at_once_c", data["stop_held_c"]), ("stop_held_s", 0),
                            ("start_below_c", data["stop_held_c"]), ("schema", "other"), ("stop_held_c", "92")):
            with self.subTest(field=name):
                with self.assertRaises(ValueError):
                    thermal_levels.check(dict(data, **{name: value}))

    def test_the_shell_form_has_one_lf_line_per_level(self):
        done = subprocess.run([sys.executable, str(HERE / "thermal_levels.py"), "--shell"], capture_output=True,
                              check=True)
        self.assertNotIn(b"\r", done.stdout, "a bash tool reads CR as part of the value")
        values = dict(line.split("=", 1) for line in done.stdout.decode("ascii").splitlines())
        levels = thermal_levels.load()
        self.assertEqual(float(values["STOP_HELD_C"]), levels.stop_held_c)
        self.assertEqual(float(values["STOP_AT_ONCE_C"]), levels.stop_at_once_c)
        self.assertEqual(float(values["STOP_HELD_S"]), levels.stop_held_s)

    def test_the_description_names_the_stop_rule(self):
        levels = thermal_levels.load()
        text = levels.describe()
        for value in (levels.stop_held_c, levels.stop_held_s, levels.stop_at_once_c):
            self.assertIn(thermal_levels.number(value), text)


class Consumers(unittest.TestCase):
    def test_every_consumer_reads_the_one_definition(self):
        for rel, needles in CONSUMERS.items():
            with self.subTest(consumer=rel):
                text = (TOOLS / rel).read_text(encoding="utf-8-sig")
                for needle in needles:
                    self.assertIn(needle, text)

    def test_no_tool_holds_a_temperature_level_of_its_own(self):
        found = second_copies()
        self.assertEqual(found, [], "a temperature level in a tool, not from tools/win/lab-thermal:\n" +
                         "\n".join(found))

    def test_the_gate_finds_a_second_copy(self):
        # The negative control: the scan rule finds each form that the tools held before BD-376.
        for line, suffix in (("  if($temp -ge 87){if(!$hotSince){$hotSince=$nowUtc}}", ".ps1"),
                             ("    [double]$ThermalStopC = 87,", ".ps1"),
                             ('            "run_worst_case_extra_seconds": 9, "temp_cap_c": 87.0}', ".py"),
                             ('    if [ -n "$t" ] && awk "BEGIN{exit !($t > 87)}"; then hot=1; fi', ".sh"),
                             ("if ([double]$Matches[1] -ge 85) { throw 'Temperature limit' }", ".ps1"),
                             ("$cap = if ($cfg.temp_cap_c) { [double]$cfg.temp_cap_c } else { 87.0 }", ".ps1"),
                             ('    "tctl_start_max_c": 80.0,', ".json")):
            with self.subTest(line=line):
                code = code_part(line, suffix)
                self.assertTrue(TEMPERATURE.search(code) and LEVEL.search(code))

    def test_a_reading_of_a_level_is_not_a_second_copy(self):
        for line in ("  if($temp -ge $thermal.stop_held_c){if(!$hotSince){$hotSince=$nowUtc}}",
                     "$cap = [double]$thermal.start_below_c",
                     "# Tctl 92 C held 10 s, or 97 C at once"):
            with self.subTest(line=line):
                code = code_part(line, ".ps1")
                self.assertFalse(TEMPERATURE.search(code) and LEVEL.search(code))


if __name__ == "__main__":
    unittest.main()
