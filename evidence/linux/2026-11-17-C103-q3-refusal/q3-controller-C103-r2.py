"""Independent C103 supervisor. No DMA, module load, unlock or activation."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import runpy
import struct
import sys
import time

HERE = Path(__file__).resolve().parent
BUILD_ID = 'd073d7b7da060ba3dc9d883f76438aa242cd28b1'
PREFLIGHT_SHA = '48c3dcd0bc53788ade998065cc8daa051f1945903558a14f7d0576dd4642408e'
HELPER_SHA = 'f419ae59fb8d2177fecef5c52549873487572c1a56db04bf418df929ba5c1d2d'


def live_build_ids():
    raw = Path('/sys/kernel/notes').read_bytes()
    off = 0
    ids = []
    while off + 12 <= len(raw):
        n, d, t = struct.unpack_from('<III', raw, off)
        off += 12
        if off + ((n+3)&~3) + ((d+3)&~3) > len(raw):
            raise RuntimeError('invalid kernel notes')
        name = raw[off:off+n]
        off += (n+3)&~3
        desc = raw[off:off+d]
        off += (d+3)&~3
        if name.rstrip(b'\0') == b'GNU' and t == 3:
            ids.append(desc.hex())
    return ids


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--execute', action='store_true')
    args = p.parse_args()
    pins = json.loads((HERE/'pins.json').read_text())
    for name, digest in pins['files'].items():
        if Path(name).name != name or hashlib.sha256((HERE/name).read_bytes()).hexdigest() != digest:
            raise RuntimeError('runtime source pin changed')
    if not args.execute:
        print('PLAN: exact kernel/preflight, one bounded q3 control worker, no DMA/unlock/UVD; reset after trial.')
        return 0
    if sys.flags.optimize or sys.platform != 'linux':
        raise RuntimeError('requires Linux without -O')
    if os.uname().release != '6.18.52-0-lts' or live_build_ids() != [BUILD_ID]:
        raise RuntimeError('kernel identity mismatch')
    if Path('/sys/module/dma_provider_C096').exists():
        raise RuntimeError('old DMA provider still present')
    result_path = HERE/'result.json'
    for name in ('result.json', 'events.jsonl', 'control.jsonl', 'control-status.json', 'control.stdout', 'control.stderr'):
        if (HERE/name).exists():
            raise RuntimeError('prior trial exists; no retry')
    preflight = Path('/root/dpm-preflight.py')
    if hashlib.sha256(preflight.read_bytes()).hexdigest() != PREFLIGHT_SHA:
        raise RuntimeError('preflight pin changed')
    helper = HERE/'dma_process_C099.py'
    if hashlib.sha256(helper.read_bytes()).hexdigest() != HELPER_SHA:
        raise RuntimeError('process helper changed')
    helpers = runpy.run_path(str(helper))
    stop = helpers['StopLatch']()
    stop.install()
    stop.check()
    # The worker alone holds the cooperative q0 lock.
    env = runpy.run_path(str(preflight))
    monitor = helpers['TrialGuard'](stop, env['sensor'].read_text)
    attempted = False
    outcome = dict(passed=False, incomplete=True, no_activation=True)
    with (HERE/'events.jsonl').open('x', encoding='utf-8', buffering=1) as log:
        def emit(row):
            log.write(json.dumps(dict(monotonic=time.monotonic(), **row))+'\n')

        try:
            monitor.check(starting=True)
            emit(dict(kind='preflight', kernel=os.uname().release, build_id=BUILD_ID, identity=env['info']))
            argv = [sys.executable, '-I', '-B', str(HERE/'q3-control-C103-r2.py'),
                    '--execute', '--kit', '/root/bc250-t001-q5-control-r1',
                    '--preflight', str(preflight), '--output', str(HERE/'control.jsonl'),
                    '--status', str(HERE/'control-status.json')]
            emit(dict(kind='process-start', label='control'))
            attempted = True
            with (HERE/'control.stdout').open('xb') as out, (HERE/'control.stderr').open('xb') as err:
                rc = helpers['run_owned'](argv, out, err, monitor.check, 25)
            monitor.check()
            if (HERE/'control.stdout').stat().st_size > 1048576 or (HERE/'control.stderr').stat().st_size > 65536:
                raise RuntimeError('unexpected worker output size')
            emit(dict(kind='process-result', label='control', exit=rc))
            if rc:
                raise RuntimeError('worker failed; no retry')
            status = json.loads((HERE/'control-status.json').read_text())
            if status.get('state') != 'finished' or status.get('intended_exit') != 0 or status.get('outcome',{}).get('passed') is not True:
                raise RuntimeError('worker did not complete successfully')
            for key in ('no_activation', 'no_unlock', 'no_dma', 'reset_required'):
                if status.get(key) is not True:
                    raise RuntimeError('worker scope field mismatch: '+key)
            monitor.check()
            outcome = dict(passed=True, no_activation=True, no_unlock=True, no_dma=True,
                           reset_required=True, min_tctl_mC=min(monitor.temperatures),
                           max_tctl_mC=max(monitor.temperatures), elapsed_s=time.monotonic()-monitor.start)
        except BaseException as exc:
            outcome = dict(passed=False, no_activation=True, no_unlock=True, no_dma=True,
                           error=type(exc).__name__+': '+str(exc), worker_attempted=attempted,
                           reset_required=attempted, no_retry=True)
        finally:
            with result_path.open('x', encoding='utf-8') as f:
                json.dump(outcome, f, indent=2)
                f.flush()
                os.fsync(f.fileno())
            emit(dict(kind='result', **outcome))
            os.fsync(log.fileno())
    print(json.dumps(outcome), flush=True)
    return 0 if outcome['passed'] else 2


if __name__ == '__main__':
    raise SystemExit(main())
