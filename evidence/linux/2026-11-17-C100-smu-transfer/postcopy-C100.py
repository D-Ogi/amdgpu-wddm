"""C-100: two fixed data controls using retained C096 slots; no activation (C097)."""
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
    support = {'q5-gpu-transport-C085.py': 'ac1d51389462c5c4bc57a04b218a2c55a27262074a0464b16b84dc65f0c03e48', 'q5-gpu-offsets-C085.json': '5545b3380af531ebbc2ce1ce3e6bcf9a7913fb9328f04925e4689103fdc41e87', 'setup-registers-C092.json': 'ea82a7da832efb9f1de82c4e7fa3a5f41c3f56634a8688f4853afc8b6d5e9896', 'setup-descriptor-core-C092.py': '6ccb049edbdab3bace940a685b3aeb5d68f4fd09786e2aa697197f11a732a8fd', 'postcopy-core-C100.py': 'eca11ee63e6758fd9713bdadb450f150859069f8aa0b3946a7a296c9d3c69fca', 'postcopy-buffers-C100.py': 'b1be71d2f26325c42246e6ad3f9523f5f0b7085cba8bcf3514b3b98129994154'}
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
        print('PLAN: q0 controls, setup00+restore04, two LOAD-refusal/data controls; sub14 requires1/3; kernel slots retained until AC; no unlock/activation.')
        return 0
    if sys.platform != 'linux' or not args.output.is_absolute() or not args.status.is_absolute() or args.status.exists() or args.output.exists() or args.status.resolve() == args.output.resolve():
        raise RuntimeError('requires Linux and absolute new output')
    import fcntl
    # Import exactly the verified q5 kit, never an ambient module.
    def module(name, filename):
        spec = importlib.util.spec_from_file_location(name, args.kit/filename)
        result = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(result)
        return result
    core = module('c082_core', 'observer_core.py')
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
    buffer_module = local_module('c100_buffers','postcopy-buffers-C100.py')
    diagnostic = local_module('c100_core','postcopy-core-C100.py')
    device = None
    transport = None
    page = None
    code = 2
    stop_requested = False
    outcome = dict(passed=False, reason='incomplete')
    lock = open('/run/lock/bc250-smu-q0-observer.lock', 'a')
    class AbortTrial(RuntimeError):
        pass
    def stop(signum, frame):
        nonlocal stop_requested
        stop_requested = True  # Persistent STOP; kernel owns exposed allocations.
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
        emit(dict(kind='preflight', approval='C-100', **env['info']))
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
        guard()
        if Path('/sys/module/dma_provider_C096/refcnt').read_text().strip() != '0':
            raise RuntimeError('requires fresh unexposed C096 module')
        device = buffer_module.Device()
        page = buffer_module.Buffers(device, emit, guard)
        guard()
        registers = json.loads((Path(__file__).parent/'setup-registers-C092.json').read_text())['queues']
        addresses = [registers['0'][name] for name in ('command','response','parameter')]
        addresses += list(registers['2'].values())
        guard()
        offsets = json.loads((Path(__file__).parent/'q5-gpu-offsets-C085.json').read_text())['offsets']
        transport = gpu.GpuIndirectTransport(env['dev'], env['info']['bar5_size'],
                    addresses, offsets, env['STOCK']/'mmio32.so', emit)
        transport.open()
        class Backend:
            def __init__(self, queue):
                self.queue = queue
            def read(self, name):
                guard()
                address = registers[str(self.queue)][name]
                event = dict(kind='smn-io', queue=self.queue, operation='read', name=name, address=address)
                emit(dict(event, phase='before', monotonic=time.monotonic()))
                guard()
                value = transport.read_smu_reg(address)
                emit(dict(event, phase='after', value=value, monotonic=time.monotonic()))
                guard()
                return value
            def write(self, name, value):
                if self.queue == 2:
                    if not ((name == 'command' and value == 0x0A) or
                            (name == 'response' and value == 0) or
                            (name in ['arg'+str(i) for i in range(6)] and type(value) is int and 0 <= value <= 0xFFFFFFFF)):
                        raise RuntimeError('q2 write outside diagnostic mailbox')
                elif self.queue != 0:
                    raise RuntimeError('queue not allowed')
                guard()
                address = registers[str(self.queue)][name]
                event = dict(kind='smn-io', queue=self.queue, operation='write', name=name, address=address, value=value)
                emit(dict(event, phase='before', monotonic=time.monotonic()))
                guard()
                transport.write_smu_reg(address, value)
                emit(dict(event, phase='after', monotonic=time.monotonic()))
                guard()
        control = core.run_stock(Backend(0), lambda e: emit(dict(e, queue=0)), guard,
                                 version_message=2, test_message=1,
                                 expected_version=core.STOCK_VERSION, timeout_s=0.5)
        emit(dict(kind='q0-control', passed=control['pass']))
        if control['pass'] is not True:
            raise RuntimeError('q0 positive controls failed')
        setup = local_module('c092_core','setup-descriptor-core-C092.py')
        setup.run_setup(Backend(2), emit, guard)
        observation = diagnostic.observe(Backend(2), page, emit, guard)
        q0 = Backend(0)
        for name, expected in (('parameter',0xFFFFFFFD),('response',1),('command',1)):
            if q0.read(name) != expected:
                raise RuntimeError('q0 control changed during setup')
        guard()
        outcome = dict(passed=True, observation=observation, no_activation=True)
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
                      buffers=page.state() if page else None, pid=os.getpid(), outcome=outcome,
                      no_activation=True, c097_withdrawal_retained=True)
        try:
            with args.status.open('x', encoding='utf-8') as stream:
                json.dump(status, stream, indent=2)
                stream.flush()
                os.fsync(stream.fileno())
            emit(dict(kind='lifecycle', **status))
            os.fsync(log.fileno())
        finally:
            try:
                if device is not None:
                    device.close()  # No release of retained kernel buffers.
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
