#!/usr/bin/env python3
"""L35: read the BIOS SPI flash through the CPU's memory-mapped ROM window, with no SPI controller access.

  spiread.py probe <outdir>   kernel devmem policy, /proc/iomem around the window, FCH LPC bridge 00:14.3
                              config space (ROM decode and ROM protect registers, config reads only), and a
                              one-page read test at 0xFFFFF000.
  spiread.py read <outdir>    two full reads of 0xFF000000-0xFFFFFFFF (16 MiB, the size of the P3.00 image),
                              hashes, byte comparison, structure checks, LPC config compared with the probe.

No SPI command is issued: the controller's registers (SPI base, FIFO, opcode, restricted-command, protection
lock) are not read or written. The FCH turns CPU memory reads of the window into SPI read cycles, the same
cycles that fetched the reset vector at boot. The images go to <outdir> (RAM); nothing here prints image bytes.
"""
import hashlib
import json
import os
import struct
import sys
import time

WIN_BASE, WIN_SIZE = 0xFF000000, 0x1000000
LPC = '/sys/bus/pci/devices/0000:00:14.3/config'
# AMD Embedded Firmware Structure candidates for a 16 MiB part mapped at 0xFF000000 (AMD PSP spec offsets).
EFS_OFFSETS = [0xFA0000, 0xF20000, 0xE20000, 0xC20000, 0x820000, 0x020000]


def lpc_snapshot():
    cfg = open(LPC, 'rb').read()
    vid, did = struct.unpack_from('<HH', cfg, 0)
    regs = {f'0x{o:02X}': f'0x{struct.unpack_from("<I", cfg, o)[0]:08X}'
            for o in (0x08, 0x44, 0x48, 0x4C, 0x50, 0x54, 0x58, 0x5C, 0x60, 0x64, 0x68, 0x6C, 0xA0, 0xB8, 0xBC)
            if o + 4 <= len(cfg)}
    return {'id': f'{vid:04x}:{did:04x}', 'bytes': len(cfg), 'sha256': hashlib.sha256(cfg).hexdigest(), 'dwords': regs}


def iomem_overlap():
    rows = []
    for line in open('/proc/iomem'):
        rng, _, name = line.partition(' : ')
        a, b = (int(x, 16) for x in rng.strip().split('-'))
        if b >= WIN_BASE:
            rows.append(line.rstrip())
    return rows


def devmem_read(fd, addr, n):
    return os.pread(fd, n, addr)


def probe(out):
    res = {'utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'cmdline_flags': [],
           'iomem': iomem_overlap(), 'lpc': lpc_snapshot()}
    cmd = open('/proc/cmdline').read()
    res['cmdline_flags'] = [w for w in cmd.split() if w.startswith(('iomem=', 'amd_smn'))]
    os.system('modprobe configs 2>/dev/null')
    if os.path.exists('/proc/config.gz'):
        import gzip
        res['config'] = [l.strip() for l in gzip.open('/proc/config.gz', 'rt') if 'DEVMEM' in l]
    try:
        fd = os.open('/dev/mem', os.O_RDONLY)
        page = devmem_read(fd, 0xFFFFF000, 4096)
        os.close(fd)
        res['test_page'] = {'allowed': True, 'bytes': len(page), 'sha256': hashlib.sha256(page).hexdigest(),
                            'all_ff': page == b'\xff' * len(page), 'reset_vector_first_byte': f'0x{page[0xFF0]:02X}'}
    except OSError as e:
        res['test_page'] = {'allowed': False, 'errno': e.errno, 'error': str(e),
                            'next': 'boot an entry with iomem=relaxed (IO_STRICT_DEVMEM refuses a busy range)'}
    json.dump(res, open(os.path.join(out, 'l35-probe.json'), 'w'), indent=1)
    print(json.dumps(res, indent=1))
    return 0 if res['test_page'].get('allowed') else 4


def full_read(fd):
    buf = bytearray()
    t0 = time.monotonic()
    for off in range(0, WIN_SIZE, 0x10000):
        chunk = devmem_read(fd, WIN_BASE + off, 0x10000)
        if len(chunk) != 0x10000:
            raise OSError(f'short read at 0x{off:x}: {len(chunk)}')
        buf += chunk
    return bytes(buf), time.monotonic() - t0


def read(out):
    before = lpc_snapshot()
    probe_snap = json.load(open(os.path.join(out, 'l35-probe.json')))['lpc'] if os.path.exists(os.path.join(out, 'l35-probe.json')) else None
    fd = os.open('/dev/mem', os.O_RDONLY)
    img1, t1 = full_read(fd)
    open(os.path.join(out, 'spi-read1.bin'), 'wb').write(img1)
    img2, t2 = full_read(fd)
    open(os.path.join(out, 'spi-read2.bin'), 'wb').write(img2)
    os.close(fd)
    after = lpc_snapshot()
    diff_pages = [i for i in range(0, WIN_SIZE, 4096) if img1[i:i + 4096] != img2[i:i + 4096]]
    efs = {f'0x{o:06X}': img1[o:o + 4] == b'\xaa\x55\xaa\x55' for o in EFS_OFFSETS}
    res = {
        'utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
        'window': f'0x{WIN_BASE:08X}+0x{WIN_SIZE:X}',
        'read1': {'seconds': round(t1, 2), 'sha256': hashlib.sha256(img1).hexdigest()},
        'read2': {'seconds': round(t2, 2), 'sha256': hashlib.sha256(img2).hexdigest()},
        'identical': img1 == img2, 'differing_4k_pages': len(diff_pages),
        'first_differing_page': f'0x{diff_pages[0]:06X}' if diff_pages else None,
        'all_ff_4k_pages': sum(1 for i in range(0, WIN_SIZE, 4096) if img1[i:i + 4096] == b'\xff' * 4096),
        'halves_equal_8MiB_alias': img1[:WIN_SIZE // 2] == img1[WIN_SIZE // 2:],
        'quarters_equal_4MiB_alias': img1[WIN_SIZE // 2:WIN_SIZE * 3 // 4] == img1[WIN_SIZE * 3 // 4:],
        'efs_signature_55AA55AA': efs,
        'fvh_signatures': img1.count(b'_FVH'),
        'reset_vector_first_byte': f'0x{img1[0xFFFFF0]:02X}',
        'lpc_unchanged_during_reads': before == after,
        'lpc_unchanged_since_probe': (probe_snap == after) if probe_snap else None,
    }
    json.dump(res, open(os.path.join(out, 'l35-read.json'), 'w'), indent=1)
    print(json.dumps(res, indent=1))
    ok = res['identical'] and any(efs.values()) and res['lpc_unchanged_during_reads']
    return 0 if ok else 5


if __name__ == '__main__':
    if len(sys.argv) != 3 or sys.argv[1] not in ('probe', 'read'):
        sys.exit(__doc__)
    os.makedirs(sys.argv[2], exist_ok=True)
    sys.exit(probe(sys.argv[2]) if sys.argv[1] == 'probe' else read(sys.argv[2]))
