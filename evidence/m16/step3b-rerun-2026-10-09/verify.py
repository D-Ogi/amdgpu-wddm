"""Check the exported historical trial, without a GPU or lab connection."""
from pathlib import Path
import hashlib
import json
import math
import re

ROOT = Path(__file__).resolve().parent


def require(condition, message):
    if not condition:
        raise SystemExit(message)


def read(relative):
    return (ROOT / relative).read_text(encoding='utf-8-sig')


def main():
    manifest = json.loads(read('EXPORT.json'))
    summary = json.loads(read('summary.json'))
    expected = {entry['file'] for entry in manifest['files']}
    actual = {p.relative_to(ROOT).as_posix() for p in (ROOT / 'raw').iterdir() if p.is_file()}
    require(actual == expected and len(actual) == 52, 'Raw file inventory differs')
    for entry in manifest['files']:
        data = (ROOT / entry['file']).read_bytes()
        require(len(data) == entry['bytes'], 'Size differs: ' + entry['file'])
        digest = hashlib.sha256(data).hexdigest()
        require(digest == entry['export_sha256'], 'Digest differs: ' + entry['file'])
        if not entry['redactions']:
            require(digest == entry['source_sha256'], 'Unredacted source differs')

    for row in summary['arms']:
        text = read(row['source'])
        match = re.search(r'arm (\S+) exit (-?\d+) after ([\d.]+) s', text)
        require(match is not None, 'Missing wrapper verdict')
        require((match[1], int(match[2]), float(match[3])) ==
                (row['arm'], row['exit_code'], row['wrapper_wall_s']), 'Wrapper verdict differs')
        require(int(re.search(r' bound (\d+) s', text)[1]) == row['bound_s'], 'Bound differs')
        require(row['wrapper_wall_s'] < row['bound_s'] <= 180, 'Historical arm exceeded its bound')
        if '3a-threads' in row['arm']:
            match = re.search(r'4 threads x 4 waits took ([\d.]+) ms, serialised that is ([\d.]+) ms', text)
            require(match is not None, 'Missing thread timing')
            parallel, serial = map(float, match.groups())
            require((parallel, serial) == (row['parallel_ms'], row['serialised_ms']), 'Thread times differ')
            require(math.isclose(parallel / serial, row['parallel_over_serialised']), 'Thread ratio differs')
            require(parallel / serial <= 1.1 and '0 API failures, 0 verdicts failed' in text, 'Thread control failed')
    require(len(summary['arms']) == 13, 'Arm count differs')

    for point, snapshot in summary['node0_last_summary'].items():
        rows = re.findall(r'node 0 hardware: (\d+) submitted, (\d+) completed, (\d+) timeouts, (\d+) refused, (\d+) soft-recovered', read(snapshot['source']))
        fields = ('submitted', 'completed', 'timeouts', 'refused', 'soft_recovered')
        require(rows and list(map(int, rows[-1])) == [snapshot[key] for key in fields], 'Node summary differs: ' + point)
    snapshots = summary['node0_last_summary']
    for key in ('submitted', 'completed'):
        delta = snapshots['final'][key] - snapshots['before'][key]
        require(delta == summary['node0_' + key + '_delta'], 'Counter delta differs')
    before = read('raw/state-00-before.txt').splitlines()[1:]
    after = read('raw/state-99-after.txt').splitlines()[1:]
    require(before == after and len(before) == summary['unchanged_installed_state_lines'] == 24, 'State differs')
    for point in ('before', 'after'):
        rows = len(re.findall(r'^  ok\s', read('raw/artifact-hashes-' + point + '.txt'), re.M))
        require(rows == summary['hash_verifier_ok_rows'][point] == 39, 'Hash verifier row count differs')
    require((ROOT / 'raw/m16-hip3b--hip-64.txt').read_bytes() == b'\xef\xbb\xbf', 'HIP text witness differs')
    vk = (ROOT / 'raw/m16-vk--vk-64.txt').read_bytes()
    require(hashlib.sha256(vk).hexdigest() == '00d338aec0070bef86cc8d7d113cbb2d874b8082892e0edf08dc307d3d55100e', 'Vulkan text differs')
    print('PASS: 52 raw files, 13 arms, thread ratios, counter deltas, 24 state lines and text controls')


if __name__ == '__main__':
    main()
