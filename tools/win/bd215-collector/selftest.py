"""Host-only proof of collector coverage and continuation/ownership/timeout behavior."""
import hashlib
import json
import mmap
import os
from pathlib import Path
import struct
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
# The build directory of build.ps1: the first argument, else <BC250_ROOT>\scratch\build\bd215-collector.
# Every result goes there, never into the source directory.
BIN = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(os.environ.get('BC250_ROOT') or HERE.parents[3]) / 'scratch' / 'build' / 'bd215-collector'
COLLECTOR = BIN / 'bd215collector.exe'
FAULT = BIN / 'bd215fault32.exe'
ROOT = BIN / ('host-test-' + time.strftime('%Y%m%dT%H%M%SZ', time.gmtime()))
ROOT.mkdir()
rows = []
CLEAN_ENV = {k: os.environ[k] for k in ('SystemRoot', 'WINDIR', 'COMSPEC', 'SystemDrive') if k in os.environ}
CLEAN_ENV.update({'TEMP': str(BIN), 'TMP': str(BIN), 'PATH': str(Path(os.environ['SystemRoot']) / 'System32')})

def verify_dump(directory, record):
    with (directory / 'first-exception.dmp').open('rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as d:
        sig, version, count, at, checksum, timestamp, flags = struct.unpack_from('<6IQ', d)
        assert sig == 0x504d444d and flags & 0x1802 == 0x1802
        streams = {}
        for i in range(count):
            kind, size, off = struct.unpack_from('<III', d, at + i * 12)
            assert off + size <= len(d)
            if kind:
                streams[kind] = (size, off)
        assert {3, 4, 6, 7, 16, 17, 0xB2150001, 0xB2150002, 0xB2150003} <= streams.keys()
        assert 9 in streams or 5 in streams
        for kind, key in ((0xB2150001, 'native_context'), (0xB2150002, 'wow64_context'), (0xB2150003, 'raw_debug_event')):
            n, p = streams[kind]
            assert d[p:p+n].hex() == record[key]['raw_hex']
        return {'bytes': len(d), 'flags': hex(flags), 'streams': [hex(x) for x in streams]}

def verify_record(directory, indirect=False, sentinel=True):
    p = json.loads((directory / 'first-exception.json').read_text())
    assert p['raw_debug_event']['first_chance'] and p['raw_debug_event']['exception_code'] == '0xc0000005'
    if sentinel:
        assert int(p['raw_debug_event']['parameters'][1], 16) in (0xffffffff, 0xffffffffffffffff)
    for key, ip, sp in [('native_context', 'Rip', 'Rsp'), ('wow64_context', 'Eip', 'Esp')]:
        c = p[key]
        assert c['ok'] and c['error'] == 0 and len(c['raw_hex']) == 2 * c['byte_count']
        for r in [ip, sp, 'SegCs', 'SegSs']:
            assert int(c['registers'][r], 16) != 0
    assert p['native_context']['byte_count'] == 1232 and p['wow64_context']['byte_count'] == 716
    for seg in ('cs', 'ds', 'es', 'fs', 'gs', 'ss'):
        s = p['selectors'][seg]
        assert s['ok'] and s['present'] and s['error'] == 0 and len(s['raw_hex']) == 16
        assert int(s['selector'], 16) and int(s['limit'], 16)
    assert p['instruction']['ok'] and p['instruction']['bytes_read'] == 64
    for region in ('ip', 'stack'):
        r = p['regions'][region]
        assert r['ok'] and int(r['size'], 16) > 0 and r['state'] == '0x1000'
    m = p['module']
    assert int(m['base'], 16) > 0 and int(m['size'], 16) > 0
    assert m['sha256'] == hashlib.sha256(FAULT.read_bytes()).hexdigest() and m['hash_error'] == 0
    assert p['thread_processor_number']['available'] is False and p['thread_processor_number']['number'] is None
    assert p['thread_processor_number']['reason'] and p['thread_processor_number']['group_affinity']['ok']
    assert len(p['process_mitigations']) == 19 and p['process_mitigations']['DEP']['ok']
    for policy in p['process_mitigations'].values():
        assert set(('policy', 'ok', 'error', 'byte_count', 'raw_hex')) <= policy.keys()
        assert policy['ok'] or policy['error'] != 0
    if indirect:
        t = p['jump_table']
        assert t['applicable'] and t['slot']['ok'] and t['target']['ok']
        assert t['slot']['bytes_read'] == 64 and t['target']['bytes_read'] == 64
        assert p['instruction']['hex'].startswith('ff24bd') and p['wow64_context']['registers']['Edi'] == '0x2'
        assert int(t['target']['address'], 16) == int(p['instruction']['address'], 16) + 7
        assert p['regions']['table']['ok'] and p['regions']['table']['state'] == '0x1000'
        # The intentionally executed MOV DS,AX (AX=0) is immediately before the faulting indirect JMP.
        import pefile
        pe = pefile.PE(str(FAULT), fast_load=True)
        rva = int(p['instruction']['address'], 16) - int(m['base'], 16)
        off = pe.get_offset_from_rva(rva)
        assert FAULT.read_bytes()[off-5:off] in (bytes.fromhex('31c0668ed8'), bytes.fromhex('33c0668ed8'))
        assert p['wow64_context']['registers']['SegDs'] == '0x2b'
    result = json.loads((directory / 'capture-result.json').read_text())
    assert result['complete'] and result['dump_ok'] and result['dump_error'] == 0
    return verify_dump(directory, p)

def run(name, args, expected, **kwargs):
    directory = ROOT / name
    cmd = [str(COLLECTOR), '--out', str(directory), '--seconds', str(kwargs.pop('seconds', 15)), *args]
    start = time.monotonic()
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=22, env=CLEAN_ENV)
    elapsed = round(time.monotonic() - start, 3)
    (ROOT / (name + '.stdout.txt')).write_text(r.stdout, encoding='utf-8')
    (ROOT / (name + '.stderr.txt')).write_text(r.stderr, encoding='utf-8')
    assert r.returncode == expected, (name, r.returncode, r.stdout, r.stderr)
    result = json.loads((directory / 'run-result.json').read_text())
    assert result['collector_exit'] == expected
    row = {'name': name, 'exit': r.returncode, 'elapsed_s': elapsed, 'run': result}
    if expected == 11:
        row['dump'] = verify_record(directory, **kwargs)
    else:
        assert not (directory / 'first-exception.json').exists()
    rows.append(row)
    print('PASS', name, 'exit', r.returncode, 'seconds', elapsed, flush=True)
    return result

try:
    for name, mode, indirect in [('null-code-selector-gp', '--gp', False), ('null-ds-indirect-gp', '--indirect-gp', True)]:
        r = run(name, ['--', str(FAULT), mode], 11, indirect=indirect)
        assert r['debuggee_exit_code'] == 77 and r['debuggee_exited']
    r = run('module-matched-ordinary-av', ['--module', FAULT.name, '--', str(FAULT), '--read'], 11, sentinel=False)
    assert r['debuggee_exit_code'] == 77
    r = run('nonmatching-av', ['--module', 'no-such-module.dll', '--', str(FAULT), '--read'], 10)
    assert r['debuggee_exit_code'] == 77
    r = run('clean', ['--', str(FAULT), '--clean'], 10)
    assert r['debuggee_exit_code'] == 0
    r = run('owned-timeout', ['--', str(FAULT), '--wait'], 10, seconds=1)
    assert r['timed_out'] and r['started_by_collector'] and r['debuggee_exit_code'] == 1460
    for name, mode, expected, seconds in [('attach-timeout', '--wait', 10, 1), ('attach-gp', '--delay-gp', 11, 10)]:
        target = subprocess.Popen([str(FAULT), mode], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=CLEAN_ENV)
        try:
            r = run(name, ['--pid', str(target.pid)], expected, seconds=seconds)
            assert not r['started_by_collector']
            if name == 'attach-timeout':
                assert target.poll() is None, 'collector must leave attached target alive'
            else:
                stdout, stderr = target.communicate(timeout=5)
                assert target.returncode == 77 and 'handler-completed' in stdout
        finally:
            if target.poll() is None:  # This process belongs to this host test, never to the collector.
                target.terminate()
                target.communicate(timeout=5)
    summary = {'status': 'PASS', 'cases': rows,
               'artifacts': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in (COLLECTOR, FAULT, HERE / 'collector.cpp', HERE / 'fault-x86.c')},
               'semantic_control': 'Null DS immediately before the valid FF24BD jump-to-next produces AV(-1), while Win32 WoW64 context exposes DS2b on this host. This is a coverage/control observation, not a diagnosis of the lab fault.'}
except Exception:
    (ROOT / 'summary.json').write_text(json.dumps({'status': 'FAIL', 'cases': rows}, indent=2) + '\n')
    raise
(ROOT / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
(BIN / 'latest-host-test.txt').write_text(str(ROOT) + '\n', encoding='utf-8')
print('PASS all', len(rows), 'cases;', ROOT, flush=True)
