"""Compare complete or interrupted upstream piglit runs against an exact case list.

Reads JSON, JSON.bz2 or a tar of an interrupted JSON backend without extraction.
This checks test results only; matching source/device identities and explanations
for platform differences remain separate acceptance evidence.
"""
import argparse
import bz2
from collections import Counter
import hashlib
import json
from pathlib import Path
import tarfile

STATUSES = {'pass', 'fail', 'skip', 'warn', 'crash', 'timeout', 'notrun',
            'incomplete', 'dmesg-warn', 'dmesg-fail'}


def unique_object(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise ValueError('Duplicate JSON key: ' + key)
        value[key] = item
    return value


def decode(data):
    return json.loads(data.decode('utf-8-sig'), object_pairs_hook=unique_object)


def read_results(path):
    results = {}

    def merge(tests):
        for name, result in tests.items():
            if name in results:
                raise ValueError('Duplicate case: ' + name)
            if not isinstance(result, dict) or result.get('result') not in STATUSES:
                raise ValueError('Invalid result: ' + name)
            subtests = result.get('subtests', {})
            if not isinstance(subtests, dict):
                raise ValueError('Invalid subtests: ' + name)
            subtests = {k: v for k, v in subtests.items() if k != '__type__'}
            if any(v not in STATUSES for v in subtests.values()):
                raise ValueError('Invalid subtest status: ' + name)
            results[name] = {'result': result['result'], 'subtests': subtests}

    if path.suffix == '.tar':
        with tarfile.open(path, 'r') as archive:
            for member in archive:
                name = member.name.removeprefix('./').removeprefix('results/')
                if name.startswith('tests/') and name.endswith('.json'):
                    if not member.isfile():
                        raise ValueError('Non-file test archive entry')
                    merge(decode(archive.extractfile(member).read()))
    else:
        data = path.read_bytes()
        if path.suffix == '.bz2':
            data = bz2.decompress(data)
        merge(decode(data)['tests'])
    if not results:
        raise ValueError('No test results')
    return results


def inventory(path):
    names = [line.strip() for line in path.read_text(encoding='utf-8-sig').splitlines()
             if line.strip() and not line.lstrip().startswith('#')]
    if not names or len(names) != len(set(names)):
        raise ValueError('Empty or duplicate inventory')
    return set(names)


def compare(expected, windows, linux):
    differences = []
    sides = {'windows': windows, 'linux': linux}
    complete = {}
    counts = {}
    for side, results in sides.items():
        counts[side] = dict(Counter(row['result'] for row in results.values()))
        complete[side] = (set(results) == expected and
                          all(row['result'] not in {'notrun', 'incomplete'}
                              for row in results.values()))
    for name in sorted(expected | set(windows) | set(linux)):
        w, l = windows.get(name), linux.get(name)
        reasons = []
        if name not in expected:
            reasons.append('unexpected')
        for side, row in [('windows', w), ('linux', l)]:
            if row is None:
                reasons.append(side + '_missing')
            elif row['result'] in {'notrun', 'incomplete'}:
                reasons.append(side + '_unfinished')
            elif row['result'] not in {'pass', 'skip'}:
                reasons.append(side + '_failure')
            if row and any(v not in {'pass', 'skip'} for v in row['subtests'].values()):
                reasons.append(side + '_subtest_failure')
        if w != l:
            reasons.append('different_result')
        if reasons:
            differences.append({'case': name, 'reasons': reasons, 'windows': w, 'linux': l})
    return {'expected': len(expected), 'counts': counts, 'complete': complete,
            'complete_matching_pass_or_skip': all(complete.values()) and not differences,
            'differences': differences}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('cases', type=Path)
    parser.add_argument('windows', type=Path)
    parser.add_argument('linux', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    report = compare(inventory(args.cases), read_results(args.windows), read_results(args.linux))
    report['input_sha256'] = {name: hashlib.sha256(getattr(args, name).read_bytes()).hexdigest()
                             for name in ['cases', 'windows', 'linux']}
    with args.output.open('x', encoding='utf-8', newline='\n') as output:
        json.dump(report, output, indent=2)
        output.write('\n')
    return 0 if report['complete_matching_pass_or_skip'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
