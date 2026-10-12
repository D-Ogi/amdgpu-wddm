"""Host test of kmdcommon.historical_manifest: the nine historical helpers are verified, not merely found.

    python tools/test_hist171.py

The live set (the tracked hist171-manifest.json and the files under kmdcommon.HIST) must verify. Every way the
set can go wrong must be refused: a changed helper byte, a missing helper, an extra file beside them, a changed
manifest, and a manifest that no longer names the 171 stage manifest the frozen attempts record. The refusal
cases run against copies under host-tests/, which this test removes again; the live set is never written to.
Prints PASS and exits 0.
"""
import json
import shutil
import sys
import tempfile
from pathlib import Path

BASE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(BASE))
import kmdcommon  # noqa: E402
import stage  # noqa: E402
from kmdcommon import HIST, HIST171_STAGE_MANIFEST_SHA256, HIST_MANIFEST, HIST_MANIFEST_SHA256, HOST_TESTS, digest  # noqa: E402

NINE = 9


def refused(hist, manifest):
    """historical_manifest() with HIST and the manifest moved to copies: the refusal text, or None if it passed."""
    saved = (kmdcommon.HIST, kmdcommon.HIST_MANIFEST, kmdcommon.HIST_MANIFEST_SHA256)
    kmdcommon.HIST, kmdcommon.HIST_MANIFEST = hist, manifest
    kmdcommon.HIST_MANIFEST_SHA256 = digest(manifest)
    try:
        kmdcommon.historical_manifest()
        return None
    except SystemExit as error:
        return str(error)
    finally:
        kmdcommon.HIST, kmdcommon.HIST_MANIFEST, kmdcommon.HIST_MANIFEST_SHA256 = saved


def main():
    checks = []

    def check(name, ok):
        checks.append(name)
        if not ok:
            raise SystemExit('FAIL: ' + name)

    # The live set, through the pinned manifest and the pinned digests.
    files = kmdcommon.historical_manifest()
    check(f'live set: {NINE} files', len(files) == NINE)
    check('live set: the kmd168 helpers, bounded-child.exe and select-driver.exe',
          sorted(files) == sorted([rel for rel in files if rel.startswith('kmd168-transition/')] +
                                  ['bounded-child.exe', 'select-driver.exe']))
    check('live set: the manifest is the pinned one', digest(HIST_MANIFEST) == HIST_MANIFEST_SHA256)
    check('live set: no path of the closed M14 milestone', 'm14' not in HIST.as_posix().lower().split('/'))

    HOST_TESTS.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='hist171-', dir=HOST_TESTS))
    try:
        hist = work / 'hist171'
        shutil.copytree(HIST, hist)
        manifest_path = work / 'hist171-manifest.json'
        shutil.copy2(HIST_MANIFEST, manifest_path)
        manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
        # The copies themselves verify, so every refusal below is caused by the one change it makes.
        check('copied set verifies', refused(hist, manifest_path) is None)

        helper = hist / 'kmd168-transition/deadline.ps1'
        original = helper.read_bytes()
        helper.write_bytes(original + b'# one more line\r\n')
        check('refused: a changed helper', (refused(hist, manifest_path) or '').startswith('Historical helper file changed'))
        helper.write_bytes(original)

        helper.unlink()
        check('refused: a missing helper', 'differs from its manifest' in (refused(hist, manifest_path) or ''))
        helper.write_bytes(original)

        extra = hist / 'kmd168-transition/extra.ps1'
        extra.write_bytes(b'# not in the 171 set\r\n')
        check('refused: a file beside the set', 'differs from its manifest' in (refused(hist, manifest_path) or ''))
        extra.unlink()

        # A changed manifest, against the pin the kit ships: the digest is not re-read here.
        saved = kmdcommon.HIST, kmdcommon.HIST_MANIFEST
        kmdcommon.HIST, kmdcommon.HIST_MANIFEST = hist, manifest_path
        try:
            manifest_path.write_text(json.dumps(manifest | {'schema': 2}, indent=2) + '\n', encoding='utf-8')
            try:
                kmdcommon.historical_manifest()
                refusal = None
            except SystemExit as error:
                refusal = str(error)
            check('refused: a changed manifest', (refusal or '').startswith('Historical helper manifest changed'))
        finally:
            kmdcommon.HIST, kmdcommon.HIST_MANIFEST = saved

        # A manifest re-pinned after an edit still has to name the 171 stage manifest the attempts record.
        wrong = dict(manifest, historical_stage_manifest_sha256='0' * 64)
        manifest_path.write_text(json.dumps(wrong, indent=2) + '\n', encoding='utf-8')
        check('refused: a manifest that does not name the 171 stage manifest',
              'does not name the 171 stage manifest' in (refused(hist, manifest_path) or ''))
        check('the 171 stage manifest SHA256 is still the one the attempts record',
              HIST171_STAGE_MANIFEST_SHA256 == 'CF57DB7375B4FE787CC98EF83D6D06F078185A34D041045BA3782A914B8C0E91')

        # A set that is not there at all names its provenance file, so the operator can recover it again.
        shutil.copy2(HIST_MANIFEST, manifest_path)
        check('refused: no set at all, and the refusal names the manifest',
              str(HIST_MANIFEST.name) in (refused(work / 'absent', manifest_path) or ''))

        # What a freeze does with the set: the same selection and the same digest-verified copy as stage.freeze,
        # against the live HIST. This is the step that breaks when the historical input is not where HIST points.
        staged = work / 'staged'
        for rel, value in sorted(files.items()):
            if not (rel.startswith('kmd168-transition/') or rel in ('bounded-child.exe', 'select-driver.exe')):
                continue
            template_copy = BASE / 'template' / rel
            check('freeze: template copy of ' + rel + ' agrees',
                  not template_copy.exists() or digest(template_copy) == value)
            stage.copy_verified(HIST / rel, staged / rel, value)
        check(f'freeze: all {NINE} files staged from the live set',
              sorted(p.relative_to(staged).as_posix() for p in staged.rglob('*') if p.is_file()) == sorted(files))
    finally:
        shutil.rmtree(work, ignore_errors=True)

    for name in checks:
        print('  ' + name)
    print(f'PASS: historical helper set, {len(checks)} checks ({len(files)} files under {HIST}, '
          f'manifest {HIST_MANIFEST.name} {HIST_MANIFEST_SHA256[:8]})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
