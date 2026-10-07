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
# Attempt names: kmdRRR-deployNNN for a build 1 candidate (every attempt before 0.7.216), kmdRRR-B-deployNNN for a
# build B >= 2 candidate. "-1" is never written, so one (R, B) has exactly one name (label_suffix).
ATTEMPT_RE = re.compile(r'^kmd([0-9]{3})(?:-(?!1-)([1-9][0-9]*))?-deploy([0-9]{3})$')
CORE = ['bc250-lab-test.cer', 'bc250kmd.cat', 'bc250kmd.inf', 'bc250kmd.sys']
DRIVER_VER_RE = re.compile(r'^DriverVer\s*=\s*[0-9/]+,([0-9.]+)\s*$', re.M)
FIELD_RE = re.compile(r'0|[1-9][0-9]*')
# A release package (tools/release/build-release.ps1): payload/kmd holds the KMD, payload/cert the certificate that
# signed it, manifest.json the hashes. The release makes exactly two changes to the INF of the build it packages:
# DriverVer gets the release's own fourth field, and Add-InfRebootDirective (tools/release/installer/common.ps1)
# puts this line after each install section header. It re-signs the SYS and makes a new catalog; the code is the
# build's.
RELEASE_KMD = 'payload/kmd'
RELEASE_CER = 'amdgpu-wddm-release.cer'
RELEASE_REBOOT_LINE = 'Reboot                                          ; release package: the GPU changes driver at the next restart'
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


def parse_version(version, what):
    """'0.7.R.B' -> (R, B). R is the ABI revision, B the build counter, B >= 1.

    Until 0.7.215 every build was 0.7.R.1. From 0.7.216 a build counts up (0.7.216.14, 0.7.216.16), and a release
    package carries a fourth field of its own (0.7.216.100). Fields are canonical decimals, so that one (R, B) has
    one spelling and one label."""
    parts = str(version).split('.')
    if (len(parts) != 4 or parts[:2] != ['0', '7'] or not all(FIELD_RE.fullmatch(p) for p in parts[2:])
            or int(parts[3]) < 1):
        fail(f'{what}: DriverVer {version} is not 0.7.R.B with canonical decimal R and B >= 1')
    return int(parts[2]), int(parts[3])


def check_revision(version, abi, role):
    """(R, B) of `version`, refused unless R is the low 16 bits of the ABI (0.7.R.B <-> 0x0007RRRR)."""
    if not re.fullmatch(r'0x0007[0-9A-F]{4}', abi):
        fail(f'{role}: ABI {abi} is not 0x0007RRRR')
    revision, build = parse_version(version, role)
    if int(abi[6:], 16) != revision:
        fail(f'{role}: version {version} and ABI {abi} do not encode one revision')
    return revision, build


def label_suffix(version):
    """'0.7.175.1' -> '175' (the names of every attempt before 0.7.216), '0.7.216.16' -> '216-16'. The labels
    candidate<suffix> and rollback<suffix> and the attempt prefix kmd<suffix>-deploy are therefore unique per (R, B)."""
    revision, build = parse_version(version, 'label')
    return f'{revision:03d}' if build == 1 else f'{revision:03d}-{build}'


def inf_driver_ver(text, role, where):
    found = DRIVER_VER_RE.findall(text)
    if len(found) != 1:
        fail(f'{role}: {where} DriverVer unreadable')
    return found[0]


def read_text_exact(path):
    """The file as text with its own line ends (CRLF stays CRLF), for byte-exact comparisons."""
    return Path(path).read_bytes().decode('utf-8-sig', errors='replace')


def build_version(package_inf, source_inf, abi, role):
    """A build package names the DriverVer of its source INF exactly, and that version names the ABI's revision."""
    package = inf_driver_ver(package_inf, role, 'package INF')
    source = inf_driver_ver(source_inf, role, 'source INF')
    if package != source:
        fail(f'{role}: package and source INF disagree on DriverVer: {sorted({package, source})}')
    check_revision(package, abi, role)
    return package


def release_version(release_inf, build_inf, build_ver, manifest, role):
    """The one rule under which a package INF and its source INF may name different DriverVer values.

    build-release.ps1 keeps the first three fields of the build (release-sources.json kmd_version, which manifest.json
    records as kmd_build), writes its own fourth field (manifest.json kmd_version) into DriverVer and adds
    RELEASE_REBOOT_LINE after each install section header. Refused unless all of that holds: the release INF names
    manifest kmd_version, the build names manifest kmd_build, both name the same 0.7.R, and the release INF without
    those two edits is the build INF character for character. Returns the release version."""
    kmd_version, kmd_build = str(manifest.get('kmd_version')), str(manifest.get('kmd_build'))
    package = inf_driver_ver(release_inf, role, 'release INF')
    if package != kmd_version:
        fail(f'{role}: release INF DriverVer {package} is not manifest.json kmd_version {kmd_version}')
    if build_ver != kmd_build:
        fail(f'{role}: build DriverVer {build_ver} is not manifest.json kmd_build {kmd_build}')
    if parse_version(package, role + ' release')[0] != parse_version(build_ver, role + ' build')[0]:
        fail(f'{role}: release {package} and build {build_ver} name different revisions')
    stripped = ''.join(line for line in release_inf.splitlines(keepends=True)
                       if line.rstrip('\r\n') != RELEASE_REBOOT_LINE)
    restored = DRIVER_VER_RE.sub(lambda m: m.group(0).replace(',' + package, ',' + build_ver, 1), stripped)
    if restored != build_inf:
        fail(f'{role}: the release INF differs from the build INF in more than DriverVer and the Reboot directive')
    return package


def image_digest(path):
    """SHA256 of a PE file without its signature: the checksum field, the security directory entry and the
    certificate table are left out (the Authenticode digest, in file order). A re-signed file keeps this digest."""
    data = Path(path).read_bytes()
    pe = int.from_bytes(data[0x3C:0x40], 'little')
    if data[:2] != b'MZ' or data[pe:pe + 4] != b'PE\0\0':
        fail(f'{path}: not a PE file')
    optional = pe + 24
    magic = int.from_bytes(data[optional:optional + 2], 'little')
    if magic not in (0x10B, 0x20B):
        fail(f'{path}: unknown optional header {magic:#x}')
    security = optional + (96 if magic == 0x10B else 112) + 4 * 8
    table = int.from_bytes(data[security:security + 4], 'little')
    size = int.from_bytes(data[security + 4:security + 8], 'little')
    end = table or len(data)
    if table and table + size != len(data):
        fail(f'{path}: data after the certificate table')
    checksum = optional + 64
    h = hashlib.sha256()
    h.update(data[:checksum])
    h.update(data[checksum + 4:security])
    h.update(data[security + 8:end])
    return h.hexdigest().upper()


def check_detector(inf, repo, commit, role):
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


def is_release(directory):
    return (Path(directory) / 'manifest.json').is_file() and (Path(directory) / RELEASE_KMD).is_dir()


def read_package(directory, repo, role, build=None):
    """Everything about a package: a build's package directory, or a release package with the build it packages.

    A build directory holds bc250kmd.sys/.inf/.cat, bc250-lab-test.cer and source-manifest.json: a package directory
    of driver/kmd/build.ps1, or a candidate directory of an accepted attempt. A release package is the directory of
    tools/release/build-release.ps1 (manifest.json, payload/kmd, payload/cert); `build` must then name the build
    package directory it was made from (release-sources.json `source` of payload/kmd)."""
    if is_release(directory):
        if build is None:
            fail(f'{role}: {directory} is a release package; name the build it packages with --rollback-build')
        return read_release(directory, build, repo, role)
    if build is not None:
        fail(f'{role}: --rollback-build goes only with a release package')
    return read_build(directory, repo, role)


def read_build(directory, repo, role):
    """A build package, from its own directory, its source-manifest.json and its commit.

    Version from DriverVer (package INF = source INF at the commit), ABI from BC250_KMD_VERSION at the commit, and
    the version 0.7.R.B must name the ABI's revision (R = the ABI's low 16 bits, B >= 1)."""
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
    inf = read_text_exact(directory / 'bc250kmd.inf')
    package = build_version(inf, git_show(repo, commit, 'driver/kmd/bc250kmd.inf'), abi, role)
    revision, build = parse_version(package, role)
    check_detector(inf, repo, commit, role)
    files = dict(listed)
    files['source-manifest.json'] = digest(manifest_path)
    return {'kind': 'build', 'directory': directory, 'commit': commit, 'tree': source.get('tree'), 'abi': abi,
            'version': package, 'revision': revision, 'build': build, 'flavor': flavor, 'files': files,
            'paths': {n: directory / n for n in files}, 'certificate': directory / 'bc250-lab-test.cer',
            'signed': directory, 'build_manifest_sha256': digest(manifest_path), 'inf_text': inf}


def read_release(root, build_dir, repo, role):
    """A release package of tools/release over the build it packages, under the rule of release_version.

    Each payload file must have its hash in manifest.json, manifest kmd_abi must be the build's ABI, the release SYS
    must be the build SYS re-signed (same image_digest), and the release INF must pass release_version. The
    identity is the release's own DriverVer (0.7.R.<release field>); commit, tree and the detector source are the
    build's. The SYS and CAT are signed by the release certificate, which manifest.json names."""
    root = Path(root).resolve()
    build = read_build(build_dir, repo, role + ' build')
    manifest_path = root / 'manifest.json'
    manifest = json.loads(manifest_path.read_text(encoding='utf-8-sig'))
    if manifest.get('schema') != 1 or not isinstance(manifest.get('files'), list):
        fail(f'{role}: {manifest_path} is not a schema 1 release manifest')
    listed = {f.get('path'): str(f.get('sha256', '')).upper() for f in manifest['files']}
    kmd = root / RELEASE_KMD
    paths = {n: kmd / n for n in ('bc250kmd.sys', 'bc250kmd.inf', 'bc250kmd.cat')}
    paths[RELEASE_CER] = root / 'payload/cert' / RELEASE_CER
    present = {f'{RELEASE_KMD}/{p.name}' for p in kmd.iterdir() if p.is_file()}
    if present != {k for k in listed if k.startswith(RELEASE_KMD + '/')}:
        fail(f'{role}: {kmd} differs from manifest.json: {sorted(present ^ {k for k in listed if k.startswith(RELEASE_KMD + "/")})}')
    files = {}
    for name, path in paths.items():
        rel = path.relative_to(root).as_posix()
        if not path.is_file() or rel not in listed or digest(path) != listed[rel]:
            fail(f'{role}: {rel} is missing or differs from manifest.json')
        files[name] = listed[rel]
    if str(manifest.get('kmd_abi', '')).upper().replace('0X', '0x') != build['abi']:
        fail(f'{role}: manifest.json kmd_abi {manifest.get("kmd_abi")} is not the build ABI {build["abi"]}')
    inf = read_text_exact(paths['bc250kmd.inf'])
    version = release_version(inf, build['inf_text'], build['version'], manifest, role)
    revision, release_build = check_revision(version, build['abi'], role)
    check_detector(inf, repo, build['commit'], role)
    if image_digest(paths['bc250kmd.sys']) != image_digest(build['paths']['bc250kmd.sys']):
        fail(f'{role}: the release SYS is not the build SYS re-signed (image digests differ)')
    files['release-manifest.json'] = digest(manifest_path)
    files['build-source-manifest.json'] = build['files']['source-manifest.json']
    paths['release-manifest.json'] = manifest_path
    paths['build-source-manifest.json'] = build['paths']['source-manifest.json']
    return {'kind': 'release', 'directory': root, 'commit': build['commit'], 'tree': build['tree'], 'abi': build['abi'],
            'version': version, 'revision': revision, 'build': release_build, 'flavor': 'release', 'files': files,
            'paths': paths, 'certificate': paths[RELEASE_CER], 'signed': kmd,
            'release_certificate': str(manifest.get('release_certificate', '')).upper(),
            'release': manifest.get('release'), 'kmd_build': build['version'],
            'build_manifest_sha256': build['build_manifest_sha256'], 'release_manifest_sha256': digest(manifest_path),
            'build_directory': build['directory'], 'inf_text': inf}


def compare_packages(candidate, rollback):
    """The candidate must differ from the rollback in its full version (R, B) and in its SYS. The lab arms tell the
    two drivers apart by DriverVer and by the SYS hash; the ABI may be equal when both share a revision. The
    same-package control (one package as both) is never frozen by this kit."""
    if candidate['kind'] != 'build':
        fail('candidate: a release package is never a candidate; freeze its build package')
    if (candidate['revision'], candidate['build']) == (rollback['revision'], rollback['build']):
        fail(f'candidate and rollback are the same version {candidate["version"]}; the same-package control is not frozen here')
    if candidate['files']['bc250kmd.sys'] == rollback['files']['bc250kmd.sys']:
        fail('candidate and rollback carry the same SYS; the same-package control is not frozen here')
    if label_suffix(candidate['version']) == label_suffix(rollback['version']):
        fail('candidate and rollback labels collide')


def attempt_name(arg):
    if not ATTEMPT_RE.match(arg):
        raise SystemExit('Attempt names look like kmd175-deploy001 or kmd216-16-deploy001, got ' + arg)
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
