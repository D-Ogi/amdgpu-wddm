"""Host tests for the files that `target.py ps` pushes beside a script (BD-051, the in-process KMD reader).

No network: the push is replaced and only the file list it would send is checked. Run:
    python -m unittest discover -s tools/win -p "test_target*.py"
"""

import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import target  # noqa: E402


class Companions(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.root = Path(self.dir.name)

    def tearDown(self):
        self.dir.cleanup()

    def script(self, name, text):
        path = self.root / name
        path.write_text(text, encoding="utf-8")
        return path

    def test_the_repository_reader_exists(self):
        self.assertTrue(target.KMD_READER.is_file(), target.KMD_READER)

    def test_a_script_that_names_the_reader_gets_the_repository_copy(self):
        s = self.script("poll.ps1", ". (Join-Path $PSScriptRoot 'kmd-reader.ps1')\n")
        self.assertEqual(target.script_companions(s), [str(target.KMD_READER)])

    def test_a_copy_beside_the_script_comes_first(self):
        s = self.script("poll.ps1", ". \"$PSScriptRoot\\kmd-reader.ps1\"\n")
        own = self.script("kmd-reader.ps1", "# a frozen kit copy\n")
        self.assertEqual(target.script_companions(s), [str(own)])

    def test_a_script_without_the_reader_gets_nothing(self):
        self.assertEqual(target.script_companions(self.script("plain.ps1", "hostname\n")), [])

    def test_the_reader_itself_gets_nothing(self):
        self.assertEqual(target.script_companions(target.KMD_READER), [])

    def test_run_script_pushes_the_reader_with_the_script(self):
        s = self.script("poll.ps1", ". (Join-Path $PSScriptRoot 'kmd-reader.ps1')\n")
        t = target.Target.__new__(target.Target)
        t.cfg = {"tmp_dir": "C:\\BC250\\tmp"}
        pushed = []
        t.push = lambda paths, remote_dir=None, timeout=300: pushed.append((list(paths), remote_dir))
        t.run_staged = lambda remote, args=(), timeout=600: remote
        remote = t.run_script(str(s))
        self.assertEqual(remote, "C:\\BC250\\tmp\\poll.ps1")
        self.assertEqual(pushed, [([str(s), str(target.KMD_READER)], "C:\\BC250\\tmp")])


if __name__ == "__main__":
    unittest.main()
