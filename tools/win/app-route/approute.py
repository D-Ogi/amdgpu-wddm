"""Application routing (M14.1): applications' D3D10/D3D11 to the DXVK-based GPU UMD through the router's AppRouter
policy. Host side of the lab steps. Each LAB step is one bounded SSH call (target.py). The receipts, the runs and
the package live in the work directory outside this repository (README.md, "What it needs").

    python approute.py package                 host: assemble package-001\\app-route-001 (refuses changed inputs)
    python approute.py --self-test             host: package checks, ops tests (PowerShell 5.1), no lab
    python approute.py push                    LAB: copy the package to C:\\BC250\\m14 and admit it (app-stage.ps1)
    python approute.py swap install|rollback|status|cleanup      LAB: router-swap.ps1
    python approute.py policy cpu|allowlist|gpu-default|remove [--allow a.exe,b.exe] [--deny c.exe]
                                               LAB: app-policy.ps1 (cpu = the application kill switch)
    python approute.py status [--no-mappers]   LAB, read-only: app-status.ps1
    python approute.py run CLIENT [--args "..."] [--seconds N] [--interactive] [--exe PATH] [--screenshot-at 20,60]
                                               LAB: app-run.ps1, then pulls the run directory into runs\\

CLIENT is d3d11bench, d3d11mt, d3d11fl12, dxdiag, taskmgr or exe. Arguments may not contain double quotes.
--screenshot-at offsets count from the start of the call (the script push comes first, a second or two).
Rollback, in order of reach: `policy cpu` (applications back on the CPU UMD at their next OpenAdapter),
`swap rollback` (the frozen gpu-dwm-003 router back at its path), then lab-baseline.json restored and
`route.py 003 gpu` (DWM on the frozen router). The desktop kill switch `route.py 003 cpu` works in either state.
Nothing here touches the GPU DWM kit's attempt files; the lab-baseline.json update after an accepted swap belongs to
the operator (kit RUNBOOK.md, "Router re-pin").
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOLS_WIN = HERE.parent
# BC250_ROOT is the workspace root; by default the parent directory of this repository.
ROOT = Path(os.environ.get('BC250_ROOT') or HERE.parents[3])
# Everything this tool reads or writes outside the repository lives under one work directory in the
# workspace scratch area (BC250_APPROUTE_WORK overrides it): the built router, the host gate runs, the
# assembled package, the receipt of every lab call and the pulled run directories. Nothing is written
# into the repository, and no binary is kept in it.
WORK = Path(os.environ.get('BC250_APPROUTE_WORK') or (ROOT / 'scratch/m15/app-route'))
OPS = HERE / 'ops'
RECEIPTS = WORK / 'receipts'
RUNS = WORK / 'runs'
NAME = 'app-route-001'
PACKAGE = WORK / 'package-001' / NAME
LAB_PARENT = 'C:\\BC250\\m14'
LAB_ROOT = LAB_PARENT + '\\' + NAME
LAB_TMP = 'C:\\BC250\\tmp\\app-route-ops'
LAB_BASELINE = ROOT / 'scratch/m15/native-caps001/lab-baseline.json'
GPU_DWM_ATTEMPT = ROOT / 'scratch/m15/gpu-dwm-t0/kit/attempts/gpu-dwm-003'
ROUTER_BUILD = WORK / 'build-router'
HOST_GATES = WORK / 'work/host-gates'
FL12 = ROOT / 'scratch/m14/fl12/fl12native002/package'
POWERSHELL = 'C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe'
ROUTER = 'bc250d3d_router.dll'
SHELL = 'amdgpu_wddm_d3d11.dll'
# The M770 quartet (fl12native002, frozen and verified there) and the clients, by full SHA-256.
GPU_FILES = {
    'amdgpu_wddm_d3d11.dll': 'E748418C1F7DF57309E424D5DB7D464F68DA1BF8F82820707FA56F0E58FA00BB',
    'amdgpu_wddm_d3d11.config': 'A9B498ED8911B81D76693C1BC90066929364D661AE43D2B971FAAE198D4E9917',
    'amdgpu_wddm_dxvk.dll': '8E9B31875F3A97C046F91397D7DEEBE7037E419503A13CEDEE772401BCB2780A',
    'amdgpu_wddm_radv.dll': 'D672813F87B39CAB0DAFA41AD699C15F785D97DFBDF416B0516966D951E84F27',
}
CLIENTS = {
    'd3d11bench.exe': (ROOT / 'scratch/build/d3d11bench/d3d11bench.exe',
                       '2570A2F5259E1E7424E8D9850E4AF2D2982C01D6A8C4DFF963D1E7566AD43349'),
    'd3d11mt.exe': (ROOT / 'scratch/build/d3d11mt/d3d11mt.exe',
                    '1626B9B3A8A99154DC41AC4CA453AA96F6B5AAC2E50FFB2CC5AACE5A98F41E10'),
    'd3d11fl12.exe': (FL12 / 'd3d11fl12.exe', '0EC07C7ABE5D4035EF4FEF7A342FFBB70C2A85902494D387465D4EDE142DC034'),
}
OPS_FILES = ['approute-lib.ps1', 'durable.ps1', 'app-stage.ps1', 'router-swap.ps1', 'app-policy.ps1', 'app-status.ps1',
             'app-run.ps1', 'app-child.ps1']
# Kept on the CPU UMD in every mode: the D3D12 game target (its process would otherwise map a second
# amdgpu_wddm_radv.dll from another directory) and the GPU DWM kit's two oracle windows (their verdicts expect the
# CPU route for applications).
DENY_ALWAYS = ['witcher3.exe', 'composition-control.exe', 'gpu-window-control.exe']
CLIENT_TASK = 'Lab-App-Route-Client'
POLICY_KEY = 'SOFTWARE\\amdgpu-wddm\\AppRouter'
EXE_RE = re.compile(r'^[A-Za-z0-9_.-]+\.exe$')


def fail(message):
    raise SystemExit('REFUSED: ' + message)


def digest(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest().upper()


def passing_gate(router_sha, gates=HOST_GATES):
    """The newest host gate run (run-host-tests.ps1) that tested this router file and passed every scenario."""
    for d in sorted((p for p in Path(gates).iterdir() if p.is_dir()), reverse=True) if Path(gates).is_dir() else []:
        inputs, run = d / 'inputs.sha256', d / 'run.txt'
        if not inputs.is_file() or not run.is_file():
            continue
        first = inputs.read_text(encoding='utf-8-sig').split()
        text = run.read_text(encoding='utf-8-sig', errors='replace')
        if first and first[0].upper() == router_sha and re.search(r'^\d+ scenarios, 0 failed\s*$', text, re.M) and \
                text.rstrip().endswith('exit=0'):
            return d
    return None


def package_sources(fl12=FL12, router_build=ROUTER_BUILD, baseline=LAB_BASELINE, attempt=GPU_DWM_ATTEMPT,
                    gates=HOST_GATES, clients=None):
    """published relative path -> (source, sha256), and the manifest. Every input is checked before anything is
    written: the quartet and the clients against their pins, the router against a passing host gate, the baseline
    router against lab-baseline.json and the GPU DWM attempt it was frozen in."""
    clients = CLIENTS if clients is None else clients
    files = {}
    for name, sha in GPU_FILES.items():
        if digest(Path(fl12) / name) != sha:
            fail(f'{name} in {fl12} is not the M770 file {sha[:8]}')
        files[f'gpu/{name}'] = (Path(fl12) / name, sha)
    for name, (path, sha) in clients.items():
        if not Path(path).is_file() or digest(path) != sha:
            fail(f'{name} at {path} is not {sha[:8]}')
        files[f'clients/{name}'] = (Path(path), sha)
    router = Path(router_build) / ROUTER
    router_sha = digest(router)
    gate = passing_gate(router_sha, gates)
    if gate is None:
        fail(f'router {router_sha[:8]} has no passing host gate under {gates}')
    files[ROUTER] = (router, router_sha)
    pdb = Path(router_build) / 'bc250d3d_router.pdb'
    files['bc250d3d_router.pdb'] = (pdb, digest(pdb))
    lab = json.loads(Path(baseline).read_text(encoding='utf-8'))
    desktop = lab.get('desktop') or {}
    active, base_sha = str(desktop.get('registered_path', '')), str(desktop.get('registered_sha256', '')).upper()
    if not re.fullmatch(r'[A-Za-z]:\\.+\\' + re.escape(ROUTER), active, re.I) or not re.fullmatch(r'[0-9A-F]{64}', base_sha):
        fail('lab-baseline.json desktop registration malformed')
    frozen = Path(attempt) / ROUTER
    if Path(attempt).name.lower() != Path(active.replace('\\', '/')).parent.name.lower() or digest(frozen) != base_sha:
        fail(f'lab-baseline.json registers {active} {base_sha[:8]}, which is not {frozen}')
    if router_sha == base_sha:
        fail('the router is the registered one already')
    files['baseline/' + ROUTER] = (frozen, base_sha)
    for name in OPS_FILES:
        files[f'ops/{name}'] = (OPS / name, digest(OPS / name))
    manifest = {
        'schema': 1, 'name': NAME,
        'router': {'file': ROUTER, 'sha256': router_sha, 'baseline_sha256': base_sha, 'baseline_file': 'baseline\\' + ROUTER,
                   'active_path': active, 'host_gate': gate.name},
        'cpu_umd_path': lab['umd_path'], 'cpu_umd_sha256': lab['umd_sha256'].upper(),
        'gpu': {'dir': 'gpu', 'shell': SHELL, 'files': GPU_FILES},
        'clients': {n: s for n, (_, s) in clients.items()},
        'deny_always': DENY_ALWAYS, 'policy_key': POLICY_KEY, 'client_task': CLIENT_TASK,
    }
    return files, manifest


def write_package(out=PACKAGE, **sources):
    files, manifest = package_sources(**sources)
    out = Path(out)
    if out.exists():
        fail(f'{out} exists; a package is never rewritten (name a new one)')
    for rel, (src, sha) in files.items():
        dest = out / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, dest)
        if digest(dest) != sha:
            fail(f'copy of {rel} does not hash to {sha[:8]}')
    (out / 'app-route.json').write_text(json.dumps(manifest, indent=1) + '\n', encoding='utf-8')
    lines = [f'{digest(out / rel).lower()} *{rel}' for rel in sorted(list(files) + ['app-route.json'])]
    (out / 'SHA256SUMS.txt').write_text('\n'.join(lines) + '\n', encoding='ascii')
    return manifest


def verify_package(out=PACKAGE):
    """The package as written: SHA256SUMS.txt covers exactly the files present, and every line matches."""
    out = Path(out)
    listed = {}
    for line in (out / 'SHA256SUMS.txt').read_text(encoding='ascii').splitlines():
        m = re.fullmatch(r'([0-9a-f]{64}) \*(.+)', line)
        if not m:
            fail(f'SHA256SUMS.txt line {line!r}')
        listed[m.group(2)] = m.group(1).upper()
    present = {p.relative_to(out).as_posix() for p in out.rglob('*') if p.is_file()} - {'SHA256SUMS.txt'}
    if present != set(listed):
        fail(f'package files and SHA256SUMS.txt differ: {sorted(present ^ set(listed))}')
    bad = [rel for rel, sha in listed.items() if digest(out / rel) != sha]
    if bad:
        fail(f'package files changed: {bad}')
    manifest = json.loads((out / 'app-route.json').read_text(encoding='utf-8'))
    for name in OPS_FILES:
        if digest(OPS / name) != listed[f'ops/{name}']:
            fail(f'ops/{name} on the host differs from the package copy (repackage under a new name)')
    return manifest, listed


# ---------------------------------------------------------------- lab

def target():
    sys.path.insert(0, str(TOOLS_WIN))
    from target import Target
    return Target()


def receipt(stem, text):
    RECEIPTS.mkdir(parents=True, exist_ok=True)
    n = 0
    while (RECEIPTS / f'{stem}-{n}.txt').exists():
        n += 1
    path = RECEIPTS / f'{stem}-{n}.txt'
    path.write_text(text, encoding='utf-8')
    return path


def lab_script(t, script, args, timeout=200):
    verify_package()
    r = t.run_script(OPS / script, args=list(args) + ['-Root', LAB_ROOT], timeout=timeout, remote_dir=LAB_TMP)
    path = receipt(Path(script).stem, f'args {list(args)}\nexit {r.returncode}\n{r.stdout}\nSTDERR\n{r.stderr}')
    print(r.stdout[-6000:])
    if r.stderr.strip():
        print(r.stderr[-1500:])
    print(f'receipt {path}')
    return r


def json_tail(text):
    start = text.find('{')
    try:
        return json.loads(text[start:]) if start >= 0 else None
    except ValueError:
        return None


def screenshots(offsets, folder, started):
    mon = TOOLS_WIN / 'bc250mon/mon.py'
    for at in offsets:
        delay = started + at - time.monotonic()
        if delay > 0:
            time.sleep(delay)
        out = folder / f'shot-{at:03d}s.png'
        subprocess.run([sys.executable, str(mon), 'screenshot', '--out', str(out)], capture_output=True, timeout=60)


def cmd_run(t, a):
    args = ['-Client', a.client, '-Seconds', str(a.seconds)]
    if a.args:
        args += ['-Arguments', a.args]
    if a.interactive:
        args += ['-Interactive']
    if a.exe:
        args += ['-Exe', a.exe]
    local = RUNS / (datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ') + '-' + a.client)
    local.mkdir(parents=True, exist_ok=True)
    offsets = [int(x) for x in a.screenshot_at.split(',') if x.strip()] if a.screenshot_at else []
    if any(x < 1 or x >= a.seconds for x in offsets):
        fail('--screenshot-at offsets must lie inside the run')
    shooter = threading.Thread(target=screenshots, args=(offsets, local, time.monotonic()), daemon=True)
    if offsets:
        shooter.start()
    r = lab_script(t, 'app-run.ps1', args, timeout=a.seconds + 60)
    if offsets:
        shooter.join(timeout=70)
    result = json_tail(r.stdout)
    remote = (result or {}).get('run')
    if remote:
        for name in ('run.json', 'result.json', 'stdout.txt', 'stderr.txt', 'child.json', 'dxdiag.txt', 'spec.json'):
            try:
                t.pull(remote + '\\' + name, str(local / name))
            except Exception:  # noqa: BLE001 - absent files are normal (not every client writes every one)
                pass
    print(f'local run directory {local}')
    return r.returncode


# ---------------------------------------------------------------- self-test

def self_test():
    checks = []

    def check(name, ok):
        checks.append(name)
        if not ok:
            raise SystemExit('FAIL: ' + name)

    def refused(fn):
        try:
            fn()
        except SystemExit as e:
            return str(e).startswith('REFUSED')
        return False

    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    work = WORK / 'work/self-test' / stamp
    work.mkdir(parents=True)
    out = work / 'pkg' / NAME
    manifest = write_package(out)
    check('package written from the pinned inputs', manifest['router']['sha256'] != manifest['router']['baseline_sha256'])
    m2, listed = verify_package(out)
    check('package verifies: sums cover exactly the files', len(listed) == len(GPU_FILES) + len(CLIENTS) + len(OPS_FILES) + 4)
    check('manifest names the M770 shell and the deny list', m2['gpu']['shell'] == SHELL and m2['deny_always'] == DENY_ALWAYS)
    check('package refuses a rewrite', refused(lambda: write_package(out)))
    damaged = work / 'damaged'
    shutil.copytree(out, damaged)
    (damaged / 'gpu' / SHELL).write_bytes(b'x')
    check('changed package file refused', refused(lambda: verify_package(damaged)))
    (damaged / 'gpu' / SHELL).unlink()
    check('missing package file refused', refused(lambda: verify_package(damaged)))
    shutil.copyfile(out / 'gpu' / SHELL, damaged / 'gpu' / SHELL)
    (damaged / 'extra.txt').write_text('x')
    check('unlisted package file refused', refused(lambda: verify_package(damaged)))
    fake_fl12 = work / 'fl12'
    fake_fl12.mkdir()
    for n in GPU_FILES:
        shutil.copyfile(FL12 / n, fake_fl12 / n)
    (fake_fl12 / 'amdgpu_wddm_radv.dll').write_bytes(b'not the ICD')
    check('a quartet file other than M770 refused', refused(lambda: package_sources(fl12=fake_fl12)))
    check('a router without a passing host gate refused', refused(lambda: package_sources(gates=work / 'no-gates')))
    bad_client = {'d3d11mt.exe': (CLIENTS['d3d11bench.exe'][0], CLIENTS['d3d11mt.exe'][1])}
    check('a client other than its pin refused', refused(lambda: package_sources(clients=bad_client)))
    fake_baseline = work / 'lab-baseline.json'
    lab = json.loads(LAB_BASELINE.read_text(encoding='utf-8'))
    lab['desktop'] = dict(lab['desktop'], registered_sha256='0' * 64)
    fake_baseline.write_text(json.dumps(lab), encoding='utf-8')
    check('a baseline router other than the attempt file refused', refused(lambda: package_sources(baseline=fake_baseline)))
    lab['desktop'] = dict(lab['desktop'], registered_path='C:\\BC250\\m15\\gpu-dwm-002\\' + ROUTER,
                          registered_sha256=digest(GPU_DWM_ATTEMPT / ROUTER))
    fake_baseline.write_text(json.dumps(lab), encoding='utf-8')
    check('a registration in another attempt directory refused', refused(lambda: package_sources(baseline=fake_baseline)))
    for name in DENY_ALWAYS:
        check(f'deny name {name} is an image base name', bool(EXE_RE.fullmatch(name)))
    check('client task name stays clear of the competing-task gates', not re.search('BC250|DWM|G0|WSI', CLIENT_TASK, re.I))
    env = {k: v for k, v in os.environ.items() if k.upper() != 'PSMODULEPATH'}
    r = subprocess.run([POWERSHELL, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(OPS / 'tests/test-approute-lib.ps1'),
                        '-Out', str(work / 'lib')], capture_output=True, text=True, timeout=300, env=env)
    last = (r.stdout.strip().splitlines() or [''])[-1]
    check('ops library test (PowerShell 5.1): ' + last, r.returncode == 0 and last.startswith('PASS'))
    parse = ROOT / 'scratch/m15/gpu-dwm-t0/kit/ops/parse-check.ps1'
    r = subprocess.run([POWERSHELL, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(parse), '-Root', str(OPS)],
                       capture_output=True, text=True, timeout=120, env=env)
    check('ops scripts parse under PowerShell 5.1', r.returncode == 0 and '0 with errors' in r.stdout)
    print(f'PASS: approute self-test, {len(checks)} checks ({work})')
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--self-test', action='store_true')
    sub = p.add_subparsers(dest='cmd')
    sub.add_parser('package')
    sub.add_parser('push')
    s = sub.add_parser('swap')
    s.add_argument('action', choices=['install', 'rollback', 'status', 'cleanup'])
    s = sub.add_parser('policy')
    s.add_argument('mode', choices=['cpu', 'allowlist', 'gpu-default', 'remove'])
    s.add_argument('--allow', default='')
    s.add_argument('--deny', default='')
    s = sub.add_parser('status')
    s.add_argument('--no-mappers', action='store_true')
    s.add_argument('--tail', type=int, default=40)
    s = sub.add_parser('run')
    s.add_argument('client', choices=['d3d11bench', 'd3d11mt', 'd3d11fl12', 'dxdiag', 'taskmgr', 'exe'])
    s.add_argument('--args', default='')
    s.add_argument('--seconds', type=int, default=120)
    s.add_argument('--interactive', action='store_true')
    s.add_argument('--exe', default='')
    s.add_argument('--screenshot-at', default='')
    a = p.parse_args()
    if a.self_test:
        sys.exit(self_test())
    if a.cmd == 'package':
        m = write_package()
        verify_package()
        print(f"package {PACKAGE}: router {m['router']['sha256'][:8]} (host gate {m['router']['host_gate']}), "
              f"baseline {m['router']['baseline_sha256'][:8]} at {m['router']['active_path']}")
        return
    if not a.cmd:
        p.error('a command is required')
    for text in (getattr(a, 'allow', ''), getattr(a, 'deny', '')):
        for n in filter(None, (x.strip() for x in text.split(','))):
            if not EXE_RE.fullmatch(n):
                fail(f'not an image base name: {n}')
    if a.cmd == 'run' and not 5 <= a.seconds <= 170:
        fail('--seconds must lie in 5..170 (the lab bound is three minutes)')
    t = target()
    if a.cmd == 'push':
        verify_package()
        for remote in t.push([str(PACKAGE)], LAB_PARENT):
            print(remote)
        r = lab_script(t, 'app-stage.ps1', [], timeout=120)
    elif a.cmd == 'swap':
        r = lab_script(t, 'router-swap.ps1', ['-Action', a.action], timeout=150)
    elif a.cmd == 'policy':
        args = ['-Mode', a.mode] + (['-Allow', a.allow] if a.allow else []) + (['-Deny', a.deny] if a.deny else [])
        r = lab_script(t, 'app-policy.ps1', args, timeout=60)
    elif a.cmd == 'status':
        r = lab_script(t, 'app-status.ps1', ['-Tail', str(a.tail)] + (['-NoMappers'] if a.no_mappers else []), timeout=150)
    else:
        sys.exit(cmd_run(t, a))
    sys.exit(r.returncode)


if __name__ == '__main__':
    main()
