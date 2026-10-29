"""The state-history gate (check_history.py) on small git repositories made from the catalogs of this tree.

Positive: the tags of the earlier releases hold the tester.32 and tester.33 catalogs, and driver/state/history holds
them; a tag from before the state store needs nothing. Negative controls: the tester.33 or the tester.32 history file
removed, a history file whose contents are another catalog, a history file with a name that is not a catalog
identity, and a checkout without tags.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))

import check_history as g  # noqa: E402

OUT = os.environ.get("BC250_TEST_OUT") or None
ENV = dict(os.environ, GIT_AUTHOR_NAME="probe", GIT_AUTHOR_EMAIL="probe@example.com",
           GIT_COMMITTER_NAME="probe", GIT_COMMITTER_EMAIL="probe@example.com", GIT_CONFIG_NOSYSTEM="1")
T33 = "55ac2dcef76b92b5c52faf145d4a38e3fd31a77ed43b366bab38e33f3d6d1bf9"
T32 = "49c220dc550899786b5fe743513dafe78b5acb5f24be5a848f66777744a3bb73"
HISTORY = ROOT / "driver" / "state" / "history"


def git(tree, *args):
    r = subprocess.run(["git", "-C", str(tree), "-c", "core.autocrlf=false"] + list(args),
                       capture_output=True, text=True, env=ENV)
    if r.returncode:
        raise RuntimeError(f"git {' '.join(args)}: {r.stderr}")
    return r.stdout.strip()


class StateHistoryGate(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(prefix="state-history-", dir=OUT))
        self.state = self.dir / "driver" / "state"
        (self.state / "history").mkdir(parents=True)
        git(self.dir, "init", "-q")
        # A release before the state store: no catalog.json.
        (self.dir / "README.txt").write_text("a release before the state store\n", encoding="utf-8")
        self.commit("tester.20")
        git(self.dir, "tag", "v0.7.216.90-tester.20")
        # tester.32, then tester.33: catalog.json as each release tag holds it, no history yet.
        for identity, tag in ((T32, "v0.7.216.104-tester.32"), (T33, "v0.7.216.105-tester.33")):
            shutil.copyfile(HISTORY / f"{identity}.json", self.state / "catalog.json")
            self.commit(tag)
            git(self.dir, "tag", tag)
        # This release: the current catalog and the history of this tree.
        shutil.copyfile(ROOT / "driver" / "state" / "catalog.json", self.state / "catalog.json")
        for f in HISTORY.glob("*.json"):
            shutil.copyfile(f, self.state / "history" / f.name)
        self.commit("tester.34")

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def commit(self, message):
        git(self.dir, "add", "-A")
        git(self.dir, "commit", "-q", "--allow-empty", "-m", message)

    def run_check(self, version="0.7.216.106-tester.34"):
        failures, lines = g.check(self.dir, "HEAD", version)
        print(f"  {self._testMethodName}: {'FAIL' if failures else 'PASS'} " + "; ".join(lines + failures))
        return failures, lines

    def test_previous_release_in_history(self):
        failures, lines = self.run_check()
        self.assertEqual(failures, [])
        self.assertTrue(any(l.startswith("v0.7.216.105-tester.33: catalog 55ac2dce is in") for l in lines), lines)

    def test_every_tag_below_the_version(self):
        failures, lines = self.run_check()
        self.assertTrue(any(l.startswith("v0.7.216.104-tester.32: catalog 49c220dc is in") for l in lines), lines)
        failures, lines = self.run_check("0.7.216.105-tester.33")
        self.assertEqual(failures, [])
        self.assertTrue(any(l.startswith("v0.7.216.104-tester.32:") for l in lines), lines)
        self.assertFalse(any("tester.33" in l for l in lines), lines)

    def test_an_older_release_removed(self):
        (self.state / "history" / f"{T32}.json").unlink()
        self.commit("remove the tester.32 catalog")
        failures, _ = self.run_check()
        self.assertEqual(len(failures), 1, failures)
        self.assertIn(f"v0.7.216.104-tester.32: catalog {T32} is not in driver/state/history", failures[0])

    def test_a_tag_before_the_state_store(self):
        failures, lines = self.run_check()
        self.assertEqual(failures, [])
        self.assertTrue(any("1 tags without driver/state/catalog.json" in l and "v0.7.216.90-tester.20" in l
                            for l in lines), lines)

    def test_tester33_history_removed(self):
        (self.state / "history" / f"{T33}.json").unlink()
        self.commit("remove the tester.33 catalog")
        failures, _ = self.run_check()
        self.assertEqual(len(failures), 1, failures)
        self.assertIn(f"v0.7.216.105-tester.33: catalog {T33} is not in driver/state/history", failures[0])

    def test_history_file_with_another_catalog(self):
        shutil.copyfile(HISTORY / f"{T32}.json", self.state / "history" / f"{T33}.json")
        self.commit("tester.32 catalog under the tester.33 name")
        failures, _ = self.run_check()
        self.assertTrue(any(f"history/{T33}.json: the catalog identity of its contents is {T32}" in f for f in failures),
                        failures)
        self.assertTrue(any("v0.7.216.105-tester.33" in f for f in failures), failures)

    def test_history_file_name(self):
        shutil.copyfile(HISTORY / f"{T32}.json", self.state / "history" / "tester32.json")
        self.commit("a name that is not an identity")
        failures, _ = self.run_check()
        self.assertTrue(any("tester32.json: the name is not" in f for f in failures), failures)

    def test_current_catalog_unchanged_since_the_tag(self):
        shutil.copyfile(HISTORY / f"{T33}.json", self.state / "catalog.json")
        (self.state / "history" / f"{T33}.json").unlink()
        self.commit("no catalog change")
        failures, lines = self.run_check()
        self.assertEqual(failures, [])
        self.assertTrue(any("is the current catalog" in l for l in lines), lines)

    def test_no_tags(self):
        for tag in git(self.dir, "tag", "--list").split():
            git(self.dir, "tag", "-d", tag)
        failures, _ = self.run_check()
        self.assertTrue(any("no tag v*-tester.* below" in f for f in failures), failures)


class ThisTree(unittest.TestCase):
    def test_history_files_named_by_identity(self):
        names = sorted(f.name for f in HISTORY.glob("*.json"))
        self.assertIn(f"{T33}.json", names)
        for f in HISTORY.glob("*.json"):
            self.assertEqual(g.identity_of(json.loads(f.read_text(encoding="utf-8"))) + ".json", f.name)


if __name__ == "__main__":
    unittest.main()
