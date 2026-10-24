# Lists what the GC 10.1 and 10.3 headers (Linux gc_10_*_offset.h / _sh_mask.h) and the Mesa register tables
# (src/amd/registers gfx10.json, gfx103.json) name at the GFX10.3 VRS addresses, and the fields of the three
# registers whose VRS bits the probe sets. Read only. Usage: python -I reg-at-vrs.py
import json
import re

KH = 'P:/BC-250/ref/linux-src/drivers/gpu/drm/amd/include/asic_reg/gc/'
MESA = 'P:/BC-250/scratch/vrs-probe/mesa-wt/src/amd/registers/'
# Mesa byte address -> (kernel BASE_IDX, dword offset). Context and uconfig registers are BASE_IDX 1 at 0x28000.
ADDRS = {'DB_VRS_OVERRIDE_CNTL': 0x28064, 'PA_CL_VRS_CNTL': 0x28848, 'GE_VRS_RATE': 0x3098C}
FIELDS = {'PA_CL_VS_OUT_CNTL': [28, 29, 30], 'VGT_DRAW_PAYLOAD_CNTL': [6], 'DB_HTILE_SURFACE': [19, 20]}


def kernel_offsets(ver):
    names, base = {}, {}
    for line in open(KH + 'gc_%s_offset.h' % ver, encoding='ascii', errors='replace'):
        m = re.match(r'#define\s+(mm\w+?)(_BASE_IDX)?\s+(0x[0-9a-fA-F]+|\d+)\s*$', line)
        if m:
            (base if m.group(2) else names)[m.group(1)] = int(m.group(3), 0)
    at = {}
    for n, off in names.items():
        at.setdefault((base.get(n), off), []).append(n[2:])
    return at


def kernel_fields(ver, reg):
    out = []
    for line in open(KH + 'gc_%s_sh_mask.h' % ver, encoding='ascii', errors='replace'):
        m = re.match(r'#define\s+%s__(\w+)_MASK\s+(0x[0-9a-fA-F]+)L?\s*$' % reg, line)
        if m:
            out.append((m.group(1), int(m.group(2), 16)))
    return out


def mesa(ver):
    j = json.load(open(MESA + '%s.json' % ver))
    by_addr = {}
    for m in j['register_mappings']:
        by_addr.setdefault(m['map']['at'], []).append(m['name'])
    return j, by_addr


for ver, mver in (('10_1_0', 'gfx10'), ('10_3_0', 'gfx103')):
    at = kernel_offsets(ver)
    j, by_addr = mesa(mver)
    print('== GC %s (kernel headers) / Mesa %s.json' % (ver, mver))
    for name, addr in ADDRS.items():
        off = (addr - 0x28000) // 4
        print('  %-22s 0x%05X  kernel base1 0x%04X: %s  mesa: %s' % (
            name, addr, off, ','.join(at.get((1, off), [])) or '(none)', ','.join(by_addr.get(addr, [])) or '(none)'))
    for reg, bits in FIELDS.items():
        fields = kernel_fields(ver, reg)
        for b in bits:
            hit = [f for f, mask in fields if mask >> b & 1]
            mf = []
            for m in j['register_mappings']:
                if m['name'] == reg:
                    t = j['register_types'].get(m.get('type_ref', reg), {})
                    mf = [f['name'] for f in t.get('fields', []) if f['bits'][0] <= b <= f['bits'][1]]
            print('  %-22s bit %2d   kernel: %s  mesa: %s' % (reg, b, ','.join(hit) or '(none)', ','.join(mf) or '(none)'))
