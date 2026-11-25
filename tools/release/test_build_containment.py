"""Tests of the containment gate (containment.py): the decision on fakes, then the git check on a real repository
with a fix commit that a row has and one that it does not have (the mutant: a row pinned before its fix)."""
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import containment  # noqa: E402
import build_release_set  # noqa: E402

OUT = os.environ.get("BC250_TEST_OUT") or None
A, B, C, D = ("a" * 40), ("b" * 40), ("c" * 40), ("d" * 40)
F, FIX = ("f" * 40), ("9" * 40)

# A is the manifest's recipe_commit. desktop-rel is a row of another repository with a recipe_commit of its own (F);
# desktop-old has none, so the manifest's recipe builds it.
ROWS = [
    {"id": "kmd-rel", "component": "kmd", "repo": "wddm", "commit": B, "inputs": {}},
    {"id": "kmd-lab", "component": "kmd", "repo": "wddm", "commit": B, "inputs": {}},
    {"id": "d3d11-dxvk", "component": "d3d11-umd", "repo": "dxvk", "commit": C, "inputs": {}},
    {"id": "d3d11-shell", "component": "d3d11-umd", "repo": "wddm", "commit": D,
     "inputs": {"mesa": {"repo": "mesa", "commit": C}}},
    {"id": "desktop-rel", "component": "desktop-umd", "repo": "mesa", "commit": C, "recipe_commit": F, "inputs": {}},
    {"id": "desktop-old", "component": "desktop-umd", "repo": "mesa", "commit": C, "inputs": {}},
]


def run(containment_list, ancestors, release="tester.38", missing=()):
    """ancestors: set of (fix, commit) pairs that are ancestors; missing: commits in no checkout."""
    def find(repo, commit):
        return None if commit in missing else f"co-{repo}"

    def is_ancestor(checkout, fix, commit):
        return (fix, commit) in ancestors

    return containment.check(release, containment_list, ROWS, "wddm", A, find, is_ancestor)


class Decision(unittest.TestCase):
    def test_missing_list_fails_from_tester_38(self):
        status, lines = run(None, set())
        self.assertEqual(status, "FAIL")
        self.assertIn("no 'containment' list", lines[0])
        self.assertEqual(run(None, set(), release="t1")[0], "FAIL")

    def test_missing_list_passes_before_tester_38(self):
        self.assertEqual(run(None, set(), release="tester.37")[0], "PASS")
        self.assertEqual(run(None, set(), release="tester.31v2")[0], "PASS")

    def test_empty_list_passes(self):
        self.assertEqual(run([], set())[0], "PASS")

    def test_component_checks_every_row_of_the_repository(self):
        entry = {"defect": "BD-306", "repo": "wddm", "commit": A, "rows": ["kmd"]}
        status, lines = run([entry], {(A, B)})
        self.assertEqual(status, "PASS", lines)
        self.assertEqual(sum(1 for l in lines if l.startswith("PASS BD-306")), 2)

    def test_fix_not_in_row_fails(self):
        entry = {"defect": "BD-306", "repo": "wddm", "commit": A, "rows": ["kmd"]}
        status, lines = run([entry], set())
        self.assertEqual(status, "FAIL")
        self.assertTrue(any("is not in kmd-rel" in l for l in lines), lines)

    def test_rows_of_other_repositories_are_passed_over(self):
        # d3d11-umd has a dxvk row and a wddm row; a wddm fix is checked only against the wddm row.
        entry = {"defect": "BD-330", "repo": "wddm", "commit": A, "rows": ["d3d11-umd"]}
        status, lines = run([entry], {(A, D)})
        self.assertEqual(status, "PASS", lines)
        self.assertFalse(any("d3d11-dxvk" in l for l in lines), lines)

    def test_input_of_the_repository_is_a_target(self):
        entry = {"defect": "BD-200", "repo": "mesa", "commit": A, "rows": ["d3d11-shell"]}
        status, lines = run([entry], {(A, C)})
        self.assertEqual(status, "PASS", lines)
        self.assertTrue(any("d3d11-shell input mesa" in l for l in lines), lines)

    def test_name_without_target_fails(self):
        entry = {"defect": "BD-200", "repo": "mesa", "commit": A, "rows": ["kmd"]}
        status, lines = run([entry], set())
        self.assertEqual(status, "FAIL")
        self.assertIn("names no row that builds from mesa", lines[0])
        entry = {"defect": "BD-200", "repo": "wddm", "commit": A, "rows": ["no-such-row"]}
        self.assertEqual(run([entry], set())[0], "FAIL")

    def test_recipe(self):
        entry = {"defect": "BD-317", "repo": "wddm", "commit": B, "rows": ["@recipe"]}
        self.assertEqual(run([entry], {(B, A)})[0], "PASS")
        self.assertEqual(run([entry], set())[0], "FAIL")
        entry = {"defect": "BD-317", "repo": "mesa", "commit": B, "rows": ["@recipe"]}
        self.assertEqual(run([entry], {(B, A)})[0], "FAIL")

    def test_row_recipe_was_not_a_target(self):
        # The hole of BD-389: a recipe fix of a row with its own recipe_commit had no form. The row name gives no
        # target of the recipe repository, and '@recipe' is the manifest's recipe_commit only.
        entry = {"defect": "BD-389", "repo": "wddm", "commit": FIX, "rows": ["desktop-rel"]}
        status, lines = run([entry], {(FIX, F)})
        self.assertEqual(status, "FAIL")
        self.assertIn("names no row that builds from wddm", lines[0])
        entry = dict(entry, rows=["@recipe"])
        self.assertEqual(run([entry], {(FIX, F)})[0], "FAIL")

    def test_row_recipe_is_a_target(self):
        entry = {"defect": "BD-389", "repo": "wddm", "commit": FIX, "rows": ["@recipe:desktop-rel"]}
        status, lines = run([entry], {(FIX, F)})
        self.assertEqual(status, "PASS", lines)
        self.assertTrue(any(l.startswith(f"PASS BD-389 wddm {FIX[:9]} in desktop-rel recipe {F[:9]}") for l in lines),
                        lines)
        status, lines = run([entry], {(FIX, A)})
        self.assertEqual(status, "FAIL")
        self.assertTrue(any("is not in desktop-rel recipe" in l for l in lines), lines)

    def test_row_recipe_is_the_recipe_that_builds_the_row(self):
        # The planner's rule: the row's own recipe_commit, else the row commit for a row of the recipe repository,
        # else the manifest's recipe_commit.
        self.assertEqual(containment.recipe_commit_of(ROWS[4], "wddm", A), F)
        self.assertEqual(containment.recipe_commit_of(ROWS[5], "wddm", A), A)
        self.assertEqual(containment.recipe_commit_of(ROWS[0], "wddm", A), B)
        entry = {"defect": "BD-389", "repo": "wddm", "commit": FIX, "rows": ["@recipe:desktop-old", "@recipe:kmd-rel"]}
        self.assertEqual(run([entry], {(FIX, A), (FIX, B)})[0], "PASS")
        self.assertEqual(run([entry], {(FIX, A)})[0], "FAIL")

    def test_row_recipe_of_a_component(self):
        entry = {"defect": "BD-389", "repo": "wddm", "commit": FIX, "rows": ["@recipe:desktop-umd"]}
        status, lines = run([entry], {(FIX, F), (FIX, A)})
        self.assertEqual(status, "PASS", lines)
        self.assertEqual(sum(1 for l in lines if l.startswith("PASS BD-389")), 2, lines)
        self.assertEqual(run([entry], {(FIX, F)})[0], "FAIL")

    def test_row_recipe_needs_the_recipe_repository_and_a_row(self):
        entry = {"defect": "BD-389", "repo": "mesa", "commit": FIX, "rows": ["@recipe:desktop-rel"]}
        status, lines = run([entry], {(FIX, F)})
        self.assertEqual(status, "FAIL")
        self.assertIn("'@recipe:desktop-rel' names a recipe, and the recipes are commits of wddm, not of mesa", lines[0])
        entry = {"defect": "BD-389", "repo": "wddm", "commit": FIX, "rows": ["@recipe:no-such-row"]}
        status, lines = run([entry], {(FIX, F)})
        self.assertEqual(status, "FAIL")
        self.assertIn("'@recipe:no-such-row' names no row", lines[0])

    def test_plan_rows_carry_the_row_recipe(self):
        # build_release_set.py puts plan_rows(manifest rows) into the plan, and check_plan reads them back.
        rows = containment.plan_rows(ROWS)
        self.assertEqual(rows[4]["recipe_commit"], F)
        self.assertNotIn("recipe_commit", rows[5])
        entry = {"defect": "BD-389", "repo": "wddm", "commit": FIX, "rows": ["@recipe:desktop-rel"]}
        status, _ = containment.check("tester.39", [entry], rows, "wddm", A, lambda repo, commit: "co",
                                      lambda checkout, fix, commit: (fix, commit) == (FIX, F))
        self.assertEqual(status, "PASS")

    def test_row_commit_in_no_checkout_fails(self):
        entry = {"defect": "BD-306", "repo": "wddm", "commit": A, "rows": ["kmd-rel"]}
        status, lines = run([entry], {(A, B)}, missing={B})
        self.assertEqual(status, "FAIL")
        self.assertIn("in no checkout", lines[0])

    def test_schema_and_manifest_rules(self):
        base = build_release_set.load_json(HERE / "release-tester.38.json")
        good = dict(base, containment=[{"defect": "BD-306", "repo": "amdgpu-wddm", "commit": A, "rows": ["kmd"]}])
        self.assertEqual(build_release_set.check_manifest(good), [])
        for bad in ({"defect": "306", "repo": "amdgpu-wddm", "commit": A, "rows": ["kmd"]},
                    {"defect": "BD-306", "repo": "amdgpu-wddm", "commit": "abc", "rows": ["kmd"]},
                    {"defect": "BD-306", "repo": "amdgpu-wddm", "commit": A, "rows": []},
                    {"defect": "BD-306", "repo": "amdgpu-wddm", "commit": A, "rows": ["kmd"], "extra": 1},
                    {"defect": "BD-306", "repo": "nowhere", "commit": A, "rows": ["kmd"]}):
            self.assertNotEqual(build_release_set.check_manifest(dict(base, containment=[bad])), [], bad)


def git(cwd, *a):
    r = subprocess.run(["git", "-C", str(cwd), *a], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(r.stderr)
    return r.stdout.strip()


class Git(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="containment-", dir=OUT)
        self.repo = Path(self.tmp.name) / "r"
        self.repo.mkdir()
        git(self.repo, "init", "-q")
        git(self.repo, "config", "user.email", "test@example.com")
        git(self.repo, "config", "user.name", "test")
        git(self.repo, "config", "commit.gpgsign", "false")

        def commit(msg):
            (self.repo / "f.txt").write_text(msg, encoding="utf-8")
            git(self.repo, "add", "f.txt")
            git(self.repo, "commit", "-q", "-m", msg)
            return git(self.repo, "rev-parse", "HEAD")

        self.base = commit("base")
        self.fix = commit("fix")
        self.after = commit("after")

    def tearDown(self):
        self.tmp.cleanup()

    def test_ancestor_answers(self):
        self.assertTrue(containment.git_is_ancestor(self.repo, self.fix, self.after))
        self.assertTrue(containment.git_is_ancestor(self.repo, self.fix, self.fix))
        # The mutant: a row pinned to the commit before its fix.
        self.assertFalse(containment.git_is_ancestor(self.repo, self.fix, self.base))
        # A fix commit that the checkout does not hold is no answer, never a pass.
        self.assertIsNone(containment.git_is_ancestor(self.repo, "e" * 40, self.after))

    def test_gate_on_real_history(self):
        rows = [{"id": "kmd-rel", "component": "kmd", "repo": "wddm", "commit": self.after, "inputs": {}},
                {"id": "old", "component": "old", "repo": "wddm", "commit": self.base, "inputs": {}}]
        find = lambda repo, commit: self.repo  # noqa: E731
        entry = {"defect": "BD-306", "repo": "wddm", "commit": self.fix, "rows": ["kmd"]}
        self.assertEqual(containment.check("tester.38", [entry], rows, "wddm", self.after, find,
                                           containment.git_is_ancestor)[0], "PASS")
        entry = dict(entry, rows=["kmd", "old"])
        status, lines = containment.check("tester.38", [entry], rows, "wddm", self.after, find,
                                          containment.git_is_ancestor)
        self.assertEqual(status, "FAIL")
        self.assertTrue(any("is not in old" in l for l in lines), lines)

    def test_row_recipe_on_real_history(self):
        # A row of another repository whose own recipe_commit has the fix, and the mutant: one pinned before it.
        rows = [{"id": "desktop-rel", "component": "desktop-umd", "repo": "mesa", "commit": C,
                 "recipe_commit": self.after, "inputs": {}},
                {"id": "desktop-old", "component": "desktop-umd", "repo": "mesa", "commit": C,
                 "recipe_commit": self.base, "inputs": {}}]
        find = lambda repo, commit: self.repo  # noqa: E731
        entry = {"defect": "BD-389", "repo": "wddm", "commit": self.fix, "rows": ["@recipe:desktop-rel"]}
        self.assertEqual(containment.check("tester.39", [entry], rows, "wddm", self.base, find,
                                           containment.git_is_ancestor)[0], "PASS")
        entry = dict(entry, rows=["@recipe:desktop-umd"])
        status, lines = containment.check("tester.39", [entry], rows, "wddm", self.after, find,
                                          containment.git_is_ancestor)
        self.assertEqual(status, "FAIL")
        self.assertTrue(any("is not in desktop-old recipe" in l for l in lines), lines)


if __name__ == "__main__":
    unittest.main()
