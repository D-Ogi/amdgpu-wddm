"""Held control channel of a game session (BD-108): one ssh session for the whole session instead of one per command.

    python tools/win/lab-runner/input/channel.py serve NAME SECONDS SCRIPT.ps1 [SCRIPT.ps1 ...]
    python tools/win/lab-runner/input/channel.py status NAME
    python tools/win/lab-runner/input/channel.py stop NAME

`serve` pushes channel-host.ps1 and the named scripts to C:\\BC250\\tmp\\channel\\NAME on the lab, holds one elevated
ssh session that runs channel-host.ps1 there, and passes requests to it until SECONDS (at most 1230) or `stop`.
Start it with the Bash tool's run_in_background, never with '&' inside a call. It has no window and no hotkey, and it
ends by itself at its bound. Open it only once the game session runs (after "Start Running" for a kit attempt): a held
session opened before a trial coincided with sshd accept stalls (BD-051). Tools such as inp.py call `call()`, which
uses the held session when one serves NAME and returns None otherwise, so the caller takes its one-connection path.

Why. One command through `target.py ps` is three ssh connections (a directory, the push, the run) plus an elevated
PowerShell start: 1.98 s median on an idle lab against 0.59 s for a bare round trip (2026-10-12), and about 10 s
beside a running game (b26 sessions 519-523). Through the held session a command costs only the lab-side script. The
held session also survives an sshd accept stall, which refuses only new connections (BD-051).

Each request appends one line to stamps.tsv in the channel directory, with the four stamps that split a round trip:
the client's request, the time the broker sent it, the time the lab received it and finished it, and the time the
broker read the reply. The local files live under <BC250_ROOT>\\scratch\\m15\\game-runner\\channel\\NAME, outside the
repository (BC250_CHANNEL_DIR names another directory).
"""
import base64
import json
import os
import re
import subprocess
import sys
import threading
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
WIN = HERE.parents[1]
REPO = HERE.parents[3]
ROOT = Path(os.environ.get('BC250_ROOT') or REPO.parent)
HOST_SCRIPT = HERE / 'channel-host.ps1'
NAME_RE = re.compile(r'^[a-z0-9][a-z0-9-]{2,47}$')
MAX_SECONDS = 1230          # the 1200 s game-session bound plus its closure
REPLY_TIMEOUT = 150         # one request; control-exec.ps1 waits up to its own TimeoutSec plus a picture


def utc():
    return datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%S.%f')[:-3] + 'Z'


def channel_dir(name):
    if not NAME_RE.match(name):
        raise ValueError(f'bad channel name {name!r}')
    base = Path(os.environ.get('BC250_CHANNEL_DIR') or ROOT / 'scratch/m15/game-runner/channel')
    return base / name


def write_json(path, value):
    tmp = path.with_name(path.name + '.tmp')
    tmp.write_text(json.dumps(value), encoding='utf-8')
    for _ in range(50):
        try:
            os.replace(tmp, path)
            return
        except PermissionError:
            time.sleep(0.02)
    os.replace(tmp, path)


def read_json(path):
    """A just-renamed file can be held open by another process for a moment (a scanner, on Windows), and the read then
    fails with a sharing violation: try again for up to one second before the error counts."""
    for _ in range(50):
        try:
            return json.loads(path.read_text(encoding='utf-8'))
        except PermissionError:
            time.sleep(0.02)
    return json.loads(path.read_text(encoding='utf-8'))


def unlink(path):
    for _ in range(50):
        try:
            path.unlink(missing_ok=True)
            return
        except PermissionError:
            time.sleep(0.02)
    path.unlink(missing_ok=True)


def pid_alive(pid):
    if os.name == 'nt':
        import ctypes
        handle = ctypes.windll.kernel32.OpenProcess(0x1000, False, int(pid))  # PROCESS_QUERY_LIMITED_INFORMATION
        if not handle:
            return False
        code = ctypes.c_ulong()
        ok = ctypes.windll.kernel32.GetExitCodeProcess(handle, ctypes.byref(code))
        ctypes.windll.kernel32.CloseHandle(handle)
        return bool(ok) and code.value == 259  # STILL_ACTIVE
    try:
        os.kill(int(pid), 0)
        return True
    except OSError:
        return False


def serving(name):
    """The broker record of NAME when a live broker serves it, else None."""
    d = channel_dir(name)
    try:
        rec = json.loads((d / 'broker.json').read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return None
    if rec.get('state') != 'serving' or time.time() >= rec.get('expires', 0) or not pid_alive(rec.get('pid', 0)):
        return None
    return rec


def call(name, script, params, timeout=REPLY_TIMEOUT):
    """Run a staged script through the held session. Returns {'exit', 'lines', 'stamps'}, or None when no broker
    serves NAME (the caller then uses its own path). A broker that ends during the request gives an 'exit' of None
    and an error line: the request is not sent again, because a command may not be repeatable."""
    if serving(name) is None:
        return None
    d = channel_dir(name)
    rid = uuid.uuid4().hex
    write_json(d / f'req-{rid}.json', {'id': rid, 'script': script, 'params': params, 'client_utc': utc()})
    reply = d / f'rep-{rid}.json'
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if reply.exists():
            rep = read_json(reply)
            unlink(reply)
            return rep
        if serving(name) is None and not reply.exists():
            # One last look: the broker writes the reply before it gives up its record.
            time.sleep(0.05)
            if reply.exists():
                continue
            request = d / f'req-{rid}.json'
            if request.exists():
                # The broker never took it, so nothing was sent: the caller may use its own path.
                unlink(request)
                return None
            return {'exit': None, 'lines': ['error: the held channel ended during the request; it was not sent again'],
                    'stamps': {}}
        time.sleep(0.02)
    return {'exit': None, 'lines': [f'error: no reply from the held channel in {timeout} s'], 'stamps': {}}


class Broker:
    """Holds the session. argv starts the far end (an ssh command line in production, a local powershell.exe in the
    host test); push(paths, remote_dir) stages the scripts there first."""

    def __init__(self, name, seconds, argv, push, remote_dir, scripts):
        self.name = name
        self.seconds = max(10, min(int(seconds), MAX_SECONDS))
        self.argv = argv
        self.push = push
        self.remote_dir = remote_dir
        self.scripts = [Path(s) for s in scripts]
        self.dir = channel_dir(name)
        self.proc = None
        self.lines = []
        self.cond = threading.Condition()
        self.closed = False

    def _reader(self):
        for raw in self.proc.stdout:
            line = raw.decode('utf-8', 'replace').rstrip('\r\n')
            with self.cond:
                self.lines.append(line)
                self.cond.notify_all()
        with self.cond:
            self.closed = True
            self.cond.notify_all()

    def _next_line(self, deadline):
        with self.cond:
            while not self.lines:
                if self.closed:
                    return None
                left = deadline - time.monotonic()
                if left <= 0:
                    return None
                self.cond.wait(min(left, 0.5))
            return self.lines.pop(0)

    def _record(self, state, **more):
        write_json(self.dir / 'broker.json', {'name': self.name, 'state': state, 'pid': os.getpid(),
                                              'started': self.started, 'expires': self.expires,
                                              'remote_dir': self.remote_dir, **more})

    def start(self):
        for s in self.scripts:
            if not s.is_file():
                raise FileNotFoundError(s)
        self.dir.mkdir(parents=True, exist_ok=True)
        for old in list(self.dir.glob('req-*.json')) + list(self.dir.glob('rep-*.json')):
            unlink(old)
        (self.dir / 'stop').unlink(missing_ok=True)
        self.started = time.time()
        self.expires = self.started + self.seconds
        self.push([str(HOST_SCRIPT)] + [str(s) for s in self.scripts], self.remote_dir)
        # The far end's stderr (ssh's own messages included) goes to a file of the channel, not into the replies.
        self.errlog = open(self.dir / 'far-end.err', 'ab')
        self.proc = subprocess.Popen(self.argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.errlog)
        self.reader = threading.Thread(target=self._reader, daemon=True)
        self.reader.start()
        line = self._next_line(time.monotonic() + 45)
        while line is not None and not line.startswith('@@CHANNEL READY'):
            line = self._next_line(time.monotonic() + 45)
        if line is None:
            self.close()
            raise RuntimeError('the far end did not report READY within 45 s')
        self._record('serving', ready=line)
        return line

    def _send(self, text):
        self.proc.stdin.write((text + '\n').encode('utf-8'))
        self.proc.stdin.flush()

    def handle(self, req_path):
        rid = req_path.stem[len('req-'):]
        try:
            req = read_json(req_path)
            if not re.fullmatch(r'[0-9a-f]{32}', rid) or req.get('id') != rid or not isinstance(req.get('script'), str):
                raise ValueError('request without its id or script')
        except (OSError, ValueError) as e:
            # One bad request file gets an error reply and does not end the session.
            unlink(req_path)
            write_json(self.dir / f'rep-{rid}.json', {'exit': None, 'lines': [f'error: unreadable request ({e})'],
                                                      'stamps': {}})
            return True
        unlink(req_path)
        body = base64.b64encode(json.dumps({'script': req['script'], 'params': req.get('params') or {}}).encode()).decode()
        sent = utc()
        out, inside, code, stamps = [], False, None, {'client': req.get('client_utc', ''), 'sent': sent}
        try:
            self._send(f'REQ {rid} {body}')
        except OSError:
            self.closed = True  # the pipe to the far end is gone; the loop below ends at once
        deadline = time.monotonic() + REPLY_TIMEOUT
        while True:
            line = self._next_line(deadline)
            if line is None:
                out.append('error: the far end ended or did not answer in time')
                break
            if line == '@@BAD request line':
                out.append('error: the far end refused the request line')
                break
            got = re.match(rf'^@@GOT {rid} (\S+)$', line)
            if got:
                # The far end has the request (a far end before 2026-11-02 sends no such line): a reply that then
                # does not come is a script that hangs, not a request lost on the way (bd108-k2).
                stamps['lab_got'] = got.group(1)
                continue
            if line == f'@@BEGIN {rid}':
                inside = True
                continue
            m = re.match(rf'^@@END {rid} (-?\d+) (\S+) (\S+)$', line)
            if m:
                code = int(m.group(1))
                stamps.update(lab_received=m.group(2), lab_done=m.group(3))
                break
            if inside:
                out.append(line)
        stamps['got'] = utc()
        write_json(self.dir / f'rep-{rid}.json', {'exit': code, 'lines': out, 'stamps': stamps})
        with open(self.dir / 'stamps.tsv', 'a', encoding='utf-8') as f:
            f.write('\t'.join([rid[:8], req['script'], stamps['client'], stamps['sent'], stamps.get('lab_received', ''),
                               stamps.get('lab_done', ''), stamps['got'], str(code)]) + '\n')
        return code is not None

    def serve(self, poll=0.02):
        """Until the bound, a stop file or the end of the far end. Returns the reason."""
        reason = 'bound'
        try:
            while time.time() < self.expires:
                if (self.dir / 'stop').exists():
                    reason = 'stop'
                    break
                if self.proc.poll() is not None or self.closed:
                    reason = 'far end ended'
                    break
                reqs = sorted(self.dir.glob('req-*.json'), key=lambda p: p.stat().st_mtime)
                if not reqs:
                    time.sleep(poll)
                    continue
                if not self.handle(reqs[0]):
                    reason = 'far end ended'
                    break
        except BaseException as e:
            reason = f'broker error: {e!r}'
            raise
        finally:
            self.close()
            self._record('ended', reason=reason, ended=utc())
        return reason

    def close(self):
        if self.proc is None:
            return
        try:
            if self.proc.poll() is None:
                self._send('QUIT')
                self.proc.stdin.close()
                self.proc.wait(timeout=10)
        except (OSError, subprocess.TimeoutExpired, ValueError):
            pass
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait(timeout=10)
        # The far end is gone: its pipes end, so the reader stops and every handle can be closed.
        self.reader.join(10)
        for stream in (self.proc.stdin, self.proc.stdout):
            try:
                stream.close()
            except (OSError, ValueError):
                pass
        self.errlog.close()


def lab_broker(name, seconds, scripts):
    sys.path.insert(0, str(WIN))
    import target as tgt
    t = tgt.Target()
    channel_dir(name)  # the name check, before it goes into a remote path
    seconds = max(10, min(int(seconds), MAX_SECONDS))
    remote = 'C:\\BC250\\tmp\\channel\\' + name
    command = (f'powershell -NoProfile -ExecutionPolicy Bypass -File "{remote}\\channel-host.ps1" '
               f'-Seconds {seconds} -Dir "{remote}"')
    # Keep-alive: the session lasts up to 20 minutes and must notice a dead link.
    argv = t.ssh_argv(command, options=('-o', 'ServerAliveInterval=15', '-o', 'ServerAliveCountMax=4'))
    return Broker(name, seconds, argv, t.push, remote, scripts)


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    cmd, name = argv[0], argv[1]
    if cmd == 'serve':
        if len(argv) < 4:
            raise SystemExit(__doc__)
        b = lab_broker(name, argv[2], [Path(s).resolve() for s in argv[3:]])
        # Each line is stamped when its step has ended: an f-string evaluates utc() before the call inside it, which
        # gave the end of a 90 s session the time of its start (bd108-k2, 2026-11-02).
        ready = b.start()
        print(f'{utc()} {ready}', flush=True)
        print(f'{utc()} channel {name} serving for {b.seconds} s; requests in {b.dir}', flush=True)
        reason = b.serve()
        print(f'{utc()} channel {name} ended: {reason}', flush=True)
        return 0
    if cmd == 'status':
        rec = serving(name)
        print(json.dumps(rec) if rec else f'no broker serves {name}')
        return 0 if rec else 1
    if cmd == 'stop':
        d = channel_dir(name)
        d.mkdir(parents=True, exist_ok=True)
        (d / 'stop').write_text(utc(), encoding='utf-8')
        print(f'stop requested for {name}')
        return 0
    raise SystemExit(__doc__)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
