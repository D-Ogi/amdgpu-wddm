"""Host tests of the held control channel (input/channel.py and input/channel-host.ps1, BD-108).

The far end is the real channel-host.ps1 under the local Windows PowerShell 5.1, the shell the lab runs, started the
way the broker starts ssh: its stdin and stdout are the pipes. No network and no lab. Run:
    python -m unittest discover -s tools/win/lab-runner -p "test_channel.py"
"""
import contextlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / 'input'))
import channel  # noqa: E402
import inp  # noqa: E402

POWERSHELL = shutil.which('powershell.exe')

ECHO = r'''param([string]$Text = '', [int]$Code = 0, [switch]$Long, [switch]$Fail, [switch]$Soft, [int]$SleepMs = 0)
if ($SleepMs) { Start-Sleep -Milliseconds $SleepMs }
"echo $Text"
Write-Host 'host line'
if ($Long) { 'L' * 200000 }
if ($Soft) { Write-Error 'soft failure'; 'after soft' }
if ($Fail) { throw 'planned failure' }
exit $Code
'''


def local_push(paths, remote_dir):
    os.makedirs(remote_dir, exist_ok=True)
    for p in paths:
        shutil.copy(p, remote_dir)


@unittest.skipUnless(POWERSHELL, 'needs Windows PowerShell 5.1 as the far end')
class HeldChannel(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='bc250-channel-', dir=os.environ.get('BC250_TEST_TMP')))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        os.environ['BC250_CHANNEL_DIR'] = str(self.tmp / 'host')
        self.addCleanup(os.environ.pop, 'BC250_CHANNEL_DIR', None)
        self.lab = self.tmp / 'lab'
        src = self.tmp / 'src'
        src.mkdir()
        (src / 'echo.ps1').write_text(ECHO, encoding='utf-8-sig')
        self.scripts = [src / 'echo.ps1']

    def broker(self, name='game-test', seconds=60):
        argv = [POWERSHELL, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(self.lab / 'channel-host.ps1'),
                '-Seconds', str(seconds), '-Dir', str(self.lab)]
        b = channel.Broker(name, seconds, argv, local_push, str(self.lab), self.scripts)
        self.assertTrue(b.start().startswith('@@CHANNEL READY pid '))
        result = {}
        t = threading.Thread(target=lambda: result.update(reason=b.serve()), daemon=True)
        t.start()
        self.addCleanup(b.close)
        return b, t, result

    def test_requests_through_one_session(self):
        b, t, result = self.broker()
        self.assertIsNotNone(channel.serving('game-test'))
        rep = channel.call('game-test', 'echo.ps1', {'Text': 'za\u017c\u00f3\u0142\u0107 1;2 "q"'})
        self.assertEqual(rep['exit'], 0)
        self.assertEqual(rep['lines'], ['echo za\u017c\u00f3\u0142\u0107 1;2 "q"', 'host line'])
        # The far end confirms the request before the script runs; the GOT line is a stamp, not a reply line.
        self.assertEqual(rep['stamps']['lab_got'], rep['stamps']['lab_received'])
        self.assertEqual(channel.call('game-test', 'echo.ps1', {'Code': 3})['exit'], 3)
        rep = channel.call('game-test', 'echo.ps1', {'Fail': True})
        self.assertEqual(rep['exit'], 1)
        self.assertEqual(rep['lines'][-1], 'error: planned failure')
        rep = channel.call('game-test', 'echo.ps1', {'Long': True})
        self.assertEqual(rep['lines'][2], 'L' * 200000)
        # A non-terminating error does not stop the script, as under 'powershell -File'.
        rep = channel.call('game-test', 'echo.ps1', {'Soft': True})
        self.assertEqual(rep['exit'], 0)
        self.assertEqual(rep['lines'][2:], ['soft failure', 'after soft'])
        for name, why in (('..\\echo.ps1', 'refused script name'), ('channel-host.ps1', 'refused script name'),
                          ('nope.ps1', 'no staged script nope.ps1')):
            rep = channel.call('game-test', name, {})
            self.assertEqual(rep['exit'], 1, name)
            self.assertIn(why, rep['lines'][-1])
        # A broken request file gets an error reply, and the session goes on.
        bad = '0' * 32
        (b.dir / f'req-{bad}.json').write_text('not json', encoding='utf-8')
        rep_path = b.dir / f'rep-{bad}.json'
        end = time.monotonic() + 10
        while not rep_path.exists() and time.monotonic() < end:
            time.sleep(0.02)
        rep = channel.read_json(rep_path)
        self.assertIsNone(rep['exit'])
        self.assertIn('unreadable request', rep['lines'][0])
        # Two clients at once: each gets its own reply.
        out = {}
        threads = [threading.Thread(target=lambda k=k: out.update({k: channel.call('game-test', 'echo.ps1',
                                    {'Text': k, 'SleepMs': 300})})) for k in ('one', 'two')]
        for th in threads:
            th.start()
        for th in threads:
            th.join(30)
        self.assertEqual(out['one']['lines'][0], 'echo one')
        self.assertEqual(out['two']['lines'][0], 'echo two')
        # The four stamps of a round trip, in order.
        rows = [line.split('\t') for line in (b.dir / 'stamps.tsv').read_text(encoding='utf-8').splitlines()]
        self.assertEqual(len(rows), 10)
        for r in rows:
            self.assertEqual(len(r), 8)
            self.assertTrue(r[2] <= r[3] <= r[6], r)
            self.assertRegex(r[4], r'^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z$')
        # Latency of the held session on this PC: well under the 2 s of one target.py ps call. The bound is half of
        # that, not less: the quick gate runs this beside 24 other workers (0.53 s measured under that load).
        start = time.monotonic()
        for _ in range(5):
            channel.call('game-test', 'echo.ps1', {'Text': 'x'})
        self.assertLess((time.monotonic() - start) / 5, 1.0)
        channel.main(['stop', 'game-test'])
        t.join(20)
        self.assertEqual(result.get('reason'), 'stop')
        rec = json.loads((b.dir / 'broker.json').read_text(encoding='utf-8'))
        self.assertEqual(rec['state'], 'ended')
        self.assertIsNone(channel.serving('game-test'))
        self.assertIsNone(channel.call('game-test', 'echo.ps1', {}))  # no broker: the caller's own path
        self.assertIsNotNone(b.proc.poll())  # the far end ended with the session

    def test_far_end_lost(self):
        b, t, result = self.broker('game-lost')
        b.proc.kill()
        t.join(20)
        self.assertEqual(result.get('reason'), 'far end ended')
        self.assertIsNone(channel.call('game-lost', 'echo.ps1', {}))

    def test_far_end_lost_during_a_request(self):
        b, t, result = self.broker('game-drop')
        killer = threading.Timer(1.0, b.proc.kill)
        killer.start()
        rep = channel.call('game-drop', 'echo.ps1', {'SleepMs': 5000})
        killer.join()
        # The broker gives up the request and does not send it again.
        self.assertIsNone(rep['exit'])
        self.assertEqual(rep['lines'], ['error: the far end ended or did not answer in time'])  # killed in the sleep
        # The far end had confirmed it: the request reached the script, which is what bd108-k2 could not tell.
        self.assertRegex(rep['stamps'].get('lab_got', ''), r'^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z$')
        t.join(20)
        self.assertEqual(result.get('reason'), 'far end ended')
        self.assertIsNone(channel.serving('game-drop'))

    def far_end(self, seconds):
        local_push([str(channel.HOST_SCRIPT)], str(self.lab))
        p = subprocess.Popen([POWERSHELL, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                              str(self.lab / 'channel-host.ps1'), '-Seconds', str(seconds), '-Dir', str(self.lab)],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        def cleanup():
            if p.poll() is None:
                p.kill()
            p.wait(10)
            for stream in (p.stdin, p.stdout):
                try:
                    stream.close()
                except (OSError, ValueError):
                    pass
        self.addCleanup(cleanup)
        return p

    def test_far_end_keeps_its_bound(self):
        # stdin stays open and no request comes: the far end ends by itself at its bound (at least 10 s).
        p = self.far_end(1)
        guard = threading.Timer(40, p.kill)
        guard.start()
        start = time.monotonic()
        text = p.stdout.read().decode('utf-8')
        elapsed = time.monotonic() - start
        guard.cancel()
        p.stdin.close()
        p.wait(10)
        self.assertIn('bound 10 s', text)
        self.assertIn('@@CHANNEL END', text)
        self.assertGreater(elapsed, 9)
        self.assertLess(elapsed, 25)

    def test_far_end_ends_at_eof_and_refuses_a_bad_line(self):
        start = time.monotonic()
        out, _ = self.far_end(600).communicate(b'REQ nothex AAAA\n', timeout=40)  # communicate() closes stdin
        self.assertLess(time.monotonic() - start, 20)
        lines = out.decode('utf-8').splitlines()
        self.assertTrue(lines[0].startswith('@@CHANNEL READY'))
        self.assertEqual(lines[1], '@@BAD request line')
        self.assertTrue(lines[2].startswith('@@CHANNEL END'))

    def test_no_broker_is_no_channel(self):
        self.assertIsNone(channel.serving('game-none'))
        self.assertIsNone(channel.call('game-none', 'echo.ps1', {}))
        with self.assertRaises(ValueError):
            channel.channel_dir('../x')


class ServeLog(unittest.TestCase):
    """The serve log of channel.main: each line carries the time its step ended (bd108-k2 read a 90 s session as one
    that ended 2 s after READY, because utc() ran before the call in the same f-string)."""

    def test_ended_line_has_the_end_time(self):
        class Fake:
            seconds, dir = 90, Path('x')

            def start(self):
                time.sleep(0.2)
                return '@@CHANNEL READY pid 1 bound 90 s'

            def serve(self):
                time.sleep(0.4)
                return 'stop'
        original = channel.lab_broker
        channel.lab_broker = lambda name, seconds, scripts: Fake()
        out = io.StringIO()
        try:
            with contextlib.redirect_stdout(out):
                channel.main(['serve', 'game-log', '90', 'echo.ps1'])
        finally:
            channel.lab_broker = original
        lines = out.getvalue().splitlines()
        stamp = [datetime.strptime(line.split()[0], '%Y-%m-%dT%H:%M:%S.%fZ') for line in lines]
        self.assertIn('@@CHANNEL READY', lines[0])
        self.assertTrue(lines[2].endswith('ended: stop'), lines)
        self.assertGreaterEqual((stamp[2] - stamp[1]).total_seconds(), 0.35, lines)


class LabCommandLine(unittest.TestCase):
    """The ssh command line of the lab broker, with a stand-in for target.py (no address, no probe)."""

    def test_command_line(self):
        import types
        fake = types.ModuleType('target')

        class Target:
            def ssh_argv(self, remote_command=None, options=()):
                return ['ssh', *options, 'lab', remote_command]

            def push(self, paths, remote_dir):
                pass
        fake.Target = Target
        real = sys.modules.get('target')
        sys.modules['target'] = fake
        try:
            b = channel.lab_broker('game-x', '5000', [HERE / 'input' / 'input-exec.ps1'])
            with self.assertRaises(ValueError):
                channel.lab_broker('../x', '60', [])
        finally:
            if real is None:
                sys.modules.pop('target', None)
            else:
                sys.modules['target'] = real
        remote = 'C:\\BC250\\tmp\\channel\\game-x'
        self.assertEqual(b.argv, ['ssh', '-o', 'ServerAliveInterval=15', '-o', 'ServerAliveCountMax=4', 'lab',
                                  f'powershell -NoProfile -ExecutionPolicy Bypass -File "{remote}\\channel-host.ps1" '
                                  f'-Seconds 1230 -Dir "{remote}"'])
        self.assertEqual((b.seconds, b.remote_dir), (1230, remote))


class InpFallback(unittest.TestCase):
    """inp.py without a held session: one ssh connection to the staged input-exec.ps1."""

    class Lab:
        def __init__(self, replies):
            self.replies = list(replies)
            self.calls = []
            self.pushed = []

        def run_staged(self, remote, args, timeout=600):
            self.calls.append((remote, list(args)))
            code, text = self.replies.pop(0)
            return subprocess.CompletedProcess([], code, text, '')

        def push(self, paths, remote_dir):
            self.pushed.append((paths, remote_dir))

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix='bc250-inp-', dir=os.environ.get('BC250_TEST_TMP'))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        os.environ['BC250_CHANNEL_DIR'] = self.tmp
        self.addCleanup(os.environ.pop, 'BC250_CHANNEL_DIR', None)

    def test_one_connection_per_command(self):
        lab = self.Lab([(0, 'done-001 after 0.3 s\ntap 1C 1/1')])
        text = inp.send('game-rottr', inp.exec_params('game-rottr', actions='tap:1C'), lab)
        self.assertIn('done-001', text)
        self.assertEqual(lab.calls, [('C:\\BC250\\tmp\\input\\input-exec.ps1',
                                      ['-Session', 'game-rottr', '-ActionsB64', 'dGFwOjFD'])])
        self.assertEqual(lab.pushed, [])

    def test_peek_is_a_switch(self):
        lab = self.Lab([(0, 'queue: 0 commands, 0 done')])
        inp.send('game-rottr', inp.exec_params('game-rottr', peek=True), lab)
        self.assertEqual(lab.calls[0][1], ['-Session', 'game-rottr', '-Peek'])

    def test_unstaged_copy_is_pushed_once(self):
        missing = 'The argument \'C:\\BC250\\tmp\\input\\input-exec.ps1\' to the -File parameter does not exist.'
        lab = self.Lab([(1, missing), (0, 'done-001 after 0.2 s')])
        text = inp.send('game-rottr', inp.exec_params('game-rottr', actions='tap:39'), lab)
        self.assertEqual(text, 'done-001 after 0.2 s')
        self.assertEqual(len(lab.calls), 2)
        self.assertEqual(lab.pushed, [([str(inp.HERE / 'input-exec.ps1')], 'C:\\BC250\\tmp\\input')])

    def test_other_failure_is_not_repeated(self):
        lab = self.Lab([(1, 'TIMEOUT after 20 s')])
        self.assertEqual(inp.send('game-rottr', inp.exec_params('game-rottr', actions='tap:39'), lab), 'TIMEOUT after 20 s')
        self.assertEqual(len(lab.calls), 1)


if __name__ == '__main__':
    unittest.main()
