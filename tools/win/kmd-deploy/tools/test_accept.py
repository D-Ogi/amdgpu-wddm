"""Host test of accept.py on a dry-run attempt with synthetic receipts, in a scratch copy (never the real files).

    python tools/test_accept.py <dry-run attempt dir>     (a `stage.py freeze --dry-run --out` output)
"""
import contextlib
import io
import json
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(HERE))
import accept  # noqa: E402
import kmdcommon  # noqa: E402


def run(argv):
    out = io.StringIO()
    sys.argv = ['accept.py', *argv]
    try:
        with contextlib.redirect_stdout(out):
            accept.main()
        return 0, out.getvalue()
    except SystemExit as e:
        return 1, str(e) + out.getvalue()


def main():
    source = Path(sys.argv[1]).resolve()
    work = kmdcommon.HOST_TESTS / ('accept-' + source.name)
    shutil.rmtree(work, ignore_errors=True)
    manifest = json.loads((source / 'stage-manifest.json').read_text(encoding='utf-8'))
    name = kmdcommon.ATTEMPT_RE.sub(lambda m: m.group(0), manifest['candidate']['label'].replace('candidate', 'kmd') + '-deploy001')
    attempt, ops = work / 'attempts' / name, work / 'attempts' / (name + '-ops')
    shutil.copytree(source, attempt)
    shutil.copytree(source.with_name(source.name + '-ops'), ops)
    baseline = work / 'lab-baseline.json'
    shutil.copyfile(kmdcommon.LAB_BASELINE, baseline)
    kmdcommon.ATTEMPTS, accept.LAB_BASELINE = work / 'attempts', baseline
    sha = (ops / 'stage-manifest.sha256').read_text().strip()
    checks = []

    def expect(ok, label, argv=(name,)):
        code, text = run(list(argv))
        checks.append((label, (code == 0) == ok, text.strip().splitlines()[-1] if text.strip() else ''))

    expect(False, 'no receipts')
    (ops / 'collected-0' / name).mkdir(parents=True)
    (ops / 'collected-0' / name / 'watch-result.json').write_text(json.dumps({'status': 'closed', 'restored': True}))
    expect(False, 'restored is not accepted')
    (ops / 'collected-1' / name).mkdir(parents=True)
    (ops / 'collected-1' / name / 'watch-result.json').write_text(json.dumps({'status': 'closed', 'candidate_retained': True, 'elapsed': 91.2}))
    expect(False, 'no cleanup')
    (ops / 'Cleanup-0.txt').write_text('{"state":  "Missing"}')
    expect(False, 'no postflight')
    (ops / 'postflight-0.json').write_text(json.dumps({'returncode': 1, 'manifest_sha256': sha}))
    expect(False, 'failed postflight')
    (ops / 'postflight-1.json').write_text(json.dumps({'returncode': 0, 'manifest_sha256': 'X' * 64}))
    expect(False, 'postflight of another manifest')
    (ops / 'postflight-2.json').write_text(json.dumps({'returncode': 0, 'manifest_sha256': sha}))
    before = baseline.read_bytes()
    expect(True, 'print only')
    checks.append(('print only leaves the file', baseline.read_bytes() == before, ''))
    expect(True, 'apply', (name, '--apply'))
    after = json.loads(baseline.read_text())
    ident = kmdcommon.identity(attempt / 'kmd-transition' / 'identity.ps1')
    hashes = json.loads((attempt / 'package-hashes.json').read_text())
    ok = (after['kmd_version'], after['kmd_abi'], after['kmd_sys_sha256']) == (
        ident['KmdCandidateVersion'], ident['KmdCandidateAbi'], hashes[ident['KmdCandidateLabel']]['bc250kmd.sys'])
    ok = ok and after['umd_sha256'] == json.loads(before)['umd_sha256'] and (ops / 'lab-baseline-before.json').exists()
    checks.append(('apply wrote the candidate pins only', ok, ''))
    expect(False, 'second apply refuses (baseline moved)', (name, '--apply'))
    for label, passed, detail in checks:
        print(('PASS ' if passed else 'FAIL ') + label + (': ' + detail if detail else ''))
    failed = [c for c in checks if not c[1]]
    print(f'{len(checks) - len(failed)}/{len(checks)} passed')
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
