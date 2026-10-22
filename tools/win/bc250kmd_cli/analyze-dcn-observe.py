"""Describe sequential OBSERVE_DCN samples. Never certify a latch or buffer retirement."""
import argparse
import json
from pathlib import Path

# dcn_2_0_1_sh_mask.h: named masks, not register offsets.
SURFACE_FLIP_PENDING = 0x100  # HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK
ADDRESS_HIGH = 0xFFFF  # both PRIMARY_SURFACE_ADDRESS_HIGH and SURFACE_EARLIEST_INUSE_HIGH
FIELDS = '''Magic Command Status Version NtStatus AbiVersion RegisterCount ValidMask
PrimaryAddressLow PrimaryAddressHigh EarliestInUseLow EarliestInUseHigh FlipControl SurfacePitch
OtgStatusPosition OtgGlobalControl0 OtgBlankControl OtgDoubleBufferControl OtgFrameCount TimingControl
TimingHTotal TimingVTotal TimingHBlank TimingVBlank TimingPixelControl TimingPhase TimingModulo
TimingInterlace TimingVTotalControl TimingReference SequenceBefore SequenceAfter'''.split()


def summarize(records):
    if not records or records[0].get('type') != 'header' or records[0].get('schema') != 1:
        raise ValueError('Missing observer schema 1 header')
    header = records[0]
    if (type(header.get('samples')) is not int or not 1 <= header['samples'] <= 40 or
            type(header.get('expected_version')) is not int or not 1 <= header['expected_version'] <= 0xFFFFFFFF or
            header.get('atomic') is not False or header.get('hardware_access') is not True):
        raise ValueError('Invalid capture header')
    if any(r.get('type') != 'sample' for r in records[1:-1]):
        raise ValueError('Unexpected intermediate record')
    if len(records) > 1 and records[-1].get('type') not in ('sample', 'end'):
        raise ValueError('Unexpected terminal record')
    frequency = header['qpc_frequency']
    if type(frequency) is not int or frequency <= 0:
        raise ValueError('Invalid QPC frequency')
    rows = [r for r in records if r.get('type') == 'sample']
    end = records[-1] if records[-1].get('type') == 'end' else {}
    invalid, software_changed, pending, unequal_clear = [], [], [], []
    frames, durations, primary, earliest = set(), [], set(), set()
    last_end = None
    for index, row in enumerate(rows):
        d = row['data']
        if set(d) != set(FIELDS) or any(type(v) is not int or v < 0 or v > 0xFFFFFFFF for v in d.values()):
            raise ValueError('Non-DWORD register field')
        begin, finish = row['begin_qpc'], row['end_qpc']
        if type(begin) is not int or type(finish) is not int or finish < begin or (last_end is not None and begin < last_end):
            raise ValueError('Invalid sample timing')
        last_end = finish
        durations.append((finish - begin) * 1000 / frequency)
        # Magic is BC250_ESCAPE_MAGIC ('B250'), command is OBSERVE_DCN.
        valid = (row['index'] == index and row['escape_status'] == 0 and
                 d['Magic'] == 0x30353242 and d['Command'] == 20 and d['Status'] == 0 and
                 d['NtStatus'] == 0 and d['Version'] == header['expected_version'] and
                 d['AbiVersion'] == 1 and d['RegisterCount'] == 22 and d['ValidMask'] == (1 << 22) - 1)
        if not valid:
            invalid.append(index)
            continue
        # An even, unchanged software sequence does NOT bracket physical latching.
        if d['SequenceBefore'] != d['SequenceAfter'] or d['SequenceBefore'] & 1:
            software_changed.append(index)
            continue
        frames.add(d['OtgFrameCount'])
        p = ((d['PrimaryAddressHigh'] & ADDRESS_HIGH) << 32) | d['PrimaryAddressLow']
        e = ((d['EarliestInUseHigh'] & ADDRESS_HIGH) << 32) | d['EarliestInUseLow']
        primary.add(p)
        earliest.add(e)
        if d['FlipControl'] & SURFACE_FLIP_PENDING:
            pending.append(index)
        elif p != e:
            unequal_clear.append(index)
    return {
        'capture_complete': len(rows) == header['samples'] and not invalid and end.get('exit_code') == 0,
        'samples': len(rows), 'invalid_samples': invalid, 'software_change_samples': software_changed,
        'pending_samples': pending, 'clear_with_unequal_address_words': unequal_clear,
        'distinct_frame_words': len(frames), 'distinct_primary_word_pairs': len(primary),
        'distinct_earliest_word_pairs': len(earliest), 'max_escape_ms': max(durations, default=0),
        'latch_proven': False, 'retirement_proven': False,
        'limits': [
            'Sequential low/high reads can tear even with an unchanged software sequence.',
            'A clear pending bit and unequal words are a lead, not a coherent hardware counterexample.',
            'Equal addresses do not prove geometry, color, disable completion, or safe allocation retirement.',
            'HUBP0 only. No HUBP1, coefficient-bank or CSC-bank observation.',
            'HardwareAccess idles GPU scheduling. This is not an unperturbed latency measurement.',
            'Frame movement alone does not prove visible pixels or a particular plane latch.',
        ],
    }


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('samples', type=Path)
    args = parser.parse_args()
    try:
        records = [json.loads(line) for line in args.samples.read_text(encoding='utf-8-sig').splitlines() if line.strip()]
        print(json.dumps(summarize(records), indent=2))
    except (ValueError, KeyError, TypeError) as exc:
        parser.error(str(exc))
