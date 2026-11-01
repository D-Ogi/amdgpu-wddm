"""Offline release pins for the native trial kit. This module never contacts a target.

STATE.md's Current block is the local publication authority. A separately pinned
publication receipt binds its release to the exact manifest. Scratch candidates
and newer build directories are not publication records.
"""
import argparse
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath, PureWindowsPath
import re
import shlex
import sys
import zipfile


class Refusal(RuntimeError):
    pass


def require(ok, message):
    if not ok:
        raise Refusal(message)


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest().upper()


def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def sha(value):
    require(isinstance(value, str) and re.fullmatch('[0-9A-F]{64}', value), 'Malformed SHA-256')
    return value


def local(root, path):
    path = Path(path).resolve()
    require(path.is_relative_to(Path(root).resolve()), 'Path outside workspace: ' + str(path))
    return path


# The two STATE.md blocks that describe what is deployed now. "Current" changes with every lab action and often names
# only the last trial; "Installed package baseline" names the package and its GitHub release (STATE.md since
# 2026-11-01). History blocks ("Previous ...") never count.
LIVE_BLOCKS = ('## Current', '## Installed package baseline')


def current_publication(state):
    first = {}
    for m in re.finditer(r'(?ms)^## [^\n]*\n.*?(?=^## |\Z)', state):
        for kind in LIVE_BLOCKS:
            # Only the first block of each kind: a later "## Current..." heading is an older copy, not the state.
            if m.group(0).startswith(kind):
                first.setdefault(kind, m.group(0))
    require('## Current' in first, 'STATE.md has no Current block')
    blocks = list(first.values())
    found = set()
    for text in blocks:
        if 'GitHub release' not in text:
            continue
        versions = set(re.findall(r'v(\d+\.\d+\.\d+\.\d+-tester\.\d+)\b', text))
        require(len(versions) == 1, 'Current publication tag is missing or ambiguous')
        hashes = set(re.findall(r'\bmanifest ([0-9A-F]{8,64})\b', text))
        require(len(hashes) == 1, 'Current publication manifest hash is missing or ambiguous')
        found.add((versions.pop(), hashes.pop()))
    require(found, 'Current deployment has no publication record')
    require(len(found) == 1, 'The Current and Installed package blocks name different publications')
    return found.pop()


def validate_fresh(baseline, published, current):
    """A candidate selection cannot waive an obsolete installed baseline."""
    release = baseline.get('release', {})
    require(published.get('release') == current, 'Published pin is stale: refresh it from the current receipt')
    require(release.get('release') == current,
            'Baseline does not match the newest published release ' + current)
    require(release.get('manifest_sha256') == published.get('manifest_sha256'),
            'Baseline manifest differs from the published manifest')


def receipt_release(text):
    records = []
    for line in text.splitlines():
        try:
            entry = json.loads(line)
        except ValueError:
            continue
        if isinstance(entry, dict) and 'tag' in entry and 'draft' in entry:
            records.append(entry)
    require(len(records) == 1 and records[0]['draft'] is False,
            'Publication receipt must contain one non-draft release record')
    return records[0]['tag'].removeprefix('v')


def load_published(kit, root):
    pin = read(kit / 'published-release.json')
    manifest_path = local(root, pin['manifest_path'])
    require(digest(manifest_path) == sha(pin['manifest_sha256']), 'Published manifest bytes changed')
    receipt = local(root, pin['publication_receipt'])
    require(digest(receipt) == sha(pin['publication_receipt_sha256']), 'Publication receipt bytes changed')
    require(receipt_release(receipt.read_text(encoding='utf-8-sig')) == pin['release'],
            'Publication receipt names another release')
    version, prefix = current_publication((root / 'STATE.md').read_text(encoding='utf-8-sig'))
    require(pin['release'] == version and pin['manifest_sha256'].startswith(prefix),
            'Published pin is stale relative to STATE.md Current')
    manifest = read(manifest_path)
    require(manifest['release'] == version and manifest.get('flavour') == 'release',
            'Published manifest is not the current release flavour')
    return pin, manifest, manifest_path


# Logical source names from the legacy kit. The package member map supplies the
# actual installed names (including renamed ICD and WOW64 members).
SOURCES = {
    'router': 'payload/desktop/bc250d3d_router.dll',
    'cpu_umd': 'payload/desktop/bc250d3d.dll',
    'zink': 'payload/desktop/bc250d3d_zink.dll',
    'desktop_icd': 'payload/desktop/amdgpu_wddm_radv.dll',
    'icd': 'payload/vulkan/vulkan_radeon.dll',
    'd3d11': 'payload/d3d11/amdgpu_wddm_d3d11.dll',
    'shell': 'payload/d3d12/amdgpu_wddm_d3d12.dll',
    'engine': 'payload/d3d12/amdgpu_wddm_vkd3d.dll',
    'd3d12_icd': 'payload/d3d12/amdgpu_wddm_radv.dll',
}


def package_members(manifest):
    package = manifest['driver_package']
    require(package.get('flavour') == 'release', 'Driver package is not release flavour')
    members, paths = {}, set()
    files = {f['path']: f['sha256'] for f in manifest['files']}
    components = {}
    for c in manifest['components']:
        if not c.get('package_path'):
            continue  # Firmware provenance rows are not driver-package members.
        require(c['package_path'] not in components, 'Duplicate component package_path')
        components[c['package_path']] = c
    for m in package['members']:
        member = PurePosixPath(m['member'])
        require(not member.is_absolute() and '..' not in member.parts and '\\' not in m['member'],
                'Invalid DriverStore member path')
        require(m['path'] == package['root'] + '/' + m['member'], 'Member root mismatch')
        require(m['source'] not in members and m['member'].lower() not in paths, 'Duplicate package member')
        require(files.get(m['path']) == sha(m['sha256']), 'Member/file hash mismatch')
        require(components.get(m['path'], {}).get('sha256') == m['sha256'], 'Member/component hash mismatch')
        require(components[m['path']]['install_path'] == 'DriverStore (bc250kmd.inf)\\' + m['member'].replace('/', '\\'),
                'Member/component installation path mismatch')
        members[m['source']] = m
        paths.add(m['member'].lower())
    require(set(SOURCES.values()).issubset(members), 'Manifest is missing required driver members')
    return members


def derive(manifest, manifest_hash, manifest_path, install_root, admission=None):
    """Derive pins without asserting that publication proves an installed boot."""
    require(manifest.get('flavour') == 'release', 'Expected release manifest')
    require(re.fullmatch(r'[A-Za-z]:\\[^\x00\r\n\'@]+', install_root) and not install_root.endswith('\\'),
            'Malformed install root')
    version = manifest['kmd_version'].split('.')
    abi = manifest['kmd_abi']
    require(len(version) == 4 and version[:2] == ['40', '7'] and all(p.isdigit() for p in version)
            and re.fullmatch('0x000700[0-9A-F]{2}', abi) and int(version[2]) == int(abi[6:], 16),
            'Release KMD version/ABI mismatch')
    members = package_members(manifest)
    selected = {k: members[v] for k, v in SOURCES.items()}
    cli = [c for c in manifest['components'] if c['install_path'] == '<InstallDir>\\tools\\bc250kmd_cli.exe']
    require(len(cli) == 1, 'Expected exactly one installed CLI')
    kmd = [c for c in manifest['components'] if c['role'] == 'kmd'
           and c.get('package_path', '').endswith('/bc250kmd.sys')]
    require(len(kmd) == 1, 'Expected exactly one KMD')
    for c in cli + kmd:
        require(any(f['path'] == c['package_path'] and f['sha256'] == sha(c['sha256']) for f in manifest['files']),
                'Component/file hash mismatch')
    root = '<DriverStore>'
    if admission is not None:
        require(admission.get('manifest_sha256') == manifest_hash, 'Admission names another manifest')
        require(admission.get('kmd_version') == manifest['kmd_version'], 'Admission KMD version mismatch')
        root = admission.get('driver_store_root', '')
        require(re.fullmatch(r'[A-Za-z]:\\Windows\\System32\\DriverStore\\FileRepository\\bc250kmd\.inf_[A-Za-z0-9_]+', root, re.I),
                'Invalid admitted DriverStore root')
        require(re.fullmatch(r'\d{4}-\d\d-\d\dT[0-9:.]+Z', admission.get('boot_utc', '')), 'Admission needs exact boot UTC')
        require(admission.get('state_reader') == 'cli-state-snapshot-v1', 'Admission lacks shared-state reader')
        require(isinstance(admission.get('games'), dict) and admission['games'], 'Admission needs game records')
    def path(key):
        return root + '\\' + selected[key]['member'].replace('/', '\\')
    def h(key):
        return selected[key]['sha256']
    def registration(key):
        entry = manifest['driver_package']['registration'][key]
        values = entry['value'] if isinstance(entry['value'], list) else [entry['value']]
        result = []
        for value in values:
            if value == '<>':
                result.append(value)
                continue
            require(value.startswith('%13%\\'), 'Registration outside DriverStore')
            member = value[5:].replace('\\', '/')
            require(member in {m['member'] for m in members.values()}, 'Registration names no member')
            result.append(root + '\\' + member.replace('/', '\\'))
        if key.startswith('Vulkan'):
            require(len(result) == 1, 'Expected exactly one Vulkan registration')
            return result[0]
        return result if isinstance(entry['value'], list) else result[0]
    names = {PurePosixPath(selected[k]['member']).name: h(k) for k in ('shell', 'engine', 'd3d12_icd')}
    b = dict(
        note='Published release pins. Installed admission is a separate, boot-bound input.',
        release=dict(name=manifest['name'], release=manifest['release'], kmd_build=manifest['kmd_build'],
                     manifest_sha256=sha(manifest_hash), install_root=install_root,
                     manifest_path=install_root + '\\manifest.json'),
        admission_required=admission is None,
        kmd_version=manifest['kmd_version'], kmd_abi=abi, kmd_sys_sha256=kmd[0]['sha256'],
        umd_path=path('cpu_umd'), umd_sha256=h('cpu_umd'), icd_path=path('icd'), icd_sha256=h('icd'),
        d3d11_path=path('d3d11'), d3d11_sha256=h('d3d11'),
        cli_path=install_root + '\\tools\\bc250kmd_cli.exe', cli_sha256=cli[0]['sha256'],
        desktop=dict(registered_path=path('router'), registered_sha256=h('router'), hosted_umd_path=path('zink'),
                     switches=1, default_route='gpu' if manifest['defaults']['desktop_router']['DwmForceCpu'] == 0 else 'cpu',
                     dwm_routes=dict(cpu=[h('router'), h('cpu_umd')], gpu=[h('router'), h('zink'), h('desktop_icd')])),
        registration=dict(umd_names=registration('UserModeDriverName'), umd_names_wow=registration('UserModeDriverNameWow'),
                          vulkan_name=registration('VulkanDriverName'), vulkan_name_wow=registration('VulkanDriverNameWow')),
        d3d12=dict(directory=str(PureWindowsPath(path('shell')).parent), accepted=names),
        session={k: str(Path(manifest_path).parent / selected[v]['path'])
                 for k, v in (('shell', 'shell'), ('engine', 'engine'), ('icd', 'd3d12_icd'))},
        session_names={k: PurePosixPath(selected[v]['member']).name
                       for k, v in (('shell', 'shell'), ('engine', 'engine'), ('icd', 'd3d12_icd'))})
    if admission is not None:
        expected = {root + '\\' + m['member'].replace('/', '\\'): m['sha256'] for m in members.values()}
        require(admission.get('member_hashes') == expected, 'Installed member hashes differ from publication')
        require(admission.get('registration') == b['registration'], 'Installed registration differs from publication')
        b['final_installed'] = copy.deepcopy(admission)
    return b


def load_baseline(kit, root, baseline_path=None, require_admission=True):
    kit, root = Path(kit), Path(root)
    published, manifest, manifest_path = load_published(kit, root)
    path = local(root, baseline_path or kit / 'lab-baseline.json')
    b = read(path)
    validate_fresh(b, published, published['release'])
    require('session_names' in b and 'admission_required' in b,
            'Legacy baseline schema: regenerate with release-baseline.py and a current installed admission')
    expected = derive(manifest, published['manifest_sha256'], manifest_path,
                      b['release']['install_root'], b.get('final_installed'))
    for key, value in expected.items():
        require(b.get(key) == value, 'Baseline field differs from manifest/admission: ' + key)
    if require_admission:
        require(not b['admission_required'],
                'Installed admission missing: capture current boot, DriverStore members and games after maintenance')
        spec = importlib.util.spec_from_file_location('baseline_final_installed', kit / 'final-installed.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        module.validate(b)
    return b


def check_staged(config, baseline):
    require(config.get('published_release') == baseline['release'],
            'Staged attempt predates the current published baseline: stage a new attempt')
    require(config.get('final_installed') == baseline.get('final_installed'),
            'Staged attempt admission changed: stage a new attempt')


def triplet(baseline, root, env):
    """Retain explicit selections; never silently replace them with release files."""
    paths = dict(baseline['session'])
    adapter = env.get('SESSION_ADAPTER') or 'adapter000'
    require(re.fullmatch('adapter[0-9]{3}', adapter), 'SESSION_ADAPTER must be adapterNNN')
    if env.get('SESSION_ADAPTER') and adapter != 'adapter000':
        paths['shell'] = str(Path(root) / 'scratch/m15' / adapter / 'amdgpu_wddm_d3d12.dll')
        if env.get('SESSION_SHELL'):
            require(local(root, env['SESSION_SHELL']) == local(root, paths['shell']),
                    'Conflicting explicit adapter and shell selections')
    for key, variable in (('shell', 'SESSION_SHELL'), ('engine', 'SESSION_ENGINE'), ('icd', 'SESSION_ICD')):
        if env.get(variable):
            paths[key] = env[variable]
    for key, variable in (('engine', 'M157_ENGINE'), ('icd', 'M157_ICD')):
        if env.get(variable):
            paths[key] = env[variable]
    values = [local(root, paths[k]) for k in ('shell', 'engine', 'icd')]
    require(all(p.is_file() for p in values), 'Session artifact missing')
    hashes = [digest(p) for p in values]
    accepted = [baseline['d3d12']['accepted'][baseline['session_names'][k]] for k in ('shell', 'engine', 'icd')]
    in_place = hashes == accepted
    return adapter, values, hashes, in_place


def write_new(path, data):
    path = Path(path)
    text = json.dumps(data, indent=2) + '\n'
    if path.exists():
        # Each refresh retains the exact previous bytes; no destructive overwrite.
        old = path.with_name(path.name + '.before-' + digest(path)[:16])
        if old.exists():
            require(old.read_bytes() == path.read_bytes(), 'Backup collision')
        else:
            old.write_bytes(path.read_bytes())
    path.write_text(text, encoding='utf-8')


def pin_publication(kit, root, manifest_path, receipt_path, archive_path):
    manifest_path, receipt_path, archive_path = [local(root, p) for p in (manifest_path, receipt_path, archive_path)]
    manifest = read(manifest_path)
    text = receipt_path.read_text(encoding='utf-8-sig')
    version, prefix = current_publication((root / 'STATE.md').read_text(encoding='utf-8-sig'))
    require(receipt_release(text) == version == manifest['release'], 'Receipt, manifest and STATE disagree')
    require(digest(manifest_path).startswith(prefix), 'Manifest differs from STATE publication')
    archive_hash = digest(archive_path)
    require(f'local {archive_path.name} {archive_hash} ' in text
            and f'downloaded {archive_path.name} {archive_hash[:8]} ok' in text,
            'Publication receipt does not bind the downloaded archive')
    with zipfile.ZipFile(archive_path) as archive:
        names = [n for n in archive.namelist() if n.endswith('/files/manifest.json') or n == 'files/manifest.json']
        require(len(names) == 1 and archive.read(names[0]) == manifest_path.read_bytes(),
                'Published archive manifest differs')
    pin = dict(release=version, manifest_path=str(manifest_path), manifest_sha256=digest(manifest_path),
               publication_receipt=str(receipt_path), publication_receipt_sha256=digest(receipt_path),
               archive_path=str(archive_path), archive_sha256=archive_hash)
    write_new(kit / 'published-release.json', pin)
    return pin


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--kit', type=Path, required=True)
    p.add_argument('--root', type=Path, required=True)
    sub = p.add_subparsers(dest='mode', required=True)
    sub.add_parser('check')
    sub.add_parser('pins')
    sub.add_parser('env')
    pin = sub.add_parser('pin')
    pin.add_argument('--manifest', type=Path, required=True)
    pin.add_argument('--receipt', type=Path, required=True)
    pin.add_argument('--archive', type=Path, required=True)
    generate = sub.add_parser('generate')
    generate.add_argument('--installed', type=Path)
    generate.add_argument('--install-root', default=r'C:\Program Files\amdgpu-wddm')
    generate.add_argument('--apply', action='store_true')
    args = p.parse_args()
    try:
        args.root = args.root.resolve()
        args.kit = local(args.root, args.kit)
        if args.mode == 'pin':
            pin_publication(args.kit, args.root, args.manifest, args.receipt, args.archive)
        elif args.mode == 'generate':
            pub, manifest, path = load_published(args.kit, args.root)
            admission = read(local(args.root, args.installed)) if args.installed else None
            b = derive(manifest, pub['manifest_sha256'], path, args.install_root, admission)
            if args.apply:
                write_new(args.kit / 'lab-baseline.json', b)
            print(json.dumps(b, indent=2))
        else:
            baseline_path = os.environ.get('BC250_FINAL_BASELINE') or None
            b = load_baseline(args.kit, args.root, baseline_path, require_admission=args.mode != 'pins')
            if args.mode == 'env':
                adapter, paths, _, accepted = triplet(b, args.root, os.environ)
                require(accepted, 'Final DriverStore trial requires the published triplet; candidate selection differs')
                require(os.environ.get('SESSION_LAUNCH', 'steam') in ('', 'steam'),
                        'Final DriverStore acceptance requires Steam launch')
                require(os.environ.get('SESSION_EXPERIMENT', '') in ('', 'none')
                        and not os.environ.get('PAIR_EXPERIMENT') and not os.environ.get('BC250_TRIAL_EXPERIMENT')
                        and os.environ.get('BC250_TRIAL_PROMOTE') != '1',
                        'Final DriverStore acceptance refuses diagnostic experiments and promotion')
                values = dict(SESSION_ADAPTER=adapter, SESSION_ENGINE=str(paths[1]), SESSION_ICD=str(paths[2]),
                              SESSION_SHELL=str(paths[0]), BC250_FINAL_SHELL=str(paths[0]),
                              BC250_FINAL_BASELINE=str(baseline_path or args.kit / 'lab-baseline.json'),
                              SESSION_ACCEPTED='1', BC250_TRIAL_ACCEPTED='1', SESSION_LAUNCH='steam',
                              BC250_TRIAL_LAUNCH='steam', PAIR_EXPERIMENT='')
                for key, value in values.items():
                    print('export ' + key + '=' + shlex.quote(value))
            else:
                print('PASS published baseline ' + b['release']['release'] +
                      ('; installed admission pending' if b['admission_required'] else '; admission present'))
    except (RuntimeError, OSError, ValueError, KeyError, TypeError) as error:
        print('REFUSED: ' + str(error), file=sys.stderr)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
