"""Check same-process GPU content across the gate; OS S4 evidence is separate."""
import argparse
import json
import re
from pathlib import Path


def validate(text, size):
    assert 'GPU_RETAINED_RESUME_RESULT PASS' in text, 'retained result'
    assert not any(x in text for x in ('GPU_MISMATCH', 'TIMEOUT', 'RESUME_GATE FAIL')), 'failure marker'
    chunks = (size + (1 << 20) - 1) // (1 << 20)
    assert re.findall(r'GPU_READBACK bytes=(\d+) all_words_match=1 fence=(\d+)', text) == [
        (str(size), str(chunks)), (str(size), str(2 * chunks))], 'two full readbacks'
    rows = [dict(re.findall(r'(\w+)=([^ ]+)', line)) for line in text.splitlines() if line.startswith('RESUME_ID ')]
    assert [r.pop('phase') for r in rows] == ['ready', 'released', 'verified'], 'phase progression'
    assert [r.pop('sequence') for r in rows] == [str(chunks), str(chunks), str(2 * chunks)], 'fence progression'
    expected_keys = {'pid', 'adapter', 'device', 'context', 'paging_queue', 'paging_fence', 'fence',
                     'data', 'data_va', 'command', 'command_va', 'readback', 'readback_va'}
    assert all(set(r) == expected_keys for r in rows), 'identity completeness'
    assert rows[0] == rows[1] == rows[2], 'retained identities'
    assert all(int(v, 10 if k == 'pid' else 16) != 0 for k, v in rows[0].items()), 'live identities'
    pid = rows[0]['pid']
    assert re.findall(r'RESUME_GATE READY pid=(\d+)', text) == [pid], 'ready PID'
    assert re.findall(r'RESUME_GATE RELEASED pid=(\d+)', text) == [pid], 'release PID'
    assert re.findall(r'GPU_RETAINED_RESUME_RESULT PASS pid=(\d+) bytes=(\d+) final_sequence=(\d+)', text) == [
        (pid, str(size), str(2 * chunks))], 'final identity'
    tail = text[text.index('RESUME_GATE READY'):]
    assert not any(x in tail for x in ('CPU_FILL ', 'CreateAllocation2', 'CreateContextVirtual', 'CYCLE ')), 'no source refill/recreation'
    first_read = text.index('GPU_READBACK ')
    ready = text.index('RESUME_GATE READY')
    released = text.index('RESUME_GATE RELEASED')
    second_read = text.index('GPU_READBACK ', first_read + 1)
    assert first_read < ready < released < second_read, 'readback gate order'
    return dict(pid=int(pid), bytes=size, preserved_identities=rows[0], full_readbacks=2,
                actual_s4_verified=False, external_os_power_evidence_required=True)


def self_test():
    identity = 'pid=99 adapter=1 device=2 context=3 paging_queue=4 paging_fence=5 fence=6 data=7 data_va=1000 command=8 command_va=2000 readback=9 readback_va=3000'
    good = '\n'.join(['GPU_READBACK bytes=65536 all_words_match=1 fence=1',
        f'RESUME_ID phase=ready {identity} sequence=1', 'RESUME_GATE READY pid=99',
        'RESUME_GATE RELEASED pid=99', f'RESUME_ID phase=released {identity} sequence=1',
        'GPU_READBACK bytes=65536 all_words_match=1 fence=2',
        f'RESUME_ID phase=verified {identity} sequence=2',
        'GPU_RETAINED_RESUME_RESULT PASS pid=99 bytes=65536 final_sequence=2'])
    assert validate(good, 65536)['pid'] == 99
    bad = [good.replace('phase=verified pid=99', 'phase=verified pid=100'),
           good.replace('phase=verified '+identity, 'phase=verified '+identity.replace('context=3', 'context=10')),
           good.replace('all_words_match=1 fence=2', 'all_words_match=1 fence=1'),
           good.replace('RELEASED pid=99', 'RELEASED pid=9'),
           good.replace('RESUME_GATE RELEASED', 'CPU_FILL bytes=65536\nRESUME_GATE RELEASED'),
           good.replace('bytes=65536 all_words_match=1 fence=2', 'bytes=4096 all_words_match=1 fence=2'),
           good.replace('RETAINED_RESUME_RESULT PASS', 'RETAINED_RESUME_RESULT FAIL')]
    for sample in bad:
        try:
            validate(sample, 65536)
        except AssertionError:
            continue
        raise AssertionError('negative control unexpectedly accepted')
    print('retained output validator: 1 positive, 7 negative controls pass')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path, nargs='?')
    parser.add_argument('--bytes', type=int, default=64 << 20)
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        self_test()
    else:
        if not args.output:
            parser.error('output is required')
        raw = args.output.read_bytes()
        text = raw.decode('utf-16' if raw.startswith((b'\xff\xfe', b'\xfe\xff')) else 'utf-8-sig')
        print(json.dumps(validate(text, args.bytes), indent=2))
