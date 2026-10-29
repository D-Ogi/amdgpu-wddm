"""Exercise the exact proposed C header on x86/x64 with a loaded fixture DLL and with none."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import pefile

HERE = Path(__file__).resolve().parent
# The build directory of build.ps1: the first argument, else <BC250_ROOT>\scratch\build\bd215-collector.
# Every result goes there, never into the source directory.
BIN = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(os.environ.get('BC250_ROOT') or HERE.parents[3]) / 'scratch' / 'build' / 'bd215-collector'
rows = []
for arch in ('x64', 'x86'):
    binary = BIN / arch / 'identity-test.exe'
    dll = binary.with_name('amdgpu_wddm_bd215_fixture.dll')
    for loaded in (False, True):
        r = subprocess.run([str(binary), *([str(dll)] if loaded else [])], capture_output=True, text=True, timeout=10)
        assert r.returncode == 0, (arch, r.stderr)
        lines = [json.loads(s.split('identity ', 1)[1]) for s in r.stdout.splitlines() if s.startswith('bd201scratch: identity ')]
        assert [p['stage'] for p in lines] == ['before-device', 'after-first-submit']
        for p in lines:
            assert p['pid'] > 0 and p['tid'] > 0 and p['modules_ok'] and not p['modules_truncated']
            assert p['process_affinity']['ok'] and int(p['process_affinity']['process_mask'], 16) > 0
            assert p['thread_group_affinity']['ok'] and int(p['thread_group_affinity']['mask'], 16) > 0
            assert isinstance(p['processor_at_snapshot']['number'], int)
            assert len(p['modules']) == int(loaded)
            if loaded:
                m = p['modules'][0]
                assert m['name'] == dll.name and m['info_ok'] and m['hash_error'] == 0
                assert m['sha256'] == hashlib.sha256(dll.read_bytes()).hexdigest()
                assert int(m['base'], 16) > 0
                assert int(m['size'], 16) == pefile.PE(str(dll), fast_load=True).OPTIONAL_HEADER.SizeOfImage
        rows.append({'arch': arch, 'loaded_fixture': loaded, 'records': lines})
        print('PASS client identity', arch, 'fixture', loaded)
(BIN / 'client-host-test.json').write_text(json.dumps({'status': 'PASS', 'cases': rows}, indent=2) + '\n')
