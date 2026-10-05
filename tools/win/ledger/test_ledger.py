#!/usr/bin/env python3
"""Host tests for scratch/ledger/ledger.py. No lab, no network, no GPU.

    python scratch/ledger/test_ledger.py          runs every case
    python scratch/ledger/test_ledger.py -v

Each case builds a throw-away workspace in a temporary directory: a scratch tree, a real git
repository (an "origin" remote made with `git clone --bare`, so the published ref exists
locally), a release-sources.json and a reconciliation document. Every check rule has a case
that must fail and a negative control that must pass, so a rule that stops working is caught
here and not on the lab.
"""

from __future__ import annotations

import datetime as _dt
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout, redirect_stderr

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ledger as L  # noqa: E402

TODAY = _dt.date.today()
OLD = (TODAY - _dt.timedelta(days=L.STALE_DAYS + 1)).isoformat()


def run_git(cwd: str, *args: str) -> None:
    subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True, text=True)


def commit(repo: str, name: str, text: str) -> str:
    path = os.path.join(repo, name)
    os.makedirs(os.path.dirname(path), exist_ok=True) if os.path.dirname(name) else None
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text)
    run_git(repo, "add", "-A")
    run_git(repo, "-c", "user.name=t", "-c", "user.email=t@example.com",
            "commit", "-m", f"add {name}")
    out = subprocess.run(["git", "rev-parse", "--short=8", "HEAD"], cwd=repo,
                         capture_output=True, text=True, check=True)
    return out.stdout.strip()


RECONCILIATION = """\
# reconciliation

| Criterion | Evidence | State | Remains | Bound |
|---|---|---|---|---|
| M15.11 video encoding on the GPU | none | not met | the encoder | 3 min |
| M15.2 FL 12_0 | CTS batches | met | none | 3 min |
"""


class Fixture:
    """A throw-away workspace with one git repository and its published ref."""

    def __init__(self) -> None:
        self.root = tempfile.mkdtemp(prefix="ledgertest-")
        self.scratch = os.path.join(self.root, "scratch")
        os.makedirs(os.path.join(self.scratch, "ledger"))
        os.makedirs(os.path.join(self.scratch, "m15", "video-encode"))
        os.makedirs(os.path.join(self.scratch, "evidence-dir"))
        # the repository and its "origin"
        self.repo = os.path.join(self.root, "bc250-win")
        os.makedirs(self.repo)
        run_git(self.repo, "init", "-q")
        run_git(self.repo, "symbolic-ref", "HEAD", "refs/heads/main")
        self.base = commit(self.repo, "README.md", "repo\n")
        os.makedirs(os.path.join(self.repo, "docs"))
        with open(os.path.join(self.repo, "docs", "m15-reconciliation.md"), "w",
                  encoding="utf-8") as fh:
            fh.write(RECONCILIATION)
        self.landed = commit(self.repo, "docs/landed.md", "landed\n")
        self.origin = os.path.join(self.root, "origin.git")
        run_git(self.root, "clone", "-q", "--bare", self.repo, self.origin)
        run_git(self.repo, "remote", "add", "origin", self.origin)
        run_git(self.repo, "fetch", "-q", "origin")
        # a branch that is not published
        run_git(self.repo, "checkout", "-q", "-b", "feature/unlanded")
        self.unlanded = commit(self.repo, "feature.md", "work\n")
        self.unlanded2 = commit(self.repo, "feature2.md", "more work\n")
        # a second branch that forked before the first one's last commit, so it does not
        # contain it: the material for the superseded rule.
        run_git(self.repo, "checkout", "-q", "-b", "feature/sibling", self.unlanded)
        self.sibling = commit(self.repo, "sibling.md", "sibling\n")
        run_git(self.repo, "checkout", "-q", "main")
        # an untracked file of the repository: a doc landing must not accept it
        with open(os.path.join(self.repo, "docs", "untracked.md"), "w", encoding="utf-8") as fh:
            fh.write("not in git\n")
        # release sources
        rel = os.path.join(self.scratch, "release")
        os.makedirs(rel)
        with open(os.path.join(rel, "release-sources.json"), "w", encoding="utf-8") as fh:
            json.dump({"files": [{"component": "kmd", "path": "p", "source": "s", "sha256": "x"},
                                 {"component": "d3d12-shell", "path": "p", "source": "s",
                                  "sha256": "x"}]}, fh)

    def close(self) -> None:
        shutil.rmtree(self.root, ignore_errors=True)

    # -- ledger construction

    def data(self, entries: list[dict], ignore: list[dict] | None = None,
             census: list[str] | None = None) -> dict:
        return {
            "schema": 1,
            "updated": TODAY.isoformat(),
            "repos": {"bc250-win": {"path": "bc250-win", "published_ref": "origin/main",
                                    "branch_prefix": ""}},
            "umbrellas": ["m15"],
            "release_sources": "scratch/release/release-sources.json",
            "reconciliation": "bc250-win/docs/m15-reconciliation.md",
            "ignore": ignore if ignore is not None else [],
            "census": census if census is not None else [],
            "entries": entries,
        }

    def write(self, entries: list[dict], ignore: list[dict] | None = None,
              census: list[str] | None = None) -> None:
        """Write the ledger. Without an explicit census, today's world is grandfathered in,
        so a case that tests novelty creates its directory or branch after this call."""
        data = self.data(entries, ignore, census)
        if census is None:
            data["census"] = L.world_keys(L.World(self.root, data))
        with open(L.ledger_path(self.root), "w", encoding="utf-8") as fh:
            json.dump(data, fh, indent=2)

    def entry(self, **kw) -> dict:
        base = {
            "id": "e1",
            "kind": "scratch-dir",
            "where": "scratch/ledger",
            "what": "the ledger itself",
            "serves": [],
            "state": "scratch-only",
            "landed_in": "",
            "updated": TODAY.isoformat(),
        }
        base.update(kw)
        return base

    def done(self, **kw) -> dict:
        """A done-unlanded entry: components and done_since are part of the schema."""
        kw.setdefault("state", "done-unlanded")
        kw.setdefault("components", [])
        kw.setdefault("done_since", TODAY.isoformat())
        return self.entry(**kw)

    def covering(self) -> list[dict]:
        """Entries that cover everything the fixture contains, all states clean."""
        return [
            self.entry(id="ledger", where="scratch/ledger"),
            self.entry(id="release-dir", where="scratch/release"),
            self.entry(id="evidence", where="scratch/evidence-dir"),
            self.entry(id="m15", where="scratch/m15"),
            self.entry(id="video-encode", where="scratch/m15/video-encode",
                       what="the encoder", serves=[]),
            self.entry(id="main-branch", kind="branch", where=f"bc250-win:main@{self.landed}",
                       state="landed", landed_in=f"main:{self.landed}"),
            self.entry(id="feature", kind="branch",
                       where=f"bc250-win:feature/unlanded@{self.unlanded2}",
                       state="abandoned:a test fixture branch"),
            self.entry(id="sibling", kind="branch",
                       where=f"bc250-win:feature/sibling@{self.sibling}",
                       state="abandoned:a test fixture branch"),
        ]

    def run(self, *args: str) -> tuple[int, str]:
        buf = io.StringIO()
        try:
            with redirect_stdout(buf), redirect_stderr(buf):
                code = L.main(["--root", self.root, *args])
        except SystemExit as exc:
            code = exc.code if isinstance(exc.code, int) else 2
        return code, buf.getvalue()

    def load(self) -> dict:
        with open(L.ledger_path(self.root), encoding="utf-8") as fh:
            return json.load(fh)


class LedgerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.fx = Fixture()
        self.addCleanup(self.fx.close)

    def mark(self, entries: list[dict], eid: str, **kw) -> list[dict]:
        for e in entries:
            if e["id"] == eid:
                e.update(kw)
        return entries

    # ---------------------------------------------------------- negative control

    def test_negative_control_passes(self) -> None:
        self.fx.write(self.fx.covering())
        code, out = self.fx.run("check")
        self.assertEqual(code, 0, out)
        self.assertIn("0 error(s)", out)

    # ---------------------------------------------------------- novelty (the census)

    def test_a_new_directory_is_novel_even_under_a_wildcard_ignore(self) -> None:
        self.fx.write(self.fx.covering(),
                      ignore=[{"pattern": "scratch/brand-*", "reason": "a family",
                               "glob": True}])
        os.makedirs(os.path.join(self.fx.scratch, "brand-new-instrument"))
        code, out = self.fx.run("check")
        self.assertEqual(code, 1)
        self.assertIn("novel", out)
        self.assertIn("scratch/brand-new-instrument", out)

    def test_a_new_umbrella_subdirectory_is_novel(self) -> None:
        self.fx.write(self.fx.covering(),
                      ignore=[{"pattern": "scratch/m15/*", "reason": "trial evidence",
                               "glob": True}])
        os.makedirs(os.path.join(self.fx.scratch, "m15", "brand-new"))
        code, out = self.fx.run("check")
        self.assertEqual(code, 1)
        self.assertIn("scratch/m15/brand-new", out)

    def test_a_new_branch_is_novel(self) -> None:
        self.fx.write(self.fx.covering())
        run_git(self.fx.repo, "branch", "bd099-secret-fix")
        code, out = self.fx.run("check")
        self.assertEqual(code, 1)
        self.assertIn("bc250-win:bd099-secret-fix", out)

    def test_census_accept_refuses_a_name_with_no_decision_behind_it(self) -> None:
        self.fx.write(self.fx.covering())
        os.makedirs(os.path.join(self.fx.scratch, "unfiled-work"))
        code, out = self.fx.run("census", "--accept")
        self.assertEqual(code, 1)
        self.assertIn("REFUSED", out)
        self.assertIn("scratch/unfiled-work", out)
        self.assertNotIn("scratch/unfiled-work", self.fx.load()["census"])

    def test_census_accept_records_a_name_an_entry_covers(self) -> None:
        self.fx.write(self.fx.covering())
        os.makedirs(os.path.join(self.fx.scratch, "filed-work"))
        code, out = self.fx.run("add", "--id", "filed", "--kind", "scratch-dir",
                                "--where", "scratch/filed-work", "--what", "filed at once",
                                "--state", "in-progress")
        self.assertEqual(code, 0, out)
        code, out = self.fx.run("census", "--accept")
        self.assertEqual(code, 0, out)
        self.assertIn("scratch/filed-work", self.fx.load()["census"])
        code, out = self.fx.run("check")
        self.assertEqual(code, 0, out)

    def test_census_accept_records_a_name_an_exact_ignore_pattern_names(self) -> None:
        self.fx.write(self.fx.covering())
        os.makedirs(os.path.join(self.fx.scratch, "downloads"))
        code, out = self.fx.run("ignore", "--pattern", "scratch/downloads",
                                "--reason", "download staging area")
        self.assertEqual(code, 0, out)
        code, out = self.fx.run("census", "--accept")
        self.assertEqual(code, 0, out)
        code, out = self.fx.run("check")
        self.assertEqual(code, 0, out)

    # ---------------------------------------------------------- uncovered

    def test_uncovered_directory_fails(self) -> None:
        entries = [e for e in self.fx.covering() if e["id"] != "evidence"]
        self.fx.write(entries)
        code, out = self.fx.run("check")
        self.assertEqual(code, 1)
        self.assertIn("uncovered", out)
        self.assertIn("scratch/evidence-dir", out)

    def test_uncovered_directory_passes_when_ignored(self) -> None:
        entries = [e for e in self.fx.covering() if e["id"] != "evidence"]
        self.fx.write(entries, ignore=[{"pattern": "scratch/evidence-*",
                                        "reason": "pure evidence", "glob": True}])
        code, out = self.fx.run("check")
        self.assertEqual(code, 0, out)

    def test_uncovered_branch_fails(self) -> None:
        entries = [e for e in self.fx.covering() if e["id"] != "feature"]
        self.fx.write(entries)
        code, out = self.fx.run("check")
        self.assertEqual(code, 1)
        self.assertIn("bc250-win:feature/unlanded", out)

    def test_a_deeper_entry_covers_its_parent_directory(self) -> None:
        entries = [e for e in self.fx.covering() if e["id"] != "release-dir"]
        entries.append(self.fx.entry(id="rel-sources",
                                     where="scratch/release/release-sources.json",
                                     what="the release source list"))
        self.fx.write(entries)
        code, out = self.fx.run("check")
        self.assertEqual(code, 0, out)

    def test_an_unreadable_repository_is_an_error(self) -> None:
        data = self.fx.data(self.fx.covering())
        data["repos"]["gone-repo"] = {"path": "not-here", "published_ref": "origin/main"}
        data["census"] = L.world_keys(L.World(self.fx.root, data))
        with open(L.ledger_path(self.fx.root), "w", encoding="utf-8") as fh:
            json.dump(data, fh, indent=2)
        code, out = self.fx.run("check")
        self.assertEqual(code, 1)
        self.assertIn("not readable", out)

    def test_a_wildcard_ignore_pattern_must_say_it_is_one(self) -> None:
        self.fx.write(self.fx.covering(),
                      ignore=[{"pattern": "scratch/e*", "reason": "evidence"}])
        code, out = self.fx.run("check")
        self.assertEqual(code, 1)
        self.assertIn('needs "glob": true', out)

    def test_the_ignore_helper_refuses_an_undeclared_wildcard(self) -> None:
        self.fx.write(self.fx.covering())
        code, out = self.fx.run("ignore", "--pattern", "scratch/e*", "--reason", "evidence")
        self.assertEqual(code, 2)
        self.assertIn("--glob", out)

    # ---------------------------------------------------------- vanished

    def test_vanished_path_fails(self) -> None:
        entries = self.fx.covering()
        entries.append(self.fx.done(id="gone", where="scratch/was-here"))
        self.fx.write(entries)
        code, out = self.fx.run("check")
        self.assertEqual(code, 1)
        self.assertIn("vanished", out)
        self.assertIn("scratch/was-here", out)

    def test_vanished_branch_fails(self) -> None:
        entries = self.fx.covering()
        entries.append(self.fx.entry(id="gone-branch", kind="branch",
                                     where="bc250-win:feature/deleted@abcdef12",
                                     state="in-progress"))
        self.fx.write(entries)
        code, out = self.fx.run("check")
        self.assertEqual(code, 1)
        self.assertIn("is gone while the state is in-progress", out)

    def test_vanished_path_is_allowed_after_a_state_change(self) -> None:
        entries = self.fx.covering()
        entries.append(self.fx.entry(id="gone", where="scratch/was-here",
                                     state="abandoned:the directory was deleted on purpose"))
        self.fx.write(entries)
        code, out = self.fx.run("check")
        self.assertEqual(code, 0, out)

    def test_a_branch_that_moved_forward_is_only_a_warning(self) -> None:
        entries = [e for e in self.fx.covering() if e["id"] != "feature"]
        entries.append(self.fx.entry(id="feature", kind="branch",
                                     where=f"bc250-win:feature/unlanded@{self.fx.unlanded}",
                                     state="in-progress"))
        self.fx.write(entries)
        code, out = self.fx.run("check")
        self.assertEqual(code, 0, out)
        self.assertIn("drift", out)

    def test_a_rewritten_branch_loses_the_recorded_commit_and_fails(self) -> None:
        entries = [e for e in self.fx.covering() if e["id"] != "feature"]
        entries.append(self.fx.entry(id="feature", kind="branch",
                                     where=f"bc250-win:feature/unlanded@{self.fx.sibling}",
                                     state="in-progress"))
        self.fx.write(entries)
        code, out = self.fx.run("check")
        self.assertEqual(code, 1)
        self.assertIn("is no longer on", out)

    # ---------------------------------------------------------- stale debt

    def test_stale_done_unlanded_fails(self) -> None:
        entries = self.fx.covering()
        entries = self.mark(entries, "video-encode", state="done-unlanded", components=[],
                            done_since=OLD, updated=TODAY.isoformat())
        self.fx.write(entries)
        code, out = self.fx.run("check")
        self.assertEqual(code, 1)
        self.assertIn("stale-debt", out)
        self.assertIn(f"unlanded for {L.STALE_DAYS + 1} days", out)

    def test_fresh_done_unlanded_passes(self) -> None:
        entries = self.fx.covering()
        entries = self.mark(entries, "video-encode", state="done-unlanded", components=[],
                            done_since=TODAY.isoformat())
        self.fx.write(entries)
        code, out = self.fx.run("check")
        self.assertEqual(code, 0, out)

    def test_an_edit_does_not_reset_the_stale_clock(self) -> None:
        entries = self.fx.covering()
        entries = self.mark(entries, "video-encode", state="done-unlanded", components=[],
                            done_since=OLD, updated=OLD)
        self.fx.write(entries)
        code, out = self.fx.run("update", "--id", "video-encode",
                                "--landing-step", "still thinking about it")
        self.assertEqual(code, 0, out)
        entry = next(e for e in self.fx.load()["entries"] if e["id"] == "video-encode")
        self.assertEqual(entry["done_since"], OLD)
        self.assertEqual(entry["updated"], TODAY.isoformat())
        code, out = self.fx.run("check")
        self.assertEqual(code, 1)
        self.assertIn("stale-debt", out)

    def test_done_unlanded_without_components_fails(self) -> None:
        entries = self.fx.covering()
        entries = self.mark(entries, "video-encode", state="done-unlanded",
                            done_since=TODAY.isoformat())
        self.fx.write(entries)
        code, out = self.fx.run("check")
        self.assertEqual(code, 1)
        self.assertIn("done-unlanded without components", out)

    # ---------------------------------------------------------- landed

    def test_landed_sha_not_in_published_main_fails(self) -> None:
        entries = [e for e in self.fx.covering() if e["id"] != "feature"]
        entries.append(self.fx.entry(id="feature", kind="branch",
                                     where=f"bc250-win:feature/unlanded@{self.fx.unlanded2}",
                                     state="landed",
                                     landed_in=f"main:{self.fx.unlanded2}"))
        self.fx.write(entries)
        code, out = self.fx.run("check")
        self.assertEqual(code, 1)
        self.assertIn("not-landed", out)
        self.assertIn("is not an ancestor of bc250-win origin/main", out)

    def test_landed_in_release_needs_a_known_component(self) -> None:
        entries = self.fx.covering()
        entries.append(self.fx.entry(id="rel", state="landed", landed_in="release:t.1",
                                     components=["no-such-component"]))
        self.fx.write(entries)
        code, out = self.fx.run("check")
        self.assertEqual(code, 1)
        self.assertIn("are not in release-sources.json", out)

    def test_landed_in_release_with_a_known_component_passes(self) -> None:
        entries = self.fx.covering()
        entries.append(self.fx.entry(id="rel", state="landed", landed_in="release:t.1",
                                     components=["d3d12-shell"]))
        self.fx.write(entries)
        code, out = self.fx.run("check")
        self.assertEqual(code, 0, out)

    def test_landed_in_a_missing_document_fails(self) -> None:
        entries = self.fx.covering()
        entries.append(self.fx.entry(id="doc", kind="doc", state="landed",
                                     landed_in="doc:bc250-win/docs/not-there.md"))
        self.fx.write(entries)
        code, out = self.fx.run("check")
        self.assertEqual(code, 1)
        self.assertIn("does not exist", out)

    def test_landed_in_an_untracked_document_fails(self) -> None:
        entries = self.fx.covering()
        entries.append(self.fx.entry(id="doc", kind="doc", state="landed",
                                     landed_in="doc:bc250-win/docs/untracked.md"))
        self.fx.write(entries)
        code, out = self.fx.run("check")
        self.assertEqual(code, 1)
        self.assertIn("not a tracked file", out)

    def test_landed_in_a_tracked_document_passes(self) -> None:
        entries = self.fx.covering()
        entries.append(self.fx.entry(id="doc", kind="doc", state="landed",
                                     landed_in="doc:bc250-win/docs/landed.md"))
        self.fx.write(entries)
        code, out = self.fx.run("check")
        self.assertEqual(code, 0, out)

    def test_landed_with_no_landed_in_fails(self) -> None:
        entries = self.fx.covering()
        entries.append(self.fx.entry(id="half", state="landed", landed_in=""))
        self.fx.write(entries)
        code, out = self.fx.run("check")
        self.assertEqual(code, 1)
        self.assertIn("nothing is done until landed_in is set", out)

    def test_fork_landing_without_a_remote_fails(self) -> None:
        entries = [e for e in self.fx.covering() if e["id"] != "feature"]
        entries.append(self.fx.entry(
            id="feature", kind="fork-branch",
            where=f"bc250-win:feature/unlanded@{self.fx.unlanded2}",
            state="landed",
            landed_in=f"fork:bc250-win:feature/unlanded@{self.fx.unlanded2}"))
        self.fx.write(entries)
        code, out = self.fx.run("check")
        self.assertEqual(code, 1)
        self.assertIn("no remote branch", out)

    def test_fork_landing_is_judged_by_containment_not_by_the_branch_name(self) -> None:
        # The commit is on origin/main under another name: a published fork branch that
        # contains it is the landing, whatever the local branch is called.
        entries = self.fx.covering()
        entries.append(self.fx.entry(id="fork-ok", kind="fork-branch",
                                     where=f"bc250-win:main@{self.fx.landed}",
                                     state="landed",
                                     landed_in=f"fork:bc250-win:some/other-name@{self.fx.base}"))
        self.fx.write(entries)
        code, out = self.fx.run("check")
        self.assertEqual(code, 0, out)

    # ---------------------------------------------------------- superseded

    def test_a_successor_that_does_not_contain_its_predecessor_fails(self) -> None:
        entries = [e for e in self.fx.covering() if e["id"] not in ("feature", "sibling")]
        entries.append(self.fx.entry(id="old-line", kind="branch",
                                     where=f"bc250-win:feature/unlanded@{self.fx.unlanded2}",
                                     state="superseded-by:new-line"))
        entries.append(self.fx.entry(id="new-line", kind="branch",
                                     where=f"bc250-win:feature/sibling@{self.fx.sibling}",
                                     state="in-progress"))
        self.fx.write(entries)
        code, out = self.fx.run("check")
        self.assertEqual(code, 1)
        self.assertIn("superseded", out)
        self.assertIn("it supersedes nothing", out)

    def test_a_successor_that_contains_its_predecessor_passes(self) -> None:
        entries = [e for e in self.fx.covering() if e["id"] not in ("feature", "sibling")]
        entries.append(self.fx.entry(id="old-line", kind="branch",
                                     where=f"bc250-win:feature/sibling@{self.fx.unlanded}",
                                     state="superseded-by:new-line"))
        entries.append(self.fx.entry(id="new-line", kind="branch",
                                     where=f"bc250-win:feature/unlanded@{self.fx.unlanded2}",
                                     state="in-progress"))
        self.fx.write(entries)
        code, out = self.fx.run("check")
        self.assertEqual(code, 0, out)

    # ---------------------------------------------------------- unreviewed, abandoned

    def test_unreviewed_entries_are_reported(self) -> None:
        entries = self.fx.covering()
        entries = self.mark(entries, "feature", state="unreviewed")
        self.fx.write(entries)
        code, out = self.fx.run("check")
        self.assertEqual(code, 1)
        self.assertIn("unreviewed", out)
        self.assertIn("never reviewed", out)

    def test_abandoned_may_not_restate_being_unlanded(self) -> None:
        entries = self.fx.covering()
        entries = self.mark(
            entries, "feature",
            state="abandoned:not in the published main, not in the release")
        self.fx.write(entries)
        code, out = self.fx.run("check")
        self.assertEqual(code, 1)
        self.assertIn("machine reason", out)

    def test_scratch_only_with_authored_work_warns(self) -> None:
        os.makedirs(os.path.join(self.fx.scratch, "evidence-dir", "tests"))
        self.fx.write(self.fx.covering())
        code, out = self.fx.run("check")
        self.assertEqual(code, 0, out)
        self.assertIn("looks like authored work", out)

    # ---------------------------------------------------------- criteria

    def test_not_met_criterion_with_unlanded_work_warns(self) -> None:
        entries = self.fx.covering()
        entries = self.mark(entries, "video-encode", state="done-unlanded", components=[],
                            done_since=TODAY.isoformat(), serves=["M15.11"])
        self.fx.write(entries)
        code, out = self.fx.run("check")
        self.assertEqual(code, 0, out)
        self.assertIn("criterion-debt", out)
        self.assertIn("M15.11", out)

    # ---------------------------------------------------------- schema

    def test_duplicate_id_fails(self) -> None:
        entries = self.fx.covering()
        entries.append(self.fx.entry(id="ledger", where="scratch/ledger"))
        self.fx.write(entries)
        code, out = self.fx.run("check")
        self.assertEqual(code, 1)
        self.assertIn("duplicate id", out)

    def test_unknown_state_fails(self) -> None:
        entries = self.fx.covering()
        entries.append(self.fx.entry(id="odd", state="nearly-done"))
        self.fx.write(entries)
        code, out = self.fx.run("check")
        self.assertEqual(code, 1)
        self.assertIn("unknown state", out)

    def test_superseded_by_an_unknown_id_fails(self) -> None:
        entries = self.fx.covering()
        entries.append(self.fx.entry(id="odd", state="superseded-by:ghost"))
        self.fx.write(entries)
        code, out = self.fx.run("check")
        self.assertEqual(code, 1)
        self.assertIn("unknown id", out)

    def test_bad_kind_fails(self) -> None:
        entries = self.fx.covering()
        entries.append(self.fx.entry(id="odd", kind="whatever"))
        self.fx.write(entries)
        code, out = self.fx.run("check")
        self.assertEqual(code, 1)
        self.assertIn("is not one of", out)

    def test_an_ignore_pattern_without_a_reason_fails(self) -> None:
        self.fx.write(self.fx.covering(), ignore=[{"pattern": "scratch/evidence-dir",
                                                   "reason": ""}])
        code, out = self.fx.run("check")
        self.assertEqual(code, 1)
        self.assertIn("no reason", out)

    # ---------------------------------------------------------- release gate

    def test_release_gate_fails_on_unlanded_work_in_the_release(self) -> None:
        entries = self.fx.covering()
        entries = self.mark(entries, "video-encode", state="done-unlanded",
                            done_since=TODAY.isoformat(), components=["d3d12-shell"])
        self.fx.write(entries)
        code, out = self.fx.run("check", "--release", "0.0.0-test")
        self.assertEqual(code, 1)
        self.assertIn("release", out)
        self.assertIn("d3d12-shell", out)

    def test_release_gate_fails_on_a_component_the_release_does_not_carry_yet(self) -> None:
        entries = self.fx.covering()
        entries = self.mark(entries, "video-encode", state="done-unlanded",
                            done_since=TODAY.isoformat(), components=["mft-h264"])
        self.fx.write(entries)
        code, out = self.fx.run("check", "--release", "0.0.0-test")
        self.assertEqual(code, 1)
        self.assertIn("not yet in release-sources.json", out)

    def test_release_gate_fails_on_an_entry_that_serves_the_release_tag(self) -> None:
        entries = self.fx.covering()
        entries = self.mark(entries, "video-encode", state="done-unlanded",
                            done_since=TODAY.isoformat(), components=[],
                            serves=["GUI tester.11"])
        self.fx.write(entries)
        code, out = self.fx.run("check", "--release", "0.7.205.100-tester.11")
        self.assertEqual(code, 1)
        self.assertIn("serves GUI tester.11", out)

    def test_release_gate_passes_on_work_that_touches_no_component(self) -> None:
        entries = self.fx.covering()
        entries = self.mark(entries, "video-encode", state="done-unlanded",
                            done_since=TODAY.isoformat(), components=[])
        self.fx.write(entries)
        code, out = self.fx.run("check", "--release", "0.0.0-test")
        self.assertEqual(code, 0, out)

    def test_release_gate_runs_every_other_rule_too(self) -> None:
        entries = [e for e in self.fx.covering() if e["id"] != "evidence"]
        self.fx.write(entries)
        code, out = self.fx.run("check", "--release", "0.0.0-test")
        self.assertEqual(code, 1)
        self.assertIn("uncovered", out)

    def test_release_gate_passes_when_the_debt_is_landed(self) -> None:
        self.fx.write(self.fx.covering())
        code, out = self.fx.run("check", "--release", "0.0.0-test")
        self.assertEqual(code, 0, out)

    # ---------------------------------------------------------- brief, render, helpers

    def test_brief_names_the_counts_and_the_landing_step(self) -> None:
        entries = self.fx.covering()
        entries = self.mark(entries, "video-encode", state="done-unlanded", components=[],
                            done_since=TODAY.isoformat(),
                            landing_step="move it to tools/win")
        self.fx.write(entries)
        code, out = self.fx.run("brief")
        self.assertEqual(code, 0)
        self.assertIn("done-unlanded", out)
        self.assertIn("move it to tools/win", out)

    def test_brief_runs_the_gate_and_names_new_work(self) -> None:
        self.fx.write(self.fx.covering())
        os.makedirs(os.path.join(self.fx.scratch, "unfiled-work"))
        code, out = self.fx.run("brief")
        self.assertEqual(code, 0)
        self.assertIn("GATE:", out)
        self.assertIn("scratch/unfiled-work", out)

    def test_brief_says_the_gate_is_clean_when_it_is(self) -> None:
        self.fx.write(self.fx.covering())
        code, out = self.fx.run("brief")
        self.assertEqual(code, 0)
        self.assertIn("GATE: clean", out)

    def test_brief_without_a_ledger_exits_nonzero(self) -> None:
        code, out = self.fx.run("brief")
        self.assertEqual(code, 2)
        self.assertIn("LEDGER MISSING", out)

    def test_render_writes_the_view(self) -> None:
        self.fx.write(self.fx.covering(),
                      ignore=[{"pattern": "scratch/x*", "reason": "a reason", "glob": True}])
        code, out = self.fx.run("render")
        self.assertEqual(code, 0, out)
        with open(os.path.join(self.fx.root, "LEDGER.md"), encoding="utf-8") as fh:
            text = fh.read()
        self.assertIn("BC-250 work ledger", text)
        self.assertIn("video-encode", text)
        self.assertIn("a reason", text)

    def test_add_then_update_to_landed(self) -> None:
        self.fx.write(self.fx.covering())
        code, out = self.fx.run("add", "--id", "new-thing", "--kind", "tool",
                                "--where", "scratch/evidence-dir", "--what", "a new tool",
                                "--serves", "M15.11,BD-001", "--state", "done-unlanded",
                                "--components", "", "--landing-step", "move it")
        self.assertEqual(code, 0, out)
        entry = next(e for e in self.fx.load()["entries"] if e["id"] == "new-thing")
        self.assertEqual(entry["done_since"], TODAY.isoformat())
        code, out = self.fx.run("update", "--id", "new-thing", "--state", "landed",
                                "--landed-in", f"main:{self.fx.landed}")
        self.assertEqual(code, 0, out)
        entry = next(e for e in self.fx.load()["entries"] if e["id"] == "new-thing")
        self.assertEqual(entry["state"], "landed")
        self.assertEqual(entry["serves"], ["M15.11", "BD-001"])
        self.assertEqual(entry["updated"], TODAY.isoformat())

    def test_add_refuses_a_done_unlanded_entry_without_components(self) -> None:
        self.fx.write(self.fx.covering())
        code, out = self.fx.run("add", "--id", "new-thing", "--kind", "tool",
                                "--where", "scratch/evidence-dir", "--what", "a new tool",
                                "--state", "done-unlanded")
        self.assertEqual(code, 2)
        self.assertIn("--components", out)

    def test_add_refuses_an_invented_state(self) -> None:
        self.fx.write(self.fx.covering())
        code, out = self.fx.run("add", "--id", "odd", "--kind", "tool",
                                "--where", "scratch/evidence-dir", "--what", "x",
                                "--state", "totally-made-up")
        self.assertEqual(code, 2)
        self.assertIn("unknown state", out)

    def test_add_refuses_a_superseded_state_naming_nothing(self) -> None:
        self.fx.write(self.fx.covering())
        code, out = self.fx.run("add", "--id", "odd", "--kind", "tool",
                                "--where", "scratch/evidence-dir", "--what", "x",
                                "--state", "superseded-by:ghost")
        self.assertEqual(code, 2)
        self.assertIn("unknown id", out)

    def test_update_refuses_an_invented_state(self) -> None:
        self.fx.write(self.fx.covering())
        code, out = self.fx.run("update", "--id", "video-encode", "--state", "almost")
        self.assertEqual(code, 2)
        self.assertIn("unknown state", out)

    def test_add_refuses_a_duplicate_id(self) -> None:
        self.fx.write(self.fx.covering())
        code, out = self.fx.run("add", "--id", "ledger", "--kind", "tool",
                                "--where", "scratch/ledger", "--what", "x", "--state",
                                "scratch-only")
        self.assertEqual(code, 2)
        self.assertIn("already exists", out)

    def test_update_to_landed_without_landed_in_is_refused(self) -> None:
        self.fx.write(self.fx.covering())
        code, out = self.fx.run("update", "--id", "video-encode", "--state", "landed")
        self.assertEqual(code, 2)
        self.assertIn("needs --landed-in", out)

    def test_ignore_helper_adds_a_pattern_with_a_reason(self) -> None:
        entries = [e for e in self.fx.covering() if e["id"] != "evidence"]
        self.fx.write(entries)
        code, out = self.fx.run("ignore", "--pattern", "scratch/evidence-*",
                                "--reason", "pure evidence", "--glob")
        self.assertEqual(code, 0, out)
        code, out = self.fx.run("check")
        self.assertEqual(code, 0, out)


if __name__ == "__main__":
    unittest.main(verbosity=2 if "-v" in sys.argv else 1,
                  argv=[a for a in sys.argv if a != "-v"])
