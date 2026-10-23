"""Build twice in different directories and verify source/output refusal controls."""
import argparse
import copy
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import build_helpers
import stage
from kmdcommon import digest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    first, second = args.out / 'first', args.out / 'second'
    a, b = build_helpers.build(first), build_helpers.build(second)
    assert a['files'] == b['files'], 'Independent builds differ'
    build_helpers.verify(first, a)
    checks = ['two fresh builds produce identical executables', 'fresh source build verifies']

    def refused(name, manifest, write_manifest=True):
        manifest_path = first / 'build-manifest.json'
        original_manifest = manifest_path.read_bytes()
        if write_manifest:
            manifest_path.write_text(json.dumps(manifest), encoding='utf-8')
        try:
            build_helpers.verify(first, manifest)
        except (ValueError, FileNotFoundError):
            checks.append(name)
        else:
            raise AssertionError('Accepted: ' + name)
        finally:
            manifest_path.write_bytes(original_manifest)

    exe = first / 'bounded-child.exe'
    original = exe.read_bytes()
    exe.write_bytes(original + b'changed')
    refused('modified executable refused', a)
    exe.unlink()
    refused('missing executable refused', a)
    exe.write_bytes(original)
    extra = first / 'extra.exe'
    extra.write_bytes(original)
    refused('extra executable refused', a)
    extra.unlink()
    changed = copy.deepcopy(a)
    changed['source_files']['template/kmd168-transition/bounded-child.cpp'] = '0' * 64
    refused('stale source hash refused', changed)
    changed = copy.deepcopy(a)
    changed['files']['../foreign.exe'] = changed['files'].pop('bounded-child.exe')
    refused('foreign manifest filename refused', changed)
    for tool in ('compiler', 'linker'):
        changed = copy.deepcopy(a)
        changed[tool]['sha256'] = '0' * 64
        refused('changed ' + tool + ' refused', changed)
    build_helpers.verify(first, a)
    staged = args.out / 'staged'
    binding = stage.stage_helpers(staged, first, a)
    for name in ('bounded-child.exe', 'select-driver.exe'):
        assert digest(staged / name) == a['files'][name]
    assert not (staged / 'receipt-pipe-fixture.exe').exists()
    assert binding == {'helper_build_manifest_sha256': digest(staged / 'helper-build-manifest.json'),
                       'helper_source_commit': a['source_commit']}
    assert json.loads((staged / 'helper-build-manifest.json').read_text()) == a
    checks.append('freeze helper stage binds receipt and both new executables, excludes fixture binary')
    manifest_path = first / 'build-manifest.json'
    manifest_bytes = manifest_path.read_bytes()
    manifest_path.write_text('{}', encoding='utf-8')
    refused('changed manifest refused', a, write_manifest=False)
    manifest_path.write_bytes(manifest_bytes)
    receipt = {'schema': 1, 'passed': checks, 'source_commit': a['source_commit'], 'files': a['files'],
               'build_manifest_sha256': digest(first / 'build-manifest.json'),
               'second_build_manifest_sha256': digest(second / 'build-manifest.json')}
    (args.out / 'test-manifest.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    print('PASS: source helpers, ' + str(len(checks)) + ' checks')


if __name__ == '__main__':
    main()
