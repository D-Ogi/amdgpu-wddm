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
    hits = git(repo, "grep", "-I", "-l", "-E", GREP_ERE, rev, "--", *paths)
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


if __name__ == "__main__":
    sys.exit(main())
