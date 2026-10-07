"""Host test of the package identity rules (no lab, nothing written outside a temporary directory).

    python tools/test_versions.py

The identity of a package is (R, B) from DriverVer 0.7.R.B: R must be the low 16 bits of BC250_KMD_VERSION, B is the
build counter (B >= 1), and a release package carries its own B under the checked rule of release_version. Synthetic
cases cover every refusal. When the tester.20 release and the 0.7.216.x builds are present in the workspace, they are
read as well (live cases; each prints SKIP when its directory is absent). Prints PASS and exits 0.
"""
import json
import os
import sys
import tempfile
from pathlib import Path

BASE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(BASE))
import kmdcommon as k  # noqa: E402
import stage  # noqa: E402

checks, skips = [], []


def check(name, ok):
    checks.append(name)
    if not ok:
        raise SystemExit('FAIL: ' + name)


def refused(fn, *args, contains=''):
    try:
        fn(*args)
    except SystemExit as e:
        return str(e).startswith('REFUSED') and contains in str(e)
    return False


SECTIONS = ['Bc250_Install.NTamd64', 'Bc250_Install.NTamd64.Services']


def build_inf(version):
    lines = ['; bc250kmd', '[Version]', 'Signature = "$Windows NT$"', f'DriverVer   = 10/07/2026,{version}', '',
             '[Manufacturer]', '%P% = Models,NTamd64']
    for s in SECTIONS:
        lines += [f'[{s}]', 'CopyFiles = F', '']
    lines += ['[Parameters]', 'HKR, Parameters, EnableHangBugcheck, 0x00010001, 0', '']
    return '\r\n'.join(lines)


def release_inf(build, release_version):
    """What build-release.ps1 does to the build INF: DriverVer gets the release field, Reboot after each section."""
    text = build.replace('10/07/2026,' + build.split('10/07/2026,')[1].split('\r\n')[0], '10/07/2026,' + release_version)
    for s in SECTIONS:
        text = text.replace(f'[{s}]\r\n', f'[{s}]\r\n{k.RELEASE_REBOOT_LINE}\r\n')
    return text


def pe(image_byte, cert, trailing=b''):
    """A minimal PE32+ file: header, an optional header with its security directory entry, an image, a cert table."""
    data = bytearray(0x200)
    data[:2] = b'MZ'
    data[0x3C:0x40] = (0x80).to_bytes(4, 'little')
    data[0x80:0x84] = b'PE\0\0'
    optional = 0x80 + 24
    data[optional:optional + 2] = (0x20B).to_bytes(2, 'little')
    data[optional + 64:optional + 68] = os.urandom(4)  # the checksum changes with every signature
    data[0x1F0] = image_byte
    security = optional + 112 + 32
    data[security:security + 8] = len(data).to_bytes(4, 'little') + len(cert).to_bytes(4, 'little')
    return bytes(data) + cert + trailing


def synthetic():
    # parse_version: 0.7.R.B, canonical decimal fields, B >= 1.
    check('0.7.175.1 is (175, 1)', k.parse_version('0.7.175.1', 't') == (175, 1))
    check('0.7.216.16 is (216, 16)', k.parse_version('0.7.216.16', 't') == (216, 16))
    check('0.7.216.100 is (216, 100)', k.parse_version('0.7.216.100', 't') == (216, 100))
    for bad in ('0.7.216.0', '0.7.216', '0.8.216.1', '0.7.216.014', '0.7.0216.1', '0.7.216.x', '0.7.216.1.1'):
        check(f'refused: DriverVer {bad}', refused(k.parse_version, bad, 't'))
    # check_revision: R is the ABI's low 16 bits.
    check('0.7.216.16 over ABI 0x000700D8', k.check_revision('0.7.216.16', '0x000700D8', 't') == (216, 16))
    check('refused: version/ABI mismatch 0.7.216.16 over 0x000700D7',
          refused(k.check_revision, '0.7.216.16', '0x000700D7', 't', contains='do not encode one revision'))
    check('refused: ABI outside 0x0007RRRR', refused(k.check_revision, '0.7.216.16', '0x000800D8', 't'))
    # Labels and attempt names: unique per (R, B); build 1 keeps the old names.
    check('labels', [k.label_suffix(v) for v in ('0.7.175.1', '0.7.216.16', '0.7.216.100')] == ['175', '216-16', '216-100'])
    for name in ('kmd175-deploy001', 'kmd216-16-deploy001', 'kmd216-100-deploy002'):
        check('attempt name ' + name, bool(k.ATTEMPT_RE.match(name)))
    for name in ('kmd216-1-deploy001', 'kmd216-016-deploy001', 'kmd216-0-deploy001', 'kmd216--deploy001', 'kmd216-16'):
        check('refused attempt name ' + name, not k.ATTEMPT_RE.match(name))
    # build_version: package INF and source INF must agree, unless the release rule says otherwise.
    b14 = build_inf('0.7.216.14')
    check('build 0.7.216.14', k.build_version(b14, b14, '0x000700D8', 't') == '0.7.216.14')
    check('refused: unchecked package/source DriverVer difference',
          refused(k.build_version, build_inf('0.7.216.100'), b14, '0x000700D8', 't', contains='disagree on DriverVer'))
    check('refused: build version/ABI mismatch', refused(k.build_version, b14, b14, '0x000700D7', 't'))
    # release_version: the checked rule for a release package's own build field.
    manifest = {'kmd_version': '0.7.216.100', 'kmd_build': '0.7.216.14'}
    r100 = release_inf(b14, '0.7.216.100')
    check('release 0.7.216.100 over build 0.7.216.14', k.release_version(r100, b14, '0.7.216.14', manifest, 't') == '0.7.216.100')
    check('refused: release INF is not manifest kmd_version',
          refused(k.release_version, release_inf(b14, '0.7.216.101'), b14, '0.7.216.14', manifest, 't', contains='kmd_version'))
    check('refused: build is not manifest kmd_build',
          refused(k.release_version, r100, b14, '0.7.216.14', dict(manifest, kmd_build='0.7.216.13'), 't', contains='kmd_build'))
    check('refused: release of another revision',
          refused(k.release_version, release_inf(b14, '0.7.217.100'), b14, '0.7.216.14',
                  dict(manifest, kmd_version='0.7.217.100'), 't', contains='different revisions'))
    check('refused: release INF with one more change',
          refused(k.release_version, r100.replace('CopyFiles = F', 'CopyFiles = G', 1), b14, '0.7.216.14', manifest, 't',
                  contains='more than DriverVer'))
    check('refused: release INF that keeps the build DriverVer',
          refused(k.release_version, release_inf(b14, '0.7.216.14'), b14, '0.7.216.14', manifest, 't', contains='kmd_version'))
    # image_digest: a re-signed file keeps it, a changed image does not, data after the certificate table is refused.
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        files = {'a': pe(1, b'CERT-A' * 10), 'b': pe(1, b'CERT-BB' * 13), 'c': pe(2, b'CERT-A' * 10),
                 'd': pe(1, b'CERT-A' * 10, trailing=b'x')}
        for n, data in files.items():
            (tmp / n).write_bytes(data)
        check('image digest: a re-signed image keeps it', k.image_digest(tmp / 'a') == k.image_digest(tmp / 'b'))
        check('image digest: a changed image changes it', k.image_digest(tmp / 'a') != k.image_digest(tmp / 'c'))
        check('refused: data after the certificate table', refused(k.image_digest, tmp / 'd'))
    # compare_packages: full version and SYS; the same package is never frozen.
    cand = {'kind': 'build', 'version': '0.7.216.16', 'revision': 216, 'build': 16, 'files': {'bc250kmd.sys': 'A' * 64}}
    roll = {'kind': 'release', 'version': '0.7.216.100', 'revision': 216, 'build': 100, 'files': {'bc250kmd.sys': 'B' * 64}}
    k.compare_packages(cand, roll)
    check('216.16 over 216.100 (one ABI) is admitted', True)
    check('refused: same package as rollback', refused(k.compare_packages, cand, dict(cand)))
    check('refused: same version, other SYS', refused(k.compare_packages, cand, dict(roll, version='0.7.216.16', build=16),
                                                       contains='same version'))
    check('refused: same SYS, other version', refused(k.compare_packages, cand, dict(roll, files=cand['files']), contains='same SYS'))
    check('refused: a release package as candidate', refused(k.compare_packages, dict(roll, kind='release'), cand))
    check('216.16 over 215.1 is admitted', k.compare_packages(cand, dict(roll, kind='build', version='0.7.215.1', revision=215, build=1)) is None)
    # Certificates and baseline: a release rollback answers to its own manifest and to lab-baseline.json.
    sig = {'candidate': {'certificate': 'C' * 40}, 'rollback': {'certificate': 'R' * 40}}
    stage.check_certificates(cand, dict(roll, release_certificate='R' * 40), sig)
    check('release rollback signed by its manifest certificate is admitted', True)
    check('refused: release rollback signed by another certificate',
          refused(stage.check_certificates, cand, dict(roll, release_certificate='X' * 40), sig))
    check('refused: two builds signed by different certificates',
          refused(stage.check_certificates, cand, dict(roll, kind='build'), sig))
    roll_full = dict(roll, abi='0x000700D8', release_manifest_sha256='M' * 64, kmd_build='0.7.216.14')
    baseline = {'kmd_version': '0.7.216.100', 'kmd_sys_sha256': 'b' * 64, 'kmd_abi': '0x000700D8',
                'release': {'manifest_sha256': 'M' * 64, 'kmd_build': '0.7.216.14'}}
    stage.check_baseline(roll_full, baseline)
    check('release rollback named by lab-baseline.json is admitted', True)
    check('refused: lab-baseline.json names another release manifest',
          refused(stage.check_baseline, roll_full, dict(baseline, release={'manifest_sha256': 'N' * 64, 'kmd_build': '0.7.216.14'})))
    check('refused: lab-baseline.json names another KMD', refused(stage.check_baseline, roll_full, dict(baseline, kmd_version='0.7.216.14')))
    # The identity lines freeze writes.
    ident = stage.generated_identity(dict(cand, commit='1' * 40, abi='0x000700D8'),
                                     dict(roll, commit='2' * 40, abi='0x000700D8',
                                          files={'bc250kmd.sys': 'B' * 64, 'bc250kmd.inf': 'D' * 64, 'bc250kmd.cat': 'E' * 64}),
                                     {'umd_path': 'u', 'umd_sha256': 'F' * 64, 'icd_path': 'i', 'icd_sha256': '0' * 64},
                                     'kmd216-16-deploy')
    check('generated labels candidate216-16 / rollback216-100 and the kmd216-16 pattern',
          "$KmdCandidateLabel='candidate216-16'" in ident and "$KmdRollbackLabel='rollback216-100'" in ident
          and 'kmd216-16-deploy[0-9]{3}$' in ident and '0.7.216.16 (commit 11111111)' in ident)


def live():
    root = k.ROOT / 'scratch'
    release = root / 'train/b22-build/release/amdgpu-wddm-tester-0.7.216.100-tester.20'
    build = root / 'train/b22-build/kmd-r20/a/package'
    candidate = root / 'hang-recovery/build-b23/kmd/package'
    old = k.ROOT / 'scratch/kmd-deploy/attempts/kmd197-deploy001/candidate197'
    if old.is_dir():
        p = k.read_package(old, k.REPO, 'old')
        check('live: an accepted 0.7.R.1 candidate still reads (kmd197-deploy001)',
              (p['version'], p['revision'], p['build'], k.label_suffix(p['version'])) == ('0.7.197.1', 197, 1, '197'))
    else:
        skips.append('live old candidate')
    if not (release.is_dir() and build.is_dir()):
        skips.append('live tester.20 release')
        return
    b = k.read_package(build, k.REPO, 'build')
    check('live: build 0.7.216.14 ABI 0x000700D8', (b['version'], b['abi']) == ('0.7.216.14', '0x000700D8'))
    r = k.read_package(release, k.REPO, 'rollback', build)
    manifest = json.loads((release / 'manifest.json').read_text(encoding='utf-8-sig'))
    check('live: tester.20 release reads as 0.7.216.100 over build 0.7.216.14',
          (r['kind'], r['version'], r['build'], r['kmd_build'], r['abi'], r['commit']) ==
          ('release', '0.7.216.100', 100, '0.7.216.14', '0x000700D8', b['commit'])
          and r['release_certificate'] == manifest['release_certificate'].upper())
    check('refused: a release package without its build', refused(k.read_package, release, k.REPO, 'rollback'))
    check('refused: --rollback-build with a build package', refused(k.read_package, build, k.REPO, 'rollback', build))
    other = root / 'k137/build/package'
    if other.is_dir():
        check('refused: a release over a build it was not made from',
              refused(k.read_package, release, k.REPO, 'rollback', other))
    else:
        skips.append('live foreign build')
    if candidate.is_dir():
        c = k.read_package(candidate, k.REPO, 'candidate')
        k.compare_packages(c, r)
        check('live: candidate 0.7.216.16 over the tester.20 release', c['version'] == '0.7.216.16')
    else:
        skips.append('live 0.7.216.16 candidate')


def main():
    synthetic()
    live()
    print(f'PASS: versions, {len(checks)} checks' + (f'; SKIP {", ".join(skips)}' if skips else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
