"""Host tests for `target.py boot`: the boot wait that replaced grep loops over other commands' output.

No network: the probes are fakes and the clock is simulated. Run:
    python -m unittest discover -s tools/win -p "test_target*.py"
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import target  # noqa: E402

BOOT_OLD = 1_791_660_000   # a boot before the restart
BOOT_NEW = 1_791_670_000   # the boot the caller waits for


class FakeClock:
    def __init__(self):
        self.now = 0.0

    def clock(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


def script(*answers):
    """A probe that gives the answers in order and repeats the last one."""
    state = {"i": 0}

    def probe():
        answer = answers[min(state["i"], len(answers) - 1)]
        state["i"] += 1
        if isinstance(answer, Exception):
            raise answer
        return answer
    return probe


DOWN = (None, "rc 255 connection refused")


class BootWaitTest(unittest.TestCase):
    def run_wait(self, probes, want, bound=600, after=None, every=30):
        clock, lines = FakeClock(), []
        verdict = target.boot_wait(probes, want, bound, every=every, after=after, clock=clock.clock,
                                   sleep=clock.sleep, out=lambda text, flush=False: lines.append(text))
        return verdict, lines, clock.now

    def test_up_after_rounds_with_a_line_per_round(self):
        verdict, lines, _ = self.run_wait([("windows", script(DOWN, DOWN, (BOOT_NEW, "answers")))], "windows")
        self.assertEqual(verdict, ("up", "windows"))
        self.assertEqual(len([l for l in lines if l.startswith("boot-wait ")]), 3)
        self.assertTrue(lines[-1].startswith("BOOT UP windows"))

    def test_old_boot_is_not_accepted(self):
        probe = script((BOOT_OLD, "answers"), (BOOT_OLD, "answers"), (BOOT_NEW, "answers"))
        verdict, lines, _ = self.run_wait([("windows", probe)], "windows", after=BOOT_OLD + 60)
        self.assertEqual(verdict, ("up", "windows"))
        self.assertIn("old boot", lines[0])

    def test_linux_up_is_seen_by_any(self):
        # The miss of 2026-10-10: Linux was up and the wait did not notice.
        probes = [("windows", script(DOWN)), ("linux", script((BOOT_NEW, "answers")))]
        verdict, lines, _ = self.run_wait(probes, "linux")
        self.assertEqual(verdict, ("up", "linux"))
        self.assertTrue(lines[-1].startswith("BOOT UP linux"))

    def test_wrong_os_ends_the_wait(self):
        probes = [("windows", script(DOWN)), ("linux", script((BOOT_NEW, "answers")))]
        verdict, lines, now = self.run_wait(probes, "windows")
        self.assertEqual(verdict, ("wrong-os", "linux"))
        self.assertTrue(lines[-1].startswith("BOOT WRONG-OS linux"))
        self.assertLess(now, 600)

    def test_timeout_has_a_verdict_and_respects_the_bound(self):
        verdict, lines, now = self.run_wait([("windows", script(DOWN))], "windows", bound=300)
        self.assertEqual(verdict, ("timeout", None))
        self.assertTrue(lines[-1].startswith("BOOT TIMEOUT"))
        self.assertLessEqual(now, 300)

    def test_a_raising_probe_is_down_not_fatal(self):
        probe = script(RuntimeError("ssh vanished"), (BOOT_NEW, "answers"))
        verdict, lines, _ = self.run_wait([("windows", probe)], "windows")
        self.assertEqual(verdict, ("up", "windows"))
        self.assertIn("probe error RuntimeError", lines[0])

    def test_negative_control_a_wait_that_ignored_linux_would_time_out(self):
        # Same lab state as test_linux_up_is_seen_by_any, but only the Windows probe: no BOOT UP, a TIMEOUT.
        verdict, lines, _ = self.run_wait([("windows", script(DOWN))], "linux", bound=120)
        self.assertEqual(verdict, ("timeout", None))


class ParseTest(unittest.TestCase):
    def test_windows_boot(self):
        self.assertEqual(target.utc(target.windows_boot_epoch("noise\r\n2026-10-10T22:40:05\r\n")),
                         "2026-10-10T22:40:05Z")
        self.assertIsNone(target.windows_boot_epoch("Get-CimInstance : access denied"))

    def test_linux_boot(self):
        self.assertEqual(target.linux_boot_epoch("660.42\n1791671000\n"), 1791671000 - 660)
        self.assertIsNone(target.linux_boot_epoch("sh: date: not found"))


if __name__ == "__main__":
    unittest.main()
