"""Positive and negative controls of third_party_doc.py on small git repositories."""
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import third_party_doc

HEADER = '| File (`payload\\`) | Component @ commit | Licence texts (`licenses\\`) |\n|---|---|---|\n'


def run(cwd, *args):
    return subprocess.run(['git', '-C', str(cwd), *args], check=True, capture_output=True, text=True).stdout.strip()


def commit(repo, name, text, message):
    (repo / name).write_text(text, encoding='utf-8')
    run(repo, 'add', name)
    run(repo, '-c', 'user.name=t', '-c', 'user.email=t@example.com', 'commit', '-q', '-m', message)
    return run(repo, 'rev-parse', 'HEAD')


def make_repo(path):
    path.mkdir(parents=True)
    run(path, 'init', '-q')
    return path


class ThirdPartyDocTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=os.environ.get('BC250_TEST_OUT'))
        self.root = Path(self.temp.name)
        self.engine = make_repo(self.root / 'engine')
        self.old = commit(self.engine, 'LICENSE', 'licence one\n', 'old')
        self.new = commit(self.engine, 'a.c', 'int a;\n', 'new')
        self.sub = make_repo(self.root / 'sub')
        self.sub_commit = commit(self.sub, 'LICENSE', 'sub licence\n', 'sub')
        self.doc = self.root / 'THIRD-PARTY.md'

    def tearDown(self):
        self.temp.cleanup()

    def sources(self, submodules=None, licences=()):
        entry = {'url': 'https://github.com/D-Ogi/engine', 'checkouts': ['engine']}
        if submodules is not None:
            entry['submodules'] = submodules
        return {
            'schema': 4,
            'repositories': {'engine': entry,
                             'sub': {'url': 'https://github.com/D-Ogi/sub', 'checkouts': ['sub']}},
            'files': [{'path': 'payload/engine/engine.dll', 'flavour': flavour,
                       'built_from': {'repo': 'engine', 'commit': self.new}} for flavour in ('release', 'lab')],
            'licenses': list(licences),
        }

    def check(self, rows, sources, write=False):
        self.doc.write_text(HEADER + rows, encoding='utf-8')
        errors, notes, _ = third_party_doc.check(self.doc, sources, self.root, self.root, write)
        return errors

    def gitlink(self, path='subprojects/sub'):
        run(self.engine, 'update-index', '--add', '--cacheinfo', f'160000,{self.sub_commit},{path}')
        (self.engine / '.gitmodules').write_text(f'[submodule "s"]\n\tpath = {path}\n\turl = https://github.com/D-Ogi/sub\n',
                                                 encoding='utf-8')
        run(self.engine, 'add', '.gitmodules')
        run(self.engine, '-c', 'user.name=t', '-c', 'user.email=t@example.com', 'commit', '-q', '-m', 'link')
        self.new = run(self.engine, 'rev-parse', 'HEAD')

    def test_current_commit_passes(self):
        self.assertEqual(self.check(f'| `engine\\engine.dll` | engine @ `{self.new}` | - |\n', self.sources()), [])

    def test_stale_commit_fails_and_write_replaces_it(self):
        rows = f'| `engine\\engine.dll` | engine @ `{self.old}` | - |\n\nShort `{self.old[:8]}`.\n'
        errors = self.check(rows, self.sources())
        self.assertTrue(any('is not a commit of this build' in e for e in errors), errors)
        self.assertTrue(any('names ' + self.new[:8] in e for e in errors), errors)
        self.assertEqual(self.check(rows, self.sources(), write=True), [])
        text = self.doc.read_text(encoding='utf-8')
        self.assertIn(self.new, text)
        self.assertIn(f'`{self.new[:8]}`', text)
        self.assertNotIn(self.old[:8], text)

    def test_unnamed_file_fails(self):
        sources = self.sources()
        sources['files'].append({'path': 'payload/engine/extra.json', 'flavour': 'shared',
                                 'built_from': {'repo': 'engine', 'commit': self.new}})
        errors = self.check(f'| `engine\\engine.dll` | engine @ `{self.new}` | - |\n', sources)
        self.assertTrue(any('extra.json: no row' in e for e in errors), errors)

    def test_unknown_file_fails(self):
        rows = f'| `engine\\engine.dll`, `engine\\gone.dll` | engine @ `{self.new}` | - |\n'
        self.assertTrue(any('gone.dll' in e for e in self.check(rows, self.sources())))

    def test_branch_fails(self):
        rows = f'| `engine\\engine.dll` | engine @ `{self.new}` (branch `main`) | - |\n'
        self.assertTrue(any('names a branch' in e for e in self.check(rows, self.sources())))

    def test_short_hash_of_no_commit_fails(self):
        rows = f'| `engine\\engine.dll` | engine @ `{self.new}` | - |\n\nOld build `1234abcd`.\n'
        self.assertTrue(any('1234abcd' in e for e in self.check(rows, self.sources())))

    def test_submodule_must_be_named(self):
        self.gitlink()
        sources = self.sources(submodules={'subprojects/sub': 'sub'})
        row = f'| `engine\\engine.dll` | engine @ `{self.new}` | - |\n'
        errors = self.check(row, sources)
        self.assertTrue(any('submodule subprojects/sub' in e for e in errors), errors)
        self.assertEqual(self.check(row + f'| | with sub @ `{self.sub_commit}` | - |\n', sources), [])

    def test_submodule_that_is_not_built_is_not_required(self):
        third_party_doc.NOT_BUILT[('engine', 'subprojects/sub')] = 'test'
        try:
            self.gitlink()
            row = f'| `engine\\engine.dll` | engine @ `{self.new}` | - |\n'
            self.assertEqual(self.check(row, self.sources(submodules={})), [])
        finally:
            del third_party_doc.NOT_BUILT[('engine', 'subprojects/sub')]

    def test_licence_text_must_be_the_same_at_build_commits(self):
        licence = {'file': 'engine-LICENSE', 'repository': 'D-Ogi/engine', 'path': 'LICENSE', 'ref': self.old}
        row = f'| `engine\\engine.dll` | engine @ `{self.new}` | - |\n'
        self.assertEqual(self.check(row, self.sources(licences=[licence])), [])
        self.new = commit(self.engine, 'LICENSE', 'licence two\n', 'relicence')
        errors = self.check(f'| `engine\\engine.dll` | engine @ `{self.new}` | - |\n', self.sources(licences=[licence]))
        self.assertTrue(any('is not the text of' in e for e in errors), errors)


if __name__ == '__main__':
    unittest.main()
