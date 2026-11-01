"""Host tests of the lab's sshd watch (sshd-watch.ps1, BD-051).

The watch runs under the local Windows PowerShell 5.1, the engine of the lab, against a stand-in sshd on 127.0.0.1:
a TCP server that gives each connection a scripted answer (a banner, silence, a close or a wrong first line). The
heal is log-only and the forensics script is a stub, so the test restarts no service and needs no elevation. The one
scenario with the service check names a service that does not exist. No lab and no network. Run:
    python -m unittest discover -s tools/win/lab-emerg -p "test_*.py"
"""
import json
import os
import shutil
import socket
import subprocess
import tempfile
import threading
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
WATCH = HERE / 'sshd-watch.ps1'
POWERSHELL = shutil.which('powershell.exe')

STUB = "param([int]$Minutes = 0)\n\"stub forensics minutes $Minutes\"\n"
SLOW_STUB = "param([int]$Minutes = 0)\n'slow stub starts'\nStart-Sleep -Seconds 60\n"


class FakeSshd:
    def __init__(self, default, script=()):
        self.default = default
        self.script = list(script)
        self.sock = socket.socket()
        self.sock.bind(('127.0.0.1', 0))
        self.sock.listen(16)
        self.port = self.sock.getsockname()[1]
        self.held = []
        self.accepted = 0
        threading.Thread(target=self._loop, daemon=True).start()

    def _loop(self):
        while True:
            try:
                conn, _ = self.sock.accept()
            except OSError:
                return
            mode = self.script.pop(0) if self.script else self.default
            self.accepted += 1
            if mode == 'close':
                conn.close()
                continue
            if mode == 'banner':
                conn.sendall(b'SSH-2.0-OpenSSH_for_Windows_9.8 stand-in\r\n')
            elif mode == 'junk':
                conn.sendall(b'HTTP/1.0 400 Bad Request\r\n')
            self.held.append(conn)  # 'silent' sends nothing and keeps the connection open

    def close(self):
        self.sock.close()
        for c in self.held:
            c.close()


def free_port():
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    s.close()
    return port


@unittest.skipUnless(POWERSHELL, 'needs Windows PowerShell 5.1, the engine of the lab')
class SshdWatch(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix='bc250-sshd-watch-', dir=os.environ.get('BC250_TEST_TMP')))
        (cls.tmp / 'stub.ps1').write_text(STUB, encoding='utf-8')
        (cls.tmp / 'slow-stub.ps1').write_text(SLOW_STUB, encoding='utf-8')
        cls.results = {}
        # The scenarios are independent (each has its own stand-in, port and directory) and mostly wait on
        # timeouts, so they run at once.
        cases = {
            'ok': (FakeSshd('banner'), 3, ()),
            'stall': (FakeSshd('silent'), 4, ('-MinRestartGapSeconds', '600')),
            'transient': (FakeSshd('banner', ['silent', 'banner', 'silent', 'banner']), 4, ()),
            'limit': (FakeSshd('silent'), 4, ('-MinRestartGapSeconds', '0', '-MaxRestartsPerHour', '1',
                                               '-ForensicsScript', str(cls.tmp / 'slow-stub.ps1'),
                                               '-ForensicsSeconds', '2')),
            'closed': (FakeSshd('close'), 2, ()),
            'junk': (FakeSshd('junk'), 2, ()),
            # A connect to a closed port on Windows retries for about a second before it reports the refusal.
            'refused': (None, 2, ('-ProbeTimeoutSeconds', '5')),
            # With a 1 s timeout the same connect ends as connect-timeout: no handshake, so no stall either.
            'refused-fast': (None, 3, ()),
            'service': (FakeSshd('silent'), 2, ('-Heal', 'restart', '-Service', 'bc250-no-such-service')),
        }
        threads = [threading.Thread(target=cls._run, args=(name,) + case) for name, case in cases.items()]
        for t in threads:
            t.start()
        for t in threads:
            t.join(120)
        for fake, _, _ in cases.values():
            if fake:
                fake.close()

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, True)

    @classmethod
    def _run(cls, name, fake, cycles, extra):
        base = cls.tmp / name
        port = fake.port if fake else free_port()
        named = {'-Port': str(port), '-Base': str(base), '-EverySeconds': '1', '-ConfirmSeconds': '1',
                 '-ProbeTimeoutSeconds': '1', '-AfterHealSeconds': '0', '-Cycles': str(cycles),
                 '-ForensicsScript': str(cls.tmp / 'stub.ps1'), '-Heal': 'log-only'}
        named.update(zip(extra[::2], extra[1::2]))  # a scenario's own values replace the defaults
        args = [POWERSHELL, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(WATCH)]
        for key, value in named.items():
            args += [key, value]
        if named['-Heal'] == 'log-only':
            args.append('-NoServiceCheck')
        start = time.monotonic()
        done = subprocess.run(args, capture_output=True, text=True, timeout=110)
        watch_dir = base / 'sshd-watch'
        log = (watch_dir / 'watch.log').read_text(encoding='utf-8') if (watch_dir / 'watch.log').exists() else ''
        lines = [line.split(' ', 1)[1] for line in log.splitlines() if ' ' in line]
        state = json.loads((watch_dir / 'state.json').read_text(encoding='utf-8-sig')) \
            if (watch_dir / 'state.json').exists() else {}
        forensics = sorted(watch_dir.glob('forensics-*.txt'))
        cls.results[name] = dict(code=done.returncode, out=done.stdout + done.stderr, lines=lines, state=state,
                                 forensics=[f.read_text(encoding='utf-8', errors='replace') for f in forensics],
                                 seconds=time.monotonic() - start, accepted=fake.accepted if fake else 0)

    def result(self, name):
        r = self.results[name]
        self.assertEqual(r['code'], 0, r['out'])
        return r

    def has(self, lines, start):
        return [line for line in lines if line.startswith(start)]

    def test_banner_is_quiet(self):
        r = self.result('ok')
        self.assertEqual(len(self.has(r['lines'], 'probe ok')), 1)  # one line per change of the result
        self.assertFalse(self.has(r['lines'], 'stall'))
        self.assertEqual((r['state']['result'], r['state']['cycle'], r['state']['failures_in_a_row']), ('ok', 3, 0))
        self.assertEqual(r['accepted'], 3)
        self.assertTrue(self.has(r['lines'], 'watch ends after 3 probes'))

    def test_stall_runs_forensics_then_heals_once_per_gap(self):
        r = self.result('stall')
        lines = r['lines']
        confirmed = self.has(lines, 'stall confirmed: 2 banner timeouts in a row')
        self.assertEqual(len(confirmed), 1)
        i = lines.index(confirmed[0])
        self.assertTrue(lines[i + 1].startswith('forensics ended with exit code 0'), lines)
        self.assertEqual(lines[i + 2], 'log-only: service sshd not restarted')
        self.assertTrue(lines[i + 3].startswith('after the heal: banner-timeout'), lines)
        self.assertEqual(len(self.has(lines, 'stall confirmed; no heal: the last heal was')), 1)
        self.assertEqual(len(r['forensics']), 1)
        self.assertIn('stub forensics minutes 15', r['forensics'][0])

    def test_one_timeout_between_banners_is_no_stall(self):
        r = self.result('transient')
        self.assertFalse(self.has(r['lines'], 'stall'))
        self.assertEqual(len(self.has(r['lines'], 'probe banner-timeout')), 2)

    def test_hourly_limit_and_forensics_limit(self):
        r = self.result('limit')
        self.assertTrue(self.has(r['lines'], 'forensics stopped at its limit of 2 s'), r['lines'])
        self.assertTrue(self.has(r['lines'], 'stall confirmed; no heal: 1 heals in the last hour, the limit is 1'))
        self.assertLess(r['seconds'], 60)  # the slow stub sleeps 60 s: it was stopped

    def test_refusals_are_not_stalls(self):
        for name, result in (('closed', 'probe closed'), ('junk', 'probe bad-banner'), ('refused', 'probe refused')):
            with self.subTest(name=name):
                r = self.result(name)
                self.assertTrue(self.has(r['lines'], result), r['lines'])
                self.assertFalse(self.has(r['lines'], 'stall'))
                self.assertEqual(r['state']['failures_in_a_row'], 0)

    def test_no_handshake_is_no_stall(self):
        r = self.result('refused-fast')
        self.assertTrue(self.has(r['lines'], 'probe connect-timeout') or self.has(r['lines'], 'probe refused'),
                        r['lines'])
        self.assertFalse(self.has(r['lines'], 'stall'))
        self.assertFalse(r['forensics'])

    def test_restart_needs_a_running_service(self):
        r = self.result('service')
        self.assertTrue(self.has(r['lines'], 'service bc250-no-such-service is not in the state Running: not restarted'))
        self.assertFalse(self.has(r['lines'], 'service bc250-no-such-service restarted'))

    def test_stop_file_ends_the_watch(self):
        base = self.tmp / 'stop'
        fake = FakeSshd('banner')
        self.addCleanup(fake.close)
        p = subprocess.Popen([POWERSHELL, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(WATCH), '-Port',
                              str(fake.port), '-Base', str(base), '-Heal', 'log-only', '-NoServiceCheck',
                              '-EverySeconds', '30'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.addCleanup(lambda: p.poll() is None and p.kill())
        state = base / 'sshd-watch' / 'state.json'
        end = time.monotonic() + 30
        while not state.exists() and time.monotonic() < end:
            time.sleep(0.2)
        (base / 'sshd-watch' / 'stop').write_text('stop', encoding='ascii')
        self.assertEqual(p.wait(15), 0)  # within the 30 s pause: the wait checks the stop file every second
        self.assertIn('stop file: the watch ends', (base / 'sshd-watch' / 'watch.log').read_text(encoding='utf-8'))

    def test_restart_without_the_service_check_is_refused(self):
        done = subprocess.run([POWERSHELL, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(WATCH),
                               '-Base', str(self.tmp / 'refuse'), '-NoServiceCheck', '-Cycles', '1'],
                              capture_output=True, text=True, timeout=60)
        self.assertNotEqual(done.returncode, 0)
        self.assertIn('a restart needs the service check', done.stdout + done.stderr)


if __name__ == '__main__':
    unittest.main()
