#!/usr/bin/env python3
"""Work ledger for the P:\\BC-250 workspace: the source of truth for unfinished work.

The ledger answers one question that no other file answers: which piece of finished or
half-finished work is not yet in main, in a release or in the documentation.

    python tools/win/ledger/ledger.py brief            one screen for session start
    python tools/win/ledger/ledger.py check            the gate; exit 1 on debt or drift
    python tools/win/ledger/ledger.py check --release 0.7.205.100-tester.11
    python tools/win/ledger/ledger.py render           writes LEDGER.md
    python tools/win/ledger/ledger.py add --id ... --kind ... --where ... --what ...
    python tools/win/ledger/ledger.py update --id ... --state landed --landed-in main:<sha>
    python tools/win/ledger/ledger.py ignore --pattern ... --reason ... [--glob]
    python tools/win/ledger/ledger.py census [--accept]  record today's names as known
    python tools/win/ledger/ledger.py list [--state done-unlanded] [--serves M15.11]

The ledger file is LEDGER.json at the workspace root (BC250_ROOT, or --root). The file
itself stays in the workspace and never goes into a repository: it names scratch paths
and lab flows. This tool reads and writes it; the tool is repository content. No network
access, no lab access, no resident process: every git operation is local and read-only.

Entry schema (see also the "schema_doc" key of LEDGER.json):

    id              short stable slug, unique
    kind            scratch-dir | branch | fork-branch | tool | doc | defect-fix
    where           a path relative to the workspace root, or <repo>:<branch>@<sha>
    what            one sentence: what the work is
    serves          list of criterion ids (M15.11), defect ids (BD-045) or named lab flows
    state           in-progress | done-unlanded | landed | superseded-by:<id>
                    | scratch-only | unreviewed | abandoned:<reason>
    landed_in       main:<sha> | release:<version> | doc:<path> | fork:<repo>:<branch>@<sha>
                    (empty unless state is landed; nothing is "done" until this is set)
    components      release components the work touches (release-sources.json names).
                    Required on a done-unlanded entry: [] means "touches no component",
                    and the release gate blocks while it is unknown.
    done_since      ISO date on which the work first became done-unlanded. Set once, never
                    bumped by `update`: the stale-debt clock cannot be reset by an edit.
    owner_decision  optional: the owner's dated instruction behind the item
    updated         ISO date of the last change to this entry

Check rules, each one an exit-1 error:

    novel           a scratch directory or branch that is not in the census: new work that
                    nobody has filed. No wildcard ignore pattern can silence it; it needs an
                    entry, or an exact ignore pattern with a reason, and `census --accept`.
    uncovered       a censused scratch directory or branch with no entry and no ignore pattern
    vanished        an entry whose "where" no longer exists, or a sha that is no longer on its
                    branch, while its state still claims live work
    stale-debt      a done-unlanded entry whose done_since is older than STALE_DAYS days
    not-landed      a landed entry whose landed_in is not verifiable locally: a main sha that
                    is not an ancestor of the published ref, a release version whose component
                    is not in release-sources.json, a doc that is not a tracked file of a
                    configured repository, or a fork sha that no remote branch contains
    superseded      a superseded-by entry whose commits are not contained in its successor
    unreviewed      entries that only a machine ever classified (one aggregated error)
    schema          a malformed entry, an unknown state, a wildcard ignore pattern that is
                    not declared with "glob": true, or a repository that cannot be read
    release         (release mode) finished but unlanded work in a component of the release,
                    or a done-unlanded entry whose components are unknown

Warnings do not fail the gate: criterion-debt (a not-met reconciliation row served by
done-unlanded work), and a scratch-only directory that looks like authored work.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import fnmatch
import json
import os
import re
import subprocess
import sys

STALE_DAYS = 3
KINDS = {"scratch-dir", "branch", "fork-branch", "tool", "doc", "defect-fix"}
SIMPLE_STATES = {"in-progress", "done-unlanded", "landed", "scratch-only", "unreviewed"}
LIVE_STATES = {"in-progress", "done-unlanded", "scratch-only", "unreviewed"}
TODAY = _dt.date.today()
WILDCARD = re.compile(r"[*?\[]")
# The seeder once wrote this reason on every branch it could not place. It restates being
# unlanded, so it must not pass as a reason to abandon work.
MACHINE_REASON = "not in the published main"
# A scratch-only directory holding one of these looks like authored work, not evidence.
AUTHORED_MARKS = ("build.ps1", "build.sh", "tests", "test", "PROVENANCE", "PROVENANCE.md",
                  "pyproject.toml", "meson.build", "CMakeLists.txt")


# ---------------------------------------------------------------- infrastructure


def location_root() -> str:
    """The workspace root this copy of the tool sits in, from its own path.

    The tool runs from two places: <repo>/tools/win/ledger (this file in the repository) and
    <workspace>/scratch/ledger (the operator copy that the session-start hook calls). Both are
    under the workspace root, which is the first ancestor that holds the repository directory.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    candidate = here
    for _ in range(5):
        candidate = os.path.dirname(candidate)
        if not candidate:
            break
        if os.path.isdir(os.path.join(candidate, "bc250-win")):
            return candidate
    # No repository directory above us: fall back on the two known layouts.
    parts = os.path.abspath(here).replace("\\", "/").split("/")
    up = 4 if parts[-3:-1] == ["tools", "win"] else 2
    return os.path.abspath(os.path.join(here, *([".."] * up)))


def default_root() -> str:
    env = os.environ.get("BC250_ROOT")
    if env:
        return os.path.abspath(env)
    return location_root()


def self_name() -> str:
    """How to spell this script in a hint, relative to its own workspace root."""
    try:
        rel = os.path.relpath(os.path.abspath(__file__), location_root())
    except ValueError:
        return os.path.basename(__file__)
    if rel.startswith(".."):
        return os.path.basename(__file__)
    return rel.replace("\\", "/")


SELF = self_name()
# The seeding script carries this workspace's own list of work, so it stays with the operator copy and
# out of every repository. The hints below therefore name it without a path.
SEED = "seed_ledger.py, in the workspace copy of this tool"


def ledger_path(root: str) -> str:
    return os.path.join(root, "LEDGER.json")


def load(root: str) -> dict:
    path = ledger_path(root)
    if not os.path.exists(path):
        die(f"no ledger at {path}: seed it with {SEED}")
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)


def save(root: str, data: dict) -> None:
    data["updated"] = TODAY.isoformat()
    with open(ledger_path(root), "w", encoding="utf-8", newline="\n") as fh:
        json.dump(data, fh, indent=2, ensure_ascii=True)
        fh.write("\n")


def die(msg: str) -> "NoReturn":  # noqa: F821
    print(f"ledger: {msg}", file=sys.stderr)
    raise SystemExit(2)


def git(repo: str, *args: str) -> tuple[int, str]:
    try:
        proc = subprocess.run(
            ["git", "-C", repo, *args],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
    except OSError as exc:  # git missing
        return 127, str(exc)
    return proc.returncode, (proc.stdout or "").strip()


def parse_date(value: str) -> _dt.date | None:
    try:
        return _dt.date.fromisoformat((value or "").strip())
    except ValueError:
        return None


# ---------------------------------------------------------------- the world


class World:
    """What exists on disk right now: scratch directories and git branches."""

    def __init__(self, root: str, data: dict):
        self.root = root
        self.data = data
        self.repos: dict[str, dict] = data.get("repos", {})
        self.umbrellas: list[str] = data.get("umbrellas", [])
        self.dirs: list[str] = []
        self.branches: dict[str, str] = {}      # "<repo>:<branch>" -> sha
        self.repo_missing: list[str] = []
        self._scan_dirs()
        self._scan_branches()

    def _scan_dirs(self) -> None:
        scratch = os.path.join(self.root, "scratch")
        if not os.path.isdir(scratch):
            return
        for name in sorted(os.listdir(scratch)):
            if not os.path.isdir(os.path.join(scratch, name)):
                continue
            self.dirs.append(f"scratch/{name}")
            if name in self.umbrellas:
                base = os.path.join(scratch, name)
                for sub in sorted(os.listdir(base)):
                    if os.path.isdir(os.path.join(base, sub)):
                        self.dirs.append(f"scratch/{name}/{sub}")

    def _scan_branches(self) -> None:
        for key, cfg in self.repos.items():
            path = os.path.join(self.root, cfg["path"])
            if not os.path.isdir(os.path.join(path, ".git")):
                self.repo_missing.append(key)
                continue
            code, out = git(path, "for-each-ref", "--format=%(refname:short) %(objectname:short=8)", "refs/heads")
            if code != 0:
                self.repo_missing.append(key)
                continue
            # Every local head is enumerated. A branch_prefix is a classification hint for the
            # seeder, never an enumeration filter: an unprefixed branch must not be invisible.
            for line in out.splitlines():
                if not line.strip():
                    continue
                name, _, sha = line.rpartition(" ")
                self.branches[f"{key}:{name}"] = sha

    def repo_dir(self, key: str) -> str | None:
        cfg = self.repos.get(key)
        if not cfg:
            return None
        return os.path.join(self.root, cfg["path"])


def split_where(where: str) -> tuple[str | None, str | None, str | None]:
    """Return (repo, branch, sha) for a repo reference, or (None, None, None) for a path."""
    m = re.match(r"^([A-Za-z0-9_.-]+):([^@\s]+)(?:@([0-9a-fA-F]{7,40}))?$", where or "")
    if not m:
        return None, None, None
    return m.group(1), m.group(2), m.group(3)


# ---------------------------------------------------------------- checks


def finished_on(entry: dict) -> _dt.date | None:
    """The date the work became done-unlanded. `update` never bumps it."""
    return parse_date(entry.get("done_since") or entry.get("updated", ""))


class Report:
    def __init__(self) -> None:
        self.errors: list[tuple[str, str]] = []
        self.warnings: list[tuple[str, str]] = []

    def error(self, rule: str, text: str) -> None:
        self.errors.append((rule, text))

    def warn(self, rule: str, text: str) -> None:
        self.warnings.append((rule, text))

    def by_rule(self) -> dict[str, int]:
        out: dict[str, int] = {}
        for rule, _ in self.errors:
            out[rule] = out.get(rule, 0) + 1
        return out

    def print(self) -> int:
        for rule, text in self.errors:
            print(f"ERROR  {rule:<15} {text}")
        for rule, text in self.warnings:
            print(f"warn   {rule:<15} {text}")
        counts = self.by_rule()
        print(f"\n{len(self.errors)} error(s), {len(self.warnings)} warning(s)")
        if counts:
            # A permanently red gate hides a new failure, so the rules are counted separately:
            # a rule that was 0 yesterday and is 1 today is visible without reading every line.
            print("  errors by rule: " + ", ".join(f"{r} {n}" for r, n in sorted(counts.items())))
        return 1 if self.errors else 0


def entry_state_kind(state: str) -> str:
    if state.startswith("superseded-by:"):
        return "superseded"
    if state.startswith("abandoned:"):
        return "abandoned"
    return state


def check_schema(data: dict, report: Report) -> None:
    seen: set[str] = set()
    ids = {e.get("id") for e in data.get("entries", [])}
    for entry in data.get("entries", []):
        eid = entry.get("id") or "<no id>"
        for field in ("id", "kind", "where", "what", "serves", "state", "updated"):
            if field not in entry:
                report.error("schema", f"{eid}: missing field {field}")
        if eid in seen:
            report.error("schema", f"{eid}: duplicate id")
        seen.add(eid)
        if entry.get("kind") not in KINDS:
            report.error("schema", f"{eid}: kind {entry.get('kind')!r} is not one of {sorted(KINDS)}")
        state = entry.get("state", "")
        kind = entry_state_kind(state)
        if kind == "superseded":
            successor = state.split(":", 1)[1]
            if successor not in ids:
                report.error("schema", f"{eid}: superseded-by names unknown id {successor!r}")
        elif kind == "abandoned":
            reason = state.split(":", 1)[1].strip()
            if not reason:
                report.error("schema", f"{eid}: abandoned needs a reason")
            elif reason.lower().startswith(MACHINE_REASON):
                report.error(
                    "schema",
                    f"{eid}: abandoned with the machine reason {reason[:40]!r}: that restates being "
                    "unlanded. Use unreviewed, or give a reason to abandon the work.",
                )
        elif state not in SIMPLE_STATES:
            report.error("schema", f"{eid}: unknown state {state!r}")
        if not isinstance(entry.get("serves", []), list):
            report.error("schema", f"{eid}: serves must be a list")
        if parse_date(entry.get("updated", "")) is None:
            report.error("schema", f"{eid}: updated {entry.get('updated')!r} is not an ISO date")
        if state == "landed" and not entry.get("landed_in"):
            report.error("schema", f"{eid}: state landed with no landed_in (nothing is done until landed_in is set)")
        if state != "landed" and entry.get("landed_in"):
            report.warn("schema", f"{eid}: landed_in set while state is {state}")
        if state == "done-unlanded":
            if "components" not in entry:
                report.error(
                    "schema",
                    f"{eid}: done-unlanded without components: set the release components it "
                    "touches, or [] for none. An unknown field cannot gate a release.",
                )
            elif not isinstance(entry.get("components"), list):
                report.error("schema", f"{eid}: components must be a list")
            if entry.get("done_since") and parse_date(entry["done_since"]) is None:
                report.error("schema", f"{eid}: done_since {entry['done_since']!r} is not an ISO date")
    for item in data.get("ignore", []):
        pattern = item.get("pattern", "")
        if not item.get("reason", "").strip():
            report.error("schema", f"ignore {pattern!r}: no reason")
        if WILDCARD.search(pattern) and not item.get("glob"):
            report.error(
                "schema",
                f"ignore {pattern!r}: a wildcard pattern needs \"glob\": true, which says in the "
                "ledger that it covers a family of machine-made names. It never silences new work: "
                "only the census does that.",
            )


def covered_keys(data: dict) -> set[str]:
    """Every world key an entry speaks for, including a parent of a deeper `where`."""
    covered: set[str] = set()
    for entry in data.get("entries", []):
        where = entry.get("where", "")
        repo, branch, _ = split_where(where)
        if repo:
            covered.add(f"{repo}:{branch}")
        else:
            covered.add(where.replace("\\", "/").strip("/"))
    return covered


def covers(covered: set[str], key: str) -> bool:
    """An entry covers a directory when it names it, or names something inside it."""
    if key in covered:
        return True
    return any(c.startswith(key + "/") for c in covered)


def world_keys(world: World) -> list[str]:
    return list(world.dirs) + sorted(world.branches)


def check_coverage(data: dict, world: World, report: Report) -> None:
    covered = covered_keys(data)
    patterns = [(i.get("pattern", ""), i.get("reason", "")) for i in data.get("ignore", [])]

    def ignored(key: str) -> bool:
        return any(fnmatch.fnmatch(key, pat) for pat, _ in patterns)

    for key in world_keys(world):
        if not covers(covered, key) and not ignored(key):
            report.error("uncovered", f"{key}: no ledger entry and no ignore pattern")
    for key in world.repo_missing:
        # A repository that cannot be read drops its whole branch set out of the world. That
        # must never leave the gate green: four of the configured clones live in scratch.
        report.error("schema", f"{key}: repository not readable at the configured path")


def check_novel(data: dict, world: World, report: Report) -> None:
    """New work nobody filed. No wildcard ignore can pre-absolve a name that does not exist yet."""
    known = set(data.get("census", []))
    covered = covered_keys(data)
    for key in world_keys(world):
        if key in known or covers(covered, key):
            continue
        report.error(
            "novel",
            f"{key}: new and unfiled. Add an entry (ledger.py add), or an exact ignore pattern "
            "with a reason, then record it with `ledger.py census --accept`.",
        )


def check_scratch_only(data: dict, world: World, report: Report) -> None:
    """scratch-only is the cheapest answer to the gate, so authored work in it is re-asked."""
    for entry in data.get("entries", []):
        if entry.get("state") != "scratch-only":
            continue
        where = entry.get("where", "")
        if split_where(where)[0]:
            continue
        path = os.path.join(world.root, where.replace("/", os.sep))
        if not os.path.isdir(path):
            continue
        marks = [m for m in AUTHORED_MARKS if os.path.exists(os.path.join(path, m))]
        if marks:
            report.warn(
                "scratch-only",
                f"{entry['id']}: {where} holds {', '.join(marks)}: this looks like authored work, "
                "not evidence. Re-ask whether it is done-unlanded with a landing step.",
            )


def check_where_exists(data: dict, world: World, report: Report) -> None:
    live = LIVE_STATES
    for entry in data.get("entries", []):
        state = entry.get("state", "")
        if state not in live:
            continue
        where = entry.get("where", "")
        repo, branch, sha = split_where(where)
        if repo:
            if repo not in world.repos:
                report.error("vanished", f"{entry['id']}: unknown repository {repo!r} in where")
                continue
            have = world.branches.get(f"{repo}:{branch}")
            if have is None:
                repo_dir = world.repo_dir(repo)
                code, _ = git(repo_dir, "rev-parse", "--verify", "-q", f"refs/heads/{branch}")
                if code != 0:
                    report.error(
                        "vanished",
                        f"{entry['id']}: branch {repo}:{branch} is gone while the state is {state}",
                    )
                    continue
            elif sha and not have.startswith(sha[:8]) and not sha.startswith(have):
                # The branch moved. Forward is normal work; a rewrite that drops the recorded
                # commit means the entry no longer describes anything that exists.
                code, _ = git(world.repo_dir(repo), "merge-base", "--is-ancestor", sha, branch)
                if code == 0:
                    report.warn("drift", f"{entry['id']}: {repo}:{branch} is {have}, the entry says {sha}")
                else:
                    report.error(
                        "vanished",
                        f"{entry['id']}: {sha} is no longer on {repo}:{branch} (now {have}) while "
                        f"the state is {state}: the branch was rewritten under the entry",
                    )
        else:
            if not os.path.exists(os.path.join(world.root, where.replace("/", os.sep))):
                report.error("vanished", f"{entry['id']}: {where} does not exist while the state is {state}")


def check_stale(data: dict, report: Report) -> None:
    for entry in data.get("entries", []):
        if entry.get("state") != "done-unlanded":
            continue
        when = finished_on(entry)
        if when is None:
            continue
        age = (TODAY - when).days
        if age > STALE_DAYS:
            report.error(
                "stale-debt",
                f"{entry['id']}: finished work unlanded for {age} days since {when.isoformat()} "
                f"(serves {', '.join(entry.get('serves', [])) or '-'})",
            )


def check_superseded(data: dict, world: World, report: Report) -> None:
    """A successor that does not contain its predecessor's commits has not superseded anything."""
    by_id = {e.get("id"): e for e in data.get("entries", [])}
    for entry in data.get("entries", []):
        state = entry.get("state", "")
        if not state.startswith("superseded-by:"):
            continue
        successor = by_id.get(state.split(":", 1)[1])
        if successor is None:
            continue  # reported by check_schema
        repo, branch, sha = split_where(entry.get("where", ""))
        srepo, sbranch, ssha = split_where(successor.get("where", ""))
        if not repo or repo != srepo or not sbranch:
            continue  # not two branches of one repository: nothing to verify locally
        repo_dir = world.repo_dir(repo)
        if not repo_dir:
            continue
        target = ssha or sbranch
        code, _ = git(repo_dir, "merge-base", "--is-ancestor", sha or branch, target)
        if code != 0:
            _, out = git(repo_dir, "rev-list", "--count", f"{sha or branch}", f"^{target}")
            lost = out.strip() or "?"
            report.error(
                "superseded",
                f"{entry['id']}: {lost} commit(s) of {repo}:{branch} are not in "
                f"{successor['id']} ({successor.get('where')}): it supersedes nothing",
            )


def check_unreviewed(data: dict, report: Report) -> None:
    rows = [e for e in data.get("entries", []) if e.get("state") == "unreviewed"]
    if rows:
        report.error(
            "unreviewed",
            f"{len(rows)} entries were classified by a machine and never reviewed: "
            "ledger.py list --state unreviewed. Each needs a state a person stands behind.",
        )


def release_components(root: str, data: dict) -> set[str]:
    rel = data.get("release_sources", "scratch/gui/wt-setup/tools/release/release-sources.json")
    path = os.path.join(root, rel.replace("/", os.sep))
    if not os.path.exists(path):
        return set()
    try:
        with open(path, "r", encoding="utf-8") as fh:
            src = json.load(fh)
    except (OSError, ValueError):
        return set()
    return {f.get("component", "") for f in src.get("files", [])} - {""}


def doc_is_tracked(root: str, data: dict, rel: str) -> bool:
    """True when rel is a file git tracks in one of the configured repositories."""
    rel = rel.replace("\\", "/").strip("/")
    for cfg in data.get("repos", {}).values():
        base = cfg.get("path", "").replace("\\", "/").strip("/")
        if not base or not rel.startswith(base + "/"):
            continue
        inner = rel[len(base) + 1:]
        code, _ = git(os.path.join(root, base.replace("/", os.sep)),
                      "ls-files", "--error-unmatch", "--", inner)
        if code == 0:
            return True
    return False


def remote_holders(repo_dir: str, rev: str) -> list[str]:
    """Remote branches that contain rev, excluding the upstream author's remote."""
    code, out = git(repo_dir, "branch", "-r", "--contains", rev)
    if code != 0:
        return []
    holders = []
    for line in out.splitlines():
        ref = line.strip().lstrip("* ").split(" ")[0]
        if not ref or "->" in line or ref.endswith("/HEAD") or ref.startswith("upstream/"):
            continue
        holders.append(ref)
    return holders


def check_landed(root: str, data: dict, world: World, report: Report) -> None:
    components = release_components(root, data)
    for entry in data.get("entries", []):
        if entry.get("state") != "landed":
            continue
        target = entry.get("landed_in", "")
        eid = entry["id"]
        if target.startswith("main:"):
            sha = target.split(":", 1)[1]
            repo_key = entry.get("landed_repo", "bc250-win")
            repo_dir = world.repo_dir(repo_key)
            published = (world.repos.get(repo_key) or {}).get("published_ref", "origin/main")
            if not repo_dir:
                report.error("not-landed", f"{eid}: unknown repository {repo_key!r}")
                continue
            code, _ = git(repo_dir, "merge-base", "--is-ancestor", sha, published)
            if code != 0:
                report.error(
                    "not-landed",
                    f"{eid}: {sha} is not an ancestor of {repo_key} {published}",
                )
        elif target.startswith("release:"):
            wanted = entry.get("components", [])
            if not wanted:
                report.error("not-landed", f"{eid}: landed in a release but names no components")
                continue
            missing = [c for c in wanted if c not in components]
            if missing:
                report.error(
                    "not-landed",
                    f"{eid}: release component(s) {', '.join(missing)} are not in release-sources.json",
                )
        elif target.startswith("doc:"):
            rel = target.split(":", 1)[1]
            if not os.path.exists(os.path.join(root, rel.replace("/", os.sep))):
                report.error("not-landed", f"{eid}: document {rel} does not exist")
            elif not doc_is_tracked(root, data, rel):
                # A file that merely exists is no landing: the ledger could then declare itself
                # landed by naming its own file. Only a tracked file of a repository counts.
                report.error(
                    "not-landed",
                    f"{eid}: {rel} is not a tracked file of any configured repository, so the "
                    "work is written down only in this workspace",
                )
        elif target.startswith("fork:"):
            ref = target.split(":", 1)[1]
            repo_key, branch, sha = split_where(ref)
            repo_dir = world.repo_dir(repo_key) if repo_key else None
            if not repo_dir:
                report.error("not-landed", f"{eid}: unknown fork repository in {target!r}")
                continue
            holders = remote_holders(repo_dir, sha or branch)
            if not holders:
                report.error(
                    "not-landed",
                    f"{eid}: no remote branch of {repo_key} contains {sha or branch}, so the work "
                    "is not published",
                )
        else:
            report.error(
                "not-landed",
                f"{eid}: landed_in {target!r} is not main:<sha>, release:<v>, doc:<path> or fork:<repo>:<branch>@<sha>",
            )


ROW = re.compile(r"^\|\s*(M1[345]\.\d+)\b")


def reconciliation_rows(root: str, data: dict) -> dict[str, str]:
    """criterion id -> the row's own state cell, from docs/m15-reconciliation.md."""
    rel = data.get("reconciliation", "bc250-win/docs/m15-reconciliation.md")
    path = os.path.join(root, rel.replace("/", os.sep))
    rows: dict[str, str] = {}
    if not os.path.exists(path):
        return rows
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = ROW.match(line)
            if not m:
                continue
            cells = [c.strip() for c in line.strip().strip("|").split("|")]
            if len(cells) >= 3:
                rows[m.group(1)] = cells[2]
    return rows


def row_is_met(cell: str) -> bool:
    low = cell.lower()
    if low.startswith("not met") or low.startswith("partly") or low.startswith("partially"):
        return False
    return low.startswith("met")


def check_criteria(root: str, data: dict, report: Report) -> None:
    rows = reconciliation_rows(root, data)
    if not rows:
        report.warn("criterion-debt", "no M13/M14/M15 rows read from the reconciliation document")
        return
    for entry in data.get("entries", []):
        if entry.get("state") != "done-unlanded":
            continue
        for served in entry.get("serves", []):
            cell = rows.get(served)
            if cell is None or row_is_met(cell):
                continue
            # An observation about the reconciliation table, not an inconsistency of the ledger:
            # a warning, so that `check` can reach zero errors and a new error stays visible.
            report.warn(
                "criterion-debt",
                f"{served} reads \"{cell}\" while {entry['id']} ({entry.get('where')}) is done-unlanded",
            )


def check_release(root: str, data: dict, report: Report, version: str) -> None:
    components = release_components(root, data)
    if not components:
        report.error("release", "release-sources.json carries no components: cannot gate the build")
        return
    for entry in data.get("entries", []):
        if entry.get("state") != "done-unlanded":
            continue
        if "components" not in entry:
            # Fail closed: an entry that never said which components it touches cannot be
            # declared harmless to this release.
            report.error(
                "release",
                f"{version}: {entry['id']} is finished but unlanded and its components are "
                f"unknown ({entry.get('where')}): set components, or [] for none",
            )
            continue
        # A component that the release does not carry yet is still release work: new components
        # are exactly how a shipped surface grows.
        touched = list(entry.get("components") or [])
        if touched:
            new = [c for c in touched if c not in components]
            tail = f" (not yet in release-sources.json: {', '.join(new)})" if new else ""
            report.error(
                "release",
                f"{version}: {entry['id']} is finished but unlanded and serves "
                f"release component(s) {', '.join(touched)}{tail} "
                f"({entry.get('what', '')[:90]})",
            )
        else:
            # Work that touches no component can still be release work: an entry whose serves
            # names this release (its version, or its tail such as "tester.11") blocks it.
            tag = version.split("-")[-1].lower() if version else ""
            named = [s for s in entry.get("serves", [])
                     if s.lower() == version.lower() or (tag and tag in s.lower())]
            if named:
                report.error(
                    "release",
                    f"{version}: {entry['id']} serves {', '.join(named)} and is finished but "
                    f"unlanded ({entry.get('where')})",
                )


def run_all_checks(root: str, data: dict, world: World, report: Report) -> None:
    check_schema(data, report)
    check_novel(data, world, report)
    check_coverage(data, world, report)
    check_where_exists(data, world, report)
    check_stale(data, report)
    check_landed(root, data, world, report)
    check_superseded(data, world, report)
    check_unreviewed(data, report)
    check_scratch_only(data, world, report)
    check_criteria(root, data, report)


def cmd_check(args: argparse.Namespace) -> int:
    root = args.root
    data = load(root)
    world = World(root, data)
    report = Report()
    # The release gate runs every rule: a release must not be cut over a ledger that also
    # claims a landing nobody can verify, or hides a scratch directory nobody filed.
    run_all_checks(root, data, world, report)
    if args.release:
        check_release(root, data, report, args.release)
        print(f"release gate for {args.release}: {len(data.get('entries', []))} entries considered")
    print(
        f"ledger check: {len(data.get('entries', []))} entries, "
        f"{len(world.dirs)} scratch directories, {len(world.branches)} branches, "
        f"{len(data.get('ignore', []))} ignore patterns, "
        f"{len(data.get('census', []))} censused names"
    )
    return report.print()


# ---------------------------------------------------------------- brief / render


def counts(data: dict) -> dict[str, int]:
    out: dict[str, int] = {}
    for entry in data.get("entries", []):
        key = entry_state_kind(entry.get("state", "?"))
        out[key] = out.get(key, 0) + 1
    return out


def overdue(data: dict) -> list[dict]:
    rows = [e for e in data.get("entries", []) if e.get("state") in ("done-unlanded", "in-progress")]
    # Oldest debt first, by the day the work was finished, not by the last edit of its text.
    rows.sort(key=lambda e: (e.get("state") != "done-unlanded",
                             e.get("done_since") or e.get("updated", "")))
    return rows


def cmd_brief(args: argparse.Namespace) -> int:
    root = args.root
    if not os.path.exists(ledger_path(root)):
        print("!" * 72)
        print("LEDGER MISSING: no LEDGER.json at " + ledger_path(root))
        print("Unfinished work is untracked. Restore it from git-less backup or re-seed it")
        print(f"with {SEED} before doing any work.")
        print("!" * 72, file=sys.stderr)
        print("ledger: LEDGER.json is missing - unfinished work is untracked", file=sys.stderr)
        return 2
    data = load(root)
    c = counts(data)
    rows = overdue(data)
    total = len(data.get("entries", []))
    print("BC-250 work ledger (LEDGER.json) - the source of truth for unfinished work")
    print(
        f"  {total} entries: {c.get('done-unlanded', 0)} done-unlanded, "
        f"{c.get('in-progress', 0)} in-progress, {c.get('landed', 0)} landed, "
        f"{c.get('scratch-only', 0)} scratch-only, {c.get('superseded', 0)} superseded, "
        f"{c.get('abandoned', 0)} abandoned"
    )
    stale = [e for e in rows if e.get("state") == "done-unlanded"
             and (finished_on(e) or TODAY) < TODAY - _dt.timedelta(days=STALE_DAYS)]
    print(f"  {len(stale)} finished item(s) unlanded for more than {STALE_DAYS} days")
    # The session-start brief runs the gate itself. Nothing here depends on an agent
    # remembering to type `check`: a new scratch directory or branch shows up below.
    world = World(root, data)
    report = Report()
    run_all_checks(root, data, world, report)
    by_rule = report.by_rule()
    if report.errors:
        print(f"\n  GATE: {len(report.errors)} error(s): "
              + ", ".join(f"{r} {n}" for r, n in sorted(by_rule.items())))
        shown = [(r, t) for r, t in report.errors
                 if r in ("novel", "uncovered", "vanished", "schema", "superseded", "unreviewed")]
        for rule, text in shown[:12]:
            print(f"    {rule:<11} {text[:150]}")
        if len(shown) > 12:
            print(f"    ... and {len(shown) - 12} more: python {SELF} check")
        if by_rule.get("stale-debt"):
            print(f"    stale-debt  {by_rule['stale-debt']} finished item(s) past {STALE_DAYS} days "
                  "(listed below)")
    else:
        print("\n  GATE: clean (no novel, uncovered, vanished or unverifiable entry)")
    if rows:
        print(f"\n  top {min(10, len(rows))} overdue (oldest first):")
        for entry in rows[:10]:
            serves = ", ".join(entry.get("serves", [])) or "-"
            since = entry.get("done_since") or entry.get("updated", "????-??-??")
            print(
                f"    {since}  {entry.get('state'):<13} "
                f"{entry.get('id'):<28} {serves}"
            )
            print(f"        {entry.get('what', '')[:150]}")
            step = entry.get("landing_step")
            if step:
                print(f"        land: {step[:150]}")
    print("\n  every new scratch directory or branch gets an entry the same day;")
    print(f"  nothing is done until landed_in is set.  python {SELF} check")
    return 0


def cmd_render(args: argparse.Namespace) -> int:
    root = args.root
    data = load(root)
    out = os.path.join(root, "LEDGER.md")
    entries = data.get("entries", [])
    order = {"done-unlanded": 0, "in-progress": 1, "landed": 2, "scratch-only": 3,
             "superseded": 4, "abandoned": 5}
    groups: dict[str, list[dict]] = {}
    for entry in entries:
        groups.setdefault(entry_state_kind(entry.get("state", "?")), []).append(entry)
    lines = [
        "# BC-250 work ledger (generated view)",
        "",
        f"Generated from `LEDGER.json` on {TODAY.isoformat()} by `{SELF} render`.",
        "Do not edit this file: edit `LEDGER.json` (or use `ledger.py add` / `ledger.py update`).",
        "",
        "This file is local. It names scratch paths and lab flows, so it never goes into the",
        "public repository.",
        "",
        "## Counts",
        "",
        "| State | Entries |",
        "|---|---|",
    ]
    c = counts(data)
    for key in sorted(c, key=lambda k: order.get(k, 9)):
        lines.append(f"| {key} | {c[key]} |")
    lines += ["", f"Total: {len(entries)} entries, {len(data.get('ignore', []))} ignore patterns, "
                  f"{len(data.get('census', []))} censused names "
                  f"(census seeded {data.get('census_seeded', '-')}).",
              "",
              "A name outside the census is new work: `check` refuses it until an entry or an exact",
              "ignore pattern names it and `ledger.py census --accept` records it. done_since is the",
              "day the work became done-unlanded; an edit never moves it.", ""]
    for key in sorted(groups, key=lambda k: order.get(k, 9)):
        lines += [f"## {key} ({len(groups[key])})", "",
                  "| id | kind | where | serves | landed_in | since | what |",
                  "|---|---|---|---|---|---|---|"]
        for entry in sorted(groups[key],
                            key=lambda e: (e.get("done_since") or e.get("updated", ""),
                                           e.get("id", ""))):
            what = (entry.get("what", "") or "").replace("|", "/")
            lines.append(
                "| {id} | {kind} | `{where}` | {serves} | {landed} | {updated} | {what} |".format(
                    id=entry.get("id", ""),
                    kind=entry.get("kind", ""),
                    where=entry.get("where", ""),
                    serves=", ".join(entry.get("serves", [])) or "-",
                    landed=entry.get("landed_in", "") or "-",
                    updated=entry.get("done_since") or entry.get("updated", ""),
                    what=what,
                )
            )
        lines.append("")
    lines += ["## Ignore list", "", "| Pattern | Reason |", "|---|---|"]
    for item in data.get("ignore", []):
        lines.append(f"| `{item.get('pattern','')}` | {item.get('reason','').replace('|', '/')} |")
    lines.append("")
    with open(out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines))
    print(f"wrote {out} ({len(entries)} entries)")
    return 0


def cmd_list(args: argparse.Namespace) -> int:
    data = load(args.root)
    for entry in data.get("entries", []):
        if args.state and entry_state_kind(entry.get("state", "")) != args.state and entry.get("state") != args.state:
            continue
        if args.serves and args.serves not in entry.get("serves", []):
            continue
        if args.kind and entry.get("kind") != args.kind:
            continue
        print(f"{entry.get('id'):<30} {entry.get('state'):<20} {entry.get('where')}")
        print(f"    {entry.get('what','')}")
    return 0


def split_csv(value: str | None) -> list[str] | None:
    if value is None:
        return None
    return [v.strip() for v in value.split(",") if v.strip()]


def validate_state(data: dict, state: str) -> None:
    kind = entry_state_kind(state)
    ids = {e.get("id") for e in data.get("entries", [])}
    if kind == "superseded":
        successor = state.split(":", 1)[1].strip()
        if not successor:
            die("superseded-by needs the successor's id: superseded-by:<id>")
        if successor not in ids:
            die(f"superseded-by names unknown id {successor!r}")
    elif kind == "abandoned":
        reason = state.split(":", 1)[1].strip()
        if not reason:
            die("abandoned needs a reason: abandoned:<reason>")
        if reason.lower().startswith(MACHINE_REASON):
            die("abandoned must not restate being unlanded: use unreviewed, or give a real reason")
    elif state not in SIMPLE_STATES:
        die(f"unknown state {state!r}: one of {sorted(SIMPLE_STATES)}, "
            "superseded-by:<id> or abandoned:<reason>")


def cmd_add(args: argparse.Namespace) -> int:
    data = load(args.root)
    if any(e.get("id") == args.id for e in data.get("entries", [])):
        die(f"id {args.id} already exists: use `update`")
    validate_state(data, args.state)
    if args.state == "done-unlanded" and args.components is None:
        die("a done-unlanded entry needs --components (the release components it touches, "
            "or '' for none): an unknown field cannot gate a release")
    entry = {
        "id": args.id,
        "kind": args.kind,
        "where": args.where,
        "what": args.what,
        "serves": split_csv(args.serves) or [],
        "state": args.state,
        "landed_in": args.landed_in or "",
        "updated": TODAY.isoformat(),
    }
    if args.components is not None:
        entry["components"] = split_csv(args.components) or []
    if args.state == "done-unlanded":
        entry["done_since"] = TODAY.isoformat()
    if args.landing_step:
        entry["landing_step"] = args.landing_step
    if args.owner_decision:
        entry["owner_decision"] = args.owner_decision
    data.setdefault("entries", []).append(entry)
    save(args.root, data)
    print(f"added {args.id} ({args.state})")
    return 0


def cmd_update(args: argparse.Namespace) -> int:
    data = load(args.root)
    if args.state is not None:
        validate_state(data, args.state)
    for entry in data.get("entries", []):
        if entry.get("id") != args.id:
            continue
        was_done = entry.get("state") == "done-unlanded"
        for field, value in (
            ("kind", args.kind),
            ("where", args.where),
            ("what", args.what),
            ("state", args.state),
            ("landed_in", args.landed_in),
            ("landing_step", args.landing_step),
            ("owner_decision", args.owner_decision),
        ):
            if value is not None:
                entry[field] = value
        if args.serves is not None:
            entry["serves"] = split_csv(args.serves)
        if args.components is not None:
            entry["components"] = split_csv(args.components)
        if args.state == "landed" and not entry.get("landed_in"):
            die("state landed needs --landed-in (nothing is done until landed_in is set)")
        if entry.get("state") != "landed":
            entry["landed_in"] = entry.get("landed_in", "") if args.landed_in else ""
        if entry.get("state") == "done-unlanded":
            # Set once, on the day the work became finished-but-unlanded, and never bumped:
            # re-describing an entry must not restart the stale-debt clock.
            if not was_done or not entry.get("done_since"):
                entry.setdefault("done_since", TODAY.isoformat())
            if "components" not in entry and args.components is None:
                die("a done-unlanded entry needs --components (or '' for none)")
        entry["updated"] = TODAY.isoformat()
        save(args.root, data)
        print(f"updated {args.id} ({entry.get('state')})")
        return 0
    die(f"no entry with id {args.id}")


def cmd_census(args: argparse.Namespace) -> int:
    """Record today's world as known. A name enters the census only with a decision behind it."""
    data = load(args.root)
    world = World(args.root, data)
    known = set(data.get("census", []))
    covered = covered_keys(data)
    literal = [i.get("pattern", "") for i in data.get("ignore", [])
               if not WILDCARD.search(i.get("pattern", ""))]
    new = [k for k in world_keys(world) if k not in known]
    if not args.accept:
        for key in new:
            print(f"novel  {key}")
        print(f"{len(new)} name(s) outside the census, {len(known)} inside")
        return 1 if new else 0
    accepted, refused = [], []
    for key in new:
        if covers(covered, key) or key in literal:
            accepted.append(key)
        else:
            refused.append(key)
    if accepted:
        data["census"] = sorted(known | set(accepted))
        save(args.root, data)
    for key in accepted:
        print(f"accepted  {key}")
    for key in refused:
        print(f"REFUSED   {key}: no entry and no exact ignore pattern names it", file=sys.stderr)
    print(f"{len(accepted)} accepted, {len(refused)} refused, {len(data.get('census', []))} censused")
    return 1 if refused else 0


def cmd_ignore(args: argparse.Namespace) -> int:
    data = load(args.root)
    if WILDCARD.search(args.pattern) and not args.glob:
        die(f"{args.pattern!r} is a wildcard pattern: pass --glob to say so. A wildcard never "
            "silences new work; only `census --accept` does, and it needs an exact pattern.")
    for item in data.setdefault("ignore", []):
        if item.get("pattern") == args.pattern:
            item["reason"] = args.reason
            if args.glob:
                item["glob"] = True
            save(args.root, data)
            print(f"updated ignore {args.pattern}")
            return 0
    new = {"pattern": args.pattern, "reason": args.reason}
    if args.glob:
        new["glob"] = True
    data["ignore"].append(new)
    save(args.root, data)
    print(f"added ignore {args.pattern}")
    return 0


# ---------------------------------------------------------------- main


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="ledger.py", description=__doc__.splitlines()[0])
    p.add_argument("--root", default=default_root(), help="workspace root (default: BC250_ROOT)")
    sub = p.add_subparsers(dest="cmd", required=True)

    c = sub.add_parser("check", help="the gate: exit 1 on debt or drift")
    c.add_argument("--release", metavar="VERSION", help="release-gate mode: fail on unlanded work in the release")
    c.set_defaults(func=cmd_check)

    b = sub.add_parser("brief", help="one screen for session start")
    b.set_defaults(func=cmd_brief)

    r = sub.add_parser("render", help="write LEDGER.md")
    r.set_defaults(func=cmd_render)

    l = sub.add_parser("list", help="list entries")
    l.add_argument("--state")
    l.add_argument("--serves")
    l.add_argument("--kind")
    l.set_defaults(func=cmd_list)

    a = sub.add_parser("add", help="add an entry")
    a.add_argument("--id", required=True)
    a.add_argument("--kind", required=True, choices=sorted(KINDS))
    a.add_argument("--where", required=True)
    a.add_argument("--what", required=True)
    a.add_argument("--serves", default="")
    a.add_argument("--state", required=True)
    a.add_argument("--landed-in", dest="landed_in", default="")
    a.add_argument("--components")
    a.add_argument("--landing-step", dest="landing_step")
    a.add_argument("--owner-decision", dest="owner_decision")
    a.set_defaults(func=cmd_add)

    u = sub.add_parser("update", help="update an entry")
    u.add_argument("--id", required=True)
    u.add_argument("--kind", choices=sorted(KINDS))
    u.add_argument("--where")
    u.add_argument("--what")
    u.add_argument("--serves")
    u.add_argument("--state")
    u.add_argument("--landed-in", dest="landed_in")
    u.add_argument("--components")
    u.add_argument("--landing-step", dest="landing_step")
    u.add_argument("--owner-decision", dest="owner_decision")
    u.set_defaults(func=cmd_update)

    i = sub.add_parser("ignore", help="add or update an ignore pattern")
    i.add_argument("--pattern", required=True)
    i.add_argument("--reason", required=True)
    i.add_argument("--glob", action="store_true",
                   help="the pattern is a wildcard over a family of machine-made names")
    i.set_defaults(func=cmd_ignore)

    cn = sub.add_parser("census", help="list or accept names that are new to the census")
    cn.add_argument("--accept", action="store_true",
                    help="record the new names that an entry or an exact ignore pattern covers")
    cn.set_defaults(func=cmd_census)

    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    args.root = os.path.abspath(args.root)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
