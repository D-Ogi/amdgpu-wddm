"""Prove selected safety tests fail when their production guard is removed. No hardware."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import uuid

HERE = Path(__file__).resolve().parent
CASES = [
    ('pre-read-status', 'test_readback_cannot_hide_protection_change', "    $null = Probe ($Name + '-status')"),
    ('plan-pin', 'test_plan_hash', "    Require ((Digest $planBytes) -eq $planHash) 'plan hash mismatch'"),
    ('approved-edits', 'test_unapproved_edit', "        Require ((Digest $expected) -eq $imgHash) 'candidate differs outside the approved edits'"),
    ('probe-exit', 'test_probe_error_with_plausible_output', '    Require ($r.Code -eq 0) "$Name failed"'),
    ('read-exit', 'test_before_error_with_valid_file', '    Require ($r.Code -eq 0) "$Name command failed"'),
    ('write-exit', 'test_failed_write_stays_failed_with_matching_readback', "        Require ($write.Code -eq 0) 'write command failed; readback result is recorded separately'"),
    ('readback', 'test_bad_readback', "        Require $receipt.readback_matches 'readback hash mismatch'"),
    ('sector-limit', 'test_sector_limit', "    if (-not $Restore) { Require ($diff.sectors_4k -le $MaxSectors) 'changed-sector limit exceeded' }"),
    ('fresh-base', 'test_current_variable_store_changed', "    if (-not $Restore) { Require ($current.Sha256 -eq $baseHash) 'chip base hash mismatch: obtain a fresh backup and rebuild the candidate' }"),
]

def main():
    parent = Path(os.environ['BC250_TEST_TMP']).resolve(strict=True)
    temp = (parent / ('spi-mutations-' + uuid.uuid4().hex)).resolve()
    if not temp.is_relative_to(parent) or temp == parent:
        raise RuntimeError('invalid test directory')
    temp.mkdir()
    original = (HERE / 'spi_write.ps1').read_text(encoding='utf-8-sig')
    results = []
    try:
        for name, test, guard in CASES:
            if original.count(guard) != 1:
                raise RuntimeError(f'{name}: guard anchor not unique')
            command = [sys.executable, str(HERE / 'test_spi_write.py'), 'WriterTests.' + test, '-v']
            env = dict(os.environ)
            env.pop('BC250_SPI_WRITE_SCRIPT', None)
            baseline = subprocess.run(command, env=env, capture_output=True, text=True, timeout=120)
            if baseline.returncode != 0:
                raise RuntimeError(f'{name}: control failed\n{baseline.stdout}\n{baseline.stderr}')
            mutant = temp / (name + '.ps1')
            mutant.write_text(original.replace(guard, '    # Mutation: removed ' + name), encoding='utf-8')
            env['BC250_SPI_WRITE_SCRIPT'] = str(mutant)
            changed = subprocess.run(command, env=env, capture_output=True, text=True, timeout=120)
            output = changed.stdout + changed.stderr
            killed = (changed.returncode == 1 and ('FAIL: ' + test) in output and
                      'ERROR:' not in output and 'SPI-WRITE VERDICT PASS' in output)
            results.append({'mutation': name, 'test': test, 'control_exit': baseline.returncode,
                            'mutant_exit': changed.returncode, 'killed': killed})
            print(f'{name}: control PASS; mutant {"KILLED" if killed else "NOT PROVED"}', flush=True)
            if not killed:
                print(output)
                return 1
        print(json.dumps({'mutations': results, 'hardware_access': False}, indent=2))
        return 0
    finally:
        # This is the unique directory created above, verified within the authorized test parent.
        shutil.rmtree(temp)

if __name__ == '__main__':
    sys.exit(main())
