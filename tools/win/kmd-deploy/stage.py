"""Generic KMD promotion on unit A: freeze an immutable attempt offline, then push it.

    python stage.py freeze --package DIR --rollback DIR [--rollback-build DIR] [--mode deploy|rehearsal] [--repo DIR]
    python stage.py freeze ... --dry-run --out DIR     same checks and tree, outside attempts/, never pushed
    python stage.py push kmdRRR[-B]-deployNNN          LAB: copy the attempt to C:\\BC250\\m15, preflight, pnputil /add-driver

--package is the candidate's package directory (bc250kmd.sys/.inf/.cat, bc250-lab-test.cer, source-manifest.json),
for example scratch\\cumode\\build-kmd174\\package. --rollback is the package now deployed on the lab, in the same
form: its build package directory or the candidate directory of the attempt that promoted it. When the lab runs a
tester release, --rollback is that release package (manifest.json, payload/kmd) and --rollback-build the build
package it was made from. Version, ABI and hashes come from each manifest and its commit (read in --repo); the
rollback must equal lab-baseline.json, which also supplies the desktop UMD/ICD pins. The identity of a package is
(R, B) from DriverVer 0.7.R.B. Labels and the attempt name carry it (candidate216-16, kmd216-16-deploy001); a
build 1 keeps the old names (candidate175, kmd175-deploy001). freeze writes the attempt's own identity.ps1 from all of that; the template's
identity.ps1 is only the host-test fixture. Attempt directories are never rewritten; anything new is a new NNN.
"""
import argparse
import json
import re
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from kmdcommon import (ATTEMPTS, BASE, HIST, HIST_MANIFEST_SHA256, LAB_BASELINE, MODES, OPS, POWERSHELL,  # noqa: E402
                       REMOTE_BASE, REMOTE_TMP, REPO, TEMPLATE, TRANSITION, compare_packages, digest, fail,
                       historical_manifest, identity, label_suffix, next_receipt, ps_env, read_package, resolve_attempt,
                       target, verify_attempt)

SAFE_NAME = re.compile(r'^[a-zA-Z0-9_./-]+$')


def check_signature(package):
    """The SYS and CAT of a package are signed by the certificate it carries (bc250-lab-test.cer of a build, the
    release certificate of a release package)."""
    r = subprocess.run([POWERSHELL, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(OPS / 'verify-signature.ps1'),
                        '-Package', str(package['signed']), '-Certificate', str(package['certificate'])],
                       capture_output=True, text=True, timeout=60, env=ps_env())
    if r.returncode:
        fail(f'signature check of {package["directory"]}: {r.stdout[-400:]} {r.stderr[-400:]}')
    return json.loads(r.stdout)


def check_certificates(candidate, rollback, signatures):
    """Two builds are signed by one lab certificate. A release rollback is signed by the release certificate that
    its manifest.json names; the candidate build then keeps the lab certificate."""
    if rollback['kind'] == 'release':
        if signatures['rollback']['certificate'].upper() != rollback['release_certificate']:
            fail('rollback: the release package is not signed by the certificate its manifest.json names')
    elif signatures['candidate']['certificate'] != signatures['rollback']['certificate']:
        fail('candidate and rollback are signed by different certificates')


def check_baseline(rollback, baseline):
    """The rollback is the KMD that lab-baseline.json names; a release rollback is also the release it names."""
    deployed = (baseline['kmd_version'], baseline['kmd_sys_sha256'].upper(), baseline['kmd_abi'])
    if deployed != (rollback['version'], rollback['files']['bc250kmd.sys'], rollback['abi']):
        fail(f'rollback {rollback["version"]} {rollback["files"]["bc250kmd.sys"][:8]} is not the deployed KMD of '
             f'lab-baseline.json {deployed[0]} {deployed[1][:8]}')
    if rollback['kind'] == 'release':
        release = baseline.get('release') or {}
        if (str(release.get('manifest_sha256', '')).upper(), release.get('kmd_build')) != \
                (rollback['release_manifest_sha256'], rollback['kmd_build']):
            fail('rollback: lab-baseline.json release block does not name this release manifest and build')


def ps_file(script, *args, timeout=300):
    return subprocess.run([POWERSHELL, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(script), *map(str, args)],
                          capture_output=True, text=True, timeout=timeout, env=ps_env())


def run_gates():
    r = subprocess.run([sys.executable, str(BASE / 'check-offline.py'), '--quick'], capture_output=True, text=True, timeout=600)
    if r.returncode:
        fail('offline gates failed:\n' + r.stdout[-3000:])
    return r.stdout.strip().splitlines()[-1]


def copy_verified(source, dest, expected):
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, dest)
    if digest(dest) != expected:
        fail(f'copy mismatch {dest}')


# The router's registry key (scratch\m15\gpu-dwm-t0\kit\RUNBOOK.md "Router key"); DwmForceCpu there is the kill switch.
ROUTER_KEY = 'SOFTWARE\\amdgpu-wddm\\DesktopRouter'
HASH_RE = re.compile(r'^[0-9A-F]{64}$')


def desktop_pins(baseline):
    """The desktop a KMD promotion runs under, from lab-baseline.json: the latched interop switches (0 or 1), the
    exact driver-module hash set DWM holds, and the registered router's key. The promotion runs on the CPU route,
    router + CPU UMD with DwmForceCpu 1 (`route.py release cpu` first), because the kit's Verify needs one known
    module set and the CPU route is the stop path measured since 173. A baseline with no desktop block is refused:
    release-baseline.py derives one from every release manifest, and the pre-router shape it would stand for (the
    CPU UMD alone, both switches 0) cannot describe an installed release, whose installer registers the router."""
    umd = baseline['umd_sha256'].upper()
    desktop = baseline.get('desktop')
    if desktop is None:
        fail('lab-baseline.json has no desktop block: derive it from the installed release with release-baseline.py')
    switches = desktop.get('switches')
    modules = sorted({h.upper() for h in desktop.get('dwm_routes', {}).get('cpu', [])})
    registered = str(desktop.get('registered_sha256', '')).upper()
    if switches not in (0, 1):
        fail('lab-baseline.json desktop.switches must be 0 or 1')
    if not modules or any(not HASH_RE.match(h) for h in modules):
        fail('lab-baseline.json desktop.dwm_routes.cpu must be a non-empty list of SHA256 values')
    if umd not in modules or registered not in modules or len(modules) != 2:
        fail('lab-baseline.json desktop.dwm_routes.cpu must be exactly the registered router and the CPU UMD')
    return {'switches': str(switches), 'modules': modules, 'router_key': ROUTER_KEY}


def generated_identity(candidate, rollback, baseline, name_prefix):
    pins = {'Sys': rollback['files']['bc250kmd.sys'], 'Inf': rollback['files']['bc250kmd.inf'], 'Cat': rollback['files']['bc250kmd.cat']}
    desktop = desktop_pins(baseline)
    lines = [
        f"# Generated by stage.py freeze: {candidate['version']} (commit {candidate['commit'][:8]}) over the deployed",
        f"# {rollback['version']} (commit {rollback['commit'][:8]}), which is also the rollback; desktop pins from lab-baseline.json.",
        '# The only file of this attempt that names the promotion; test-identity.ps1 rejects such literals elsewhere.',
        f"$KmdCandidateLabel='candidate{label_suffix(candidate['version'])}'",
        f"$KmdCandidateVersion='{candidate['version']}'",
        f"$KmdCandidateAbi='{candidate['abi']}'",
        f"$KmdRollbackLabel='rollback{label_suffix(rollback['version'])}'",
        f"$KmdRollbackVersion='{rollback['version']}'",
        f"$KmdRollbackAbi='{rollback['abi']}'",
        f"$KmdRollbackSysSha256='{pins['Sys']}'",
        f"$KmdRollbackInfSha256='{pins['Inf']}'",
        f"$KmdRollbackCatSha256='{pins['Cat']}'",
        "$KmdCandidateMode='rehearsal'",
        "$KmdSameMode='same'",
        "$KmdDeployMode='deploy'",
        "$KmdDirectoryPattern='^C:\\\\BC250\\\\m15\\\\" + name_prefix + "[0-9]{3}$'",
        "$KmdTaskName='BC250-KMD-Watch'",
        f"$KmdDesktopUmdPath='{baseline['umd_path']}'",
        f"$KmdDesktopUmdSha256='{baseline['umd_sha256']}'",
        f"$KmdIcdPath='{baseline['icd_path']}'",
        f"$KmdIcdSha256='{baseline['icd_sha256']}'",
        '# The desktop the promotion runs under (lab-baseline.json desktop block, CPU route; desktop_pins in stage.py).',
        f"$KmdDesktopSwitches='{desktop['switches']}'",
        f"$KmdDesktopModules='{','.join(desktop['modules'])}'",
        f"$KmdDesktopRouterKey='{desktop['router_key']}'",
    ]
    return '\n'.join(lines) + '\n'


def freeze(args):
    candidate = read_package(args.package, args.repo, 'candidate')
    rollback = read_package(args.rollback, args.repo, 'rollback', args.rollback_build)
    compare_packages(candidate, rollback)
    baseline = json.loads(LAB_BASELINE.read_text(encoding='utf-8'))
    check_baseline(rollback, baseline)
    signatures = {'candidate': check_signature(candidate), 'rollback': check_signature(rollback)}
    check_certificates(candidate, rollback, signatures)
    hist = historical_manifest()
    gates = run_gates()

    prefix = f'kmd{label_suffix(candidate["version"])}-deploy'
    if args.dry_run:
        attempt = args.out.resolve()
        if attempt.exists():
            fail('dry-run output exists')
        name, ops = prefix + '000', attempt.with_name(attempt.name + '-ops')
    else:
        ATTEMPTS.mkdir(parents=True, exist_ok=True)
        n = 1
        while (ATTEMPTS / (prefix + '%03d' % n)).exists() or (ATTEMPTS / (prefix + '%03d-ops' % n)).exists():
            n += 1
        name = prefix + '%03d' % n
        attempt, ops = ATTEMPTS / name, ATTEMPTS / (name + '-ops')
    attempt.mkdir(parents=True)
    ops.mkdir(parents=True)

    for path in sorted((TEMPLATE / TRANSITION).iterdir()):
        if path.name != 'identity.ps1':
            copy_verified(path, attempt / TRANSITION / path.name, digest(path))
    (attempt / TRANSITION / 'identity.ps1').write_bytes(generated_identity(candidate, rollback, baseline, prefix).encode('utf-8'))
    ident = identity(attempt / TRANSITION / 'identity.ps1')
    for rel, value in hist.items():
        if rel.startswith('kmd168-transition/') or rel in ('bounded-child.exe', 'select-driver.exe'):
            template_copy = TEMPLATE / rel
            if template_copy.exists() and digest(template_copy) != value:
                fail(f'template {rel} differs from the 171 stage')
            copy_verified(HIST / rel, attempt / rel, value)
    for label, package in ((ident['KmdCandidateLabel'], candidate), (ident['KmdRollbackLabel'], rollback)):
        for fname, value in package['files'].items():
            copy_verified(package['paths'][fname], attempt / label / fname, value)
    # The generated identity passes the same literal gate and well-formedness test as the fixture.
    r = ps_file(attempt / TRANSITION / 'test-identity.ps1')
    if r.returncode or 'PASS' not in r.stdout:
        fail('generated identity: ' + (r.stdout + r.stderr)[-1500:])

    hashes = {ident['KmdCandidateLabel']: dict(sorted(candidate['files'].items())),
              ident['KmdRollbackLabel']: dict(sorted(rollback['files'].items()))}
    (attempt / 'package-hashes.json').write_text(json.dumps(hashes, indent=2) + '\n', encoding='utf-8')
    (attempt / 'transition-policy.json').write_text(json.dumps({'schema': 1, 'mode': ident[MODES[args.mode]]}) + '\n', encoding='utf-8')
    files = {}
    for path in sorted(attempt.rglob('*')):
        if path.is_file():
            rel = path.relative_to(attempt).as_posix()
            if not SAFE_NAME.match(rel) or '..' in rel.split('/'):
                fail('unsafe stage path ' + rel)
            files[rel] = digest(path)
    runner = ''.join(f'{rel}\t{value}\n' for rel, value in sorted(files.items()) if rel.startswith(TRANSITION + '/'))
    manifest = {'schema': 1,
                'runner': {'template_sha256': digest_text(runner), 'historical_stage_manifest': HIST_MANIFEST_SHA256,
                           'lab_baseline_sha256': digest(LAB_BASELINE)},
                'candidate': {k: candidate[k] for k in ('version', 'abi', 'flavor', 'commit', 'tree', 'build_manifest_sha256')} |
                             {'label': ident['KmdCandidateLabel'], 'source': str(candidate['directory'])},
                'rollback': {k: rollback[k] for k in ('version', 'abi', 'flavor', 'commit', 'tree', 'build_manifest_sha256')} |
                            {'label': ident['KmdRollbackLabel'], 'source': str(rollback['directory'])} |
                            ({'release': rollback['release'], 'kmd_build': rollback['kmd_build'],
                              'release_manifest_sha256': rollback['release_manifest_sha256'],
                              'build_source': str(rollback['build_directory'])} if rollback['kind'] == 'release' else {}),
                'mode': ident[MODES[args.mode]], 'files': files}
    (attempt / 'stage-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    manifest_sha = digest(attempt / 'stage-manifest.json')
    (ops / 'stage-manifest.sha256').write_text(manifest_sha + '\n', encoding='ascii')
    receipt = {'utc': datetime.now(timezone.utc).isoformat(), 'attempt': name, 'dry_run': args.dry_run,
               'manifest_sha256': manifest_sha, 'mode': manifest['mode'],
               'candidate': f"{candidate['version']} {candidate['abi']} {candidate['commit'][:8]}",
               'candidate_sys': candidate['files']['bc250kmd.sys'], 'candidate_inf': candidate['files']['bc250kmd.inf'],
               'candidate_cat': candidate['files']['bc250kmd.cat'],
               'rollback': f"{rollback['version']} {rollback['abi']} {rollback['commit'][:8]}",
               'rollback_sys': rollback['files']['bc250kmd.sys'], 'signatures': signatures, 'offline_gates': gates,
               'files': len(files)}
    (ops / 'freeze-receipt.json').write_text(json.dumps(receipt, indent=1) + '\n', encoding='utf-8')
    verify_attempt(attempt, ops)
    print(json.dumps({k: v for k, v in receipt.items() if k != 'signatures'}, indent=1))


def digest_text(text):
    import hashlib
    return hashlib.sha256(text.encode('utf-8')).hexdigest().upper()


def push(args):
    name, attempt, ops = resolve_attempt(args.attempt)
    manifest = verify_attempt(attempt, ops)
    t = target()
    remote = REMOTE_BASE + '\\' + name
    r = t.ssh(f'powershell -NoProfile -Command "if(Test-Path {remote}){{exit 2}}"', timeout=20)
    if r.returncode:
        raise SystemExit('Destination exists or connection failed; inspect, never restage an attempt')
    t.push([attempt], REMOTE_BASE)
    r = t.run_script(OPS / 'stage-package.ps1', args=[name, manifest], timeout=90, remote_dir=f'{REMOTE_TMP}\\{name}-ops')
    out = next_receipt(ops, 'staging', '.txt')
    out.write_text(r.stdout + '\nSTDERR\n' + r.stderr, encoding='utf-8')
    print(r.stdout)
    print(r.stderr)
    sys.exit(r.returncode)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    f = sub.add_parser('freeze')
    f.add_argument('--package', type=Path, required=True)
    f.add_argument('--rollback', type=Path, required=True)
    f.add_argument('--rollback-build', type=Path, help='with a release package as --rollback: the build package it was made from')
    f.add_argument('--mode', choices=sorted(MODES), default='deploy', help='rehearsal always rolls back')
    f.add_argument('--repo', type=Path, default=REPO, help='git repository holding both commits')
    f.add_argument('--dry-run', action='store_true')
    f.add_argument('--out', type=Path)
    p = sub.add_parser('push')
    p.add_argument('attempt')
    args = parser.parse_args()
    if args.command == 'freeze':
        if args.dry_run != bool(args.out):
            fail('--dry-run and --out go together')
        freeze(args)
    else:
        push(args)


if __name__ == '__main__':
    main()
