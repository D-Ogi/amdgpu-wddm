"""Run upstream piglit serially; retain its oracle and stop before the next case on failure."""
import json, os, sys, time, subprocess, re, urllib.request
from pathlib import Path

class CaseGate:
    def __init__(self, event_path, before=None):
        self.event_path = Path(event_path)
        self.before = before
        self.reason = None
        self.executed = 0
        self.seen = set()
    def record(self, value):
        with self.event_path.open('a', encoding='utf-8') as out:
            out.write(json.dumps(value) + '\n')
            out.flush()
    def wrap(self, original):
        gate = self
        def execute(test, name, log, options):
            if gate.reason:
                options['monitor']._abort_error = gate.reason
                return
            if gate.before:
                try:
                    gate.before()
                except Exception as error:
                    gate.reason = 'pre-case gate: ' + str(error)
                    options['monitor']._abort_error = gate.reason
                    gate.record({'event':'stop','reason':gate.reason,'next_case':name})
                    return
            if name in gate.seen:
                gate.reason = 'duplicate case: ' + name
                options['monitor']._abort_error = gate.reason
                gate.record({'event':'stop','reason':gate.reason})
                return
            gate.seen.add(name)
            gate.record({'event':'start','name':name,'utc':time.time()})
            original(test, name, log, options)
            result = str(test.result.result)
            gate.executed += 1
            gate.record({'event':'end','name':name,'result':result,'utc':time.time()})
            if result not in ('pass', 'skip'):
                gate.reason = name + ': ' + result
                options['monitor']._abort_error = gate.reason
                gate.record({'event':'stop','reason':gate.reason})
        return execute

class LabHealth:
    def __init__(self, cli):
        self.cli = cli
        self.last = 0
        self.identity = None
    def __call__(self):
        if time.monotonic() - self.last < 5:
            return
        with urllib.request.urlopen('http://127.0.0.1:2250/flags', timeout=3) as response:
            if json.load(response).get('stop'):
                raise RuntimeError('Owner STOP')
        flags = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
        health = subprocess.check_output([self.cli,'health','read'], timeout=4, text=True, creationflags=flags)
        clock = subprocess.check_output([self.cli,'clock','read'], timeout=4, text=True, creationflags=flags)
        h = re.search(r'flags=15 generation=(\d+) epoch=(\d+)', health)
        c = re.search(r'MHz=1000 VID=116 temperature_mc=(\d+) ready=1', clock)
        if not h or not c or int(c[1]) >= 85000:
            raise RuntimeError('GPU health/clock/temperature gate')
        identity = h.groups()
        if self.identity is not None and identity != self.identity:
            raise RuntimeError('GPU session changed')
        self.identity = identity
        self.last = time.monotonic()

def main():
    from framework.test.base import Test
    from framework.programs.run import run
    gate = CaseGate(os.environ['BC250_PIGLIT_EVENTS'], LabHealth(os.environ['BC250_CLI']))
    Test.execute = gate.wrap(Test.execute)
    # Fixed serial execution and timeout; remaining arguments select the official profile/output.
    try:
        run(['-1','-j','1','--timeout','45','-p','wgl'] + sys.argv[1:])
    finally:
        gate.record({'event':'runner_end','executed':gate.executed,'stop_reason':gate.reason})
    return 1 if gate.reason else 0

if __name__ == '__main__':
    raise SystemExit(main())
