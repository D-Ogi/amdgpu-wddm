"""Self-test of the both-lines gate (marker_guard.py lines, BD-204 step 2).

    python -m unittest discover -s tools/release -p test_winsys_lines.py   # the 'winsys-lines-controls' gate of quick.ps1

Each test makes a throwaway Mesa-like repository with a merge base and two lines, and a release-sources.json that pins
one commit of each line to a payload path. A fix marker (a BD id in a comment or in a commit subject) on one line and
not on the other fails, and passes again once the fix is ported or an accept entry says why; an equivalent entry
holds only while its commits are on the other line; an entry that no longer matches fails as stale; a named
both-lines fix must be on every line. BC250_TEST_OUT, when set, holds the temporary trees.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import marker_guard
from test_provenance import git, remove_tree

HERE = Path(__file__).resolve().parent
OUT = os.environ.get("BC250_TEST_OUT") or None
REASON = "a reason of the test fixture that is long enough for the gate"


class BothLines(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="winsys-lines-", dir=OUT))
        self.repo = self.tmp / "mesa"
        self.repo.mkdir()
        git(self.repo, "init", "-q")
        git(self.repo, "symbolic-ref", "HEAD", "refs/heads/system")
        self.write("src/amd/vulkan/winsys/wddm2/bo.c", "int bo_destroy(void) { return 0; }\n")
        self.write("docs/notes.txt", "/* BD-900: outside the paths */\n")
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-q", "-m", "base")
        git(self.repo, "branch", "hosted")
        self.config = {"repository": "mesa", "lines": {"system": "payload/system.dll", "hosted": "payload/hosted.dll"},
                       "pairs": [["system", "hosted"]], "paths": ["src/amd/vulkan"], "marker_pattern": r"BD-\d{3}",
                       "both_lines": [], "accept": []}

    def tearDown(self):
        remove_tree(self.tmp)

    def write(self, rel, text):
        p = self.repo / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding="utf-8")

    def commit_on(self, line, rel, text, message):
        git(self.repo, "checkout", "-q", line)
        self.write(rel, text)
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-q", "-m", message)
        return git(self.repo, "rev-parse", "HEAD")

    def run_gate(self, extra=()):
        pins = {"system": git(self.repo, "rev-parse", "system"), "hosted": git(self.repo, "rev-parse", "hosted")}
        sources = {"repositories": {"mesa": {"url": "https://example.invalid/mesa", "checkouts": ["mesa"]}},
                   "files": [{"path": self.config["lines"][n], "built_from": {"repo": "mesa", "commit": c}}
                             for n, c in pins.items()]}
        (self.tmp / "sources.json").write_text(json.dumps(sources), encoding="utf-8")
        (self.tmp / "config.json").write_text(json.dumps(self.config), encoding="utf-8")
        r = subprocess.run([sys.executable, str(HERE / "marker_guard.py"), "lines", "--config",
                            str(self.tmp / "config.json"), "--sources", str(self.tmp / "sources.json"), "--root",
                            str(self.tmp), *extra], capture_output=True, text=True)
        return r.returncode, r.stdout + r.stderr

    def test_a_fix_on_one_line_only_fails_until_it_is_ported(self):
        code, out = self.run_gate()
        self.assertEqual(code, 0, out)
        self.assertIn("PASS", out)
        self.commit_on("system", "src/amd/vulkan/winsys/wddm2/bo.c",
                       "/* BD-901: hold the BO until its work retires. */\nint bo_destroy(void) { return 1; }\n",
                       "radv/wddm2: hold a destroyed BO")
        code, out = self.run_gate()
        self.assertEqual(code, 1, out)
        self.assertIn("BD-901 is on system (comment in src/amd/vulkan/winsys/wddm2/bo.c) and not on hosted", out)
        self.assertNotIn("BD-900", out, "a marker outside the paths is not a winsys fix")
        self.commit_on("hosted", "src/amd/vulkan/winsys/wddm2/bo.c",
                       "/* BD-901: hold the BO until its work retires. */\nint bo_destroy(void) { return 1; }\n",
                       "radv/wddm2: hold a destroyed BO (port)")
        code, out = self.run_gate()
        self.assertEqual(code, 0, out)

    def test_a_subject_is_a_marker_too(self):
        self.commit_on("hosted", "src/amd/vulkan/radv_queue.c", "int q;\n", "radv: skip the preamble wait (BD-902)")
        code, out = self.run_gate()
        self.assertEqual(code, 1, out)
        self.assertIn("BD-902 is on hosted (subject of", out)

    def test_accept_kinds_and_stale_entries(self):
        self.commit_on("system", "src/amd/vulkan/wsi.c", "/* BD-903: the present route */\n", "wsi")
        self.config["accept"] = [{"marker": "BD-903", "on": "system", "missing": "hosted", "kind": "contract",
                                  "reason": REASON}]
        code, out = self.run_gate()
        self.assertEqual(code, 0, out)
        self.assertIn("ACCEPTED   BD-903 on system, not on hosted (contract)", out)
        self.config["accept"][0]["kind"] = "open"
        code, out = self.run_gate()
        self.assertEqual(code, 0, out)
        self.assertIn("OPEN       BD-903", out)
        # A short reason or an unknown kind is refused.
        self.config["accept"][0].update(kind="maybe")
        code, out = self.run_gate()
        self.assertEqual(code, 1, out)
        self.assertIn("kind 'maybe' is not one of", out)
        self.config["accept"][0].update(kind="contract", reason="short")
        code, out = self.run_gate()
        self.assertEqual(code, 1, out)
        self.assertIn("at least 40 characters", out)
        # The port makes the entry stale, and a stale entry fails until it goes.
        self.config["accept"][0]["reason"] = REASON
        self.commit_on("hosted", "src/amd/vulkan/wsi.c", "/* BD-903: the present route */\n", "wsi port")
        code, out = self.run_gate()
        self.assertEqual(code, 1, out)
        self.assertIn("STALE", out)
        self.config["accept"] = []
        code, out = self.run_gate()
        self.assertEqual(code, 0, out)

    def test_an_equivalent_holds_only_while_its_commits_are_on_the_other_line(self):
        other = self.commit_on("hosted", "src/amd/vulkan/budget.c", "int budget;\n", "radv/wddm2: memory budget")
        self.commit_on("system", "src/amd/vulkan/budget.c", "int budget;\n", "radv/wddm2: memory budget (BD-904)")
        self.config["accept"] = [{"marker": "BD-904", "on": "system", "missing": "hosted", "kind": "equivalent",
                                  "commits": [other], "reason": REASON}]
        code, out = self.run_gate()
        self.assertEqual(code, 0, out)
        self.assertIn(f"EQUIVALENT BD-904 on system, on hosted as {other[:8]}", out)
        foreign = self.commit_on("system", "src/amd/vulkan/other.c", "int o;\n", "unrelated")
        self.config["accept"][0]["commits"] = [foreign]
        code, out = self.run_gate()
        self.assertEqual(code, 1, out)
        self.assertIn(f"names {foreign[:8]} as its equivalent", out)

    def test_a_named_fix_must_be_on_every_line(self):
        self.commit_on("system", "src/amd/vulkan/winsys/wddm2/bo.c", "void destroy_now(void);\n", "radv/wddm2: defer")
        self.config["both_lines"] = [{"name": "deferred destruction", "pattern": "destroy_now",
                                      "path": "src/amd/vulkan/winsys/wddm2/bo.c"}]
        code, out = self.run_gate()
        self.assertEqual(code, 1, out)
        self.assertIn("both-lines fix 'deferred destruction' is not on hosted", out)
        self.commit_on("hosted", "src/amd/vulkan/winsys/wddm2/bo.c", "void destroy_now(void);\n", "radv/wddm2: defer")
        code, out = self.run_gate()
        self.assertEqual(code, 0, out)
        self.assertIn("BOTH       'deferred destruction' on hosted", out)

    def test_a_line_that_the_manifest_does_not_pin_once_fails(self):
        self.config["lines"]["wow64"] = "payload/wow64.dll"
        code, out = self.run_gate()
        self.assertEqual(code, 1, out)
        self.assertIn("line wow64: release-sources.json pins 0 commits", out)

    def test_the_repository_config_is_valid(self):
        # The shipped config: every pair names two of its lines, every entry is well formed, and every equivalent
        # names full commit ids. Its findings at the release pins are the 'winsys-lines' gate (test-winsys-lines.ps1).
        config = json.loads((HERE / "winsys-lines.json").read_text(encoding="utf-8"))
        for a, b in config["pairs"]:
            self.assertIn(a, config["lines"])
            self.assertIn(b, config["lines"])
        for e in config["accept"]:
            self.assertIn(e["kind"], marker_guard.MARKER_KINDS, e)
            self.assertGreaterEqual(len(e["reason"]), 40, e)
            self.assertIn(e["on"], config["lines"])
            self.assertIn(e["missing"], config["lines"])
            for c in e.get("commits", []):
                self.assertRegex(c, r"^[0-9a-f]{40}$")
        keys = [(e["marker"], e["on"], e["missing"]) for e in config["accept"]]
        self.assertEqual(len(keys), len(set(keys)), "one entry per marker and direction")


if __name__ == "__main__":
    unittest.main()
