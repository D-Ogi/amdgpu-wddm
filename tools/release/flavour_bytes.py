"""BD-167 byte check of the Mesa DLLs and the D3D11/D3D12 shells of a release and a lab package: a string that only
the lab flavour may carry is absent from every release DLL, and every such string that the DLL's own tree holds is
present in the lab DLL of the same slot. The tree of each slot is asked with git grep at the commit its
release-sources.json row names, so nothing is inferred from which line is which. build-release-set.ps1 -Package runs
it on both packages.

  python flavour_bytes.py --release <package files root> --lab <package files root> --sources <release-sources.json>
      --mesa <mesa checkout> [--shells <amdgpu-wddm checkout, default: this repository>]
"""
import argparse
import json
import pathlib
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from driver_package import members  # noqa: E402

STRINGS = ['BC250_PREAMBLE_NO_IDLE_WAIT', 'BC250_DEFERRED_LOG', 'queue wait skipped']
# The lab log lines of driver/umd/app-settings/dxvk-profile.h: the DXVK profile options that do not apply on this
# route. Only the lab flavour of log_once (app-settings.h) instantiates them.
SHELL_STRINGS = ['DXVK profile option %s=%s not applied: %s', 'no DXVK profile options (engine result %08lx)']
SHELL_FILES = ('amdgpu_wddm_d3d11.dll', 'amdgpu_wddm_d3d12.dll')
REPO = pathlib.Path(__file__).resolve().parents[2]


def in_tree(repo, commit, s, pathspec='src'):
    # git grep exits 1 when the string is absent. Any other exit (no such commit, no such repository) is an error:
    # read as absence, it let a lab DLL without its lab strings pass.
    r = subprocess.run(['git', '-C', str(repo), 'grep', '-q', '-F', s, commit, '--', pathspec], capture_output=True)
    if r.returncode not in (0, 1):
        raise RuntimeError('Git source check failed (exit %d, commit %s): %s' %
                           (r.returncode, commit, r.stderr.decode(errors='replace').strip()))
    return r.returncode == 0


def check(flavour, path, data, repo, commit, strings, pathspec):
    """The failure lines of one DLL, and prints a line for each string."""
    failures = 0
    for s in strings:
        has = s.encode() in data
        tree = in_tree(repo, commit, s, pathspec)
        if flavour == 'release' and has:
            print('FAIL release %s carries "%s" (%s)' % (path, s, commit[:8]))
            failures += 1
        elif flavour == 'lab' and tree and not has:
            print('FAIL lab %s lacks "%s", which its tree %s holds' % (path, s, commit[:8]))
            failures += 1
        else:
            print('ok   %-7s %-44s %-28s tree=%d dll=%d' % (flavour, path, s, tree, has))
    return failures


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--release', required=True)
    ap.add_argument('--lab', required=True)
    ap.add_argument('--sources', required=True, help='release-sources.json of the packages')
    ap.add_argument('--mesa', required=True)
    ap.add_argument('--shells', default=str(REPO), help='a checkout of amdgpu-wddm (default: this repository)')
    a = ap.parse_args()
    src = json.loads(pathlib.Path(a.sources).read_text(encoding='utf-8-sig'))
    # (row, checkout, strings, pathspec of the tree lookup)
    lines = []
    for r in src['files']:
        repo = (r.get('built_from') or {}).get('repo')
        p = r['path'].lower()
        if repo == 'mesa-amdgpu-wddm' and p.endswith('.dll'):
            lines.append((r, a.mesa, STRINGS, 'src'))
        elif repo == 'amdgpu-wddm' and p.rsplit('/', 1)[-1] in SHELL_FILES:
            lines.append((r, a.shells, SHELL_STRINGS, 'driver/umd'))
    # A user-mode driver goes into the Driver Store package under its member name (payload/kmd/..., an x86 file with
    # a "32" suffix), so a row's path is looked up there first; any other file keeps its row path.
    packaged = {m['source']: m['path'] for m in members(src)}
    failures = 0
    for flavour, root in (('release', pathlib.Path(a.release)), ('lab', pathlib.Path(a.lab))):
        for r, repo, strings, pathspec in [x for x in lines if x[0]['flavour'] == flavour]:
            f = root / packaged.get(r['path'], r['path']).replace('/', '\\')
            if not f.is_file():
                print('FAIL %s %s: not in the package (%s)' % (flavour, r['path'], f))
                failures += 1
                continue
            failures += check(flavour, r['path'], f.read_bytes(), repo, r['built_from']['commit'], strings, pathspec)
    print('flavour bytes: %s, %d failure(s)' % ('PASS' if not failures else 'FAIL', failures))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
