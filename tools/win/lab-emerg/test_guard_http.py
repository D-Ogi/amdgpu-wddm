"""Host tests of the stop rule in scripts/guard-http.sh (BD-376).

The guard reads its levels from tools/win/lab-thermal. The test copies the guard, the levels and their reader into a
temporary tree with a stand-in lab-emerg.py that answers each reading from a list and records a kill-game. A stand-in
sleep waits STEP seconds in place of 30, so a run of readings takes seconds. No lab and no network. Run:
    python -m unittest discover -s tools/win/lab-emerg -p "test_guard_http.py"
"""
import os
import shutil
import sys
import tempfile
import unittest
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
THERMAL = HERE.parent / 'lab-thermal'
sys.path.insert(0, str(THERMAL))
import thermal_levels  # noqa: E402

# Git's bash, not the WSL launcher that C:\Windows\System32\bash.exe can be.
GIT_BASH = Path(os.environ.get('ProgramFiles', r'C:\Program Files')) / 'Git/bin/bash.exe'
BASH = GIT_BASH if GIT_BASH.is_file() else None
LEVELS = thermal_levels.load()
# The readings come this far apart: the hold of the rule is longer than one step and not longer than two.
STEP = int(LEVELS.stop_held_s // 2) + 1

FAKE_EMERG = r'''import os, sys
from pathlib import Path
here = Path(__file__).resolve().parent
if sys.argv[1] == 'kill-game':
    (here / 'killed.txt').write_text('killed')
    print('kill-game sent')
    raise SystemExit(0)
count = here / 'count.txt'
n = int(count.read_text()) if count.exists() else 0
count.write_text(str(n + 1))
temps = os.environ['FAKE_TEMPS'].split(',')
t = temps[min(n, len(temps) - 1)]
print('1500 MHz 900 mV %s C busy 50.0%%' % t if t else 'no answer')
'''


@unittest.skipUnless(BASH, 'needs bash')
class GuardHttp(unittest.TestCase):
    def run_guard(self, temps, seconds):
        base = os.environ.get('BC250_TEST_TMP') or None
        with tempfile.TemporaryDirectory(dir=base) as td:
            root = Path(td)
            (root / 'lab-emerg/scripts').mkdir(parents=True)
            shutil.copy(HERE / 'scripts/guard-http.sh', root / 'lab-emerg/scripts/guard-http.sh')
            (root / 'lab-emerg/lab-emerg.py').write_text(FAKE_EMERG, encoding='utf-8')
            (root / 'lab-thermal').mkdir()
            for name in ('thermal_levels.py', 'thermal-levels.json'):
                shutil.copy(THERMAL / name, root / 'lab-thermal' / name)
            (root / 'bin').mkdir()
            (root / 'bin/sleep').write_bytes(b'#!/bin/sh\nexec /usr/bin/sleep %d\n' % STEP)
            env = dict(os.environ, FAKE_TEMPS=','.join(temps))
            env.pop('GAME_PROCESSES', None)
            # A PATH entry in the Windows form ("C:/...") splits at its colon: give bash its own form.
            script = 'export PATH="$(cygpath -u "$1")/bin:$PATH"; bash "$1/lab-emerg/scripts/guard-http.sh" %d "$1/out.txt"' % seconds
            done = subprocess.run([str(BASH), '-c', script, 'guard', root.as_posix()], env=env,
                                  capture_output=True, text=True, timeout=seconds + 60)
            log = (root / 'out.txt').read_text(encoding='utf-8') if (root / 'out.txt').exists() else ''
            return log + done.stdout + done.stderr, (root / 'lab-emerg/killed.txt').exists()

    def test_a_reading_at_the_once_level_stops_at_once(self):
        out, killed = self.run_guard([str(LEVELS.stop_at_once_c + 0.5)], 20)
        self.assertIn('STOP Tctl %s C at or above' % (LEVELS.stop_at_once_c + 0.5), out)
        self.assertTrue(killed, out)

    def test_a_hot_run_stops_when_it_has_lasted_the_hold(self):
        hot = str(LEVELS.stop_held_c + 0.5)
        out, killed = self.run_guard([hot, hot, hot, hot], 4 * STEP)
        self.assertIn('STOP Tctl at or above %s C, held' % thermal_levels.number(LEVELS.stop_held_c), out)
        self.assertTrue(killed, out)

    def test_a_cool_reading_ends_the_hot_run_and_the_driver_throttle_alone_never_stops(self):
        hot, cool = str(LEVELS.stop_held_c + 0.5), str(LEVELS.stop_held_c - 0.5)
        out, killed = self.run_guard([hot, cool, hot, str(LEVELS.driver_throttle_c + 0.5)], 3 * STEP + 2)
        self.assertNotIn('STOP', out)
        self.assertFalse(killed, out)

    def test_an_unreadable_reading_does_not_end_the_hot_run(self):
        hot = str(LEVELS.stop_held_c + 0.5)
        out, killed = self.run_guard([hot, '', hot, hot], 4 * STEP)
        self.assertIn('STOP Tctl at or above', out)
        self.assertTrue(killed, out)


if __name__ == '__main__':
    unittest.main()
