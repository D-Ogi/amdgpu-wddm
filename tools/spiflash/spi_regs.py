"""Read the registers, SFDP, protection bits and secured OTP of the SPI flash through a CH341A.

Read-class opcodes only (ch341a.READ_OPCODES). Each check gates the next:
1. RDID = C2 20 18 (MX25L12873F).
2. READ at 0x000000 and 0x900000 equals a programmer-made backup (positive control of bit order and CS).
3. Status, configuration and security registers; the write enable latch must be 0.
4. SFDP (512 bytes, all parameter headers), REMS, RES, lock, SPB lock, fast boot and password registers,
   SPB and DPB per 4 KiB step.
5. ENSO, READ of the OTP window, EXSO (also in a finally block), then READ at 0 equals the backup again.
6. The registers read in step 3 and 4 are unchanged.

An all-FF or all-00 reply can be the idle level of MISO and not a register value: run miso_float_check.py
without and with a pull-up on MISO and compare (README.md, "Undriven MISO").

Usage: python -I tools/spiflash/spi_regs.py <backup image> <new output dir> [--libusb DLL]
"""
import argparse
import hashlib
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))  # -I drops the script directory from sys.path
import ch341a  # noqa: E402

EXPECTED_ID = bytes([0xC2, 0x20, 0x18])
SECTOR = 0x1000
CHIP_SIZE = 0x1000000
OTP_WINDOW = 1024  # over-read: the secured OTP is 4 Kbit (512 bytes); a wrap shows as a repeat


class StepFailed(Exception):
    pass


def le(b):
    return int.from_bytes(b, 'little')  # multi-byte registers shift bits 7-0 first


def run(dev, backup, out, rep):
    def step(name, ok, **kw):
        rep['steps'].append({'step': name, 'ok': ok, **kw})
        print(f"{'OK  ' if ok else 'FAIL'} {name} {kw}")
        if not ok:
            raise StepFailed(name)

    rdid = dev.xfer([0x9F], 3)
    step('RDID', rdid == EXPECTED_ID, value=rdid.hex())
    for addr in (0x000000, 0x900000):
        got = dev.read(addr, 4096)
        step(f'READ 0x{addr:06X} equals backup', got == backup[addr:addr + 4096],
             sha256=hashlib.sha256(got).hexdigest())

    sr, cr, scur = dev.xfer([0x05], 1)[0], dev.xfer([0x15], 1)[0], dev.xfer([0x2B], 1)[0]
    rep['before'] = {'status_register': f'0x{sr:02X}', 'config_register': f'0x{cr:02X}',
                     'security_register': f'0x{scur:02X}'}
    print('registers', rep['before'])
    step('write enable latch is 0', (sr & 0x02) == 0, status_register=f'0x{sr:02X}')

    sfdp = dev.xfer([0x5A, 0, 0, 0, 0xFF], 512)
    (out / 'sfdp.bin').write_bytes(sfdp)
    step('SFDP signature', sfdp[:4] == b'SFDP', head=sfdp[:16].hex())
    rep['sfdp_headers'] = []
    for k in range(sfdp[6] + 1):  # NPH is zero-based
        h = sfdp[8 + 8 * k:16 + 8 * k]
        rep['sfdp_headers'].append({'id': f'0x{h[7]:02X}{h[0]:02X}', 'rev': f'{h[2]}.{h[1]}', 'dwords': h[3],
                                    'pointer': f'0x{h[4] | h[5] << 8 | h[6] << 16:06X}'})

    ext = {
        'REMS': dev.xfer([0x90, 0, 0, 0], 2).hex(),
        'RES_device_id': dev.xfer([0xAB, 0, 0, 0], 1).hex(),
        'lock_register': f'0x{le(dev.xfer([0x2D], 2)):04X}',
        'spb_lock_register': dev.xfer([0xA7], 2).hex(),
        'fast_boot_register': f'0x{le(dev.xfer([0x16], 4)):08X}',
        'password_register': f'0x{le(dev.xfer([0x27], 8)):016X}',
    }
    rep['extra_registers'] = ext
    print('extra registers', ext)
    spb, dpb = bytearray(), bytearray()
    for a in range(0, CHIP_SIZE, SECTOR):
        ad = [(a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF, 0xFF]  # 3-byte address + 1 dummy (not proven)
        spb += dev.xfer([0xE2] + ad, 1)
        dpb += dev.xfer([0xE0] + ad, 1)
    (out / 'spb-per-4k.bin').write_bytes(spb)
    (out / 'dpb-per-4k.bin').write_bytes(dpb)
    rep['spb_values'] = {f'0x{v:02X}': spb.count(v) for v in sorted(set(spb))}
    rep['dpb_values'] = {f'0x{v:02X}': dpb.count(v) for v in sorted(set(dpb))}
    print('spb', rep['spb_values'], 'dpb', rep['dpb_values'])
    got = dev.read(0x900000, 4096)
    step('READ 0x900000 after register sweep equals backup', got == backup[0x900000:0x901000])

    dev.xfer([0xB1], 0)  # ENSO
    rep['otp_mode'] = True
    otp = dev.read(0, OTP_WINDOW)
    dev.xfer([0xC1], 0)  # EXSO
    rep['otp_mode'] = False
    (out / 'otp-window.bin').write_bytes(otp)
    rep['otp'] = {'bytes_read': len(otp), 'sha256': hashlib.sha256(otp).hexdigest(),
                  'esn_0_15': otp[:16].hex(),
                  'first_512_all_ff': otp[:512] == b'\xff' * 512,
                  'second_half_repeats_first': otp[512:1024] == otp[:512],
                  'distinct_byte_values_first_512': len(set(otp[:512])),
                  'backup_first_1024_ff_bytes': backup[:1024].count(0xFF)}
    print('otp', rep['otp'])

    got = dev.read(0, 4096)
    step('READ 0x000000 after EXSO equals backup', got == backup[:4096])
    after = {'status_register': f'0x{dev.xfer([0x05], 1)[0]:02X}',
             'security_register': f'0x{dev.xfer([0x2B], 1)[0]:02X}',
             'lock_register': f'0x{le(dev.xfer([0x2D], 2)):04X}',
             'fast_boot_register': f'0x{le(dev.xfer([0x16], 4)):08X}'}
    rep['after'] = after
    same = (after['status_register'] == rep['before']['status_register']
            and after['security_register'] == rep['before']['security_register']
            and after['lock_register'] == ext['lock_register']
            and after['fast_boot_register'] == ext['fast_boot_register'])
    step('registers unchanged', same, **after)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('backup', type=Path)
    ap.add_argument('out', type=Path)
    ap.add_argument('--libusb', type=Path)
    a = ap.parse_args()
    backup = a.backup.read_bytes()
    a.out.mkdir(parents=True, exist_ok=False)
    rep = {'tool': 'tools/spiflash/spi_regs.py',
           'tool_sha256': {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                           for p in (Path(__file__), Path(ch341a.__file__))},
           'backup_sha256': hashlib.sha256(backup).hexdigest(), 'steps': [], 'otp_mode': False}
    dev = ch341a.open_programmer(a.libusb)
    rep['drained_bytes_at_open'] = dev.drained
    try:
        run(dev, backup, a.out, rep)
        rep['verdict'] = 'PASS'
    except StepFailed as e:
        rep['failed_step'] = str(e)
    finally:
        if rep['otp_mode']:
            try:
                dev.xfer([0xC1], 0)
                rep['exso_in_finally'] = True
            except Exception as e:  # a board power cycle also leaves the OTP mode
                rep['exso_in_finally'] = f'failed: {e}'
        dev.close()
        rep.setdefault('verdict', 'FAIL')
        (a.out / 'spi-regs.json').write_text(json.dumps(rep, indent=2) + '\n', encoding='utf-8')
        print('SPI-REGS VERDICT', rep['verdict'])
    return 0 if rep['verdict'] == 'PASS' else 1


if __name__ == '__main__':
    sys.exit(main())
