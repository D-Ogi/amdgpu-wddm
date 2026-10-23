"""Positive and malformed/unsafe byte controls; no image is executed."""
import contextlib
import io
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

import kmd_pe


PE = 128
OP = PE + 24
OPTIONAL_SIZE = 240
TABLE = OP + OPTIONAL_SIZE
IAT = OP + 112 + 12 * 8


def put(raw, offset, fmt, *values):
    struct.pack_into('<' + fmt, raw, offset, *values)


def fixture():
    raw = bytearray(1536)
    raw[:2] = b'MZ'
    put(raw, 60, 'I', PE)
    raw[PE:PE + 4] = b'PE\0\0'
    put(raw, PE + 4, 'HH', 0x8664, 2)
    put(raw, PE + 20, 'H', OPTIONAL_SIZE)
    put(raw, OP, 'H', 0x20b)
    put(raw, OP + 32, 'II', 4096, 512)
    put(raw, OP + 56, 'II', 12288, 512)
    put(raw, OP + 68, 'HH', 1, 0x100)
    put(raw, OP + 108, 'I', 16)
    put(raw, IAT, 'II', 8192, 16)
    for index, name, rva, file_at, flags in [
        (0, b'.text', 4096, 512, 0x60000020),
        (1, b'.rdata', 8192, 1024, 0x40000040),
    ]:
        at = TABLE + index * 40
        raw[at:at + len(name)] = name
        put(raw, at + 8, 'IIII', 512, rva, 512, file_at)
        put(raw, at + 36, 'I', flags)
    return raw


class LayoutTests(unittest.TestCase):
    def test_positive(self):
        result = kmd_pe.inspect(fixture())
        self.assertEqual(result['iat_rva'], 8192)
        self.assertEqual(len(result['sections']), 2)

    def test_every_truncated_header_fails(self):
        raw = fixture()
        for size in range(TABLE + 80):
            with self.subTest(size=size), self.assertRaises(ValueError):
                kmd_pe.inspect(raw[:size])

    def test_mutations(self):
        cases = [
            ('PE offset', 60, 'I', (0xffffffff,)),
            ('wrong machine', PE + 4, 'H', (0x14c,)),
            ('no sections', PE + 6, 'H', (0,)),
            ('too many sections', PE + 6, 'H', (97,)),
            ('short optional header', PE + 20, 'H', (100,)),
            ('PE32', OP, 'H', (0x10b,)),
            ('non-native', OP + 68, 'H', (3,)),
            ('NX absent', OP + 70, 'H', (0,)),
            ('sub-page alignment', OP + 32, 'I', (512,)),
            ('non-power alignment', OP + 32, 'I', (12288,)),
            ('bad file alignment', OP + 36, 'I', (513,)),
            ('small image', OP + 56, 'I', (8192,)),
            ('unaligned image', OP + 56, 'I', (12289,)),
            ('small headers', OP + 60, 'I', (256,)),
            ('headers beyond file', OP + 60, 'I', (4096,)),
            ('missing directories', OP + 108, 'I', (12,)),
            ('truncated directories', OP + 108, 'I', (17,)),
            ('unaligned RVA', TABLE + 12, 'I', (4097,)),
            ('RVA in header', TABLE + 12, 'I', (0,)),
            ('empty section', TABLE + 8, 'III', (0, 4096, 0)),
            ('overflow extent', TABLE + 8, 'I', (0xffffffff,)),
            ('raw overlaps headers', TABLE + 20, 'I', (0,)),
            ('raw outside file', TABLE + 20, 'I', (1536,)),
            ('unaligned raw', TABLE + 20, 'I', (513,)),
            ('unaligned raw size', TABLE + 16, 'I', (513,)),
            ('WX', TABLE + 36, 'I', (0xe0000020,)),
            ('raw overlaps section', TABLE + 40 + 20, 'I', (512,)),
            ('mapped overlaps section', TABLE + 40 + 12, 'I', (4096,)),
            ('page overlap', TABLE + 8, 'I', (4097,)),
            ('IAT absent', IAT, 'II', (0, 0)),
            ('IAT code', IAT, 'I', (4096,)),
            ('IAT crosses section', IAT, 'II', (8696, 16)),
            ('IAT overflow', IAT, 'II', (8192, 0xfffffff8)),
            ('IAT misaligned', IAT, 'I', (8193,)),
        ]
        for name, offset, fmt, values in cases:
            with self.subTest(name=name):
                raw = fixture()
                put(raw, offset, fmt, *values)
                with self.assertRaises(ValueError):
                    kmd_pe.inspect(raw)

    def test_iat_in_uninitialized_tail_fails(self):
        raw = fixture()
        put(raw, TABLE + 40 + 8, 'I', 1024)
        put(raw, IAT, 'I', 8704)
        with self.assertRaisesRegex(ValueError, 'file-backed'):
            kmd_pe.inspect(raw)

    def test_bss_and_signed_overlay_allowed(self):
        raw = fixture()
        put(raw, PE + 6, 'H', 3)
        # Exercise expanded headers and shift the file data to match.
        raw[512:512] = bytes(512)
        put(raw, OP + 60, 'I', 1024)
        put(raw, TABLE + 20, 'I', 1024)
        put(raw, TABLE + 40 + 20, 'I', 1536)
        put(raw, OP + 56, 'I', 16384)
        put(raw, TABLE + 80 + 8, 'IIII', 4096, 12288, 0, 0)
        put(raw, TABLE + 80 + 36, 'I', 0xc0000080)
        raw.extend(b'certificate overlay')
        self.assertEqual(len(kmd_pe.inspect(raw)['sections']), 3)

    def test_cli_fails_closed_and_writes_receipt(self):
        with tempfile.TemporaryDirectory() as tmp, contextlib.redirect_stdout(io.StringIO()):
            image = Path(tmp) / 'test.sys'
            receipt = Path(tmp) / 'receipt.json'
            args = [str(image), '--out', str(receipt)]
            self.assertEqual(kmd_pe.main(args), 1)  # Missing input.
            image.write_bytes(fixture())
            self.assertEqual(kmd_pe.main(args), 0)
            result = json.loads(receipt.read_text())
            self.assertEqual(result['status'], 'pass')
            self.assertFalse(result['hvci_acceptance'])
            self.assertEqual(len(result['sha256']), 64)
            image.write_bytes(b'bad PE')
            self.assertEqual(kmd_pe.main(args), 1)
            self.assertEqual(json.loads(receipt.read_text())['status'], 'fail')
            image.write_bytes(fixture())
            self.assertEqual(kmd_pe.main([str(image), '--out', str(Path(tmp) / 'missing' / 'r.json')]), 1)

    @unittest.skipUnless(os.name == 'nt', 'Windows build hosts')
    def test_real_build_gate_stops_before_packaging(self):
        root = Path(__file__).resolve().parents[2]
        source = (root / 'driver/kmd/build.ps1').read_text(encoding='utf-8-sig')
        start = source.index('# Static HVCI layout requirements;')
        end = source.index('if($CompileOnly)', start)
        self.assertLess(source.index("Write-Host 'link'"), start)
        self.assertLess(end, source.index("Write-Host 'catalog'"))
        block = source[start:end]  # Execute the production code, not a copy.

        def quote(value):
            return "'" + str(value).replace("'", "''") + "'"

        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            script = base / 'gate.ps1'
            script.write_text("$ErrorActionPreference='Stop'\n"
                              + '$repo=' + quote(root) + '\n'
                              + '$Out=' + quote(base) + '\n$pkg=$Out\n'
                              + block + "Write-Output 'REACHED_PACKAGING'\n", encoding='utf-8-sig')
            for name in ('powershell.exe', 'pwsh.exe'):
                host = shutil.which(name)
                self.assertIsNotNone(host, name + ' is required on Windows')
                for valid in (True, False):
                    with self.subTest(host=name, valid=valid):
                        raw = fixture()
                        if not valid:
                            put(raw, TABLE + 36, 'I', 0xe0000020)
                        (base / 'bc250kmd.sys').write_bytes(raw)
                        run = subprocess.run([host, '-NoProfile', '-NonInteractive',
                                              '-ExecutionPolicy', 'Bypass', '-File', str(script)],
                                             capture_output=True, timeout=30,
                                             creationflags=subprocess.CREATE_NO_WINDOW)
                        self.assertEqual(run.returncode == 0, valid, run.stdout + run.stderr)
                        self.assertEqual(b'REACHED_PACKAGING' in run.stdout, valid)
                        receipt = json.loads((base / 'kmd-pe.json').read_text())
                        self.assertEqual(receipt['status'], 'pass' if valid else 'fail')


if __name__ == '__main__':
    unittest.main()
