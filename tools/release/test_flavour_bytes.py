"""The flavour byte check must not read a Git error as the absence of a lab string."""
import pathlib
import subprocess
import tempfile
import unittest
from flavour_bytes import in_tree


def git(repo, *args):
    return subprocess.run(['git', '-C', str(repo), *args], capture_output=True, text=True, check=True).stdout.strip()


class InTreeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.repo = pathlib.Path(cls.tmp.name) / 'mesa'
        # The tree holds one lab string. The other strings of the check are absent from it.
        (cls.repo / 'src').mkdir(parents=True)
        (cls.repo / 'src' / 'lab.c').write_text('const char *s = "BC250_DEFERRED_LOG";\n', encoding='utf-8')
        git(cls.repo, 'init', '-q')
        git(cls.repo, 'add', 'src/lab.c')
        git(cls.repo, '-c', 'user.name=test', '-c', 'user.email=test@example.com', 'commit', '-q', '-m', 'lab string')
        cls.commit = git(cls.repo, 'rev-parse', 'HEAD')

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_a_string_of_the_tree_is_found_and_an_absent_one_is_not(self):
        self.assertTrue(in_tree(self.repo, self.commit, 'BC250_DEFERRED_LOG'))
        self.assertFalse(in_tree(self.repo, self.commit, 'queue wait skipped'))

    def test_a_commit_that_the_checkout_does_not_hold_is_an_error(self):
        with self.assertRaises(RuntimeError):
            in_tree(self.repo, '0' * 40, 'BC250_DEFERRED_LOG')

    def test_a_checkout_that_does_not_exist_is_an_error(self):
        with self.assertRaises(RuntimeError):
            in_tree(pathlib.Path(self.tmp.name) / 'absent', self.commit, 'BC250_DEFERRED_LOG')


if __name__ == '__main__':
    unittest.main()
