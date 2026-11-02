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


RELAY = r'''"""Holds its stdin until EOF and then gives it to the child; the child's output passes at once. The suspected
behaviour of the lab's ssh session in bd108-k2 (2026-11-02)."""
import subprocess, sys, threading
p = subprocess.Popen(sys.argv[1:], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
def out():
    while True:
        d = p.stdout.read1(4096)
        if not d:
            return
        sys.stdout.buffer.write(d)
        sys.stdout.buffer.flush()
t = threading.Thread(target=out)
t.start()
data = sys.stdin.buffer.read()
try:
    p.stdin.write(data)
    p.stdin.close()
except OSError:
    pass
p.wait()
t.join()
sys.exit(p.returncode)
'''

STANDIN = r'''"""A far end that answers the handshake and then fails in one way: 'early' ends 1 s after its PONG, 'drop' ends at
the first request, 'mute' never confirms a request."""
import os, sys, threading
from datetime import datetime, timezone
mode = sys.argv[1]
def say(s):
    sys.stdout.write(s + '\n')
    sys.stdout.flush()
say('@@CHANNEL READY pid 1 bound 60 s')
for line in sys.stdin:
    line = line.strip()
    if line.startswith('PING '):
        say('@@PONG ' + line[5:] + ' ' + datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%S.%f')[:-3] + 'Z')
        if mode == 'early':
            threading.Timer(1.0, os._exit, args=(0,)).start()
        continue
    if line.startswith('REQ '):
        if mode == 'drop':
            sys.exit(0)
        continue
    if line == 'QUIT':
        break
'''


@unittest.skipUnless(POWERSHELL, 'needs Windows PowerShell 5.1 as the far end')
class SelfChecks(unittest.TestCase):
    """The broker's checks of its own path (bd108-k2: no request line reached the far end through the session's stdin
    in 90 s, four requests stayed behind, and later commands went the per-call way without a word)."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='bc250-chself-', dir=os.environ.get('BC250_TEST_TMP')))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        os.environ['BC250_CHANNEL_DIR'] = str(self.tmp / 'host')
        self.addCleanup(os.environ.pop, 'BC250_CHANNEL_DIR', None)
        self.lab = self.tmp / 'lab'
        src = self.tmp / 'src'
        src.mkdir()
        (src / 'echo.ps1').write_text(ECHO, encoding='utf-8-sig')
        (self.tmp / 'relay.py').write_text(RELAY, encoding='utf-8')
        (self.tmp / 'standin.py').write_text(STANDIN, encoding='utf-8')
        self.scripts = [src / 'echo.ps1']

    def host_argv(self, *extra, hold=False):
        argv = [POWERSHELL, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(self.lab / 'channel-host.ps1'),
                '-Seconds', '60', '-Dir', str(self.lab), *extra]
        return [sys.executable, '-u', str(self.tmp / 'relay.py'), *argv] if hold else argv

    def standin(self, mode):
        return [sys.executable, '-u', str(self.tmp / 'standin.py'), mode]

    def serve(self, b):
        result = {}
        t = threading.Thread(target=lambda: result.update(reason=b.serve()), daemon=True)
        t.start()
        self.addCleanup(b.close)
        return t, result

    def test_far_end_that_ends_early(self):
        # The far end answers the handshake and ends 1 s later, with no request: the broker sees it within a second
        # and says so in broker.json; a later caller takes its own path at once.
        b = channel.Broker('game-early', 60, self.standin('early'), local_push, str(self.lab), self.scripts)
        b.start()
        t, result = self.serve(b)
        t.join(10)
        self.assertEqual(result.get('reason'), 'far end ended')
        rec = channel.read_json(b.dir / 'broker.json')
        self.assertEqual(rec['state'], 'ended')
        self.assertLess(channel.epoch_of(rec['ended']) - b.started, 8)
        start = time.monotonic()
        self.assertIsNone(channel.call('game-early', 'echo.ps1', {}))
        self.assertLess(time.monotonic() - start, 1)

    def test_far_end_that_ends_at_a_request(self):
        # The far end ends when the first request comes; a second request waits behind it. The first gets an error
        # and is not sent again; the second was never sent, so its caller takes its own path (None).
        b = channel.Broker('game-dropped', 60, self.standin('drop'), local_push, str(self.lab), self.scripts)
        b.start()
        out = {}
        first = threading.Thread(target=lambda: out.update(a=channel.call('game-dropped', 'echo.ps1', {'Text': 'a'})))
        first.start()
        time.sleep(0.2)
        second = threading.Thread(target=lambda: out.update(b=channel.call('game-dropped', 'echo.ps1', {'Text': 'b'})))
        second.start()
        time.sleep(0.2)
        t, result = self.serve(b)
        first.join(20)
        second.join(20)
        t.join(20)
        self.assertEqual(result.get('reason'), 'far end ended')
        self.assertIsNone(out['a']['exit'])
        self.assertEqual(out['a']['lines'], ['error: the far end ended or did not answer in time'])
        self.assertIsNone(out['b'])
        self.assertEqual(list(b.dir.glob('req-*.json')), [])

    def test_unconfirmed_request_ends_the_session(self):
        original = channel.GOT_S
        channel.GOT_S = 1
        self.addCleanup(setattr, channel, 'GOT_S', original)
        b = channel.Broker('game-mute', 60, self.standin('mute'), local_push, str(self.lab), self.scripts)
        b.start()
        t, result = self.serve(b)
        start = time.monotonic()
        rep = channel.call('game-mute', 'echo.ps1', {})
        self.assertLess(time.monotonic() - start, 5)
        self.assertIsNone(rep['exit'])
        self.assertIn('did not confirm the request in 1 s', rep['lines'][0])
        t.join(15)
        self.assertEqual(result.get('reason'), 'far end did not confirm a request')

    def test_held_stdin_moves_to_tcp(self):
        # The session's stdin comes only at its end: the handshake fails over stdin, the broker opens the tcp
        # transport, and every request goes through it. The first far end received only PING and QUIT, so nothing
        # ran late.
        b = channel.Broker('game-held', 60, self.host_argv(hold=True), local_push, str(self.lab), self.scripts,
                           tcp=(self.host_argv('-Tcp', hold=True), channel.direct_connect))
        self.assertIn(' tcp ', b.start())
        self.assertEqual(b.transport, 'tcp')
        self.assertEqual(b.attempts[0]['transport'], 'stdin')
        self.assertIn('no PONG', b.attempts[0]['failed'])
        self.assertIn('pong_s', b.attempts[1])
        self.assertLess(abs(b.clock['offset_s']), 1)
        t, result = self.serve(b)
        for k in range(3):
            rep = channel.call('game-held', 'echo.ps1', {'Text': str(k)})
            self.assertEqual((rep['exit'], rep['lines'][0]), (0, f'echo {k}'))
        rec = channel.serving('game-held')
        self.assertEqual((rec['transport'], [a['transport'] for a in rec['attempts']]), ('tcp', ['stdin', 'tcp']))
        channel.main(['stop', 'game-held'])
        t.join(20)
        self.assertEqual(result.get('reason'), 'stop')
        rows = (b.dir / 'stamps.tsv').read_text(encoding='utf-8').splitlines()
        self.assertEqual([r.split('\t')[-1] for r in rows], ['0', '0', '0'])

    def test_no_transport_is_no_channel(self):
        def refuse(port):
            raise ConnectionRefusedError('refused in the test')
        b = channel.Broker('game-none2', 60, self.host_argv(hold=True), local_push, str(self.lab), self.scripts,
                           tcp=(self.host_argv('-Tcp'), refuse))
        with self.assertRaises(RuntimeError) as e:
            b.start()
        self.assertIn('stdin: no PONG', str(e.exception))
        self.assertIn('tcp: ConnectionRefusedError', str(e.exception))
        rec = channel.read_json(b.dir / 'broker.json')
        self.assertEqual(rec['state'], 'ended')
        self.assertIsNone(channel.call('game-none2', 'echo.ps1', {}))

    def test_stale_requests_are_dropped(self):
        b = channel.Broker('game-stale', 60, self.host_argv(), local_push, str(self.lab), self.scripts)
        b.start()
        gone = subprocess.Popen([sys.executable, '-c', 'pass'])
        gone.wait()
        rids = {'gone': uuid_hex(), 'late': uuid_hex()}
        channel.write_json(b.dir / f"req-{rids['gone']}.json", {'id': rids['gone'], 'script': 'echo.ps1', 'params': {},
                                                               'pid': gone.pid, 'expires': time.time() + 60})
        channel.write_json(b.dir / f"req-{rids['late']}.json", {'id': rids['late'], 'script': 'echo.ps1', 'params': {},
                                                               'pid': os.getpid(), 'expires': time.time() - 1})
        t, result = self.serve(b)
        for key, why in (('gone', 'the client has gone'), ('late', 'expired')):
            path = b.dir / f'rep-{rids[key]}.json'
            end = time.monotonic() + 10
            while not path.exists() and time.monotonic() < end:
                time.sleep(0.02)
            rep = channel.read_json(path)
            self.assertIsNone(rep['exit'])
            self.assertIn(why, rep['lines'][0])
        self.assertEqual(channel.call('game-stale', 'echo.ps1', {'Text': 'x'})['exit'], 0)
        channel.main(['stop', 'game-stale'])
        t.join(20)
        rows = [r.split('\t') for r in (b.dir / 'stamps.tsv').read_text(encoding='utf-8').splitlines()]
        self.assertEqual(sorted(r[-1] for r in rows), ['0', 'dropped', 'dropped'])

    def test_far_end_refuses_a_late_request(self):
        # A lab clock 60 s behind the broker's estimate: the request's limit has passed when it comes, so the far end
        # does not run it.
        b = channel.Broker('game-lateness', 60, self.host_argv(), local_push, str(self.lab), self.scripts)
        b.start()
        b.clock['offset_s'] -= 60
        t, result = self.serve(b)
        rep = channel.call('game-lateness', 'echo.ps1', {'Text': 'never'})
        self.assertEqual(rep['exit'], 1)
        self.assertIn('refused: the request came', rep['lines'][-1])
        self.assertNotIn('echo never', rep['lines'])
        channel.main(['stop', 'game-lateness'])
        t.join(20)

    def test_tcp_listener_wants_the_token(self):
        local_push([str(channel.HOST_SCRIPT)], str(self.lab))
        p = subprocess.Popen(self.host_argv('-Tcp'), stdin=subprocess.PIPE, stdout=subprocess.PIPE)

        def cleanup():
            if p.poll() is None:
                p.kill()
            p.wait(10)
            p.stdin.close()
            p.stdout.close()
        self.addCleanup(cleanup)
        ready = p.stdout.readline().decode('utf-8')
        port, token = ready.split()[-2:]
        wrong = channel.direct_connect(int(port))
        wrong.sendall(b'HELLO ' + b'0' * 32 + b'\n')
        wrong.settimeout(10)
        self.assertEqual(wrong.recv(100), b'')  # closed without a word
        wrong.close()
        right = channel.direct_connect(int(port))
        right.sendall(f'HELLO {token}\nPING abc\nQUIT\n'.encode())
        lines = Lines_of(right)
        self.assertTrue(lines[0].startswith('@@PONG abc '), lines)
        p.wait(20)


def uuid_hex():
    import uuid
    return uuid.uuid4().hex


def Lines_of(sock):
    sock.settimeout(20)
    data = b''
    while True:
        part = sock.recv(4096)
        if not part:
            break
        data += part
    sock.close()
    return data.decode('utf-8').splitlines()


class ServeLog(unittest.TestCase):
    """The serve log of channel.main: each line carries the time its step ended (bd108-k2 read a 90 s session as one
    that ended 2 s after READY, because utc() ran before the call in the same f-string)."""

    def test_ended_line_has_the_end_time(self):
        class Fake:
            seconds, dir, transport, clock = 90, Path('x'), 'stdin', None

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
        command = f'powershell -NoProfile -ExecutionPolicy Bypass -File "{remote}\\channel-host.ps1" -Seconds 1230 -Dir "{remote}"'
        self.assertEqual(b.argv, ['ssh', '-o', 'ServerAliveInterval=15', '-o', 'ServerAliveCountMax=4', 'lab', command])
        self.assertEqual((b.seconds, b.remote_dir), (1230, remote))
        # The tcp transport: the same command with -Tcp, through a SOCKS listener of the same ssh connection.
        tcp = b.tcp[0]
        self.assertEqual(tcp[-1], command + ' -Tcp')
        self.assertIn('ExitOnForwardFailure=yes', tcp)
        self.assertRegex(tcp[tcp.index('-D') + 1], r'^127\.0\.0\.1:\d+$')
        self.assertEqual(b.order, ['stdin', 'tcp'])


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
