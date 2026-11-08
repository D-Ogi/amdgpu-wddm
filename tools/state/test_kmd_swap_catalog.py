"""Tests of kmd_swap_catalog.py on synthetic repository trees and images (no real KMD image is needed)."""
import contextlib
import io
import json
import pathlib
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import kmd_swap_catalog as k  # noqa: E402
from generate import identity_of  # noqa: E402


def catalog(*names):
    return {'schema': 2, 'values': [{'path': '', 'name': n, 'kind': 'setting', 'type': 'dword', 'code': 1}
                                    for n in names]}


class Tree:
    """A repository root with driver/state/catalog.json, one history file and the two generated headers."""

    def __init__(self, base):
        self.root = pathlib.Path(base)
        state = self.root / 'driver' / 'state'
        (state / 'history').mkdir(parents=True)
        self.old = catalog('A', 'B')
        self.new = catalog('A', 'B', 'WddmVersionCap')
        self.new['values'][1]['code'] = 2
        self.old_id, self.new_id = identity_of(self.old), identity_of(self.new)
        (state / 'catalog.json').write_text(json.dumps(self.new), encoding='utf-8')
        (state / 'history' / f'{self.old_id}.json').write_text(json.dumps(self.old), encoding='utf-8')
        (state / 'catalog.generated.h').write_text(f'#define AMDGPU_STATE_CATALOG_ID "{self.new_id}"\n')
        (state / 'catalog_history.generated.h').write_text(f'{{"{self.old_id}", 2u, StateHistory0}},\n')

    def image(self, name, *ids, extra=b''):
        path = self.root / name
        body = b'MZ\x90\x00' + b''.join(b'\x00rdata ' + i.encode('ascii') + b'\x00' for i in ids) + extra
        path.write_bytes(body)
        return str(path)


def run(argv):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        code = k.main(argv)
    return code, out.getvalue()


class ImageTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.t = Tree(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_one_known_id_is_the_catalog(self):
        # An unrelated 64-digit hash next to the catalog id is not a catalog (real images carry one).
        sys_path = self.t.image('a.sys', self.t.old_id, 'c' * 64)
        code, out = run(['image', sys_path, '--repo', str(self.t.root)])
        self.assertEqual((code, out.strip()), (0, self.t.old_id))

    def test_no_known_id_is_refused(self):
        code, out = run(['image', self.t.image('b.sys', 'd' * 64), '--repo', str(self.t.root)])
        self.assertEqual(code, 1)
        self.assertIn('0 known catalog ids', out)

    def test_two_known_ids_are_refused(self):
        code, out = run(['image', self.t.image('c.sys', self.t.old_id, self.t.new_id), '--repo', str(self.t.root)])
        self.assertEqual(code, 1)
        self.assertIn('2 known catalog ids', out)

    def test_utf16_id_is_read(self):
        sys_path = self.t.image('w.sys', extra=self.t.new_id.encode('utf-16-le'))
        self.assertEqual(run(['image', sys_path, '--repo', str(self.t.root)]), (0, self.t.new_id + '\n'))

    def test_pin_match_and_mismatch(self):
        sys_path = self.t.image('p.sys', self.t.new_id)
        code, out = run(['pin', sys_path, '--catalog', self.t.new_id, '--repo', str(self.t.root)])
        self.assertEqual(code, 0)
        self.assertIn('CATALOG PIN MATCH', out)
        code, out = run(['pin', sys_path, '--catalog', self.t.old_id.upper(), '--repo', str(self.t.root)])
        self.assertEqual(code, 1)
        self.assertIn('not the pinned', out)

    def test_pin_takes_only_an_id(self):
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                k.main(['pin', 'x.sys', '--catalog', 'b9c5c1b8', '--repo', str(self.t.root)])


class DeltaTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.t = Tree(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_equal_ids_match(self):
        code, out = run(['delta', '--installed', self.t.old_id, '--candidate', self.t.old_id,
                         '--repo', str(self.t.root)])
        self.assertEqual(code, 0)
        self.assertIn('CATALOG MATCH', out)

    def test_mismatch_names_each_setting(self):
        code, out = run(['delta', '--installed', self.t.old_id, '--candidate', self.t.new_id,
                         '--repo', str(self.t.root)])
        self.assertEqual(code, 3)
        self.assertIn('only in the candidate: WddmVersionCap', out)
        self.assertIn('changed: B (code)', out)
        self.assertNotIn(': A\n', out)
        self.assertNotIn(': A (', out)
        self.assertIn('install a package', out)

    def test_unknown_catalog_is_refused(self):
        code, out = run(['delta', '--installed', 'e' * 64, '--candidate', self.t.new_id,
                         '--repo', str(self.t.root)])
        self.assertEqual(code, 1)
        self.assertIn('neither driver/state/catalog.json', out)

    def test_history_file_with_wrong_contents_is_not_used(self):
        # A history file is accepted only when its contents have the identity of its name.
        bad = self.t.root / 'driver' / 'state' / 'history' / ('f' * 64 + '.json')
        bad.write_text(json.dumps(self.t.old), encoding='utf-8')
        code, out = run(['delta', '--installed', 'f' * 64, '--candidate', self.t.new_id,
                         '--repo', str(self.t.root)])
        self.assertEqual(code, 1)


class RealTreeTests(unittest.TestCase):
    def test_this_repository_knows_its_catalog(self):
        repo = HERE.parent.parent
        current = identity_of(json.loads((repo / 'driver' / 'state' / 'catalog.json').read_text(encoding='utf-8')))
        self.assertIn(current, k.known_ids(repo))
        self.assertEqual(k.catalog_json(repo, current)['values'], json.loads(
            (repo / 'driver' / 'state' / 'catalog.json').read_text(encoding='utf-8'))['values'])


if __name__ == '__main__':
    unittest.main()
