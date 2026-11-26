"""Linux single-threaded C099 supervision; import has no process/device effects."""
import ctypes
import os
import signal
import subprocess
import time


def arm_parent_death(expected_parent):
    # fork clears PDEATHSIG. Call again in every child, before its first work.
    # These trials never change credentials or execute privileged/set-id files.
    libc = ctypes.CDLL(None, use_errno=True)
    libc.prctl.argtypes = [ctypes.c_int] + [ctypes.c_ulong] * 4
    libc.prctl.restype = ctypes.c_int
    if libc.prctl(1, signal.SIGKILL, 0, 0, 0):
        code = ctypes.get_errno()
        raise OSError(code, os.strerror(code))
    # Close the race where the parent died before prctl armed the signal.
    if os.getppid() != expected_parent:
        os._exit(125)


class StopLatch:
    def __init__(self):
        self.signum = None

    def install(self):
        for sig in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
            signal.signal(sig, self.receive)

    def receive(self, signum, frame):
        if self.signum is None:
            self.signum = signum

    def check(self):
        if self.signum is not None:
            raise RuntimeError('STOP signal ' + str(self.signum))


class TrialGuard:
    def __init__(self, stop, read_temperature, now=time.monotonic):
        self.stop = stop
        self.read_temperature = read_temperature
        self.now = now
        self.start = now()
        self.high_since = None
        self.temperatures = []

    def check(self, starting=False):
        self.stop.check()
        now = self.now()
        if now - self.start >= 90:
            raise RuntimeError('whole-trial 90s bound')
        temp = int(self.read_temperature())
        self.temperatures.append(temp)
        if temp < 0 or temp >= 97000:
            raise RuntimeError('invalid temperature or thermal97C stop')
        if starting and temp >= 87000:
            raise RuntimeError('start temperature at least87C')
        if temp >= 92000:
            if self.high_since is None:
                self.high_since = now
            if now - self.high_since >= 10:
                raise RuntimeError('thermal92C held10s stop')
        else:
            self.high_since = None
        self.stop.check()


def run_owned(argv, out, err, guard, bound):
    """Do not reap the session leader until its group has been stopped.

    WNOWAIT reserves the leader PID even after exit, so killpg cannot refer to
    a new group with a recycled ID. PDEATHSIG covers abrupt supervisor death.
    The single-threaded caller must install its STOP handlers first.
    """
    parent = os.getpid()
    # Launch commits when this guard returns. An asynchronous STOP after this
    # point cancels that committed operation at the next guard; it cannot undo
    # a kernel call already entered. Later launches remain forbidden.
    guard()  # after the caller's logging/file opens, immediately before launch
    proc = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=out, stderr=err,
                            start_new_session=True,
                            preexec_fn=lambda: arm_parent_death(parent))
    began = time.monotonic()
    try:
        while True:
            guard()
            status = os.waitid(os.P_PID, proc.pid,
                               os.WEXITED | os.WNOHANG | os.WNOWAIT)
            if status is not None:
                break
            if time.monotonic() - began >= bound:
                raise RuntimeError('owned process deadline')
            time.sleep(0.1)
    finally:
        # No poll()/wait() before this kill: our child still owns its PID.
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            raise RuntimeError('owned process did not exit after SIGKILL; reset required')
    guard()
    return proc.returncode
