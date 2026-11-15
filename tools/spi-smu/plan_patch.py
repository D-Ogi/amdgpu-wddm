"""Validate a patch recipe in RAM and emit a review report, never a firmware file."""
import argparse
import json
from pathlib import Path
from offline import inspect, patch, digest, diff_ranges


def plan(image_path, recipe):
    image = Path(image_path).read_bytes()
    report = inspect(image)
    if recipe['stage'] not in ('S2', 'S3', 'S4'):
        raise ValueError('unknown stage')
    entries = [e for e in report['smu'] if e['type'] == recipe['entry_type']]
    if len(entries) != 1:
        raise ValueError('select exactly one SMU entry')
    if not recipe['edits']:
        raise ValueError('empty candidate')
    changed = patch(image, entries[0], recipe['edits'], recipe['stock_sha256'])
    from psptool import PSPTool
    parsed = PSPTool(bytearray(changed))
    if entries[0]['range'][0] not in parsed.files_by_offset:
        raise ValueError('entry offset missing after re-parse')
    candidate = parsed.files_by_offset[entries[0]['range'][0]]
    if not candidate.signed_entity or not candidate.signed_entity.certifying_keys:
        raise ValueError('candidate verification key missing')
    candidate_signature_valid = candidate.signed_entity.verify_with_tree()
    if candidate_signature_valid:
        raise ValueError('changed authenticated body still verifies')
    return {'image': str(Path(image_path).resolve()), 'stage': recipe['stage'], 'entry_type': recipe['entry_type'],
            'stock_sha256': digest(image), 'candidate_sha256': digest(changed),
            'diff_ranges': diff_ranges(image, changed), 'bytes': len(changed),
            'entry_sha256': digest(changed[slice(*entries[0]['range'])]),
            'signature_preserved': image[slice(*entries[0]['signature_range'])] == changed[slice(*entries[0]['signature_range'])],
            'release_ready': False,
            'candidate_signature_valid': candidate_signature_valid,
            'unresolved': ['runtime role of edits', 'entry selection', 'code ownership',
                           'ABI and instruction review', 'enclosing integrity checks',
                           'installed-image identity', 'owner authorization'],
            'recipe': recipe}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('image')
    p.add_argument('recipe', type=Path)
    p.add_argument('--report', required=True, type=Path)
    args = p.parse_args()
    if args.report.exists() or args.report.suffix != '.json':
        p.error('report must be a new .json file')
    result = plan(args.image, json.loads(args.recipe.read_text(encoding='utf-8')))
    with args.report.open('x', encoding='utf-8') as stream:
        json.dump(result, stream, indent=2)
        stream.write('\n')


if __name__ == '__main__':
    main()
