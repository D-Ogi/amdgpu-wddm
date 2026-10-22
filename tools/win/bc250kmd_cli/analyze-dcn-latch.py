"""Check raw BD-223 records. Report a conditional pattern, never hardware proof."""
import argparse
import hashlib
import json
from pathlib import Path

# Named fields from AMD dcn_2_0_1_sh_mask.h, not register offsets.
LOCK_REQUEST = 0x1  # OTG0_OTG_MASTER_UPDATE_LOCK__OTG_MASTER_UPDATE_LOCK_MASK
LOCK_ACK = 0x100  # OTG0_OTG_MASTER_UPDATE_LOCK__UPDATE_LOCK_STATUS_MASK
LOCK_SELECT = 0x0E000000  # OTG0_OTG_GLOBAL_CONTROL0__OTG_MASTER_UPDATE_LOCK_SEL_MASK
ADDRESS_HIGH = 0xFFFF  # HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH__SURFACE_EARLIEST_INUSE_ADDRESS_HIGH_MASK
SAMPLE_NAMES = ('before', 'armed', 'boundary', 'after_unlock')


def integer(value, name, maximum=0xFFFFFFFF):
    if type(value) is not int or not 0 <= value <= maximum:
        raise ValueError(f'{name}: expected unsigned integer <= {maximum}')
    return value


def decode_sample(words):
    if len(words) != 16:
        raise ValueError('sample size')
    lock_mask = LOCK_REQUEST | LOCK_ACK
    brackets = (words[9] == 0x1FF and words[0] == words[8] and
                not words[1] & LOCK_SELECT and not words[7] & LOCK_SELECT and
                words[2] & lock_mask == words[6] & lock_mask and
                words[3] & ADDRESS_HIGH == words[5] & ADDRESS_HIGH)
    held = brackets and words[2] & lock_mask == lock_mask
    clear = brackets and words[2] & lock_mask == 0
    physical = ((words[3] & ADDRESS_HIGH) << 32) | words[4] if brackets else 0
    # Word 13 is ABI padding, not a semantic field. Never interpret it.
    cached_physical = words[14] | words[15] << 32
    coherent = (words[10] == int(brackets) and words[11] == int(held) and
                words[12] == int(clear) and cached_physical == physical)
    return dict(brackets=brackets, held=held, clear=clear, physical=physical,
                derived_fields_match=coherent, frame_word=words[0])


def decode(records):
    if len(records) != 3 or [r.get('type') for r in records] != ['header', 'result', 'end']:
        raise ValueError('Expected exactly header, result, end; incomplete captures cannot classify')
    header, row, end = records
    if (integer(header.get('schema'), 'schema') != 1 or header.get('hardware_access') is not True or
            header.get('atomic') is not False):
        raise ValueError('Header contract mismatch')
    arm = integer(header['arm'], 'arm', 2)
    version = integer(header['expected_version'], 'version')
    frequency = integer(header['qpc_frequency'], 'frequency', (1 << 63)-1)
    begin = integer(row['begin_qpc'], 'begin', (1 << 63)-1)
    finish = integer(row['end_qpc'], 'end', (1 << 63)-1)
    if not version or not frequency or finish < begin:
        raise ValueError('Version or QPC invalid')
    words = row['raw_dwords']
    if not isinstance(words, list) or len(words) != 94:
        raise ValueError('ABI 1 must contain exactly 94 DWORDs')
    for i, word in enumerate(words):
        integer(word, f'word {i}')
    # Offsets from the Windows ABI in dcn_latch_escape.h, checked on x86/x64.
    scalars = {'nt_status': 7, 'measurement_status': 20, 'unlock_status': 21,
               'restore_status': 22, 'end_status': 23, 'samples': 18,
               'writes_attempted': 19, 'boundary_observed': 24, 'reserved_a_restored': 25}
    for name, offset in scalars.items():
        if integer(row[name], name) != words[offset]:
            raise ValueError(f'Summary disagrees with raw DWORD: {name}')
    transport = integer(row['escape_status'], 'escape_status')
    exit_code = integer(end['exit_code'], 'exit_code')
    if end.get('hardware_verdict', 'missing') is not None:
        raise ValueError('Client cannot supply a hardware verdict')
    samples = {name: decode_sample(words[30+i*16:46+i*16]) for i, name in enumerate(SAMPLE_NAMES)}
    a, b = words[26] | words[27] << 32, words[28] | words[29] << 32
    reasons = []
    def require(condition, reason):
        if not condition:
            reasons.append(reason)
    require(words[:8] == [0x30353242, 37, 0, version, 1, arm, version, 0], 'reply identity/status')
    require(not any(words[8:16]), 'reserved reply words')
    require(transport == 0 and exit_code == 0, 'transport or client failure')
    require(words[16] == arm and not words[17] & 1, 'arm or owner generation')
    require(not any(words[20:24]) and words[24:26] == [1, 1], 'measurement or cleanup incomplete')
    require(words[18] >= 4 and words[19] > 0, 'no complete operation witness')
    require(0 < a < b < (1 << 48) and not (a | b) & 0xFFFF, 'reserved address pair')
    require(finish-begin <= 30*frequency, 'exceeded runner bound')
    for name, sample in samples.items():
        require(sample['brackets'] and sample['derived_fields_match'], f'{name}: invalid raw brackets or derived fields')
    before, armed, boundary, after = (samples[n] for n in SAMPLE_NAMES)
    require(before['clear'] and before['physical'] == a, 'initial A not observed unlocked')
    require(after['clear'] and after['physical'] == b, 'B not observed after unlock')
    require(armed['frame_word'] == before['frame_word'], 'arming missed initial frame')
    require(boundary['frame_word'] != before['frame_word'], 'no frame-word movement')
    if arm == 0:
        require(armed['clear'] and boundary['clear'], 'unlocked control held a lock')
        require(armed['physical'] in (a, b) and boundary['physical'] == b, 'unlocked control did not reach B')
    else:
        require(armed['held'] and boundary['held'] and armed['physical'] == a, 'locked interval not observed from A')
        require(boundary['physical'] in (a, b), 'boundary is neither A nor B')
        if arm == 1:
            require(boundary['physical'] == a, 'lock-before-program control did not retain A')
    return dict(arm=arm, eligible=not reasons, reasons=reasons, version=version,
                address_a=a, address_b=b, samples=samples, generation=words[17],
                begin_qpc=begin, end_qpc=finish, qpc_frequency=frequency,
                boundary_address='A' if boundary['physical'] == a else 'B' if boundary['physical'] == b else 'other')


def compare(captures):
    decoded = [decode(c) for c in captures]
    if len(decoded) != 3 or [d['arm'] for d in decoded] != [0, 1, 2]:
        raise ValueError('Supply one capture per arm, in order 0, 1, 2')
    reasons = [f'arm {d["arm"]}: {r}' for d in decoded for r in d['reasons']]
    for field in ('version', 'address_a', 'address_b', 'qpc_frequency'):
        if len({d[field] for d in decoded}) != 1:
            reasons.append(f'Unmatched {field}')
    if any(a['end_qpc'] >= b['begin_qpc'] for a, b in zip(decoded, decoded[1:])):
        reasons.append('Overlapping, reversed or incomparable QPC intervals')
    pattern = None
    if not reasons:
        pattern = 'H1-compatible' if decoded[2]['boundary_address'] == 'A' else 'H2-compatible'
    return dict(eligible=not reasons, reasons=reasons, conditional_pattern=pattern, arms=decoded,
                hardware_contract_verified=False, latch_proven=False, retirement_proven=False,
                limits=[
                    'H1: the later lock holds an earlier armed address at A across the observed boundary.',
                    'H2: the earlier armed address reaches B while the later lock endpoints remain held.',
                    'An H1-compatible pattern also fits H2 with pending-bank acceptance deferred until an unlocked update.',
                    'Trigger submission alone does not establish pending-bank acceptance before the later lock.',
                    'Same loaded KMD, boot, unchanged mode and exclusive owner lifetime must be established by the frozen kit.',
                    'Matching QPC, version and addresses do not establish that external contract.',
                    'Brackets permit ABA counterexamples without writer exclusion; this analysis cannot prove exclusion.',
                    'Frame-word movement is not an elapsed-frame count or proof of displayed pixels.',
                    'Cleanup flags contain no raw post-End desktop observation.',
                    'These address-only arms establish no scaling, CSC, multi-plane or HLK result.',
                ])


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f'Duplicate JSON key: {key}')
        result[key] = value
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('captures', nargs=3, type=Path, metavar='ARM_JSONL')
    args = parser.parse_args()
    try:
        raw = [p.read_bytes() for p in args.captures]
        captures = [[json.loads(line, object_pairs_hook=unique_object) for line in
                     data.decode('utf-8-sig').splitlines() if line.strip()] for data in raw]
        result = compare(captures)
        result['input_sha256'] = [hashlib.sha256(data).hexdigest() for data in raw]
        print(json.dumps(result, indent=2))
        raise SystemExit(0 if result['eligible'] else 1)
    except (ValueError, KeyError, TypeError, AttributeError, OSError) as exc:
        parser.error(str(exc))
