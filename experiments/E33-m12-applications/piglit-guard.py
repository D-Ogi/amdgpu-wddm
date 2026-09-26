"""Run upstream piglit serially; retain its oracle and stop before the next case on failure."""
import json, os, sys, threading, time
from pathlib import Path

class CaseGate:
    def __init__(self, event_path):
        self.event_path = Path(event_path)
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

def main():
    from framework.test.base import Test
    from framework.programs.run import run
    gate = CaseGate(os.environ['BC250_PIGLIT_EVENTS'])
    Test.execute = gate.wrap(Test.execute)
    # Fixed serial execution and timeout; remaining arguments select the official profile/output.
    try:
        run(['-1','-j','1','--timeout','45','-p','wgl'] + sys.argv[1:])
    finally:
        gate.record({'event':'runner_end','executed':gate.executed,'stop_reason':gate.reason})
    return 1 if gate.reason else 0

if __name__ == '__main__':
    raise SystemExit(main())
