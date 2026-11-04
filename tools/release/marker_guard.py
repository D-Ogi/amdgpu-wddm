#!/usr/bin/env python3
"""Marker guard: keep the fixes that code comments name through merges and rebases.

A fix or an optimisation in this project leaves a comment with its id (BD-075, C45, K184, M779, E52, or a
TODO/FIXME). This tool takes every such anchored comment in one or more SOURCE revisions, together with the code
that follows it, and looks for it in the HEAD revision (a train merge, a rebase). Findings:

  GONE      an id occurs fewer times in a file of HEAD than in that file of any source, and the file did not
            just move: a fix or its explanation was dropped (a merge took the other side, a rebase lost a
            hunk). Hard finding: exit 1.
  REWORDED  the anchored comment line is not in HEAD verbatim, but the id is still in the file as often as
            before: usually a rewrap or a deliberate update. Soft.
  CHANGED   the comment is there, but the code right after it matches no source's version: a merge result
            nobody wrote. Soft, but read it.

An id that HEAD removes on purpose goes into an accept file (one line: `<id> <path> <reason>`).

Usage:
  marker_guard.py --repo R --head train/b20 --source main --source fan/read-nct6686 [--paths driver tools]
                  [--accept accepted.txt] [--json out.json] [--baseline N]
"""
import argparse
import hashlib
import json
import re
import subprocess
import sys
from collections import Counter, defaultdict

ANCHOR = re.compile(r"\b(BD-\d{3}|C\d{2}|K\d{3}|M\d{3,4}|E\d{2}|TODO|FIXME)\b")
GREP_ERE = "BD-[0-9]{3}|C[0-9]{2}|K[0-9]{3}|M[0-9]{3,4}|E[0-9]{2}|TODO|FIXME"
COMMENT = re.compile(r"(//|/\*|^\s*\*|^\s*#|<#|^\s*;|--\s)")
EXTS = (".c", ".h", ".cpp", ".hpp", ".cc", ".cs", ".ps1", ".psm1", ".py", ".sh", ".rs", ".inf", ".hlsl",
        ".comp", ".js", ".inc", ".def")
CONTEXT = 6  # code lines after the anchor that identify the fix


def git(repo, *args):
    r = subprocess.run(["git", "-C", repo, *args], capture_output=True, text=True, encoding="utf-8",
                       errors="replace")
    if r.returncode != 0:
        raise SystemExit(f"git {' '.join(args)}: {r.stderr.strip()}")
    return r.stdout


class Blobs:
    """One `git cat-file --batch` process for every file read (a process per file took minutes)."""

    def __init__(self, repo):
        self.p = subprocess.Popen(["git", "-C", repo, "cat-file", "--batch"], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE)

    def read(self, rev, path):
        self.p.stdin.write(f"{rev}:{path}\n".encode())
        self.p.stdin.flush()
        hdr = self.p.stdout.readline().decode().split()
        if len(hdr) < 3 or hdr[1] == "missing":
            return ""
        n = int(hdr[2])
        data = self.p.stdout.read(n)
        self.p.stdout.read(1)
        return data.decode("utf-8", errors="replace")


def norm(s):
    return " ".join(s.split())


def code_after(lines, i):
    """The anchor's code: the code before a trailing comment, then the next CONTEXT code lines."""
    out = []
    m = COMMENT.search(lines[i])
    if m and lines[i][:m.start()].strip():
        out.append(norm(lines[i][:m.start()]))
    j = i + 1
    while j < len(lines) and len(out) < CONTEXT:
        s = lines[j].lstrip()
        if s and not COMMENT.match(s) and not s.startswith("*"):
            out.append(norm(lines[j]))
        j += 1
    return out


def anchors(repo, blobs, rev, paths):
    files = {f for f in git(repo, "ls-tree", "-r", "--name-only", rev, "--", *paths).splitlines()
             if f.endswith(EXTS)}
    # git grep exits 1 when nothing matches: a revision without anchors has none, which is no error.
    r = subprocess.run(["git", "-C", repo, "grep", "-I", "-l", "-E", GREP_ERE, rev, "--", *paths],
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode not in (0, 1):
        raise SystemExit(f"git grep {rev}: {r.stderr.strip()}")
    hits = r.stdout
    wanted = {h.split(":", 1)[1] for h in hits.splitlines() if ":" in h}
    out, counts = [], {}
    for f in sorted(files & wanted):
        lines = blobs.read(rev, f).splitlines()
        cnt = Counter()
        for i, ln in enumerate(lines):
            m = COMMENT.search(ln)
            if not m:
                continue
            c = ln[m.start():]
            ids = set(ANCHOR.findall(c))
            for a in ids:
                cnt[a] += 1
                ctx = code_after(lines, i)
                out.append({"id": a, "path": f, "line": i + 1, "comment": norm(c),
                            "code": hashlib.sha1("\n".join(ctx).encode()).hexdigest()[:12]})
        counts[f] = cnt
    return out, counts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--head", required=True)
    ap.add_argument("--source", action="append", required=True)
    ap.add_argument("--paths", nargs="*", default=["driver", "tools"])
    ap.add_argument("--accept")
    ap.add_argument("--baseline", type=int, default=0)
    ap.add_argument("--json")
    ap.add_argument("--brief", action="store_true", help="print GONE only")
    a = ap.parse_args()

    accepted = {}
    if a.accept:
        for ln in open(a.accept, encoding="utf-8"):
            p = ln.split(None, 2)
            if len(p) >= 2 and not ln.lstrip().startswith("#"):
                accepted[(p[0], p[1])] = p[2].strip() if len(p) > 2 else ""

    blobs = Blobs(a.repo)
    head, head_counts = anchors(a.repo, blobs, a.head, a.paths)
    head_by_comment = defaultdict(list)
    for h in head:
        head_by_comment[(h["id"], h["comment"])].append(h)
    head_total = Counter()
    for f, c in head_counts.items():
        head_total.update(c)

    src_codes = defaultdict(set)
    gone, reworded, changed = {}, {}, {}
    for src in a.source:
        srcs, counts = anchors(a.repo, blobs, src, a.paths)
        for s in srcs:
            src_codes[(s["id"], s["comment"])].add(s["code"])
        for f, cnt in counts.items():
            hc = head_counts.get(f, Counter())
            for i, n in cnt.items():
                if hc[i] >= n or (i, f) in accepted:
                    continue
                # a file that moved keeps its ids elsewhere: only a drop in the id's tree-wide count is GONE
                if f not in head_counts and head_total[i] >= n:
                    continue
                k = (i, f)
                if k not in gone or gone[k]["src_count"] < n:
                    gone[k] = {"id": i, "path": f, "src_count": n, "head_count": hc[i], "source": src}
        for s in srcs:
            k = (s["id"], s["comment"])
            if (s["id"], s["path"]) in gone:
                continue
            if k not in head_by_comment:
                reworded.setdefault(k, dict(s, source=src))
    for k, hs in head_by_comment.items():
        if k in src_codes and not any(h["code"] in src_codes[k] for h in hs):
            changed[k] = {"id": k[0], "comment": k[1], "head_at": [f'{h["path"]}:{h["line"]}' for h in hs]}
    blobs.p.stdin.close()

    print(f"marker guard: head {a.head}, sources {', '.join(a.source)}, paths {' '.join(a.paths)}")
    print(f"  anchors in head {len(head)}; GONE {len(gone)}, REWORDED {len(reworded)}, CHANGED {len(changed)}")
    for g in gone.values():
        print(f"  GONE     {g['id']:8} {g['path']}: {g['src_count']} in {g['source']}, {g['head_count']} in head")
    if not a.brief:
        for r in reworded.values():
            print(f"  REWORDED {r['id']:8} {r['path']}:{r['line']} ({r['source']}): {r['comment'][:100]}")
        for c in changed.values():
            print(f"  CHANGED  {c['id']:8} {', '.join(c['head_at'])[:70]}: {c['comment'][:90]}")
    if a.json:
        with open(a.json, "w", encoding="utf-8", newline="\n") as f:
            json.dump({"gone": list(gone.values()), "reworded": list(reworded.values()),
                       "changed": list(changed.values())}, f, indent=1)
    return 1 if len(gone) > a.baseline else 0


# ---------------------------------------------------------------------------------------------------------------
# Both lines (BD-204 step 2): a fix marker that one RADV WDDM winsys line has and another line does not.
#
#   marker_guard.py lines --config winsys-lines.json --sources release-sources.json --root <BC250_ROOT>
#                         [--repo <Mesa checkout>] [--line NAME=REV ...] [--json out.json]
#
# The release builds its Vulkan ICDs from more than one Mesa line (the system ICD, its 32-bit build, the hosted ICD
# of the D3D11 and D3D12 shells and that of the desktop route). A fix ported to one line and not to the other is
# invisible to the marker guard above, which follows one line. Here each line is the commit that release-sources.json
# pins for a payload path of the config, and a marker is a BD id in a comment under the config's paths or in the
# subject of a commit since the merge base of the pair that touches those paths. For each pair of lines the gate
# lists every marker that one line has and the other does not:
#
#   FAIL      no entry of the config accepts the difference: port the fix, or record why the line does without it.
#   ACCEPTED  an entry says why: "contract" (one of the four differences the hosted line may have: kernel object
#             owner, presentation, device-loss channel, call macro) or "line-only" (the code it fixes is on this
#             line only).
#   EQUIVALENT the other line has the same fix under another name: each commit the entry names must be in that line.
#   OPEN      known drift at the pins the entry was written for, still to be ported (BD-204 step 3). Printed on
#             every run, does not fail.
#
# An entry that no longer matches a difference (the fix was ported, or the marker went) fails as STALE, so the list
# shrinks with the drift. A "both_lines" fix of the config is a named fix that every listed line must hold: its
# pattern must match under its path at each line's commit.

MARKER_KINDS = ("contract", "line-only", "equivalent", "open")


def line_markers(repo, blobs, rev, base, paths, pattern):
    """The markers of one line: ids in comments under the paths, and ids in the subjects of the commits from the
    merge base of the pair to the line that touch the paths."""
    found = defaultdict(set)
    _, counts = anchors(repo, blobs, rev, paths)
    for f, cnt in counts.items():
        for i in cnt:
            if pattern.fullmatch(i):
                found[i].add(f"comment in {f}")
    log = git(repo, "log", "--format=%h %s", f"{base}..{rev}", "--", *paths)
    for ln in log.splitlines():
        h, _, subject = ln.partition(" ")
        for i in set(re.findall(r"\b[A-Z]+-\d+\b|\b[A-Z]\d+\b", subject)):
            if pattern.fullmatch(i):
                found[i].add(f"subject of {h}")
    return found


def is_ancestor(repo, commit, rev):
    r = subprocess.run(["git", "-C", repo, "merge-base", "--is-ancestor", commit, rev], capture_output=True)
    return r.returncode == 0


def has_commit(repo, rev):
    r = subprocess.run(["git", "-C", repo, "cat-file", "-e", f"{rev}^{{commit}}"], capture_output=True)
    return r.returncode == 0


def lines_check(config, revs, repo):
    """The findings of the both-lines gate: a list of (status, text). FAIL and STALE fail the gate."""
    pattern = re.compile(config.get("marker_pattern", r"BD-\d{3}"))
    paths = config["paths"]
    out = []
    accept = {}
    for e in config.get("accept", []):
        key = (e.get("marker"), e.get("on"), e.get("missing"))
        problem = None
        if e.get("kind") not in MARKER_KINDS:
            problem = f"kind {e.get('kind')!r} is not one of {', '.join(MARKER_KINDS)}"
        elif not isinstance(e.get("reason"), str) or len(e["reason"].strip()) < 40:
            problem = "the reason must be a sentence of at least 40 characters"
        elif e["kind"] == "equivalent" and not e.get("commits"):
            problem = "an equivalent entry names the commits of the other line that carry the fix"
        elif key[1] not in revs or key[2] not in revs:
            problem = f"line {key[1]!r} or {key[2]!r} is not a line of the config"
        if problem:
            out.append(("FAIL", f"accept entry {key[0]} on {key[1]}, missing on {key[2]}: {problem}"))
        else:
            accept[key] = e
    used = set()
    blobs = Blobs(repo)
    try:
        cache = {}
        for a, b in config["pairs"]:
            base = git(repo, "merge-base", revs[a], revs[b]).strip()
            for n in (a, b):
                if (n, base) not in cache:
                    cache[(n, base)] = line_markers(repo, blobs, revs[n], base, paths, pattern)
            ma, mb = cache[(a, base)], cache[(b, base)]
            for on, missing, have, lack in ((a, b, ma, mb), (b, a, mb, ma)):
                for m in sorted(set(have) - set(lack)):
                    where = sorted(have[m])[0]
                    e = accept.get((m, on, missing))
                    if e is None:
                        out.append(("FAIL", f"{m} is on {on} ({where}) and not on {missing}: port the fix, or add an "
                                            f"accept entry that says why {missing} does without it"))
                        continue
                    used.add((m, on, missing))
                    if e["kind"] == "equivalent":
                        absent = [c for c in e["commits"] if not is_ancestor(repo, c, revs[missing])]
                        if absent:
                            out.append(("FAIL", f"{m} is on {on} and not on {missing}; the accept entry names "
                                                f"{', '.join(c[:8] for c in absent)} as its equivalent, which "
                                                f"{missing} {revs[missing][:8]} does not hold"))
                            continue
                        out.append(("EQUIVALENT", f"{m} on {on}, on {missing} as {', '.join(c[:8] for c in e['commits'])}: "
                                                  f"{e['reason']}"))
                    elif e["kind"] == "open":
                        out.append(("OPEN", f"{m} on {on}, not on {missing}: {e['reason']}"))
                    else:
                        out.append(("ACCEPTED", f"{m} on {on}, not on {missing} ({e['kind']}): {e['reason']}"))
    finally:
        blobs.p.stdin.close()
    for key in sorted(set(accept) - used):
        out.append(("STALE", f"accept entry {key[0]} on {key[1]}, missing on {key[2]} matches no difference any more "
                             f"(the fix reached both lines or the marker went): remove the entry"))
    for fix in config.get("both_lines", []):
        for n in fix.get("lines") or sorted(revs):
            r = subprocess.run(["git", "-C", repo, "grep", "-q", "-E", fix["pattern"], revs[n], "--", fix["path"]],
                               capture_output=True)
            if r.returncode != 0:
                out.append(("FAIL", f"both-lines fix '{fix['name']}' is not on {n} {revs[n][:8]}: no "
                                    f"/{fix['pattern']}/ under {fix['path']}"))
            else:
                out.append(("BOTH", f"'{fix['name']}' on {n}"))
    return out


def resolve_lines(config, sources, overrides):
    """name -> commit: the commit release-sources.json pins for each line's payload path, or an override."""
    pins = {}
    for f in sources.get("files", []):
        b = f.get("built_from") or {}
        if b.get("repo") == config["repository"] and b.get("commit"):
            pins.setdefault(f.get("path"), set()).add(b["commit"])
    revs, errors = {}, []
    for name, path in config["lines"].items():
        if name in overrides:
            revs[name] = overrides[name]
            continue
        commits = pins.get(path, set())
        if len(commits) != 1:
            errors.append(f"line {name}: release-sources.json pins {len(commits)} commits of {config['repository']} "
                          f"for {path} ({', '.join(sorted(c[:8] for c in commits)) or 'none'}), not one")
        else:
            revs[name] = commits.pop()
    return revs, errors


def lines_main(argv):
    from pathlib import Path
    ap = argparse.ArgumentParser(prog="marker_guard.py lines")
    ap.add_argument("--config", required=True)
    ap.add_argument("--sources", required=True, help="release-sources.json: the commit of each line and the checkouts")
    ap.add_argument("--root", default="", help="BC250_ROOT, for the checkouts that release-sources.json names")
    ap.add_argument("--repo", help="a Mesa checkout that holds every line (default: the first that does)")
    ap.add_argument("--line", action="append", default=[], help="NAME=REV in place of the pinned commit")
    ap.add_argument("--json")
    a = ap.parse_args(argv)
    config = json.loads(Path(a.config).read_text(encoding="utf-8-sig"))
    sources = json.loads(Path(a.sources).read_text(encoding="utf-8-sig"))
    overrides = dict(x.split("=", 1) for x in a.line)
    revs, errors = resolve_lines(config, sources, overrides)
    repo = a.repo
    if not errors and not repo:
        table = sources.get("repositories", {}).get(config["repository"], {})
        for c in table.get("checkouts", []):
            p = Path(a.root) / c
            if p.exists() and all(has_commit(str(p), r) for r in revs.values()):
                repo = str(p)
                break
        if not repo:
            errors.append(f"no checkout of {config['repository']} under {a.root or '.'} holds every line "
                          f"({', '.join(f'{n} {r[:8]}' for n, r in revs.items())}): fetch the pinned commits")
    if not errors:
        errors += [f"{repo} does not hold {n} {r}" for n, r in revs.items() if not has_commit(repo, r)]
    print(f"both lines: {repo or '?'}; " + ", ".join(f"{n} {r[:8]}" for n, r in revs.items()))
    if errors:
        for e in errors:
            print(f"  FAIL     {e}")
        return 1
    findings = lines_check(config, revs, repo)
    order = {"FAIL": 0, "STALE": 1, "OPEN": 2, "EQUIVALENT": 3, "ACCEPTED": 4, "BOTH": 5}
    for status, text in sorted(findings, key=lambda f: order[f[0]]):
        print(f"  {status:10} {text}")
    failed = [f for f in findings if f[0] in ("FAIL", "STALE")]
    counts = Counter(s for s, _ in findings)
    print(f"both lines: {', '.join(f'{k} {counts[k]}' for k in order if counts[k])}; "
          f"{'FAIL' if failed else 'PASS'}")
    if a.json:
        with open(a.json, "w", encoding="utf-8", newline="\n") as f:
            json.dump({"lines": revs, "repo": repo, "findings": [{"status": s, "text": t} for s, t in findings]},
                      f, indent=1)
    return 1 if failed else 0


if __name__ == "__main__":
    if sys.argv[1:2] == ["lines"]:
        sys.exit(lines_main(sys.argv[2:]))
    sys.exit(main())
