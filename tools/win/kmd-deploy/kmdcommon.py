"""Shared paths and checks of the generic KMD transition tool (development PC side)."""
import hashlib
import json
import os
import re
import subprocess
from pathlib import Path

# BC250_ROOT is the workspace root; by default the parent directory of this repository
# (this file is tools/win/kmd-deploy/kmdcommon.py, so the repository root is three levels up from here).
ROOT = Path(os.environ.get('BC250_ROOT', str(Path(__file__).resolve().parents[3].parent)))
# The kit the operator started: the directory this file lives in. The repository copy and the workspace
# operator copy therefore each find their own template, ops and tools.
BASE = Path(__file__).resolve().parent
TEMPLATE = BASE / 'template'
TRANSITION = 'kmd-transition'
OPS = BASE / 'ops'
# Attempts and host-test output are workspace state, never repository files: both stay under scratch whichever
# copy runs. An attempt is frozen once and never rewritten, so two copies share one attempt directory safely.
WORK = Path(os.environ.get('BC250_KMD_DEPLOY_WORK', str(ROOT / 'scratch/kmd-deploy')))
ATTEMPTS = WORK / 'attempts'
HOST_TESTS = WORK / 'host-tests'
REPO = ROOT / 'bc250-win'
# The deployed lab baseline every native trial requires; freeze pins the rollback against it, accept moves it.
LAB_BASELINE = ROOT / 'scratch/m15/native-caps001/lab-baseline.json'
HIST = ROOT / 'scratch/m14/kmd171-deploy001'
# M727 stage manifest of the measured 170 -> 171 promotion; it pins the kmd168 helpers, bounded-child.exe and
# select-driver.exe that every transition reuses byte for byte.
HIST_MANIFEST_SHA256 = 'CF57DB7375B4FE787CC98EF83D6D06F078185A34D041045BA3782A914B8C0E91'
REMOTE_BASE = 'C:\\BC250\\m15'
REMOTE_TMP = 'C:\\BC250\\tmp'
POWERSHELL = 'C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe'
ATTEMPT_RE = re.compile(r'^kmd([0-9]{3})-deploy([0-9]{3})$')
CORE = ['bc250-lab-test.cer', 'bc250kmd.cat', 'bc250kmd.inf', 'bc250kmd.sys']
IDENTITY_KEYS = ['KmdCandidateLabel', 'KmdCandidateVersion', 'KmdCandidateAbi', 'KmdRollbackLabel',
                 'KmdRollbackVersion', 'KmdRollbackAbi', 'KmdRollbackSysSha256', 'KmdRollbackInfSha256',
                 'KmdRollbackCatSha256', 'KmdCandidateMode', 'KmdSameMode', 'KmdDeployMode', 'KmdDirectoryPattern',
                 'KmdTaskName', 'KmdDesktopUmdPath', 'KmdDesktopUmdSha256', 'KmdIcdPath', 'KmdIcdSha256',
                 'KmdDesktopSwitches', 'KmdDesktopModules', 'KmdDesktopRouterKey']
MODES = {'deploy': 'KmdDeployMode', 'rehearsal': 'KmdCandidateMode'}


def ps_env():
    """Environment for a Windows PowerShell 5.1 child, with the inbox module path spelled out.

    A 5.1 child that inherits a PowerShell 7 parent's PSModulePath, or no PSModulePath at all, cannot find
    `Get-FileHash`: every staging host test then fails with CommandNotFoundException and says nothing about why.
    The kit used to answer this with an operator rule ("run it from Git Bash or cmd, never from a PowerShell 7
    parent"); setting the variable here holds whatever shell starts the kit, including tools/quality/quick.ps1."""
    system_root = os.environ.get('SystemRoot', 'C:\\Windows')
    program_files = os.environ.get('ProgramFiles', 'C:\\Program Files')
    parts = [os.path.join(os.environ['USERPROFILE'], 'Documents', 'WindowsPowerShell', 'Modules')] \
        if os.environ.get('USERPROFILE') else []
    parts += [os.path.join(program_files, 'WindowsPowerShell', 'Modules'),
              os.path.join(system_root, 'system32', 'WindowsPowerShell', 'v1.0', 'Modules')]
    env = dict(os.environ)
    env['PSModulePath'] = ';'.join(parts)
    return env


def fail(message):
    raise SystemExit('REFUSED: ' + message)


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest().upper()


def identity(path=None):
    """Parse an identity.ps1 (the template fixture by default, or an attempt's generated one)."""
    path = path or TEMPLATE / TRANSITION / 'identity.ps1'
    values = dict(re.findall(r"^\$(Kmd\w+)='([^']*)'\s*$", Path(path).read_text(encoding='utf-8'), re.M))
    missing = [n for n in IDENTITY_KEYS if n not in values]
    if missing:
        raise SystemExit(f'{path} lacks ' + ', '.join(missing))
    return values


def git_show(repo, commit, path):
    return subprocess.check_output(['git', '-C', str(repo), 'show', f'{commit}:{path}']).decode('utf-8', 'replace')


def read_package(directory, repo, role):
    """Everything about a package from its own directory, its source-manifest.json and its commit.

    `directory` holds bc250kmd.sys/.inf/.cat, the .cer and source-manifest.json: a build's package directory, or a
    candidateNNN directory of an accepted attempt. Version from DriverVer (package INF = source INF at the commit),
    ABI from BC250_KMD_VERSION at the commit, and both must encode the same revision (0.7.R.1 <-> 0x0007RRRR)."""
    directory = Path(directory).resolve()
    manifest_path = directory / 'source-manifest.json'
    if not manifest_path.is_file():
        fail(f'{role}: no source-manifest.json in {directory}')
    manifest = json.loads(manifest_path.read_text(encoding='utf-8-sig'))
    if manifest.get('status') != 'BUILT' or manifest.get('deployment_source_eligible') is not True:
        fail(f'{role}: build is not BUILT and deployment-source eligible')
    source = manifest['source']
    if source.get('source_clean') is not True:
        fail(f'{role}: build source not clean')
    flavors = [f for f, files in manifest.get('packages', {}).items()
               if {k.upper(): v for k, v in files.items()} and
               all((directory / n).is_file() and digest(directory / n) == v.upper() for n, v in files.items())]
    if len(flavors) != 1:
        fail(f'{role}: {directory} matches {len(flavors)} package flavors of its manifest')
    flavor = flavors[0]
    if flavor != 'package':
        fail(f'{role}: flavor {flavor}; the lab runs the plain package (no UMD stub, no UserModeDriverName)')
    listed = {n: v.upper() for n, v in manifest['packages'][flavor].items()}
    present = {p.name for p in directory.iterdir() if p.is_file() and p.name != 'source-manifest.json'}
    if present != set(listed):
        fail(f'{role}: files differ from the manifest: {sorted(present ^ set(listed))}')
    missing = [n for n in CORE if n not in present]
    if missing:
        fail(f'{role}: lacks {missing}')
    commit = source['commit']
    abi = re.findall(r'^#define BC250_KMD_VERSION (0x[0-9A-Fa-f]{8})u', git_show(repo, commit, 'driver/kmd/bc250kmd_escape.h'), re.M)
    if len(abi) != 1:
        fail(f'{role}: BC250_KMD_VERSION unreadable at {commit}')
    abi = '0x' + abi[0][2:].upper()
    inf = (directory / 'bc250kmd.inf').read_text(encoding='utf-8-sig', errors='replace')
    versions = set()
    for text, where in ((inf, 'package INF'), (git_show(repo, commit, 'driver/kmd/bc250kmd.inf'), 'source INF')):
        ver = re.findall(r'^DriverVer\s*=\s*[0-9/]+,([0-9.]+)\s*$', text, re.M)
        if len(ver) != 1:
            fail(f'{role}: {where} DriverVer unreadable')
        versions.add(ver[0])
    if len(versions) != 1:
        fail(f'{role}: package and source INF disagree on DriverVer: {sorted(versions)}')
    version = versions.pop()
    parts = version.split('.')
    if len(parts) != 4 or parts[:2] != ['0', '7'] or parts[3] != '1' or int(abi[6:], 16) != int(parts[2]):
        fail(f'{role}: version {version} and ABI {abi} do not encode one revision')
    # The detector handling assumes exactly this contract: the install closes EnableHangBugcheck and never writes
    # HangBugcheckSeconds; hang.c reads both from the service Parameters key.
    if not re.search(r'^HKR,\s*Parameters,\s*EnableHangBugcheck,\s*0x00010001,\s*0\s*$', inf, re.M):
        fail(f'{role}: package INF does not close EnableHangBugcheck')
    if re.search(r'^HKR,.*HangBugcheckSeconds', inf, re.M):
        fail(f'{role}: package INF writes HangBugcheckSeconds; revisit the detector handling')
    hang = git_show(repo, commit, 'driver/kmd/hang.c')
    for name in ('EnableHangBugcheck', 'HangBugcheckSeconds'):
        if f'GuardReadSetting(L"{name}"' not in hang:
            fail(f'{role}: hang.c at {commit} does not read {name}')
    files = dict(listed)
    files['source-manifest.json'] = digest(manifest_path)
    return {'directory': directory, 'commit': commit, 'tree': source.get('tree'), 'abi': abi, 'version': version,
            'revision': int(parts[2]), 'flavor': flavor, 'files': files, 'build_manifest_sha256': digest(manifest_path)}


def attempt_name(arg):
    if not ATTEMPT_RE.match(arg):
        raise SystemExit('Attempt names look like kmd175-deploy001, got ' + arg)
    return arg


def resolve_attempt(arg):
    """'kmd175-deploy001' -> (name, attempt dir, ops dir), both existing."""
    name = attempt_name(arg)
    attempt, ops = ATTEMPTS / name, ATTEMPTS / (name + '-ops')
    if not attempt.is_dir() or not ops.is_dir():
        raise SystemExit('Attempt not frozen: ' + name)
    return name, attempt, ops


def verify_attempt(attempt, ops):
    """The local tree still equals its frozen manifest, and the manifest is the one recorded at freeze."""
    manifest_path = attempt / 'stage-manifest.json'
    expected = (ops / 'stage-manifest.sha256').read_text(encoding='ascii').strip()
    if digest(manifest_path) != expected:
        raise SystemExit('Local stage manifest differs from the frozen hash')
    files = json.loads(manifest_path.read_text(encoding='utf-8'))['files']
    present = {p.relative_to(attempt).as_posix() for p in attempt.rglob('*') if p.is_file()}
    if present != set(files) | {'stage-manifest.json'}:
        raise SystemExit('Local attempt tree has files outside its manifest')
    for rel, value in files.items():
        if digest(attempt / rel) != value:
            raise SystemExit('Local attempt file changed: ' + rel)
    return expected


def historical_manifest():
    path = HIST / 'stage-manifest.json'
    if digest(path) != HIST_MANIFEST_SHA256:
        raise SystemExit('Historical 171 stage manifest changed; refusing to reuse its files')
    return json.loads(path.read_text(encoding='utf-8'))['files']


def next_receipt(ops, stem, suffix):
    n = 0
    while (ops / f'{stem}-{n}{suffix}').exists():
        n += 1
    return ops / f'{stem}-{n}{suffix}'


HEARTBEAT_TASK = 'Lab-Present-Heartbeat'


def heartbeat(t, seconds):
    """Start the present heartbeat (ops/heartbeat.ps1, a small top-most window in the owner's session whose text
    changes every 250 ms, as the interactive user through the one-shot task Lab-Present-Heartbeat; it exits by
    itself after `seconds`). An idle desktop presents about once a minute, so without it the KMD start-health
    witness goes stale (age_ms > 15000) and every confirmation fails (kmd175-deploy002). Its task name must stay
    outside the BC250|DWM|G0|WSI filter of preflight/postflight: it is not a test task."""
    if re.search('BC250|DWM|G0|WSI', HEARTBEAT_TASK, re.I):
        raise SystemExit('heartbeat task name would trip the competing-task gates')
    for script in ('heartbeat-start.ps1', 'heartbeat-stop.ps1'):
        if f"$name = '{HEARTBEAT_TASK}'" not in (OPS / script).read_text(encoding='utf-8'):
            raise SystemExit(f'{script} does not name {HEARTBEAT_TASK}')
    t.push([OPS / 'heartbeat.ps1'], REMOTE_TMP)
    r = t.run_script(OPS / 'heartbeat-start.ps1', args=['-Seconds', str(seconds)], timeout=60)
    if r.returncode or 'task state Running' not in r.stdout:
        raise SystemExit('present heartbeat did not start: ' + (r.stdout + r.stderr)[-400:])
    return r.stdout.strip()


def heartbeat_stop(t):
    r = t.run_script(OPS / 'heartbeat-stop.ps1', timeout=60)
    return (r.stdout + r.stderr).strip()


def target():
    """The lab target through tools/win/target.py: the sibling copy when this kit sits in the repository, the
    workspace repository otherwise. The addresses stay in secrets/client/target.json, outside the repository."""
    import sys
    tools_win = BASE.parent if (BASE.parent / 'target.py').is_file() else REPO / 'tools/win'
    sys.path.insert(0, str(tools_win))
    from target import Target
    return Target()
