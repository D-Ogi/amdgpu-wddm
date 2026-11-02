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

Self-checks (bd108-k2, 2026-11-02: no request line came through the session's stdin in 90 s, while the far end's
output came at once; in bd108-p1 a synchronous PowerShell reader of that stdin got one line and then none, an
asynchronous one got none). The broker sends no request before the far end answers two PINGs, one after the other
(PONG_S each). It tries the transports in order, each in its own ssh session: 'tcp' (one TCP connection to a
listener on the lab's loopback, through the same ssh connection with ssh -D, so the session's stdin is not used),
then 'stdin' (the session's own stdin and stdout). When no transport answers, the broker records 'ended' and every caller takes its own path. A request
that the far end does not confirm (@@GOT) in GOT_S ends the session, and the far end refuses a request that comes
after its limit, so a held request never acts late. A request whose client has gone or whose time is over is dropped
before it is sent. The PONG also gives the offset of the lab clock (broker.json 'clock'): the two machines' stamps
are compared only through it.
"""
import base64
import json
import os
import re
import socket
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
READY_S = 45                # the far end's READY line after the ssh start
PONG_S = 6                  # the handshake; on this PC the answer takes milliseconds
GOT_S = 10                  # the far end confirms a request at once, before its script runs
PINGS = 2                   # the handshake proves that more than one line comes through
# tcp first: on the lab no PowerShell reader got more than one line through the session's stdin (bd108-p1).
TRANSPORTS = ('tcp', 'stdin')


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
    and an error line: the request is not sent again, because a command may not be repeatable. The request names
    this process and its time limit: the broker drops it when either is gone (a client killed at its own bound left
    four requests behind in bd108-k2)."""
    if serving(name) is None:
        return None
    d = channel_dir(name)
    rid = uuid.uuid4().hex
    request = d / f'req-{rid}.json'
    write_json(request, {'id': rid, 'script': script, 'params': params, 'client_utc': utc(), 'pid': os.getpid(),
                         'expires': time.time() + timeout})
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
    unlink(request)  # not taken yet: it must not go out after this caller has given up
    return {'exit': None, 'lines': [f'error: no reply from the held channel in {timeout} s'], 'stamps': {}}


class Lines:
    """Lines of one stream, read by a thread; get() waits up to a deadline."""

    def __init__(self, stream=None):
        self.items, self.closed, self.cond = [], False, threading.Condition()
        if stream is not None:
            self.thread = threading.Thread(target=self.feed, args=(stream,), daemon=True)
            self.thread.start()

    def feed(self, stream):
        try:
            for raw in stream:
                with self.cond:
                    self.items.append(raw.decode('utf-8', 'replace').rstrip('\r\n'))
                    self.cond.notify_all()
        except (OSError, ValueError):
            pass
        finally:
            with self.cond:
                self.closed = True
                self.cond.notify_all()

    def get(self, deadline):
        with self.cond:
            while not self.items:
                if self.closed:
                    return None
                left = deadline - time.monotonic()
                if left <= 0:
                    return None
                self.cond.wait(min(left, 0.5))
            return self.items.pop(0)


class SocketWriter:
    def __init__(self, sock):
        self.sock = sock

    def write(self, data):
        self.sock.sendall(data)

    def flush(self):
        pass

    def close(self):
        try:
            self.sock.shutdown(socket.SHUT_WR)
        except OSError:
            pass


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def recv_exact(sock, n):
    data = b''
    while len(data) < n:
        part = sock.recv(n - len(data))
        if not part:
            raise OSError('the proxy closed the connection')
        data += part
    return data


def socks_connect(proxy_port, port, timeout=5):
    """A TCP connection to 127.0.0.1:port on the lab, through the SOCKS5 listener of 'ssh -D 127.0.0.1:proxy_port'.
    The listener can come a moment after the session starts: try for up to `timeout` seconds."""
    end = time.monotonic() + timeout
    while True:
        try:
            s = socket.create_connection(('127.0.0.1', proxy_port), timeout=timeout)
            break
        except OSError:
            if time.monotonic() >= end:
                raise
            time.sleep(0.1)
    try:
        s.sendall(b'\x05\x01\x00')
        if recv_exact(s, 2) != b'\x05\x00':
            raise OSError('the proxy refused the greeting')
        s.sendall(b'\x05\x01\x00\x01' + socket.inet_aton('127.0.0.1') + int(port).to_bytes(2, 'big'))
        reply = recv_exact(s, 10)
        if reply[1] != 0:
            raise OSError(f'the proxy refused the connection (SOCKS reply {reply[1]})')
    except BaseException:
        s.close()
        raise
    s.settimeout(None)
    return s


def direct_connect(port, timeout=5):
    """The host tests' far end runs on this PC: its listener is reached directly."""
    s = socket.create_connection(('127.0.0.1', int(port)), timeout=timeout)
    s.settimeout(None)
    return s


def epoch_of(stamp):
    return datetime.strptime(stamp, '%Y-%m-%dT%H:%M:%S.%fZ').replace(tzinfo=timezone.utc).timestamp()


class Broker:
    """Holds the session. argv starts the far end of the 'stdin' transport (an ssh command line in production, a local
    powershell.exe in the host test); tcp is (argv, connect) for the 'tcp' transport, where connect(port) returns a
    socket to the far end's listener; order names the transports to try. push(paths, remote_dir) stages the scripts
    first."""

    def __init__(self, name, seconds, argv, push, remote_dir, scripts, tcp=None, order=None):
        self.name = name
        self.seconds = max(10, min(int(seconds), MAX_SECONDS))
        self.argv = argv
        self.tcp = tcp
        known = {'stdin': argv is not None, 'tcp': tcp is not None}
        self.order = [k for k in (order or TRANSPORTS) if known.get(k)]
        self.push = push
        self.remote_dir = remote_dir
        self.scripts = [Path(s) for s in scripts]
        self.dir = channel_dir(name)
        self.proc = None
        self.sock = None
        self.ended = True
        self.live = False
        self.errlog = None
        self.transport = None
        self.attempts = []
        self.clock = None
        self.fail_reason = None

    def _record(self, state, **more):
        write_json(self.dir / 'broker.json', {'name': self.name, 'state': state, 'pid': os.getpid(),
                                              'started': self.started, 'expires': self.expires,
                                              'remote_dir': self.remote_dir, 'transport': self.transport,
                                              'attempts': self.attempts, 'clock': self.clock, **more})

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
        for kind in self.order:
            info = self._open(kind)
            self.attempts.append(info)
            if 'failed' not in info:
                self.transport = kind
                self._record('serving', ready=self.ready)
                return self.ready
            self._shut()
        reason = 'no transport answered the handshake: ' + '; '.join(f"{a['transport']}: {a['failed']}"
                                                                     for a in self.attempts)
        self.errlog.close()
        self._record('ended', reason=reason, ended=utc())
        raise RuntimeError(reason)

    def _open(self, kind):
        info = {'transport': kind, 'utc': utc()}
        t0 = time.monotonic()
        self.ended, self.live, self.writer, self.sock = False, False, None, None
        self.proc = subprocess.Popen(self.argv if kind == 'stdin' else self.tcp[0], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=self.errlog)
        self.ctl = Lines(self.proc.stdout)
        line = self.ctl.get(t0 + READY_S)
        while line is not None and not line.startswith('@@CHANNEL READY'):
            line = self.ctl.get(t0 + READY_S)
        if line is None:
            info['failed'] = f'no READY line in {READY_S} s'
            return info
        info['ready_s'] = round(time.monotonic() - t0, 3)
        self.ready = line
        try:
            if kind == 'stdin':
                self.data, self.writer = self.ctl, self.proc.stdin
            else:
                m = re.search(r' tcp (\d+) ([0-9a-f]{32})$', line)
                if not m:
                    info['failed'] = 'READY without a port and token'
                    return info
                self.sock = self.tcp[1](int(m.group(1)))
                self.writer = SocketWriter(self.sock)
                self.data = Lines(self.sock.makefile('rb'))
                self._send(f'HELLO {m.group(2)}')
        except OSError as e:
            info['failed'] = f'{type(e).__name__}: {e}'
            return info
        # Two PINGs, one after the other: on the lab a synchronous reader of the session's stdin got its first line
        # and then no other, not even at the end of stdin (bd108-p1 variant C, 2026-11-02). One answer proves little.
        info['pong_s'] = []
        for n in range(1, PINGS + 1):
            nonce = uuid.uuid4().hex[:16]
            a = time.time()
            try:
                self._send(f'PING {nonce}')
            except OSError as e:
                info['failed'] = f'PING {n} of {PINGS}: {type(e).__name__}: {e}'
                return info
            deadline = time.monotonic() + PONG_S
            while True:
                line = self.data.get(deadline)
                if line is None:
                    why = 'the far end ended' if self.data.closed else f'no answer in {PONG_S} s'
                    info['failed'] = f'PONG {n} of {PINGS}: {why}'
                    return info
                m = re.match(rf'^@@PONG {nonce} (\S+)$', line)
                if m:
                    break
            b = time.time()
            info['pong_s'].append(round(b - a, 3))
            # The last PONG gives the lab clock offset, from the middle of its round trip.
            self.clock = {'offset_s': round(epoch_of(m.group(1)) - (a + b) / 2, 3), 'rtt_s': round(b - a, 3)}
        self.live = True
        return info

    def _send(self, text):
        self.writer.write((text + '\n').encode('utf-8'))
        self.writer.flush()

    def _next_line(self, deadline):
        return self.data.get(deadline)

    def _drop(self, req_path, rid, req, why):
        unlink(req_path)
        write_json(self.dir / f'rep-{rid}.json', {'exit': None, 'lines': [f'error: {why}; it was not sent'], 'stamps': {}})
        with open(self.dir / 'stamps.tsv', 'a', encoding='utf-8') as f:
            f.write('\t'.join([rid[:8], str(req.get('script')), str(req.get('client_utc', '')), '', '', '', utc(),
                               'dropped']) + '\n')

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
        # A request that nobody waits for any more is not sent: its client has gone, or its time is over.
        if req.get('pid') and not pid_alive(req['pid']):
            self._drop(req_path, rid, req, 'the client has gone')
            return True
        if req.get('expires') and time.time() > float(req['expires']):
            self._drop(req_path, rid, req, 'the request expired before the far end was free')
            return True
        unlink(req_path)
        # The far end refuses it after GOT_S on its own clock: a request held on the way never acts late.
        not_after_ms = int((time.time() + (self.clock or {}).get('offset_s', 0) + GOT_S) * 1000)
        body = base64.b64encode(json.dumps({'script': req['script'], 'params': req.get('params') or {},
                                            'not_after_ms': not_after_ms}).encode()).decode()
        sent = utc()
        out, inside, code, stamps = [], False, None, {'client': req.get('client_utc', ''), 'sent': sent}
        confirmed = False
        try:
            self._send(f'REQ {rid} {body}')
        except OSError:
            self.data.closed = True  # the way to the far end is gone; the loop below ends at once
        got_deadline = time.monotonic() + GOT_S
        deadline = time.monotonic() + REPLY_TIMEOUT
        while True:
            line = self._next_line(deadline if confirmed else got_deadline)
            if line is None:
                if not confirmed and not self.data.closed:
                    out.append(f'error: the far end did not confirm the request in {GOT_S} s (it refuses it after '
                               f'that, so it does not run late)')
                    self.fail_reason = 'far end did not confirm a request'
                else:
                    out.append('error: the far end ended or did not answer in time')
                break
            if line == '@@BAD request line':
                out.append('error: the far end refused the request line')
                break
            got = re.match(rf'^@@GOT {rid} (\S+)$', line)
            if got:
                # The far end has the request: a reply that then does not come is a script that hangs, not a request
                # lost on the way (bd108-k2).
                stamps['lab_got'] = got.group(1)
                confirmed = True
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
                if self.proc.poll() is not None or self.data.closed:
                    reason = 'far end ended'
                    break
                reqs = sorted(self.dir.glob('req-*.json'), key=lambda p: p.stat().st_mtime)
                if not reqs:
                    time.sleep(poll)
                    continue
                if not self.handle(reqs[0]):
                    reason = self.fail_reason or 'far end ended'
                    break
        except BaseException as e:
            reason = f'broker error: {e!r}'
            raise
        finally:
            self.close()
            self._record('ended', reason=reason, ended=utc())
        return reason

    def _shut(self):
        """Ends the current far end: QUIT, the end of its input, then a kill after 10 s. A far end that failed its
        handshake cannot hear QUIT, so its ssh ends at once (the far end ends by itself in -FirstLineSeconds)."""
        if self.proc is None or self.ended:
            return
        self.ended = True
        if self.live:
            try:
                if self.proc.poll() is None:
                    self._send('QUIT')
                    self.writer.close()
            except (OSError, ValueError, AttributeError):
                pass
            try:
                self.proc.stdin.close()
                self.proc.wait(timeout=10)
            except (OSError, subprocess.TimeoutExpired, ValueError):
                pass
        else:
            try:
                self.proc.stdin.close()
            except (OSError, ValueError):
                pass
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait(timeout=10)
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None
        # The far end is gone: its pipes end, so the reader stops and every handle can be closed.
        self.ctl.thread.join(10)
        try:
            self.proc.stdout.close()
        except (OSError, ValueError):
            pass

    def close(self):
        self._shut()
        if self.errlog is not None:
            self.errlog.close()


KEEPALIVE = ('-o', 'ServerAliveInterval=15', '-o', 'ServerAliveCountMax=4')


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
    argv = t.ssh_argv(command, options=KEEPALIVE)
    # The 'tcp' transport: a SOCKS listener of this ssh connection on this PC's loopback, to the far end's listener
    # on the lab's loopback. ExitOnForwardFailure: no session at all when the listener cannot open here.
    proxy = free_port()
    tcp_argv = t.ssh_argv(command + ' -Tcp', options=KEEPALIVE + ('-o', 'ExitOnForwardFailure=yes',
                                                                  '-D', f'127.0.0.1:{proxy}'))
    order = [k for k in os.environ.get('BC250_CHANNEL_TRANSPORTS', ','.join(TRANSPORTS)).split(',') if k]
    return Broker(name, seconds, argv, t.push, remote, scripts, tcp=(tcp_argv, lambda port: socks_connect(proxy, port)),
                  order=order)


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
        try:
            ready = b.start()
        except RuntimeError as e:
            # broker.json says 'ended': every caller takes its own path.
            print(f'{utc()} channel {name} not opened: {e}', flush=True)
            return 3
        print(f'{utc()} {ready}', flush=True)
        print(f'{utc()} channel {name} serving for {b.seconds} s over {b.transport} (lab clock {b.clock}); '
              f'requests in {b.dir}', flush=True)
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
