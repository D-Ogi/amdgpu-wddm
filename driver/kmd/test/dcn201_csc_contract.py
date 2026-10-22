#!/usr/bin/env python3
"""Audit selected AMD CSC initializers with Clang, without hardware access.

This is a source-contract test, not a hardware capability test. Expand the
original macros rather than inferring register/field initialization from the
generic function's name. DCN10 and explicit DCN20 append/debug macros are
positive controls for the extraction and preprocessor path.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[3]
REF = ROOT / 'driver/amdgpu-import/reference'


def definitions(text):
    lines = iter(text.splitlines(keepends=True))
    result = []
    for line in lines:
        if re.match(r'#define (?:TF_|DPP_)', line):
            block = line
            while line.rstrip().endswith('\\'):
                line = next(lines)
                block += line
            result.append(block)
    return '\n'.join(result)


def audit(clang):
    names = ['dcn10_dpp.h', 'dcn20_dpp.h', 'dcn201_dpp.h',
             'connector/dcn201_resource.c', 'dcn201_dpp.c']
    raw = {name: (REF / name).read_bytes() for name in names}
    source = {name: data.decode().replace('\r\n', '\n') for name, data in raw.items()}
    resource = source['connector/dcn201_resource.c']
    # Resolve the selected resource initializers, including any future additions.
    initializers = {}
    for kind in ('shift', 'mask'):
        matches = re.findall(
            r'static const struct dcn201_dpp_' + kind + r' tf_' + kind
            + r' = \{(.*?)\};', resource, re.S)
        if len(matches) != 1:
            raise ValueError('Cannot resolve DCN201 ' + kind)
        initializers[kind] = matches[0]
    regs = re.findall(r'#define tf_regs\(id\)\\\n\[id\] = \{\\\n(.*?)\\\n\}', resource, re.S)
    if len(regs) != 1:
        raise ValueError('Cannot resolve DCN201 registers')
    if not re.search(r'\.dpp_setup\s*=\s*dpp201_cnv_setup\b', source['dcn201_dpp.c']):
        raise ValueError('Review changed DCN201 setup dispatch')
    if 'dpp1_program_input_csc(dpp_base,' not in source['dcn201_dpp.c']:
        raise ValueError('Review changed DCN201 CSC dispatch')
    probes = {
        'selected_regs': regs[0].replace('id', '0'),
        'selected_shift': initializers['shift'],
        'selected_mask': initializers['mask'],
        'control_dcn10_regs': 'TF_REG_LIST_DCN10(0)',
        'control_dcn10_shift': 'TF_DEBUG_REG_LIST_SH_DCN10',
        'control_dcn10_mask': 'TF_DEBUG_REG_LIST_MASK_DCN10',
        'control_dcn20_append': 'TF_REG_LIST_DCN20_COMMON_APPEND(0)',
        'control_dcn20_shift': 'TF_DEBUG_REG_LIST_SH_DCN20',
        'control_dcn20_mask': 'TF_DEBUG_REG_LIST_MASK_DCN20',
        'selected_fields': 'TF_REG_FIELD_LIST_DCN201(uint32_t)',
        'selected_reg_members': 'DPP_DCN201_REG_VARIABLE_LIST',
        'control_common_fields': 'TF_REG_FIELD_LIST(uint32_t)',
        'control_common_reg_members': 'DPP_COMMON_REG_VARIABLE_LIST',
    }
    text = '\n'.join(definitions(source[name]) for name in names[:3])
    # No fabricated addresses or register values: replace leaf macros with names.
    text += '\n#define SRI(reg, block, id) REG_##reg\n'
    text += '#undef TF_SF\n#define TF_SF(reg, field, suffix) FIELD_##field\n'
    text += '#undef TF2_SF\n#define TF2_SF(reg, field, suffix) FIELD_##field\n'
    for key, body in probes.items():
        text += '\nBEGIN_' + key + '\n' + body + '\nEND_' + key + '\n'
    result = subprocess.run([str(clang), '-E', '-P', '-x', 'c', '-'],
                            input=text, text=True, capture_output=True, check=True)
    blocks = {}
    for key in probes:
        match = re.search(r'BEGIN_' + key + r'\b(.*?)\bEND_' + key, result.stdout, re.S)
        if not match:
            raise ValueError('Missing expanded probe ' + key)
        blocks[key] = match[1].strip()
        if re.search(r'\b(?:TF_(?:REG|DEBUG)_\w+|DPP_\w*REG_VARIABLE_LIST\w*|SRI)\b', blocks[key]):
            raise ValueError('Unexpanded macro in ' + key)
    old_field = 'CM_TEST_DEBUG_DATA_ID9_ICSC_MODE'
    new_field = 'CM_TEST_DEBUG_DATA_ICSC_MODE'
    checks = {
        'selected_primary_icsc': 'REG_CM_ICSC_C11_C12' in blocks['selected_regs'],
        'selected_coma_missing': 'REG_CM_COMA_C11_C12' not in blocks['selected_regs'],
        'selected_icsc_b_missing': 'REG_CM_ICSC_B_C11_C12' not in blocks['selected_regs'],
        'selected_old_debug_field_declared': old_field in blocks['selected_fields'],
        'selected_new_debug_field_declared': new_field in blocks['selected_fields'],
        'selected_old_debug_uninitialized': all(old_field not in blocks[k] for k in ('selected_shift', 'selected_mask')),
        'selected_new_debug_uninitialized': all(new_field not in blocks[k] for k in ('selected_shift', 'selected_mask')),
        'selected_coma_member_declared': 'CM_COMA_C11_C12' in blocks['selected_reg_members'],
        'selected_icsc_b_member_missing': 'CM_ICSC_B_C11_C12' not in blocks['selected_reg_members'],
        'selected_fields_keep_common_prefix': blocks['selected_fields'].startswith(blocks['control_common_fields']),
        'selected_registers_keep_common_prefix': blocks['selected_reg_members'].startswith(blocks['control_common_reg_members']),
        'control_coma_present': 'REG_CM_COMA_C11_C12' in blocks['control_dcn10_regs'],
        'control_icsc_b_present': 'REG_CM_ICSC_B_C11_C12' in blocks['control_dcn20_append'],
        'control_old_debug_shift': bool(re.search(old_field + r'\s*=\s*4\b', blocks['control_dcn10_shift'])),
        'control_old_debug_mask': bool(re.search(old_field + r'\s*=\s*0x30\b', blocks['control_dcn10_mask'])),
        'control_new_debug_shift': bool(re.search(new_field + r'\s*=\s*3\b', blocks['control_dcn20_shift'])),
        'control_new_debug_mask': bool(re.search(new_field + r'\s*=\s*0x18\b', blocks['control_dcn20_mask'])),
    }
    offset = (ROOT / 'third_party/linux-amdgpu/dcn_2_0_1_offset.h').read_text()
    checks['header_coma_absent'] = 'mmCM0_CM_COMA_C11_C12' not in offset
    checks['header_icsc_b_present'] = 'mmCM0_CM_ICSC_B_C11_C12' in offset
    failed = [key for key, passed in checks.items() if not passed]
    if failed:
        raise ValueError('Review changed CSC contract: ' + ', '.join(failed))
    return {
        'scope': 'Source initializers only; no MMIO, hardware or HLK verdict',
        'compiler': subprocess.check_output([str(clang), '--version'], text=True).splitlines()[0],
        'sources_sha256': {name: hashlib.sha256(data).hexdigest() for name, data in raw.items()},
        'checks': checks,
        'expanded_macros': blocks,
    }


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--clang', type=Path, required=True)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    report = audit(args.clang)
    if args.out:
        args.out.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f"DCN201 CSC source contract: {len(report['checks'])} checks PASS; no bank writer admitted")
