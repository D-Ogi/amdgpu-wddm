#!/usr/bin/env python3
"""Compile MPO member probes against real WDK headers at three DDI versions.

Header availability is not OS feature negotiation, hardware support or HLK
compliance. In particular WDDM 2.9 exposes some, but not all, hardware flip
queue fields. Negative probes must fail for the requested member, and the
same expression must compile at its positive-control version.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def audit(clang, kits, version, out):
    wdk = kits / 'microsoft.windows.wdk.x64/c/Include' / version
    sdk = kits / 'microsoft.windows.sdk.cpp/c/Include' / version
    includes = [wdk / 'km', wdk / 'km/crt', wdk / 'shared',
                sdk / 'shared', sdk / 'ucrt']
    headers = [wdk / 'shared/d3dkmddi.h', wdk / 'km/dispmprt.h',
               wdk / 'km/ntddk.h', sdk / 'shared/d3dukmdt.h']
    for path in headers:
        if not path.is_file():
            raise FileNotFoundError(path)
    compiler = subprocess.check_output([str(clang), '--version'], text=True)
    if 'clang version' not in compiler:
        raise ValueError('This probe requires Clang; no compiler fallback')
    report = {'scope': 'WDK header availability only; no runtime or HLK pass',
              'compiler': compiler.strip(), 'kit_version': version,
              'headers_sha256': {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                                 for p in headers}, 'cases': []}
    # Use the WDK's own version constants, not hand-copied numeric versions.
    versions = ['WDDM2_8', 'WDDM2_9', 'WDDM3_0']
    members = [('PrePresentNeeded', 'p->OutputFlags.PrePresentNeeded = 1;', 0),
               ('TargetFlipTime', 'p->TargetFlipTime = 0;', 1),
               ('MaxHwQueuedFlips', 'caps->MaxHwQueuedFlips = 0;', 1),
               ('HwFlipQueueDrainNeeded', 'p->OutputFlags.HwFlipQueueDrainNeeded = 1;', 2),
               ('HwFlipQueueDrainAllPlanes', 'p->OutputFlags.HwFlipQueueDrainAllPlanes = 1;', 2),
               ('HwFlipQueueDrainAllSources', 'p->OutputFlags.HwFlipQueueDrainAllSources = 1;', 2)]
    probe = out.with_suffix('.probe.c')
    for index, ddi in enumerate(versions):
        for member, expression, minimum in [('baseline', '', 0)] + members:
            source = ('#include <ntddk.h>\n#include <dispmprt.h>\n'
                      'C_ASSERT(sizeof(DXGK_SETVIDPNSOURCEADDRESS_OUTPUT_FLAGS) == 4);\n'
                      'void probe(DXGKARG_SETVIDPNSOURCEADDRESSWITHMULTIPLANEOVERLAY3 *p,\n'
                      '           DXGK_DRIVERCAPS *caps) {\n    ' + expression + '\n}\n')
            probe.write_text(source, encoding='utf-8')
            command = [str(clang), '--target=x86_64-pc-windows-msvc',
                       '/nologo', '/Zs', '/TC', '/W0', '/D_AMD64_',
                       '/DDXGKDDI_INTERFACE_VERSION=DXGKDDI_INTERFACE_VERSION_' + ddi]
            command += ['/I' + str(p) for p in includes] + [str(probe)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=60)
            expected = index >= minimum
            diagnostic = result.stdout + result.stderr
            errors = [line for line in diagnostic.splitlines() if 'error:' in line]
            passed = result.returncode == 0 if expected else (
                result.returncode != 0 and len(errors) == 1
                and "no member named '" + member + "'" in errors[0])
            report['cases'].append({'ddi': ddi, 'member': member,
                                    'expected_available': expected, 'passed': passed,
                                    'exit_code': result.returncode, 'command': command,
                                    'source': source, 'diagnostic': diagnostic})
    out.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    failures = [c for c in report['cases'] if not c['passed']]
    print(f"MPO DDI contract: {len(report['cases']) - len(failures)}/{len(report['cases'])} probes pass")
    for case in failures:
        print(case['ddi'], case['member'], case['diagnostic'])
    return bool(failures)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--clang', type=Path, required=True)
    parser.add_argument('--kits', type=Path, required=True)
    parser.add_argument('--kit-version', default='10.0.26100.0')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    raise SystemExit(audit(args.clang.resolve(), args.kits.resolve(), args.kit_version,
                           args.out.resolve()))
