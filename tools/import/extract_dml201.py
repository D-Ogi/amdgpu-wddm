"""Reproduce the MIT AMD DML20 slice used by DCN201 from pinned originals."""
import argparse
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
DEST = ROOT / 'driver/shim/dml201'
BASE = 'drivers/gpu/drm/amd/display/dc/'
FILES = {
    name: BASE + 'dml/' + name for name in (
        'display_mode_lib.h', 'display_mode_structs.h', 'display_mode_enums.h',
        'display_mode_vba.h', 'display_mode_vba.c', 'dc_features.h',
        'dml_inline_defs.h', 'dml_logger.h', 'display_rq_dlg_helpers.h', 'display_rq_dlg_helpers.c',
        'dcn20/display_mode_vba_20.h', 'dcn20/display_mode_vba_20.c',
        'dcn20/display_rq_dlg_calc_20.h', 'dcn20/display_rq_dlg_calc_20.c')}
FILES.update({'dcn_calc_math.h': BASE + 'inc/dcn_calc_math.h',
              'dcn_calc_math.c': BASE + 'dml/calcs/dcn_calc_math.c',
              'dcn201_resource.c': BASE + 'resource/dcn201/dcn201_resource.c'})


def generate():
    ref = DEST / 'reference'
    result = {}
    for name in FILES:
        source = (ref / name).read_text()
        if name == 'dcn201_resource.c':
            parts = []
            for symbol in ('dcn201_ip', 'dcn201_soc'):
                match = re.search(r'static struct [^{;]+ ' + symbol + r' = \{.*?\n\};', source, re.S)
                if not match:
                    raise ValueError('missing ' + symbol)
                parts.append(match.group().replace('static struct ', 'static const struct ', 1))
            result['bbox.inc'] = '// Generated from AMD DCN201 (MIT); no numerical changes.\n' + '\n\n'.join(parts) + '\n'
        elif name == 'dml_logger.h':
            # C99 variadic forwarding instead of GNU comma swallowing.
            result[name] = source.replace('dml_print(str, ...) {DC_LOG_DML(str, ##__VA_ARGS__); }',
                'dml_print(...) {DC_LOG_DML(__VA_ARGS__); }').replace(
                'DTRACE(str, ...) {DC_LOG_DML(str, ##__VA_ARGS__); }',
                'DTRACE(...) {DC_LOG_DML(__VA_ARGS__); }')
        elif name == 'dcn_calc_math.c':
            # These helpers have no mode_lib parameter. A zero divisor produces
            # a NaN for the caller's finite-output gate, not an unchecked divide.
            count = source.count('ASSERT(significance != 0);')
            if count != 2:
                raise ValueError('unexpected math assertion count')
            result[name] = source.replace('ASSERT(significance != 0);',
                'if (significance == 0) return bc250_dml_invalid_float();')
        else:
            result[name] = source
    return result


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--import-linux', type=Path)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    if args.import_linux and args.check:
        parser.error('--check does not import')
    if args.import_linux:
        manifest = {'revision': '7d0a66e4bb9081d75c82ec4957c50034cb0ea449', 'license': 'MIT', 'files': {}}
        for name, relative in FILES.items():
            data = (args.import_linux / relative).read_bytes()
            output = DEST / 'reference' / name
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(data)
            manifest['files'][name] = {'source': relative, 'sha256': hashlib.sha256(data).hexdigest()}
        (DEST / 'reference/manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    manifest = json.loads((DEST / 'reference/manifest.json').read_text())
    for name, item in manifest['files'].items():
        if hashlib.sha256((DEST / 'reference' / name).read_bytes()).hexdigest() != item['sha256']:
            raise SystemExit('changed DML reference: ' + name)
    for name, content in generate().items():
        target = DEST / name
        if args.check:
            if target.read_text() != content:
                raise SystemExit('stale DML import: ' + name)
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(content, newline='\n')
    print('DML201 originals and generated files ' + ('checked' if args.check else 'written'))


if __name__ == '__main__':
    main()
