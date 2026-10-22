"""BD-167 byte check of the Mesa DLLs of a release and a lab package: a string that only the lab flavour may carry is
absent from every release DLL, and every such string that the DLL's own Mesa tree holds is present in the lab DLL of
the same slot. The tree of each slot is asked with git grep at the commit its release-sources.json row names, so
nothing is inferred from which line is which. build-release-set.ps1 -Package runs it on both packages.

  python flavour_bytes.py --release <package files root> --lab <package files root> --sources <release-sources.json>
      --mesa <mesa checkout>
"""
import argparse
import json
import pathlib
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from driver_package import members  # noqa: E402

STRINGS = ['BC250_PREAMBLE_NO_IDLE_WAIT', 'BC250_DEFERRED_LOG', 'queue wait skipped']


def in_tree(mesa, commit, s):
    # git grep exits 1 when the string is absent. Any other exit (no such commit, no such repository) is an error:
    # read as absence, it let a lab DLL without its lab strings pass.
    r = subprocess.run(['git', '-C', str(mesa), 'grep', '-q', '-F', s, commit, '--', 'src'], capture_output=True)
    if r.returncode not in (0, 1):
        raise RuntimeError('Git source check failed (exit %d, commit %s): %s' %
                           (r.returncode, commit, r.stderr.decode(errors='replace').strip()))
    return r.returncode == 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--release', required=True)
    ap.add_argument('--lab', required=True)
    ap.add_argument('--sources', required=True, help='release-sources.json of the packages')
    ap.add_argument('--mesa', required=True)
    a = ap.parse_args()
    src = json.loads(pathlib.Path(a.sources).read_text(encoding='utf-8-sig'))
    mesa_rows = [r for r in src['files'] if (r.get('built_from') or {}).get('repo') == 'mesa-amdgpu-wddm'
                 and r['path'].lower().endswith('.dll')]
    # A user-mode driver goes into the Driver Store package under its member name (payload/kmd/..., an x86 file with
    # a "32" suffix), so a row's path is looked up there first; any other file keeps its row path.
    packaged = {m['source']: m['path'] for m in members(src)}
    failures = 0
    for flavour, root in (('release', pathlib.Path(a.release)), ('lab', pathlib.Path(a.lab))):
        for r in [r for r in mesa_rows if r['flavour'] == flavour]:
            f = root / packaged.get(r['path'], r['path']).replace('/', '\\')
            if not f.is_file():
                print('FAIL %s %s: not in the package (%s)' % (flavour, r['path'], f))
                failures += 1
                continue
            data = f.read_bytes()
            commit = r['built_from']['commit']
            for s in STRINGS:
                has = s.encode() in data
                tree = in_tree(a.mesa, commit, s)
                if flavour == 'release' and has:
                    print('FAIL release %s carries "%s" (%s)' % (r['path'], s, commit[:8]))
                    failures += 1
                elif flavour == 'lab' and tree and not has:
                    print('FAIL lab %s lacks "%s", which its tree %s holds' % (r['path'], s, commit[:8]))
                    failures += 1
                else:
                    print('ok   %-7s %-44s %-28s tree=%d dll=%d' % (flavour, r['path'], s, tree, has))
    print('flavour bytes: %s, %d failure(s)' % ('PASS' if not failures else 'FAIL', failures))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
