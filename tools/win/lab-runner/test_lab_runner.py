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
STREAM = (HERE / 'kmdlog-stream.ps1').read_text(encoding='utf-8', errors='replace')
WORLD_RULE = (HERE / 'etw/world-rule.ps1').read_text(encoding='utf-8', errors='replace')
# Where the stream's polling loop begins. Everything after it runs every interval of a game session.
STREAM_LOOP = 'while($timer.Elapsed.TotalSeconds -lt $streamLimit'

# What a profile may ask of the M14.1 application router, and how a session treats the game's own settings.
ROUTERS = {'allow', 'not-allow', 'unchecked'}
POLICIES = {'untouched', 'record', 'backup-restore'}
# Owner, 2026-10-01: an interactive game session runs at most 1200 s.
SESSION_BOUND = 1200
# Owner, 2026-10-01: no FSR and no dynamic resolution in a measured session.
UPSCALER = re.compile(r'\b(fsr|dlss|xess|fidelityfx|dynamicres|resolutionscale|upscal)', re.I)
# Before readiness the channel takes only these keys (scan codes Space, Esc, Enter), and an intro skip presses one.
INTRO_KEYS = {'39', '01', '1C'}


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

    def test_an_intro_skip_presses_an_intro_key_within_its_bounds(self):
        for path in PROFILES:
            skip = json.loads(path.read_text(encoding='utf-8'))['readiness'].get('intro_skip')
            if skip is None:
                continue
            with self.subTest(profile=path.name):
                self.assertIn(skip['key'], INTRO_KEYS, 'the channel takes no other key before readiness')
                self.assertTrue(2 <= skip['every_seconds'] <= 30)
                self.assertTrue(0 <= skip['after_window_seconds'] <= 120)
                self.assertTrue(1 <= skip['max_presses'] <= 100)

    def test_the_witcher3_profile_skips_its_intro_with_space(self):
        # b29 native-caps549: the intro video waited for Space from about 78 s to 183 s after the launch.
        profile = json.loads((HERE / 'profiles/witcher3.json').read_text(encoding='utf-8'))
        self.assertEqual(profile['readiness']['intro_skip']['key'], '39')

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

    def test_the_walk_marker_says_whether_anything_verified_the_world(self):
        # BD-107, and H1 of the 2026-10-10 audit: the marker that opens ETW measurement window B must say what
        # verified the world. This copy of the runner has no telemetry rule, so only the operator's explicit mark
        # may write the verified form; the automatic walk and an input command write the unverified one. The
        # grammar itself lives in etw/world-rule.ps1, and the two files must agree on its words.
        verified = re.search(r"\$script:Bc250WorldMarkerVerified = '(.+)'", WORLD_RULE)
        unverified = re.search(r"\$script:Bc250WorldMarkerUnverified = '(.+)'", WORLD_RULE)
        self.assertTrue(verified and unverified, 'etw/world-rule.ps1 no longer names the two marker forms')
        states = {'world verified: ': verified.group(1), 'world unverified: ': unverified.group(1)}
        for state, pattern in states.items():
            self.assertIn(state, pattern, 'the rule and the runner must spell the same state')
        # Every marker the runner writes, as the note's own string literal.
        markers = re.findall(r"'walk '\+\$t\+'s: start([^']*)'", RUNTIME)
        self.assertTrue(markers, 'the runner writes no walk marker any more')
        for marker in markers:
            if marker == ' (':
                continue    # built from a reason variable, checked below
            self.assertRegex(marker, r' \((world verified|world unverified): ',
                             'a walk marker without a world state')
        self.assertIn("'walk '+$t+'s: start (world unverified: no telemetry rule in this runner", RUNTIME,
                      'the automatic walk must mark its world unverified')
        self.assertIn("'world verified: operator mark'", RUNTIME, 'the operator mark is the one verified world here')
        self.assertNotRegex(RUNTIME, r"'walk '\+\$t\+'s: start'",
                            'a bare marker: the ETW capture must not read this as a verified world')

    def test_the_world_rule_resets_its_band_across_a_sampler_gap(self):
        # H2: the held band was counted in samples only, so two ends of a five-minute hole became a 300 s band.
        self.assertRegex(WORLD_RULE, r'\[double\]\$MaxGapSeconds = 5')
        self.assertIn('if ($step -gt $MaxGapSeconds) { $gap = [Math]::Round($step, 1); break }', WORLD_RULE)
        self.assertIn('-MaxGapSeconds $MaxGapSeconds', WORLD_RULE, 'the file reader must pass the gap bound on')

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


class BeforeReadiness(unittest.TestCase):
    """The channel before readiness. intro-skip-check.ps1 runs the functions themselves under Windows PowerShell
    5.1; these tests hold the key list and the world-mark refusal in the text of the script."""

    def test_the_channel_takes_only_space_esc_and_enter_before_readiness(self):
        keys = re.search(r"\$script:PreReadyKeys=@\(([^)]*)\)", RUNTIME)
        self.assertTrue(keys, 'the list of the keys before readiness is gone')
        self.assertEqual({key.strip().strip("'") for key in keys.group(1).split(',')}, INTRO_KEYS)
        self.assertIn('$bad=PreReady-Refusal $actions', RUNTIME, 'Control-Step no longer checks before readiness')

    def test_a_world_mark_is_refused_before_readiness(self):
        self.assertIn("if($a -match '^(world|note:world)$'){return $a}", RUNTIME)


class LogStream(unittest.TestCase):
    """How kmdlog-stream.ps1 reads the KMD log ring during a session. `log summary` is BC250_ESCAPE_LOG_SUMMARY,
    which the driver answers only with HardwareAccess: a Level Two escape, so dxgkrnl suspends the GPU scheduler
    for up to one VSync around it (measured cost of such a poll: a 300 ms stall every 5.4 s). It carries the WDDM,
    DPM, interop and OTG tables, which are worth having once at the start of a session and never on a schedule."""

    def test_the_loop_asks_for_no_summary(self):
        at = STREAM.find(STREAM_LOOP)
        self.assertGreater(at, 0, 'the polling loop of kmdlog-stream.ps1 was not found')
        self.assertNotIn('log summary', STREAM[at:],
                         'no LOG_SUMMARY escape from the stream loop: it runs every 500 ms of a game session')
        self.assertIn("Run-Bounded $cli 'log 0' 'log header'", STREAM[at:])

    def test_one_summary_before_the_loop(self):
        at = STREAM.find(STREAM_LOOP)
        self.assertEqual(STREAM[:at].count("'log summary'"), 1,
                         'exactly one LOG_SUMMARY escape per session, before the loop')


class Sources(unittest.TestCase):
    def test_every_script_compiles(self):
        # A syntax error in a host tool must fail the gate, not the next session that calls it.
        for path in sorted(HERE.rglob('*.py')):
            with self.subTest(script=path.name):
                compile(path.read_text(encoding='utf-8'), str(path), 'exec')


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
