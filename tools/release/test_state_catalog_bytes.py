"""The byte-level state catalog check of a staged payload (BD-322): a router that compiles another catalog than
amdgpu-state.bin is a finding, with and without the release gates, and so is a state reader without any catalog."""
import contextlib
import io
import pathlib
import tempfile
import unittest
from state_catalog_bytes import check, ids_in, known_ids, main

CAT = 'a' * 63 + '1'          # the catalog of the package
OLD = 'b' * 63 + '2'          # an older catalog of the history
ALIEN = 'c' * 63 + '3'        # a 64-digit hex string that is no catalog (a hash in a binary)


def pe(*ids, wide=False):
    """Bytes of a fake binary that carry the given ids between other bytes."""
    body = b'MZ\x90\x00' + b'\x00' * 32
    for i in ids:
        body += (i.encode('utf-16-le') if wide else i.encode('ascii')) + b'\x00padding\x00'
    return body


class PayloadCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.payload = pathlib.Path(self.tmp.name) / 'files' / 'payload'
        self.files = {
            'kmd/amdgpu-state.bin': b'STATE' + CAT.encode('ascii') + b'\x00',
            'kmd/bc250kmd.sys': pe(CAT, ALIEN),
            'kmd/desktop/bc250d3d_router.dll': pe(CAT),
            'kmd/wow64/desktop/bc250d3d_router32.dll': pe(CAT, wide=True),
            'control/bc250control.dll': pe(CAT),
            'tools/bc250control.dll': pe(CAT),
            'tools/bc250kmd_cli.exe': pe(CAT),
            'tools/amdgpu-state-installer.exe': pe(OLD, CAT),
            'tools/other.exe': pe(ALIEN),
        }

    def tearDown(self):
        self.tmp.cleanup()

    def stage(self, **changes):
        files = dict(self.files)
        for key, value in changes.items():
            files[key.replace('__', '/').replace('_dot_', '.')] = value
        for rel, data in files.items():
            if data is None:
                continue
            path = self.payload / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        return check(self.payload, {CAT, OLD})

    def test_a_consistent_payload_passes(self):
        catalog, findings, lines = self.stage()
        self.assertEqual(catalog, CAT)
        self.assertEqual(findings, [])
        self.assertIn('cat payload/kmd/wow64/desktop/bc250d3d_router32.dll', lines)   # the UTF-16 form
        self.assertTrue(any(l.startswith('cat payload/tools/amdgpu-state-installer.exe (also') for l in lines))

    def test_a_router_of_another_catalog_is_a_finding(self):
        # kit-cap-22631-r1c: the routers came from the registered set, the KMD and the store from the cap head.
        catalog, findings, _ = self.stage(**{'kmd/desktop/bc250d3d_router.dll': pe(OLD)})
        self.assertEqual(len(findings), 1)
        self.assertIn('payload/kmd/desktop/bc250d3d_router.dll compiles the catalog', findings[0])

    def test_a_reader_without_any_catalog_is_a_finding(self):
        _, findings, _ = self.stage(**{'tools/bc250kmd_cli.exe': pe(ALIEN)})
        self.assertEqual(findings, ['payload/tools/bc250kmd_cli.exe is a state reader and carries no catalog id'])

    def test_a_store_with_two_ids_or_none_is_refused(self):
        _, findings, _ = self.stage(**{'kmd/amdgpu-state.bin': CAT.encode() + b' ' + OLD.encode()})
        self.assertIn('holds 2 catalog ids', findings[0])
        self.tmp.cleanup(); self.setUp()
        _, findings, _ = self.stage(**{'kmd/amdgpu-state.bin': b'no id here'})
        self.assertIn('holds 0 catalog ids', findings[0])

    def test_a_store_catalog_outside_the_history_is_a_finding(self):
        catalog, findings, _ = self.stage(**{'kmd/amdgpu-state.bin': ALIEN.encode()})
        self.assertEqual(catalog, ALIEN)
        self.assertIn('is not in the catalog history of the build tree', findings[0])

    def test_a_non_reader_with_an_unknown_hex_string_is_ignored(self):
        _, findings, lines = self.stage()
        self.assertFalse(any('other.exe' in l for l in lines))
        self.assertEqual(findings, [])


class HelperTests(unittest.TestCase):
    def test_ids_in_reads_ascii_and_utf16_and_not_longer_runs(self):
        self.assertEqual(ids_in(pe(CAT)), {CAT})
        self.assertEqual(ids_in(pe(CAT, wide=True)), {CAT})
        self.assertEqual(ids_in(b'x' + (CAT + 'f').encode()), set())

    def test_known_ids_needs_both_headers(self):
        with tempfile.TemporaryDirectory() as t:
            repo = pathlib.Path(t)
            (repo / 'driver' / 'state').mkdir(parents=True)
            (repo / 'driver' / 'state' / 'catalog.generated.h').write_text(f'#define X "{CAT}"\n')
            with self.assertRaises(FileNotFoundError):
                known_ids(repo)
            (repo / 'driver' / 'state' / 'catalog_history.generated.h').write_text(f'{{"{OLD}", 1u, H0}},\n')
            self.assertEqual(known_ids(repo), {CAT, OLD})

    def test_main_exits_1_on_a_finding(self):
        with tempfile.TemporaryDirectory() as t:
            root = pathlib.Path(t)
            (root / 'driver' / 'state').mkdir(parents=True)
            (root / 'driver' / 'state' / 'catalog.generated.h').write_text(f'"{CAT}"\n')
            (root / 'driver' / 'state' / 'catalog_history.generated.h').write_text(f'"{OLD}"\n')
            payload = root / 'files' / 'payload'
            (payload / 'kmd' / 'desktop').mkdir(parents=True)
            (payload / 'kmd' / 'amdgpu-state.bin').write_bytes(CAT.encode())
            (payload / 'kmd' / 'desktop' / 'bc250d3d_router.dll').write_bytes(pe(OLD))
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                rc = main(['--payload', str(payload), '--repo', str(root)])
            self.assertEqual(rc, 1)
            self.assertIn('state catalog bytes: FAIL, 1 finding(s)', out.getvalue())


if __name__ == '__main__':
    unittest.main()
