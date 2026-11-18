"""Synthetic flashrom process for host tests. No USB or real flashrom dependency."""
import hashlib
import json
import os
from pathlib import Path
import sys

root = Path(os.environ['BC250_FAKE_SPI']).resolve()
config = json.loads((root / 'config.json').read_text())
scenario = config['scenario']
args = sys.argv[1:]
operation = ('version' if '--version' in args else
             'write' if '-w' in args else
             Path(args[args.index('-r') + 1]).stem if '-r' in args else 'probe')
calls = root / 'calls.jsonl'
old = calls.read_text().splitlines() if calls.exists() else []
past = [json.loads(x)['operation'] for x in old]
if operation == 'probe':
    if 'write' in past and 'after1' not in past:
        operation = 'probe-before-readback'
    elif 'after1' in past and 'after2' not in past:
        operation = 'probe-between-after' if config.get('second_readback') else 'probe-after'
    elif 'before2' in past:
        operation = 'probe-after'
    elif 'before1' in past:
        operation = 'probe-between-before'
statusfile = root / 'status.txt'
record = {'operation': operation, 'args': args}
chip = root / 'chip.bin'
code = 0

if operation == 'version':
    if scenario == 'probe-protected': statusfile.write_text('0x44')
    print('flashrom v9.0' if scenario == 'version-wrong' else 'flashrom v1.2 test fixture')
    if scenario == 'version-fail': code = 9
    if scenario == 'mutate-original':
        p = Path(config['original'])
        b = bytearray(p.read_bytes()); b[100] ^= 1; p.write_bytes(b)
elif operation.startswith('probe'):
    name = 'WRONG' if scenario == 'probe-chip' else config['chip_name']
    size = 8192 if scenario == 'probe-size' else 16384
    # The programmer of the call decides what the probe names, as the real tool does.
    programmer = args[args.index('-p') + 1] if '-p' in args else ''
    kind = 'serprog' if programmer.startswith('serprog:') else programmer
    if kind == 'serprog':
        firmware = 'other-serprog' if scenario == 'probe-programmer-name' else 'pico-serprog'
        print(f'serprog: Programmer name is "{firmware}"')
    if scenario == 'probe-programmer-kind':
        kind = 'serprog' if kind == 'ch341a_spi' else 'ch341a_spi'
    if scenario != 'probe-none':
        print(f'Found Macronix flash chip "{name}" ({size} kB, SPI) on {kind}.')
    if scenario == 'probe-multiple': print('Multiple flash chip definitions match')
    if scenario != 'probe-status':
        status = statusfile.read_text() if statusfile.exists() else '0x40'
        if scenario == 'post-status-changed' and operation == 'probe-after':
            status = '0x44'
        if scenario == 'probe-invalid-status': status = '0x00'
        print('status register is ' + status)
    if scenario == 'probe-fail' or (scenario == 'post-probe-fail' and operation == 'probe-after'):
        code = 9
    if scenario == 'receipt-fail' and operation == 'probe-after':
        # Probe has no output path argument. Find this test's sole current run directory.
        runs = sorted((root / 'outputs').glob('spi-write-*'))
        (runs[-1] / 'result.json').mkdir()
elif operation == 'write':
    path = Path(args[args.index('-w') + 1])
    data = path.read_bytes()
    record['programmed_sha256'] = hashlib.sha256(data).hexdigest()
    if scenario == 'write-partial':
        partial = bytearray(data); partial[8192] ^= 1; chip.write_bytes(partial)
    else:
        chip.write_bytes(data)
    if scenario != 'write-no-verify': print('Verifying flash... VERIFIED.')
    if scenario == 'write-fail': code = 9
    if scenario == 'write-sets-protection': statusfile.write_text('0x44')
else:
    # Model the real read unlock: without the pre-read probe, a changed BP can be hidden.
    if statusfile.exists() and statusfile.read_text() != '0x40':
        record['read_unlocked_protection'] = True
        statusfile.write_text('0x40')
    path = Path(args[args.index('-r') + 1])
    data = bytearray(chip.read_bytes())
    if scenario == 'before-short' and operation == 'before1': data = data[:64]
    if ((scenario == 'base-unstable' and operation == 'before2') or
            (scenario == 'readback-mismatch' and operation == 'after1') or
            (scenario == 'second-readback-mismatch' and operation == 'after2')):
        data[7] ^= 1
    if not (scenario == 'before-missing' and operation == 'before1'):
        path.write_bytes(data)
    if scenario == 'stale-before2' and operation == 'before1':
        path.with_name('before2.bin').write_bytes(data)
    if scenario in ('mutate-snapshot', 'mutate-backup') and operation == 'before1':
        try:
            path.with_name('backup.bin' if scenario == 'mutate-backup' else 'candidate.bin').write_bytes(data)
            record['snapshot_write_denied'] = False
        except PermissionError:
            record['snapshot_write_denied'] = True
    if scenario == 'read-sets-protection' and operation == 'before1': statusfile.write_text('0x44')
    if ((scenario == 'before-fail' and operation == 'before1') or
            (scenario == 'after-fail' and operation == 'after1')):
        code = 9

record['exit_code'] = code
with calls.open('a') as stream:
    stream.write(json.dumps(record) + '\n')
sys.exit(code)
