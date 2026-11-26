"""C-103: two q0 signatures through stock q3/2A; no unlock, DMA or activation."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import runpy
import signal
import sys
import time

KIT_SHA = '94c3800d0936ee114c633ba052beef0a76d5a1876418339657c9cd9892114873'
PREFLIGHT_SHA = '48c3dcd0bc53788ade998065cc8daa051f1945903558a14f7d0576dd4642408e'


def main():
    if sys.flags.optimize:
        raise RuntimeError('assertion-based pinned preflight requires optimization disabled')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--execute', action='store_true')
    parser.add_argument('--kit', type=Path, required=True)
    parser.add_argument('--preflight', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--status', type=Path, required=True)
    args = parser.parse_args()
    support = {'q5-gpu-transport-C085.py': 'ac1d51389462c5c4bc57a04b218a2c55a27262074a0464b16b84dc65f0c03e48', 'q5-gpu-offsets-C085.json': '5545b3380af531ebbc2ce1ce3e6bcf9a7913fb9328f04925e4689103fdc41e87', 'q3-control-core-C103.py': 'b3da1415d44b1810815955001d52c444b6a5174f2197344a633e8f4b7f8fc50c', 'q3-control-registers-C103.json': 'c31d1c41cea1fa875e11a36b090ac3a8b99a432368e2dc8d8c5d4d787066db20'}
    for name,digest in support.items():
        if hashlib.sha256((Path(__file__).parent/name).read_bytes()).hexdigest() != digest:
            raise RuntimeError('support pin changed: '+name)
    raw_manifest = (args.kit / 'SHA256.json').read_bytes()
    if hashlib.sha256(raw_manifest).hexdigest() != KIT_SHA:
        raise RuntimeError('q5 kit manifest changed')
    for name, digest in json.loads(raw_manifest)['files'].items():
        if Path(name).name != name or hashlib.sha256((args.kit/name).read_bytes()).hexdigest() != digest:
            raise RuntimeError('kit file changed: ' + name)
    if hashlib.sha256(args.preflight.read_bytes()).hexdigest() != PREFLIGHT_SHA:
        raise RuntimeError('operator preflight changed')
    if not args.execute:
        print('PLAN: q0 version + two signatures, exactly two q3/2A reads of generated q0 parameter; no unlock/DMA/UVD/activation; reset required.')
        return 0
    if sys.platform != 'linux' or not args.output.is_absolute() or not args.status.is_absolute() or args.status.exists() or args.output.exists() or args.status.resolve() == args.output.resolve():
        raise RuntimeError('requires Linux and absolute new output')
    import fcntl
    def local_module(name, filename):
        spec = importlib.util.spec_from_file_location(name, Path(__file__).parent/filename)
        result = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(result)
        return result
    gpu = local_module('c085_gpu','q5-gpu-transport-C085.py')
    log = args.output.open('x', encoding='utf-8', buffering=1)
    def emit(event):
        line = json.dumps(event, sort_keys=True)
        log.write(line+'\n')
        print(line, flush=True)
    diagnostic = local_module('c103_core','q3-control-core-C103.py')
    transport = None
    code = 2
    stop_requested = False
    outcome = dict(passed=False, reason='incomplete')
    lock = open('/run/lock/bc250-smu-q0-observer.lock', 'a')
    class AbortTrial(RuntimeError):
        pass
    def stop(signum, frame):
        nonlocal stop_requested
        stop_requested = True  # Persistent STOP; no further commands after observation STOP.
    for sig in (signal.SIGALRM, signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(sig, stop)
    started = time.monotonic()
    signal.alarm(15)
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        env = runpy.run_path(str(args.preflight))
        # Supplement the older preflight's process patterns for these new kits.
        patterns = (b'probe.py', b'q5-gpu', b'ryzen_smu', b'smu.py')
        for proc in Path('/proc').iterdir():
            if not proc.name.isdigit() or int(proc.name) == os.getpid():
                continue
            try:
                cmd = (proc/'cmdline').read_bytes().split(b'\0')
            except (FileNotFoundError, ProcessLookupError):
                continue
            if any(pattern in arg for pattern in patterns for arg in cmd):
                raise RuntimeError('competing SMU process pid ' + proc.name)
        emit(dict(kind='preflight', approval='C-103', **env['info']))
        checked_guard = env['stock'].Guard(env['dev'], env['sensor'], emit,
                                          expected_pci=bytes.fromhex(env['info']['pci_guard']))
        def guard():
            nonlocal stop_requested
            if stop_requested or time.monotonic() - started >= 15:
                raise AbortTrial('deadline or signal; no new I/O')
            try:
                checked_guard()
            except Exception as exc:
                stop_requested = True
                raise AbortTrial('guard stopped trial: ' + str(exc)) from exc
            # STOP/deadline can arrive while the legacy guard reads sysfs or logs.
            if stop_requested or time.monotonic() - started >= 15:
                raise AbortTrial('deadline or signal during guard; no new I/O')
        guard()
        register_data = json.loads((Path(__file__).parent/'q3-control-registers-C103.json').read_text())
        registers = register_data['queues']
        if set(registers) != {'0','3'} or register_data['target'] != registers['0']['parameter']:
            raise RuntimeError('wrong generated mapping target')
        addresses = [address for q in registers.values() for address in q.values()]
        if len(set(addresses)) != 6:
            raise RuntimeError('mailbox address overlap')
        guard()
        offsets = json.loads((Path(__file__).parent/'q5-gpu-offsets-C085.json').read_text())['offsets']
        transport = gpu.GpuIndirectTransport(env['dev'], env['info']['bar5_size'],
                    addresses, offsets, env['STOCK']/'mmio32.so', emit)
        transport.open()
        class Backend:
            def read(self, queue, name):
                if queue not in (0,3) or name not in ('parameter','response','command'):
                    raise RuntimeError('read outside fixed mailboxes')
                guard()
                address = registers[str(queue)][name]
                event = dict(kind='smn-io', queue=queue, operation='read', name=name, address=address)
                emit(dict(event, phase='before', monotonic=time.monotonic()))
                guard()
                value = transport.read_smu_reg(address)
                emit(dict(event, phase='after', value=value, monotonic=time.monotonic()))
                guard()
                return value
            def write(self, queue, name, value):
                if queue not in (0,3) or type(value) is not int:
                    raise RuntimeError('invalid write')
                permitted = (name == 'response' and value == 0) or (
                    name == 'command' and value in ((1,2) if queue == 0 else (0x2A,))) or (
                    name == 'parameter' and value in ((0,) + diagnostic.CHALLENGES if queue == 0 else (register_data['target'],)))
                if not permitted:
                    raise RuntimeError('write outside fixed protocol')
                guard()
                address = registers[str(queue)][name]
                event = dict(kind='smn-io', queue=queue, operation='write', name=name, address=address, value=value)
                emit(dict(event, phase='before', monotonic=time.monotonic()))
                guard()
                transport.write_smu_reg(address, value)
                emit(dict(event, phase='after', monotonic=time.monotonic()))
                guard()
        observation = diagnostic.run(Backend(), register_data['target'], emit, guard)
        guard()
        outcome = dict(passed=True, observation=observation, no_activation=True, no_unlock=True, no_dma=True)
        emit(dict(kind='result', **outcome))
        guard()
        code = 0
    except BaseException as exc:
        outcome = dict(passed=False, error_type=type(exc).__name__, error=str(exc), no_retry=True)
        try:
            emit(dict(kind='result', **outcome))
        except BaseException:
            pass
    finally:
        signal.alarm(0)
        status = dict(state='finished', intended_exit=code, reset_required=True,
                      pid=os.getpid(), outcome=outcome,
                      no_activation=True, no_unlock=True, no_dma=True, c097_withdrawal_retained=True)
        try:
            with args.status.open('x', encoding='utf-8') as stream:
                json.dump(status, stream, indent=2)
                stream.flush()
                os.fsync(stream.fileno())
            emit(dict(kind='lifecycle', **status))
            os.fsync(log.fileno())
        finally:
            try:
                if transport is not None:
                    transport.close()
            finally:
                lock.close()
                log.close()
    return code


if __name__ == '__main__':
    raise SystemExit(main())
