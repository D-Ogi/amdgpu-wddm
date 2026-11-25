"""Self-test of the BD-389 runtime import gate (runtime_import_gate.py) and its PE reader (pe_image.py).

    python -m unittest discover -s tools/quality -p test_runtime_import_gate.py

The images are built here, byte by byte: an import directory and a delay-import directory with the DLL names a case
needs, in PE32+ (x64) and PE32 (x86), and a delay descriptor in the RVA form and in the old virtual-address form.
The positive controls read real Windows images. With BC250_ROOT set, one control reads the signed driver set of
tester.39 rc3 package attempt 6, which has the defect: the gate must refuse exactly its three desktop files.
"""
import contextlib
import io
import json
import os
import pathlib
import struct
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
import pe_image  # noqa: E402
import runtime_import_gate as gate  # noqa: E402

SECTION_RVA, SECTION_RAW, SECTION_SIZE = 0x1000, 0x200, 0x1000


def image(imports=('KERNEL32.dll',), delay=(), wide=True, delay_va=False):
    """A PE image whose import directory names `imports` and whose delay-import directory names `delay`."""
    base = 0x180000000 if wide else 0x10000000
    step = 8 if wide else 4
    sec = bytearray(SECTION_SIZE)
    names_at = 0x800

    def put_name(text, hint=False):
        nonlocal names_at
        rva = SECTION_RVA + names_at
        raw = (b'\0\0' if hint else b'') + text.encode('ascii') + b'\0'
        sec[names_at:names_at + len(raw)] = raw
        names_at += len(raw) + (len(raw) & 1)
        return rva

    thunks_at = 0x400
    for i, dll in enumerate(imports):
        fn = put_name('ExitProcess', hint=True)
        thunk = SECTION_RVA + thunks_at
        struct.pack_into('<Q' if wide else '<I', sec, thunks_at, fn)
        thunks_at += 2 * step
        struct.pack_into('<IIIII', sec, 20 * i, thunk, 0, 0, put_name(dll), thunk)
    for i, dll in enumerate(delay):
        name = put_name(dll)
        iat = SECTION_RVA + thunks_at
        thunks_at += 2 * step
        fields = (0, name + base, 0, iat + base, iat + base, 0, 0, 0) if delay_va else (1, name, 0, iat, iat, 0, 0, 0)
        struct.pack_into('<8I', sec, 0x200 + 32 * i, *fields)
    optsize = 240 if wide else 224
    head = bytearray(SECTION_RAW)
    head[0:2] = b'MZ'
    struct.pack_into('<I', head, 0x3c, 0x40)
    head[0x40:0x44] = b'PE\0\0'
    struct.pack_into('<HHIIIHH', head, 0x44, 0x8664 if wide else 0x14c, 1, 0, 0, 0, optsize, 0x2022)
    opt = 0x58
    struct.pack_into('<H', head, opt, 0x20b if wide else 0x10b)
    if wide:
        struct.pack_into('<Q', head, opt + 24, base)
    else:
        struct.pack_into('<I', head, opt + 28, base)
    dirs = opt + (112 if wide else 96)
    struct.pack_into('<I', head, opt + (108 if wide else 92), 16)
    if imports:
        struct.pack_into('<II', head, dirs + 8, SECTION_RVA, 20 * (len(imports) + 1))
    if delay:
        struct.pack_into('<II', head, dirs + 8 * 13, SECTION_RVA + 0x200, 32 * (len(delay) + 1))
    s = opt + optsize
    head[s:s + 8] = b'.idata\0\0'
    struct.pack_into('<IIII', head, s + 8, SECTION_SIZE, SECTION_RVA, SECTION_SIZE, SECTION_RAW)
    return bytes(head + sec)


class Reader(unittest.TestCase):
    def test_both_tables_in_both_formats(self):
        # The virtual-address form of a delay descriptor exists only in PE32 images.
        for wide, delay_va in ((True, False), (False, False), (False, True)):
            with self.subTest(wide=wide, delay_va=delay_va):
                data = image(('KERNEL32.dll', 'USER32.dll'), ('dwmapi.dll',), wide=wide, delay_va=delay_va)
                self.assertEqual(pe_image.import_dlls(data),
                                 {'import': ['KERNEL32.dll', 'USER32.dll'], 'delay': ['dwmapi.dll']})
                self.assertEqual(pe_image.architecture(data), 'x64' if wide else 'x86')

    def test_an_image_it_cannot_read_fails_closed(self):
        with self.assertRaises(pe_image.PeError):
            pe_image.import_dlls(b'MZ' + b'\0' * 0x100)
        broken = bytearray(image())
        struct.pack_into('<I', broken, 0x58 + 112 + 8, 0x9000)
        with self.assertRaises(pe_image.PeError):
            pe_image.import_dlls(bytes(broken))
        # A table that runs to the end of the image without its terminating descriptor.
        endless = bytearray(image())
        struct.pack_into('<IIIII', endless, SECTION_RAW + SECTION_SIZE - 20, 1, 0, 0, SECTION_RVA + 0x800, 1)
        struct.pack_into('<I', endless, 0x58 + 112 + 8, SECTION_RVA + SECTION_SIZE - 20)
        with self.assertRaises(pe_image.PeError):
            pe_image.import_dlls(bytes(endless))

    def test_windows_images(self):
        # Positive control: real images of this Windows, with imports and with delay imports.
        system = pathlib.Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32'
        kernel32 = system / 'kernel32.dll'
        if not kernel32.is_file():
            self.skipTest('no Windows system directory')
        tables = pe_image.import_dlls(pe_image.read(kernel32))
        self.assertGreater(len(tables['import']), 10)
        self.assertEqual(gate.judge(pe_image.read(kernel32))['findings'], [])
        delayed = [p for p in (system / 'shell32.dll', system / 'user32.dll', system / 'comctl32.dll') if p.is_file()]
        self.assertTrue(any(pe_image.import_dlls(pe_image.read(p))['delay'] for p in delayed))


class Rule(unittest.TestCase):
    REFUSED = ('VCRUNTIME140.dll', 'vcruntime140_1.dll', 'vcruntime140d.dll', 'VCRUNTIME140_THREADS.dll',
               'MSVCP140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll', 'msvcp140_atomic_wait.dll',
               'msvcp140_codecvt_ids.dll', 'msvcp140d.dll', 'MSVCP120.dll', 'MSVCR120.dll', 'msvcr100.dll',
               'msvcr90.dll', 'concrt140.dll', 'vccorlib140.dll', 'VCOMP140.DLL', 'vcomp140d.dll', 'vcamp140.dll',
               'mfc140u.dll', 'mfcm140u.dll', 'mfcmifc80.dll', 'ucrtbased.dll')
    ALLOWED = ('ucrtbase.dll', 'UCRTBASE.DLL', 'api-ms-win-crt-runtime-l1-1-0.dll', 'api-ms-win-crt-heap-l1-1-0.dll')
    OUTSIDE = ('KERNEL32.dll', 'msvcrt.dll', 'msvcp_win.dll', 'msvcp60.dll', 'mfc42.dll', 'mfc42u.dll',
               'api-ms-win-core-synch-l1-2-0.dll', 'vcruntime.txt')

    def test_each_runtime_family_is_refused_in_both_tables(self):
        for dll in self.REFUSED:
            for table in ('import', 'delay'):
                with self.subTest(dll=dll, table=table):
                    data = image((dll,) if table == 'import' else ('KERNEL32.dll',),
                                 (dll,) if table == 'delay' else ())
                    findings = gate.judge(data)['findings']
                    self.assertEqual(len(findings), 1, findings)
                    self.assertIn(dll, findings[0])
                    self.assertTrue(findings[0].startswith('delay-imports' if table == 'delay' else 'imports'))

    def test_the_universal_crt_and_windows_dlls_pass(self):
        for dll in self.ALLOWED:
            with self.subTest(dll=dll):
                record = gate.judge(image(('KERNEL32.dll', dll)))
                self.assertEqual(record['findings'], [])
                self.assertEqual(record['ucrt'], [dll])
        for dll in self.OUTSIDE:
            with self.subTest(dll=dll):
                record = gate.judge(image(('KERNEL32.dll', dll)))
                self.assertEqual((record['findings'], record['ucrt']), ([], []))

    def test_an_x86_image_is_judged_too(self):
        self.assertEqual(len(gate.judge(image(('MSVCP140.dll',), wide=False))['findings']), 1)


class Command(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = pathlib.Path(self.tmp.name)

    def write(self, rel, data):
        p = self.dir / 'pkg' / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
        return p

    def run_gate(self, *args):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = gate.main([str(a) for a in args])
        return code, out.getvalue()

    def clean_package(self):
        self.write('setup.exe', image(('KERNEL32.dll', 'mscoree.dll')))
        self.write('files/payload/kmd/desktop/bc250d3d.dll', image(('KERNEL32.dll', 'ADVAPI32.dll')))
        self.write('files/payload/kmd/wow64/desktop/bc250d3d32.dll', image(('KERNEL32.dll',), wide=False))
        self.write('files/payload/kmd/bc250kmd.sys', image(('ntoskrnl.exe',)))
        self.write('files/payload/kmd/bc250kmd.inf', b'[Version]\r\n')

    def test_a_clean_package_passes_with_the_negative_control(self):
        self.clean_package()
        code, text = self.run_gate('--scan', self.dir / 'pkg', '--negative-control',
                                   'files/payload/kmd/desktop/bc250d3d.dll', '--out', self.dir / 'out')
        self.assertEqual(code, 0, text)
        self.assertIn('runtime import gate: pass, 4 PE file(s), 0 refused', text)
        report = json.loads((self.dir / 'out' / 'runtime-import-gate.json').read_text(encoding='utf-8'))
        self.assertEqual(report['negative_control'], {'path': 'files/payload/kmd/desktop/bc250d3d.dll',
                                                      'rewritten': 'KERNEL32.dll', 'to': 'MSVCP140.dll',
                                                      'refused': True})
        self.assertEqual(len(report['files']), 4)

    def test_the_bd389_set_fails_and_names_each_file(self):
        self.clean_package()
        self.write('files/payload/kmd/desktop/bc250d3d_router.dll', image(('KERNEL32.dll', 'VCRUNTIME140.dll')))
        self.write('files/payload/kmd/desktop/bc250d3d_zink.dll',
                   image(('KERNEL32.dll', 'MSVCP140.dll', 'VCRUNTIME140.dll', 'VCRUNTIME140_1.dll')))
        code, text = self.run_gate('--scan', self.dir / 'pkg')
        self.assertEqual(code, 1, text)
        refused = sorted({line.split(':')[0] for line in text.splitlines() if line.startswith('REFUSE ')})
        self.assertEqual(refused, ['REFUSE files/payload/kmd/desktop/bc250d3d_router.dll',
                                   'REFUSE files/payload/kmd/desktop/bc250d3d_zink.dll'])
        self.assertIn('FAIL, 6 PE file(s), 2 refused', text)

    def test_a_negative_control_that_is_not_refused_fails_the_set(self):
        self.clean_package()
        rule = gate.REFUSED
        gate.REFUSED = ()
        try:
            code, text = self.run_gate('--scan', self.dir / 'pkg', '--negative-control',
                                       'files/payload/kmd/desktop/bc250d3d.dll')
        finally:
            gate.REFUSED = rule
        self.assertEqual(code, 1, text)
        self.assertIn('REFUSE negative control files/payload/kmd/desktop/bc250d3d.dll', text)

    def test_a_renamed_pe_is_scanned_and_a_data_file_that_starts_with_mz_is_not(self):
        # build-release.ps1 runs the gate while the firmware images are still in the driver directory.
        self.clean_package()
        self.write('files/payload/kmd/gc_firmware.bin', b'MZ' + bytes(range(256)) * 16)
        self.write('files/payload/kmd/renamed.bin', image(('KERNEL32.dll', 'VCRUNTIME140.dll')))
        code, text = self.run_gate('--scan', self.dir / 'pkg')
        self.assertEqual(code, 1, text)
        self.assertIn('REFUSE files/payload/kmd/renamed.bin: imports VCRUNTIME140.dll', text)
        self.assertIn('FAIL, 5 PE file(s), 1 refused', text)

    def test_input_it_cannot_judge_fails_closed(self):
        cases = {
            'no PE file': lambda: self.write('readme.txt', b'text'),
            'a .dll that is not a PE image': lambda: self.write('bad.dll', b'MZ' + b'\0' * 0x80),
            'an unreadable import table': lambda: self.write('cut.dll', image()[:0x300]),
        }
        for name, make in cases.items():
            with self.subTest(name):
                for p in sorted((self.dir / 'pkg').rglob('*'), reverse=True) if (self.dir / 'pkg').exists() else ():
                    p.unlink() if p.is_file() else p.rmdir()
                make()
                code, text = self.run_gate('--scan', self.dir / 'pkg')
                self.assertEqual(code, 2, text)
                self.assertIn('CANNOT JUDGE', text)
        self.clean_package()
        for control in ('files/payload/kmd/bc250kmd.inf', 'files/payload/kmd/missing.dll'):
            with self.subTest(control=control):
                self.assertEqual(self.run_gate('--scan', self.dir / 'pkg', '--negative-control', control)[0], 2)
        self.write('files/short.dll', image(('a.dll',)))
        self.assertEqual(self.run_gate('--scan', self.dir / 'pkg', '--negative-control', 'files/short.dll')[0], 2)
        self.assertEqual(self.run_gate('--scan', self.dir / 'nothing')[0], 2)

    def test_the_signed_rc3_driver_set_is_refused_in_exactly_three_files(self):
        root = os.environ.get('BC250_ROOT')
        tree = pathlib.Path(root or '.') / 'scratch/glm/efi-signing/cks-manual/run6-staged/files/payload/kmd'
        if not root or not tree.is_dir():
            self.skipTest('BC250_ROOT does not hold the rc3 attempt 6 driver set')
        code, text = self.run_gate('--scan', tree, '--negative-control', 'desktop/bc250d3d.dll')
        self.assertEqual(code, 1, text)
        refused = sorted({line.split(':')[0][len('REFUSE '):] for line in text.splitlines() if line.startswith('REFUSE ')})
        self.assertEqual(refused, ['desktop/amdgpu_wddm_radv.dll', 'desktop/bc250d3d_router.dll',
                                   'desktop/bc250d3d_zink.dll'])
        self.assertIn('negative control refused (desktop/bc250d3d.dll)', text)

    def test_build_release_runs_the_gate_on_the_package(self):
        text = (REPO / 'tools' / 'release' / 'build-release.ps1').read_text(encoding='utf-8')
        self.assertEqual(text.count("'tools\\quality\\runtime_import_gate.py'"), 1)
        run = text.index("'tools\\quality\\runtime_import_gate.py'")
        self.assertLess(text.index("Invoke-FlavourGate 'flavour-gate-payload' @()"), run)
        self.assertLess(run, text.index('if ($SubmissionCab) {'))
        call = text[run:text.index('\n', text.index('-TimeoutSeconds', run))]
        self.assertIn("'--scan', $pkgRoot", call)
        self.assertIn("'--negative-control', 'files/payload/kmd/desktop/bc250d3d.dll'", call)


if __name__ == '__main__':
    unittest.main()
