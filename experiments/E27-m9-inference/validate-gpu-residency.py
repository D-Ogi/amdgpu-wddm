"""Validate captured GPU residency positive-path evidence; does not infer physical endpoints."""
import argparse
import json
import re
from pathlib import Path


def read(path):
    data = path.read_bytes()
    return data.decode('utf-16' if data.startswith((b'\xff\xfe', b'\xfe\xff')) else 'utf-8-sig')


def validate(directory, name, size):
    output = read(directory / (name + '.out'))
    assert int(read(directory / (name + '.exit'))) == 0, 'native exit'
    assert 'GPU_RESIDENCY_RESULT PASS' in output, 'probe result'
    assert not any(word in output for word in ('GPU_MISMATCH', 'TIMEOUT', 'INCONCLUSIVE')), 'failed observation'
    reads = re.findall(r'GPU_READBACK bytes=(\d+) all_words_match=1 fence=(\d+)', output)
    chunks = (size + (1 << 20) - 1) // (1 << 20)
    assert reads == [(str(size), str(chunks * i)) for i in range(1, 5)], 'full-range readbacks'
    assert re.findall(r'CYCLE (\d+) PASS', output) == ['1', '2', '3'], 'cycles'
    departures = re.findall(r'RESIDENCY after-pressure value=(\d+)', output)
    assert len(departures) == 3 and all(x in ('2', '3') for x in departures), 'residency departure'
    assert re.findall(r'RESIDENCY after-resident value=(\d+)', output) == ['1'] * 3, 'GPU residency restored'
    summary = read(directory / (name + '-after.log'))
    gfx = re.findall(r'node 0 hardware: (\d+) submitted, (\d+) completed, (\d+) timeouts, (\d+) refused', summary)[-1]
    sdma = re.findall(r'node 1 \(paging, open\): (\d+) hardware submitted, (\d+) completed, (\d+) timeouts, (\d+) refused', summary)[-1]
    for counters in (gfx, sdma):
        assert counters[0] == counters[1] and counters[2:] == ('0', '0'), 'hardware completion'
    assert 'no TDR (ResetEngine, ResetFromTimeout and RestartFromTimeout were never called)' in summary
    return dict(name=name, bytes=size, cycles=3, gpu_readbacks=reads, departure_status=departures,
                cumulative_gfx=gfx, cumulative_sdma=sdma,
                bulk_resident_ms=re.findall(r'BULK_RESIDENT bytes=\d+ elapsed_ms=(\d+) success=1', output),
                physical_endpoints_verified=False, shader_cache_policy_verified=False)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    results = [validate(args.directory, 'vram64k', 65536), validate(args.directory, 'vram1g', 1 << 30)]
    print(json.dumps(results, indent=2))
