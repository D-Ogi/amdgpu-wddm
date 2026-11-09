"""The containment gate of a release set: every fix commit that the manifest names is in the history of the rows
that ship it.

A defect row is closed for a release only when the code of its fix is in the bytes of that release. The manifest
says which commits those are, in its "containment" list:

    "containment": [
      {"defect": "BD-306", "repo": "amdgpu-wddm", "commit": "<40 hex>", "rows": ["kmd"]},
      {"defect": "BD-317", "repo": "amdgpu-wddm", "commit": "<40 hex>", "rows": ["@recipe"]}
    ]

"rows" names row ids or components of the manifest, or "@recipe" for the recipe commit. For each row that a name
matches, the commits that the row builds from the entry's repository are its targets: the row commit when the row
is of that repository, and each pinned input of that repository. A row of a component with no target in that
repository (for example the dxvk rows of d3d11-umd for a fix in amdgpu-wddm) is passed over; a name that gives no
target at all is an error. Each target must have the fix commit as an ancestor (git merge-base --is-ancestor) in a
checkout that holds both.

From tester.38 on the list is required (VERIFY-MATRIX G5): a manifest without it fails the gate, so that a release
cannot claim fixed rows without a check. An empty list is allowed and says so.

    python tools/release/containment.py <manifest.json> [--root <BC250_ROOT>] [--repo <recipe checkout>]

prints the lines of the gate and exits 0 on PASS, 1 on FAIL. build_release_set.py runs the same check in its gates.
"""
import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import provenance  # noqa: E402

REQUIRED_FROM = 38
RECIPE = "@recipe"


def release_number(release):
    m = re.fullmatch(r"tester\.(\d+)(v\d+)?", release or "")
    return int(m.group(1)) if m else None


def targets(entry, rows, recipe_repo, recipe_commit):
    """(name, row id, commit) for each commit that must contain the entry's fix; plus errors for empty names."""
    out, errors = [], []
    for name in entry["rows"]:
        found = []
        if name == RECIPE:
            if entry["repo"] == recipe_repo:
                found.append((name, "recipe", recipe_commit))
        else:
            for row in rows:
                if name not in (row["id"], row["component"]):
                    continue
                if row["repo"] == entry["repo"]:
                    found.append((name, row["id"], row["commit"]))
                for input_name, spec in sorted((row.get("inputs") or {}).items()):
                    if spec.get("repo") == entry["repo"] and spec.get("commit"):
                        found.append((name, f"{row['id']} input {input_name}", spec["commit"]))
        if not found:
            errors.append(f"{entry['defect']}: '{name}' names no row that builds from {entry['repo']}")
        out += found
    return out, errors


def check(release, containment, rows, recipe_repo, recipe_commit, find, is_ancestor):
    """Returns (status, lines). find(repo, commit) -> checkout or None; is_ancestor(checkout, a, b) -> bool | None
    (None: git could not answer)."""
    number = release_number(release)
    if containment is None:
        if number is not None and number < REQUIRED_FROM:
            return "PASS", [f"{release}: no containment list (required from tester.{REQUIRED_FROM})"]
        return "FAIL", [f"{release}: the manifest has no 'containment' list; from tester.{REQUIRED_FROM} on it names "
                        f"the fix commits of the release's rows (an empty list says that there are none)"]
    if not containment:
        return "PASS", ["the containment list is empty: the manifest names no fix commits"]
    lines, failed, checked = [], False, 0
    for entry in containment:
        found, errors = targets(entry, rows, recipe_repo, recipe_commit)
        for e in errors:
            lines.append(f"FAIL {e}")
            failed = True
        fix = entry["commit"]
        for name, where, commit in found:
            checkout = find(entry["repo"], commit)
            if checkout is None:
                lines.append(f"FAIL {entry['defect']} {entry['repo']} {fix[:9]}: {where} {commit[:9]} is in no checkout")
                failed = True
                continue
            answer = is_ancestor(checkout, fix, commit)
            checked += 1
            if answer is None:
                lines.append(f"FAIL {entry['defect']} {entry['repo']} {fix[:9]}: not in {checkout} with {where} "
                             f"{commit[:9]} (fetch the branch of the fix)")
                failed = True
            elif not answer:
                lines.append(f"FAIL {entry['defect']} {entry['repo']} {fix[:9]} is not in {where} {commit[:9]}")
                failed = True
            else:
                lines.append(f"PASS {entry['defect']} {entry['repo']} {fix[:9]} in {where} {commit[:9]}")
    lines.append(f"{len(containment)} fix commit(s), {checked} row commit(s) checked")
    return ("FAIL" if failed else "PASS"), lines


def git_is_ancestor(checkout, ancestor, commit):
    code, _ = provenance.git(checkout, "cat-file", "-e", f"{ancestor}^{{commit}}")
    if code != 0:
        return None
    code, _ = provenance.git(checkout, "merge-base", "--is-ancestor", ancestor, commit)
    if code in (0, 1):
        return code == 0
    return None


def check_plan(plan):
    repos = provenance.Repositories(plan["repositories"], plan["root"], plan["repo"])
    status, lines = check(plan["release"], plan.get("containment"), plan.get("containment_rows") or [],
                          plan["recipe_repository"], plan["recipe_commit"], repos.find, git_is_ancestor)
    return {"name": "containment", "status": status, "lines": lines}


def plan_rows(rows):
    """The fields of the manifest rows that the gate reads; the plan carries them for every row, not only the
    rows that this run builds."""
    return [{"id": r["id"], "component": r["component"], "repo": r["repo"], "commit": r["commit"],
             "inputs": {n: {"repo": s["repo"], "commit": s["commit"]}
                        for n, s in (r.get("inputs") or {}).items() if "repo" in s and "commit" in s}}
            for r in rows]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("manifest")
    ap.add_argument("--root", default=None, help="BC250_ROOT (default: the parent of the repository's checkout)")
    ap.add_argument("--repo", default=str(Path(__file__).resolve().parents[2]))
    a = ap.parse_args()
    manifest = json.loads(Path(a.manifest).read_text(encoding="utf-8"))
    root = a.root or str(Path(a.repo).resolve().parent)
    plan = {"repositories": manifest["repositories"], "root": root, "repo": a.repo, "release": manifest["release"],
            "containment": manifest.get("containment"), "containment_rows": plan_rows(manifest["rows"]),
            "recipe_repository": manifest["recipe_repository"], "recipe_commit": manifest["recipe_commit"]}
    gate = check_plan(plan)
    for line in gate["lines"]:
        print(line)
    print(f"{gate['status']} containment")
    return 0 if gate["status"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
