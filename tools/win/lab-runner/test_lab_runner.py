"""Offline host tests of the lab game runner: the game profiles, and the two owner rules the runner carries.

    python -m unittest discover -s tools/win/lab-runner

Nothing here starts a process, reads a game file or reaches the lab. The profile loader of the trial kit lives in
the workspace scratch directory, so these tests read the profiles directly and check the fields the runner uses.
"""
import importlib.util
import json
import re
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
PROFILES = sorted((HERE / 'profiles').glob('*.json'))
RUNTIME = (HERE / 'game-runtime.ps1').read_text(encoding='utf-8', errors='replace')

# What a profile may ask of the M14.1 application router, and how a session treats the game's own settings.
ROUTERS = {'allow', 'not-allow', 'unchecked'}
POLICIES = {'untouched', 'record', 'backup-restore'}
# Owner, 2026-10-01: an interactive game session runs at most 1200 s.
SESSION_BOUND = 1200
# Owner, 2026-10-01: no FSR and no dynamic resolution in a measured session.
UPSCALER = re.compile(r'\b(fsr|dlss|xess|fidelityfx|dynamicres|resolutionscale|upscal)', re.I)


class Profiles(unittest.TestCase):
    def test_profiles_exist(self):
        self.assertTrue(PROFILES, 'no game profile in profiles/')

    def test_every_profile_names_what_a_session_needs(self):
        for path in PROFILES:
            with self.subTest(profile=path.name):
                p = json.loads(path.read_text(encoding='utf-8'))
                self.assertEqual(p['schema'], 1)
                self.assertEqual(p['id'], path.stem, 'the id is the file name')
                self.assertIsInstance(p['app_id'], int)
                self.assertIn(p['api'], p['apis'], 'the default API must be one of the profile APIs')
                for name, api in p['apis'].items():
                    self.assertIn(api['router'], ROUTERS, name)
                    self.assertTrue(api['witness']['module'], name + ': the module a session must witness')
                    self.assertTrue(api['witness']['sha256'], name + ': staged, release or an exact hash')
                self.assertTrue(p['steam']['library'] and p['steam']['install_dir'])
                self.assertTrue(p['executable']['image'].lower().endswith('.exe'))
                self.assertTrue(p['processes']['main'])
                self.assertTrue(p['processes']['renderers'])
                self.assertIn(p['readiness']['mode'], {'generic', 'witcher3-menu'})
                self.assertTrue(p['modules'], 'the modules the runner reads from the running game')
                self.assertIn(p['settings']['policy'], POLICIES)
                self.assertTrue(p['events_filter'])

    def test_no_session_exceeds_the_owners_bound(self):
        for path in PROFILES:
            with self.subTest(profile=path.name):
                seconds = json.loads(path.read_text(encoding='utf-8'))['session']['default_seconds']
                self.assertIsInstance(seconds, int)
                self.assertGreaterEqual(seconds, 60)
                self.assertLessEqual(seconds, SESSION_BOUND)


class OwnerRules(unittest.TestCase):
    """The two rules a measured session must keep. Both are owner decisions, not tuning."""

    def test_thermal_stop_is_87_c_held_10_s_or_89_c_at_once(self):
        # Owner, 2026-10-04: stop when Tctl stays at or above 87 C for 10 s, or at once at 89 C.
        self.assertRegex(RUNTIME, r'\$temp\s+-ge\s+87')
        self.assertRegex(RUNTIME, r'\$temp\s+-ge\s+89\s+-or\s+\$hotSeconds\s+-ge\s+10')
        self.assertRegex(RUNTIME, r"stop_reason\s*=\s*'thermal'")

    def test_an_unreadable_temperature_does_not_clear_the_hot_run(self):
        # A failed read is -1, never a temperature: only a readable sample below 87 C ends the hot stretch.
        self.assertRegex(RUNTIME, r'elseif\(\$temp\s+-ge\s+0\)\{\$hotSince=\$null\}')

    def test_no_profile_asks_for_an_upscaler_or_dynamic_resolution(self):
        for path in PROFILES:
            with self.subTest(profile=path.name):
                p = json.loads(path.read_text(encoding='utf-8'))
                arguments = [p['steam'].get('arguments', ''), p['executable'].get('direct_arguments', '')]
                arguments += [api.get('arguments', '') for api in p['apis'].values()]
                for value in arguments:
                    self.assertNotRegex(value or '', UPSCALER, 'no FSR or dynamic resolution in a measured session')

    def test_the_input_server_keeps_the_game_session_bound(self):
        for name in ('input/input-server.ps1', 'input/start-input.ps1'):
            with self.subTest(script=name):
                text = (HERE / name).read_text(encoding='utf-8', errors='replace')
                self.assertIn('[Math]::Min([Math]::Max($Seconds, 10), 1200)', text)


class Screenshots(unittest.TestCase):
    def test_lab_screenshots_land_outside_this_repository(self):
        # The lab screen also shows game content; its images never enter the repository (owner, 2026-10-01).
        spec = importlib.util.spec_from_file_location('lab_runner_inp', HERE / 'input/inp.py')
        inp = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(inp)
        self.assertFalse(str(inp.SHOTS).startswith(str(HERE.parents[2])),
                         'BC250_SHOTS_DIR must stay outside the repository')


if __name__ == '__main__':
    unittest.main()
