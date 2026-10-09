# SPDX-License-Identifier: MIT
"""Test the Vulkan present route rules of the Mesa fork (radv_wddm2_wsi_route.h, wsi_win32_deadline.h). Run
via build-radv-wsi-route-test.ps1.

The rules are plain C in two headers: the route switch (AMDGPU_WDDM_VK_WSI, the WsiRoute registry value, the
default), the module gate that keeps the DXGI route away from processes with an application-local dxgi.dll,
d3d12.dll or d3d12core.dll, the LB7A checks of the D3D12 shared-resource import, and the deadlines of the
DXGI route with the rule that retires it when its first present never completes (BD-105). The test source
(src/amd/vulkan/winsys/wddm2/tests/radv_wddm2_wsi_route_test.c) is compiled with cl /W4 /WX on its own; no Mesa build
and no GPU are involved. It runs twice: as is (every case must pass) and with --negative-control, which inverts every
expectation and must fail every case. design: docs/design/vulkan-wsi-dxgi.md.

Usage: radv-wsi-route-test.py --source SRC --output NEW_DIR
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

TEST_REL = 'src/amd/vulkan/winsys/wddm2/tests/radv_wddm2_wsi_route_test.c'
HEADER_RELS = ('src/amd/vulkan/winsys/wddm2/radv_wddm2_wsi_route.h',
               'src/vulkan/wsi/wsi_win32_deadline.h')
TIMEOUT = 60


def sha(path):
    return hashlib.sha256(open(path, 'rb').read()).hexdigest().upper()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    src = Path(args.source).resolve()
    out = Path(args.output).resolve()
    out.mkdir(parents=True, exist_ok=False)
    test = src / TEST_REL
    inputs = {rel: sha(src / rel) for rel in (TEST_REL, *HEADER_RELS)}
    tests = re.findall(r'^\s*\{"(\w+)", test_\w+\},', test.read_text(encoding='utf-8'), re.M)
    if not tests:
        raise SystemExit('no tests[] table in ' + str(test))
    exe = out / 'radv_wddm2_wsi_route_test.exe'
    record = {'source_head': subprocess.check_output(['git', '-C', str(src), 'rev-parse', 'HEAD'], text=True).strip(),
              'source_status': subprocess.check_output(['git', '-C', str(src), 'status', '--porcelain', '--',
                                                        TEST_REL, *HEADER_RELS], text=True),
              'inputs': inputs, 'tests': tests}
    argv = ['cl', '/nologo', '/W4', '/WX', '/std:c11', '/TC', '/Fe' + str(exe), '/Fo' + str(out) + '\\', str(test)]
    p = subprocess.run(argv, cwd=out, capture_output=True, text=True, errors='replace')
    (out / 'build.log').write_text('> ' + subprocess.list2cmdline(argv) + '\n' + p.stdout + p.stderr)
    if p.returncode:
        raise SystemExit('test compile failed; see build.log')
    record['exe_sha256'] = sha(exe)
    results = {}
    for name, extra, must_pass in (('test', [], True), ('negative-control', ['--negative-control'], False)):
        try:
            p = subprocess.run([str(exe), *extra], capture_output=True, text=True, errors='replace', timeout=TIMEOUT)
            code, text = p.returncode, p.stdout + p.stderr
        except subprocess.TimeoutExpired:
            code, text = 'timeout', ''
        (out / (name + '.txt')).write_text(text)
        passed = re.findall(r'^PASS (\w+)$', text, re.M)
        failed = re.findall(r'^FAIL (\w+)$', text, re.M)
        summary = next((l for l in text.splitlines() if l.startswith('wsi route:')), None)
        if must_pass:
            ok = code == 0 and passed == tests and not failed
        else:
            ok = code == 1 and failed == tests and not passed
        results[name] = dict(exit=code, ok=ok, passed=passed, failed=failed, summary=summary)
        print(f"{name}: exit={code}, {'PASS' if ok else 'FAIL'}: {summary or 'no summary'}")
    record['results'] = results
    (out / 'record.json').write_text(json.dumps(record, indent=2))
    return 0 if all(r['ok'] for r in results.values()) else 1


if __name__ == '__main__':
    sys.exit(main())
