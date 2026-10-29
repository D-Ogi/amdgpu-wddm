#!/usr/bin/env python3
"""The plan, the gates and the release-sources.json rows of a release set build.

tools/release/build-release-set.ps1 is the tool a person runs. It calls this script for everything that is data and runs
the builds itself. The release manifest has one row per component build; release-manifest.schema.json is its contract
and release-tester.32.json an example.

  build_release_set.py plan    --manifest <release.json> --root <BC250_ROOT> --repo <recipe repository> --out <dir>
                         --work <dir> [--sources <release-sources.json>] [--only <id or pattern>,...] [--version <v>]
                         [--max-parallel <n>] [--jobs <n>] [--logical <n>] --write <plan.json>
      The manifest against the schema and its own rules; the builds in dependency order with their directories,
      jobs, disk peaks and payload inputs. With --only, the rows that are not rebuilt and that a selected row needs
      come from the artifacts of an earlier run in --out, each checked against its recorded SHA-256.
  build_release_set.py gates   --plan <plan.json> --recipe-params <json> [--allow-unpushed] --write <gates.json>
      commits: every commit is in a checkout. published: a remote-tracking branch of the repository's URL holds it
      (--allow-unpushed makes this a warning, for a local dry run). attribution: no commit message, author or
      committer from the base to the commit names an AI agent or model (ATTRIBUTION), and no message quotes the
      repository owner or attributes a rule to the owner with a date (OWNER_RX; in a commit that the fork already
      holds, a warning only). A repository with an 'upstream' commit leaves the history that commit reaches out of
      the scan, when a remote of the upstream URL holds it, and the report counts what it left out. compiler: each
      row names its compiler on the recipe's command line, and the compiler policy allows it (compiler_findings).
  build_release_set.py collect --job <job.json>
      Inside one build: copies each output and the PDB its image names into the artifact directory, with the
      build's recipe.json and the files of 'keep', and writes collect.json (SHA-256, size, PDB identity).
  build_release_set.py rows    --plan <plan.json> --write <rows.json> [--compare <release-sources.json>]
                         [--merge <release-sources.json> --merged <release-sources.json>]
      The release-sources.json rows of every output that a build of this run or an earlier one made; --compare
      holds them against registered rows (acceptance: the same bytes where the row claims a deterministic
      build), --merge writes a copy of a release-sources.json with these rows in place of the old ones.
"""
import argparse
import copy
import fnmatch
import hashlib
import importlib.util
import json
import os
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import provenance  # noqa: E402
sys.path.insert(0, str(HERE.parent / "state"))
import check_history  # noqa: E402

SCHEMA = HERE / "release-manifest.schema.json"
PLACEHOLDER = re.compile(r"\{([a-z_]+)(?::([^}]+))?\}")
# The placeholders of a recipe's arguments. {jobs}, {version} and {flavour} become their values in the written rows,
# because provenance.py accepts no placeholder for them; the others stay, so that rebuild-check.ps1 can expand them.
ARG_PLACEHOLDERS = {"src", "out", "tmp", "root", "jobs", "version", "flavour", "input", "payload", "tool"}
LITERAL_PLACEHOLDERS = ("jobs", "version", "flavour")
# A test may also name its own output directory and the recipe tree.
TEST_PLACEHOLDERS = ARG_PLACEHOLDERS | {"testout", "recipe"}
# The compilers each kind of build may name (the compiler policy): clang-cl is the default of user-mode code, msvc in
# user mode needs a reason in the row, and the kernel driver is evaluated on its own (WDK, msvc).
KIND_COMPILERS = {"user": ("clang-cl", "msvc"), "kernel": ("msvc",), "managed": ("csc",), "data": ("none",)}
# Words that say an AI agent or model wrote or co-wrote a commit. No AI agent appears as author or co-author in the
# public repositories. Qwen and Llama are left out on purpose: they are the models this project
# runs on the GPU, so their names are the subject of a measurement, not an author.
ATTRIBUTION = [
    ("Co-Authored-By", r"co-authored-by"),
    ("Claude", r"\bclaude\b"), ("Anthropic", r"\banthropic\b"), ("Opus", r"\bopus\b"), ("Sonnet", r"\bsonnet\b"),
    ("Haiku", r"\bhaiku\b"), ("Fable", r"\bfable\b"), ("Codex", r"\bcodex\b"), ("OpenAI", r"\bopen\s?ai\b"),
    ("GPT", r"\b(?:chat)?gpt(?:-?[0-9][\w.]*)?\b"), ("Gemini", r"\bgemini\b"), ("Grok", r"\bgrok\b"),
    ("Copilot", r"\bcopilot\b"), ("DeepSeek", r"\bdeepseek\b"),
]
ATTRIBUTION_RX = [(word, re.compile(rx, re.I)) for word, rx in ATTRIBUTION]
# These model names are also English words or other products (a poem, an operating system, a story). In a message line
# they are a hit only in an attribution context: a trailer line ("<Something>-by:"), a line that names the vendor or
# its product, or a line with an authorship phrase ("written by", "generated with", ...). Mesa's util code names the
# Haiku operating system beside Windows and macOS, and that is not an author. The author and committer fields are an
# attribution context by themselves, so these words are always a hit there. The gate checks these documented
# patterns; it is no general detector of authorship in prose.
CONTEXT_WORDS = {"Opus", "Sonnet", "Haiku", "Fable"}
CONTEXT_RX = re.compile(r"^\s*[a-z][a-z-]*-by\s*:|\bclaude\b|\banthropic\b"
                        r"|\b(?:written|generated|authored|recorded|assisted|created)\s+(?:by|with)\b", re.I)
# A commit message states a requirement as the project's own, never as a quote of the repository owner or an
# attribution with a date. The push gate of the workspace refuses such a message with this same pattern, so a
# release set that names such a commit could not be pushed. Technical owners (SMU owner, file owner, owner lock)
# do not match.
OWNER_RX = re.compile(
    r'\(owner\b(?:,?\s*\d{4}-\d\d-\d\d|\s*:\s*["“„])|\bowner,?\s+\d{4}-\d\d-\d\d|'
    r'\bowner\'s\s+(rule|instruction|decision|request|requirement|direction|answer|consent|wish|order|'
    r'words|message|question|observation|feedback|brand|image|fork|release gate)\b|'
    r'\bthe owner\s+(said|asks|asked|wrote|decided|decides|approved|approves|wants|wanted|requested|chose|'
    r'insisted|agreed|consented|gave|reported|reports|saw|confirmed|confirms|allowed|accepts|accepted)\b|'
    r'\bowner\s+(said|asked|wrote|decided|approved|requested|reports|reported|confirms|confirmed|decision|rule|gate)\b',
    re.I)
REPORT = "release-report.json"


def load_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def write_json(path, data):
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text(json.dumps(data, indent=1) + "\n", encoding="utf-8")


def sha256_file(path):
    return provenance.sha256_file(path)


# ---------------------------------------------------------------------------------------------------------------
# The schema. A small validator for the keywords release-manifest.schema.json uses, so the schema file is the one
# copy of the rules; a keyword it does not know is an error, not a silent pass.

KNOWN = {"$schema", "$id", "$defs", "$ref", "title", "description", "type", "required", "additionalProperties",
         "properties", "const", "enum", "pattern", "minLength", "minItems", "minProperties", "items", "minimum",
         "exclusiveMinimum", "oneOf"}
TYPES = {"object": dict, "array": list, "string": str, "boolean": bool}


def type_ok(value, name):
    if name == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if name == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    return isinstance(value, TYPES[name])


def validate(value, schema, root, where="$"):
    unknown = set(schema) - KNOWN
    if unknown:
        return [f"schema at {where}: unknown keyword(s) {sorted(unknown)}"]
    if "$ref" in schema:
        target = root
        for part in schema["$ref"][2:].split("/"):
            target = target[part]
        merged = dict(target)
        merged.update({k: v for k, v in schema.items() if k != "$ref"})
        if "$ref" in target:
            merged["$ref"] = target["$ref"]
        else:
            merged.pop("$ref", None)
        return validate(value, merged, root, where)
    if "oneOf" in schema:
        fits = [s for s in schema["oneOf"] if not validate(value, s, root, where)]
        if len(fits) != 1:
            return [f"{where}: fits {len(fits)} of the {len(schema['oneOf'])} forms it may take (exactly one)"]
        return []
    errors = []
    if "const" in schema and value != schema["const"]:
        errors.append(f"{where}: must be {schema['const']!r}")
    if "enum" in schema and value not in schema["enum"]:
        errors.append(f"{where}: {value!r} is not one of {schema['enum']}")
    if "type" in schema and not type_ok(value, schema["type"]):
        return errors + [f"{where}: must be of type {schema['type']}"]
    if isinstance(value, str):
        if len(value) < schema.get("minLength", 0):
            errors.append(f"{where}: shorter than {schema['minLength']} characters")
        if "pattern" in schema and not re.search(schema["pattern"], value):
            errors.append(f"{where}: {value[:60]!r} does not match {schema['pattern']}")
    if type_ok(value, "number"):
        if "minimum" in schema and value < schema["minimum"]:
            errors.append(f"{where}: below {schema['minimum']}")
        if "exclusiveMinimum" in schema and value <= schema["exclusiveMinimum"]:
            errors.append(f"{where}: must be above {schema['exclusiveMinimum']}")
    if isinstance(value, list):
        if len(value) < schema.get("minItems", 0):
            errors.append(f"{where}: needs at least {schema['minItems']} item(s)")
        if "items" in schema:
            for i, item in enumerate(value):
                errors += validate(item, schema["items"], root, f"{where}[{i}]")
    if isinstance(value, dict):
        for key in schema.get("required", []):
            if key not in value:
                errors.append(f"{where}: needs '{key}'")
        if len(value) < schema.get("minProperties", 0):
            errors.append(f"{where}: needs at least {schema['minProperties']} entr(ies)")
        props = schema.get("properties", {})
        extra = schema.get("additionalProperties", True)
        for key, item in value.items():
            if key in props:
                errors += validate(item, props[key], root, f"{where}.{key}")
            elif extra is False:
                errors.append(f"{where}: unknown field '{key}'")
            elif isinstance(extra, dict):
                errors += validate(item, extra, root, f"{where}.{key}")
    return errors


def check_manifest(manifest):
    """The schema, then the rules that a schema cannot say."""
    schema = load_json(SCHEMA)
    errors = validate(manifest, schema, schema)
    if errors:
        return errors
    table = manifest["repositories"]
    if manifest["recipe_repository"] not in table:
        errors.append(f"recipe_repository {manifest['recipe_repository']!r} is not in 'repositories'")
    ids, slots = set(), {}
    for row in manifest["rows"]:
        rid = row["id"]
        where = f"row {rid}"
        if rid in ids:
            errors.append(f"{where}: the id is used twice")
        ids.add(rid)
        if row["repo"] not in table:
            errors.append(f"{where}: repository {row['repo']!r} is not in 'repositories'")
        for name, spec in (row.get("inputs") or {}).items():
            if "repo" in spec and spec["repo"] not in table:
                errors.append(f"{where} input {name}: repository {spec['repo']!r} is not in 'repositories'")
        if row["deterministic"] == ("unverified" in row):
            errors.append(f"{where}: a row that claims a deterministic build has no 'unverified', and a row that "
                          f"does not claim one says in 'unverified' why")
        files = {}
        for out in row["outputs"]:
            key = (out["path"], out.get("flavour", row["flavour"]))
            if key in slots:
                errors.append(f"{where}: {key[0]} ({key[1]}) is also an output of row {slots[key]}")
            slots[key] = rid
            base = out["file"].replace("\\", "/").rsplit("/", 1)[-1].lower()
            if files.get(base, out["file"]) != out["file"]:
                errors.append(f"{where}: {out['file']} and {files[base]} have one file name; the artifact folder "
                              f"holds a build's outputs side by side")
            files.setdefault(base, out["file"])
        for text, allowed, what in ([(a, ARG_PLACEHOLDERS, "argument") for a in row["args"]] +
                                    [(t, TEST_PLACEHOLDERS, "test argument") for t in test_texts(row)]):
            for m in PLACEHOLDER.finditer(text):
                kind, arg = m.group(1), m.group(2)
                if kind not in allowed:
                    errors.append(f"{where}: {what} {text!r} has the unknown placeholder {m.group(0)}")
                elif kind == "input" and arg not in (row.get("inputs") or {}):
                    errors.append(f"{where}: {what} {text!r} names an input the row does not declare")
                elif kind in ("input", "payload", "tool") and not arg:
                    errors.append(f"{where}: {what} {text!r}: {{{kind}}} needs a name")
    return errors


def test_texts(row):
    out = []
    for t in row.get("tests") or []:
        out += [t["run"]] + list(t.get("args", [])) + [t.get("cwd", ""), t.get("requires", "")]
        out += list((t.get("env") or {}).values())
    return [x for x in out if x]


# ---------------------------------------------------------------------------------------------------------------
# The plan.

def source_rows(sources_path):
    """(path, flavour) -> release-sources.json row; a row without a flavour (schema 3) answers for both."""
    rows = {}
    if not sources_path:
        return rows
    for f in load_json(sources_path).get("files", []):
        tag = f.get("flavour")
        for flavour in ([tag] if tag in ("release", "lab") else ["release", "lab"]):
            rows.setdefault((f["path"], flavour), f)
    return rows


def short_key(text):
    return hashlib.sha256(text.encode("utf-8")).hexdigest()[:8]


def previous_builds(out):
    """The builds of an earlier run in this output directory that ended well: id -> report entry."""
    path = Path(out) / REPORT
    if not path.is_file():
        return {}
    try:
        report = load_json(path)
    except ValueError:
        return {}
    return {b["id"]: b for b in report.get("builds", []) if b.get("status") == "ok" and b.get("outputs")}


def make_plan(manifest_path, root, repo, out, work, sources_path=None, only=None, version=None, max_parallel=3,
              jobs=0, logical=None):
    manifest = load_json(manifest_path)
    errors = check_manifest(manifest)
    if errors:
        return None, errors
    root, out, work = Path(root).resolve(), Path(out).resolve(), Path(work).resolve()
    if not out.is_relative_to(root):
        return None, [f"--out {out} is not under BC250_ROOT {root}: a release-sources.json row names its source "
                      f"relative to that root"]
    logical = logical or os.cpu_count() or 1
    if max_parallel < 1:
        return None, ["--max-parallel must be at least 1"]
    jobs_per_slot = jobs or max(1, logical // max_parallel)
    version = version or manifest["version"]
    rows = manifest["rows"]
    by_id = {r["id"]: r for r in rows}
    producer = {}
    for r in rows:
        for o in r["outputs"]:
            producer[(o["path"], o.get("flavour", r["flavour"]))] = r["id"]
    registered = source_rows(sources_path)
    deps, payload_from = {}, {}
    for r in rows:
        deps[r["id"]], payload_from[r["id"]] = [], {}
        for text in r["args"] + test_texts(r):
            for m in PLACEHOLDER.finditer(text):
                if m.group(1) != "payload":
                    continue
                path = m.group(2)
                dep = producer.get((path, r["flavour"])) or producer.get((path, "shared"))
                if dep == r["id"]:
                    errors.append(f"row {r['id']}: names its own output {path} as an input")
                elif dep:
                    payload_from[r["id"]][path] = {"row": dep}
                    if dep not in deps[r["id"]]:
                        deps[r["id"]].append(dep)
                elif (path, r["flavour"]) in registered:
                    src = registered[(path, r["flavour"])]["source"]
                    payload_from[r["id"]][path] = {"file": str(root / src.replace("/", os.sep))}
                else:
                    errors.append(f"row {r['id']}: no row of this release set and no release-sources.json row gives "
                                  f"{path} ({r['flavour']})")
    order, state = [], {}

    def visit(rid, chain):
        if state.get(rid) == "done":
            return
        if state.get(rid) == "open":
            errors.append(f"rows {' -> '.join(chain + [rid])}: the payload inputs form a cycle")
            return
        state[rid] = "open"
        for d in deps[rid]:
            visit(d, chain + [rid])
        state[rid] = "done"
        order.append(rid)

    for r in rows:
        visit(r["id"], [])
    patterns = [p for p in (only or []) if p]
    if patterns:
        selected = {rid for rid in by_id if any(fnmatch.fnmatchcase(rid, p) for p in patterns)}
        for p in patterns:
            if not any(fnmatch.fnmatchcase(rid, p) for rid in by_id):
                errors.append(f"--only {p}: no row has that id")
    else:
        selected = set(by_id)
    table = manifest["repositories"]
    recipe_repo = manifest["recipe_repository"]
    shared = {}

    def shared_tree(name, commit, rid):
        key = f"{name}@{commit}"
        if key not in shared:
            shared[key] = {"key": key, "repo": name, "commit": commit, "path": str(work / f"_{short_key(key)}"),
                           "rows": []}
        if rid not in shared[key]["rows"]:
            shared[key]["rows"].append(rid)
        return shared[key]["path"]

    def define(rid):
        r = dict(by_id[rid])
        # A build of the recipe repository itself takes its recipe from its own commit, as provenance.py reads a row
        # without recipe_commit; a recipe that compiles the tree it sits in builds nothing else.
        r["recipe_commit"] = r.get("recipe_commit") or (r["commit"] if r["repo"] == manifest["recipe_repository"]
                                                        else manifest["recipe_commit"])
        # A build that asks for more slots than the run has takes all of them, and so runs alone.
        r["slots"] = min(r.get("slots", 1), max_parallel)
        r["jobs"] = r["slots"] * jobs_per_slot
        # The version a build stamps, for a row whose arguments name it: an artifact of an earlier run that was
        # stamped for another release is not this row's build.
        r["version"] = version if any("{version}" in a for a in r["args"]) else None
        return r

    earlier = previous_builds(out) if patterns else {}
    kept = []
    for rid in order:
        if rid in selected or rid not in earlier:
            continue
        entry = earlier[rid]
        if build_key(entry.get("definition") or {}) != build_key(define(rid)):
            # The row changed since that run: its artifact is not the build the manifest describes now.
            if any(rid in deps[s] for s in selected):
                errors.append(f"row {rid}: the manifest row changed since the earlier run in {out}; add it to --only")
            continue
        for o in entry["outputs"]:
            f = Path(o["artifact"])
            if not f.is_file() or sha256_file(f) != o["sha256"]:
                errors.append(f"row {rid}: the artifact {f} of the earlier run is gone or changed; rebuild the row")
        # The same bytes under today's row: its OUTPUT_NOTES reach the rows from the manifest as it is now. The jobs
        # stay those of the earlier run, so that the row gives the command line that made the artifact.
        definition = define(rid)
        if (entry.get("definition") or {}).get("jobs"):
            definition["jobs"] = entry["definition"]["jobs"]
        kept.append(dict(entry, definition=definition))
    kept_ids = {k["id"] for k in kept}
    for rid in sorted(selected):
        for d in deps[rid]:
            if d not in selected and d not in kept_ids and d in earlier:
                continue                                         # reported above
            if d not in selected and d not in kept_ids:
                errors.append(f"row {rid} needs the outputs of row {d}, which this run does not build and no earlier "
                              f"run in {out} built: add {d} to --only")
    if errors:
        return None, errors

    builds = []
    for rid in order:
        if rid not in selected:
            continue
        r = define(rid)
        r["base"] = r.get("base") or table[r["repo"]].get("base")
        r["recipe_base"] = table[recipe_repo].get("base")
        r["tree"] = r.get("tree", "worktree")
        r["disk_peak_bytes"] = int(r.get("disk_peak_gb", 2) * 1e9)
        r["deps"] = deps[rid]
        r["payload_from"] = payload_from[rid]
        base = work / rid
        dirs = {"work": str(base), "out": str(base / "o"), "tmp": str(base / "t"), "inputs": str(base / "i"),
                "tests": str(base / "x")}
        dirs["src"] = shared_tree(r["repo"], r["commit"], rid) if r["tree"] == "shared" else str(base / "s")
        dirs["src_shared"] = r["tree"] == "shared"
        if r["repo"] == recipe_repo and r["recipe_commit"] == r["commit"]:
            dirs["recipe"], dirs["recipe_shared"] = dirs["src"], dirs["src_shared"]
        else:
            dirs["recipe"], dirs["recipe_shared"] = shared_tree(recipe_repo, r["recipe_commit"], rid), True
        r["dirs"] = dirs
        r["artifact"] = str(out / "artifacts" / r["flavour"] / rid)
        r["log_dir"] = str(out / "logs" / rid)
        builds.append(r)
    if errors:
        return None, errors
    recipes = sorted({(recipe_repo, b["recipe_commit"], b["recipe"]) for b in builds})
    plan = {
        "tool": "build-release-set", "schema": 1, "manifest": str(Path(manifest_path).resolve()),
        "manifest_sha256": sha256_file(manifest_path), "release": manifest["release"], "version": version,
        "root": str(root), "repo": str(Path(repo).resolve()), "out": str(out), "work": str(work),
        "sources": str(Path(sources_path).resolve()) if sources_path else None,
        "recipe_repository": recipe_repo, "recipe_commit": manifest["recipe_commit"], "repositories": table,
        "max_parallel": max_parallel, "logical_processors": logical, "jobs_per_slot": jobs_per_slot,
        "only": patterns, "builds": builds, "kept": kept, "shared_trees": list(shared.values()),
        "recipes": [{"repo": a, "commit": b, "recipe": c} for a, b, c in recipes],
        "rows_in_manifest": [r["id"] for r in rows],
    }
    return plan, []


# ---------------------------------------------------------------------------------------------------------------
# The gates.

def scan_attribution(checkout, base, commit, upstream=None):
    """Every commit of base..commit whose author, committer or message names an AI agent or model. With upstream, the
    commits that the upstream commit reaches are not read: they are the upstream project's history, which a merge
    brings in with its own authors and trailers (the push gate admits them the same way)."""
    exclude = [f"^{upstream}"] if upstream else []
    r = subprocess.run(["git", "-C", str(checkout), "log", "--format=%H%x1f%an <%ae>%x1f%cn <%ce>%x1f%B%x1e",
                        f"{base}..{commit}", *exclude], capture_output=True)
    if r.returncode != 0:
        return None, [], r.stderr.decode("utf-8", "replace").strip()
    hits, count = [], 0
    for record in r.stdout.decode("utf-8", "replace").split("\x1e"):
        record = record.strip("\r\n")
        if not record.strip():
            continue
        count += 1
        sha, author, committer, body = (record.split("\x1f") + ["", "", ""])[:4]
        fields = [("author", author), ("committer", committer)]
        fields += [(f"message line {i + 1}", line) for i, line in enumerate(body.strip().splitlines())]
        for field, text in fields:
            message = field.startswith("message")
            for word, rx in ATTRIBUTION_RX:
                if rx.search(text) and (not message or word not in CONTEXT_WORDS or CONTEXT_RX.search(text)):
                    hits.append({"commit": sha.strip(), "field": field, "word": word, "text": text.strip()[:160]})
            if field.startswith("message") and OWNER_RX.search(text):
                hits.append({"commit": sha.strip(), "field": field, "word": "owner attribution",
                             "text": text.strip()[:160]})
    return count, hits, None


def excluded_identities(checkout, base, commit, upstream, identities):
    """The commits that the upstream rule leaves out of the scan, (ancestors(commit) - ancestors(base)) intersected
    with ancestors(upstream), and those of them whose author or committer email is one of the fork's own identities.
    The project sends no commit upstream, so a match means that the upstream-named history holds fork work.
    Returns (count, matches, error); a match is {commit, field, email}."""
    def revs(*spec):
        r = subprocess.run(["git", "-C", str(checkout), "rev-list", *spec], capture_output=True)
        if r.returncode != 0:
            raise RuntimeError(r.stderr.decode("utf-8", "replace").strip())
        return set(r.stdout.decode("ascii", "replace").split())
    try:
        left_out = revs(commit, f"^{base}") & revs(upstream)
    except RuntimeError as e:
        return None, [], str(e)
    own = {i.strip().lower() for i in identities}
    matches = []
    if left_out:
        r = subprocess.run(["git", "-C", str(checkout), "log", "--no-walk=unsorted", "--stdin",
                            "--format=%H%x1f%ae%x1f%ce"], input="\n".join(sorted(left_out)).encode("ascii"),
                           capture_output=True)
        if r.returncode != 0:
            return None, [], r.stderr.decode("utf-8", "replace").strip()
        for line in r.stdout.decode("utf-8", "replace").splitlines():
            sha, author, committer = (line.split("\x1f") + ["", ""])[:3]
            for field, email in (("author", author), ("committer", committer)):
                if email.strip().lower() in own:
                    matches.append({"commit": sha.strip(), "field": field, "email": email.strip()})
    return len(left_out), matches, None


def compiler_findings(build, params):
    """Why this row's compiler is not explicit, or not allowed by the compiler policy (empty: it is both).

    params: the parameter names of the recipe at its commit, as the PowerShell parser reads them (None when the
    recipe could not be read)."""
    rid, c, kind = build["id"], build["compiler"], build["kind"]
    found = []
    if c not in KIND_COMPILERS[kind]:
        found.append(f"{rid}: compiler {c} is not one of a {kind} build ({', '.join(KIND_COMPILERS[kind])})")
    if kind == "user" and c == "msvc" and not build.get("compiler_reason"):
        found.append(f"{rid}: a user-mode build with msvc needs 'compiler_reason': compatibility, correctness or a "
                     f"measured advantage (compiler policy)")
    args = list(build["args"])
    lower = [a.lower() for a in args]

    def value_of(flag):
        return args[lower.index(flag) + 1] if flag in lower and lower.index(flag) + 1 < len(args) else None

    if build["recipe"].lower().endswith(".ps1"):
        if params is None:
            found.append(f"{rid}: the parameters of {build['recipe']} at {build['recipe_commit'][:8]} could not be "
                         f"read, so the gate cannot tell whether the compiler is explicit")
            params = []
        names = {p.lower() for p in params}
        given = value_of("-compiler")
        if "compiler" in names:
            if given is None:
                found.append(f"{rid}: {build['recipe']} takes -Compiler and the row does not pass it, so the "
                             f"recipe's own default would choose the compiler")
            elif given.lower() != c:
                found.append(f"{rid}: the arguments say -Compiler {given}, the row says {c}")
        else:
            if given is not None:
                found.append(f"{rid}: the arguments pass -Compiler, which {build['recipe']} does not take")
            if c == "clang-cl":
                found.append(f"{rid}: clang-cl must be named on the recipe's command line, and {build['recipe']} "
                             f"has no -Compiler parameter")
        if c == "msvc" and "msvcreason" in names and not (value_of("-msvcreason") or "").strip():
            found.append(f"{rid}: {build['recipe']} asks -MsvcReason for an msvc build, and the row passes none")
    elif c == "clang-cl":
        found.append(f"{rid}: {build['recipe']} is no PowerShell recipe, so clang-cl cannot be named on its "
                     f"command line")
    for a in args:
        if re.search(r"-march[=:]znver2\b", a, re.I):
            found.append(f"{rid}: {a!r}: -march=znver2 emits SSE4a, which Intel CPUs lack; the baseline is "
                         f"-march=x86-64-v3 with -mtune=znver2 (CPU baseline)")
    return found


STATE_WORKER_RECIPE = "driver/state/installer-build.ps1"


def state_worker_commits(plan):
    """(repo, commit) -> row ids, for the rows that build the installer state worker."""
    out = {}
    for b in plan["builds"]:
        if b["recipe"] == STATE_WORKER_RECIPE:
            out.setdefault((b["repo"], b["commit"]), []).append(b["id"])
    return out


def run_gates(plan, recipe_params, allow_unpushed=False):
    repos = provenance.Repositories(plan["repositories"], plan["root"], plan["repo"])
    table = plan["repositories"]
    recipe_repo = plan["recipe_repository"]
    gates = []
    # Every commit the builds name: the build, the recipe, each pinned input, and each mapped submodule of a tree
    # that a build checks out whole (as provenance.py checks them: an input of listed paths has no submodules).
    named, whole = {}, set()

    def name(where, repo, commit, tree=True):
        named.setdefault((repo, commit), []).append(where)
        if tree:
            whole.add((repo, commit))

    for b in plan["builds"]:
        name(f"{b['id']} build", b["repo"], b["commit"])
        name(f"{b['id']} recipe", recipe_repo, b["recipe_commit"])
        for n, spec in (b.get("inputs") or {}).items():
            if "repo" in spec:
                name(f"{b['id']} input {n}", spec["repo"], spec["commit"], tree="paths" not in spec)
    missing, unpushed, lines = [], [], []
    for (repo, commit), wheres in list(named.items()):
        checkout = repos.find(repo, commit)
        if checkout is None:
            missing.append(f"{repo} {commit[:8]} ({', '.join(wheres)}) is in no checkout of "
                           f"{', '.join(str(c) for c in repos.checkouts(repo))}")
            continue
        if (repo, commit) not in whole:
            continue
        for sub_path, sub_repo in (table[repo].get("submodules") or {}).items():
            code, text = provenance.git(checkout, "ls-tree", commit, sub_path)
            parts = text.split()
            if code != 0 or len(parts) < 3 or parts[1] != "commit":
                missing.append(f"{repo} {commit[:8]} has no submodule {sub_path}")
            elif sub_repo not in table:
                missing.append(f"{repo} maps its submodule {sub_path} to {sub_repo!r}, which is not in 'repositories'")
            else:
                named.setdefault((sub_repo, parts[2]), []).append(f"submodule {sub_path} of {repo} {commit[:8]}")
    for (repo, commit), wheres in named.items():
        if repos.find(repo, commit) is None:
            if not any(f"{repo} {commit[:8]} " in m for m in missing):
                missing.append(f"{repo} {commit[:8]} ({', '.join(wheres)}) is in no checkout")
            continue
        branches = repos.published(repo, commit)
        if branches:
            lines.append(f"{repo} {commit[:8]} on {branches[0]}{f' +{len(branches) - 1}' if len(branches) > 1 else ''}")
        else:
            unpushed.append(f"{repo} {commit[:8]} ({', '.join(sorted(set(wheres)))}) is on no remote-tracking branch "
                            f"of {table[repo]['url']} (push the branch that carries it, then fetch)")
    gates.append({"name": "commits", "status": "FAIL" if missing else "PASS", "lines": missing or
                  [f"{len(named)} commits, each in a checkout"]})
    gates.append({"name": "published",
                  "status": ("WARN" if allow_unpushed else "FAIL") if unpushed else "PASS",
                  "lines": unpushed + ([f"-AllowUnpushed: a local dry run; these commits are not pushed"]
                                       if unpushed and allow_unpushed else []) or lines,
                  "unpushed": unpushed})
    # Attribution: every commit from the base to the commit, for each build and recipe commit.
    ranges = {}
    for b in plan["builds"]:
        ranges.setdefault((b["repo"], b.get("base"), b["commit"]), []).append(b["id"])
        ranges.setdefault((recipe_repo, b.get("recipe_base"), b["recipe_commit"]), []).append(f"{b['id']} recipe")
    att_lines, hits, published, failed, left_out = [], [], [], False, []
    for (repo, base, commit), wheres in sorted(ranges.items(), key=lambda kv: (kv[0][0], kv[0][2])):
        if not base:
            att_lines.append(f"{repo} {commit[:8]} ({', '.join(wheres)}): no base, so the range of the scan is "
                             f"unknown; name 'base' in the repository or the row")
            failed = True
            continue
        checkout = repos.find(repo, commit)
        if checkout is None or repos.find(repo, base) is None:
            att_lines.append(f"{repo} {base[:8]}..{commit[:8]}: not both commits are in a checkout")
            failed = True
            continue
        # The upstream commit of a fork line: only a commit that a remote of the upstream project's URL holds may
        # take its history out of the scan. The check reads the checkout's own remote configuration and fetched
        # refs, offline, so it trusts the reviewed upstream URL of the manifest and the release workflow's fetch; it
        # is no authentication of the remote. The fork-identity check below refuses an excluded set that holds a
        # commit of the fork's own identities.
        upstream = table[repo].get("upstream")
        if upstream:
            if provenance.normalize_url(upstream["url"]) == provenance.normalize_url(table[repo]["url"]):
                att_lines.append(f"{repo} upstream {upstream['commit'][:8]}: its URL is the fork's own URL "
                                 f"{table[repo]['url']}, so it proves nothing about whose history it is")
                failed = True
                continue
            branches = repos.published(repo, upstream["commit"], url=upstream["url"])
            if not branches:
                att_lines.append(f"{repo} upstream {upstream['commit'][:8]} is on no remote-tracking branch of "
                                 f"{upstream['url']} (add that remote to the checkout and fetch it)")
                failed = True
                continue
        count, found, error = scan_attribution(checkout, base, commit, upstream and upstream["commit"])
        if error:
            att_lines.append(f"{repo} {base[:8]}..{commit[:8]}: git log failed: {error}")
            failed = True
            continue
        if upstream:
            skipped, own, error = excluded_identities(checkout, base, commit, upstream["commit"],
                                                      upstream["fork_identities"])
            if error:
                att_lines.append(f"{repo} {base[:8]}..{commit[:8]}: git rev-list failed: {error}")
                failed = True
                continue
            left_out.append({"repo": repo, "range": f"{base[:8]}..{commit[:8]}", "upstream": upstream["commit"],
                             "url": upstream["url"], "on": branches[0], "commits": skipped,
                             "fork_identity_commits": own})
            att_lines.append(f"{repo} {base[:8]}..{commit[:8]}: {count} commits, {len(found)} hits; {skipped} "
                             f"commits of upstream {upstream['commit'][:8]} ({branches[0]}) left out, "
                             f"{len({m['commit'] for m in own})} of them with a fork identity")
            for m in own:
                att_lines.append(f"  {repo} {m['commit'][:8]} {m['field']} {m['email']}: a fork identity in the "
                                 f"history that upstream {upstream['commit'][:8]} reaches; the scan may not leave it out")
                failed = True
        else:
            att_lines.append(f"{repo} {base[:8]}..{commit[:8]}: {count} commits, {len(found)} hits")
        for h in found:
            # The push gate checks only the commits a push adds. An owner attribution in the message of a commit
            # that the fork already holds was published before that rule, and the project does not rewrite published
            # history, so it is reported and does not stop the build. A commit that is not published still fails.
            if h["word"] == "owner attribution" and repos.published(repo, h["commit"]):
                published.append(dict(h, repo=repo, range=f"{base[:8]}..{commit[:8]}"))
                att_lines.append(f"  {repo} {h['commit'][:8]} {h['field']}: '{h['word']}' in an already published "
                                 f"commit (reported only): {h['text']!r}")
                continue
            hits.append(dict(h, repo=repo, range=f"{base[:8]}..{commit[:8]}"))
            att_lines.append(f"  {repo} {h['commit'][:8]} {h['field']}: '{h['word']}' in {h['text']!r}")
    gates.append({"name": "attribution",
                  "status": "FAIL" if failed or hits else ("WARN" if published else "PASS"), "lines": att_lines,
                  "hits": hits, "published_owner_attributions": published, "upstream_left_out": left_out})
    # The compiler of each row.
    comp = []
    for b in plan["builds"]:
        key = f"{b['recipe_commit']}:{b['recipe']}"
        entry = recipe_params.get(key) or {}
        comp += compiler_findings(b, entry.get("params"))
    gates.append({"name": "compiler", "status": "FAIL" if comp else "PASS",
                  "lines": comp or [f"{len(plan['builds'])} rows name an allowed compiler on the command line"]})
    # The state worker of each row reads the removal record of the previous release only when driver/state/history at
    # the row's commit holds that release's catalog (BD-262, tools/state/check_history.py).
    hist, hist_failed = [], False
    for (repo, commit), ids in sorted(state_worker_commits(plan).items()):
        checkout = repos.find(repo, commit)
        if checkout is None:
            hist.append(f"{repo} {commit[:8]} ({', '.join(ids)}): in no checkout")
            hist_failed = True
            continue
        failures, lines = check_history.check(checkout, commit, plan["version"])
        hist += [f"{repo} {', '.join(ids)}: {l}" for l in lines] + [f"{repo} {', '.join(ids)}: FAIL {f}" for f in failures]
        hist_failed = hist_failed or bool(failures)
    gates.append({"name": "state-history", "status": "FAIL" if hist_failed else "PASS",
                  "lines": hist or [f"no row builds the state worker ({STATE_WORKER_RECIPE})"]})
    status = "FAIL" if any(g["status"] == "FAIL" for g in gates) else "PASS"
    return {"status": status, "gates": gates}


# ---------------------------------------------------------------------------------------------------------------
# Inside one build: the artifact.

def find_pdb(out, image_dir, ident):
    """The symbol file of an image: beside it, else the one file of that name under the output directory whose
    identity is the image's (a KMD build leaves bc250kmd.pdb in two folders)."""
    import symbols
    beside = Path(image_dir) / ident["file"]
    found = ([beside] if beside.is_file() else []) + sorted(p for p in Path(out).rglob(ident["file"])
                                                           if p.is_file() and p != beside)
    for p in found:
        if ident["kind"] != "pdb":
            return p
        try:
            have = symbols.pdb_identity(p)
        except (ValueError, OSError, struct.error):
            continue
        if (have["guid"], have["age"]) == (ident["guid"], ident["age"]):
            return p
    return None


def collect(job):
    """Copies the outputs, their PDBs and the build records into the artifact directory. Returns (entries, errors)."""
    import symbols
    out, art = Path(job["out"]), Path(job["artifact"])
    if art.exists():
        shutil.rmtree(art)
    art.mkdir(parents=True)
    entries, errors, done = [], [], {}
    for o in job["outputs"]:
        src = out / o["file"].replace("/", os.sep)
        if o["file"] in done:
            entries.append(dict(done[o["file"]], path=o["path"], flavour=o.get("flavour", job["flavour"])))
            continue
        if not src.is_file():
            errors.append(f"the build wrote no {o['file']}")
            continue
        dst = art / src.name
        shutil.copy2(src, dst)
        entry = {"file": o["file"], "path": o["path"], "flavour": o.get("flavour", job["flavour"]),
                 "artifact": str(dst), "sha256": sha256_file(dst), "size": dst.stat().st_size, "pdb": None}
        if src.suffix.lower() in symbols.PE_SUFFIXES:
            try:
                ident = symbols.pe_symbol_identity(src)
                # The digest of the image without its signature: a registered row of a file that its build signs
                # is compared by it, because a new signature changes the file and not the image.
                entry["image_sha256"] = provenance.authenticode_digest(dst)
            except (ValueError, OSError) as exc:
                errors.append(f"{o['file']}: {exc}")
                ident = {"kind": "none"}
            if ident["kind"] in ("pdb", "debuglink"):
                # The symbol file sits beside the artifact under the name the image records, where symbols.py
                # archive looks for it when the release packages this row.
                pdb = find_pdb(out, src.parent, ident)
                if pdb is None:
                    errors.append(f"{o['file']} names the symbol file {ident['file']}, and the build left no single "
                                  f"file of that name")
                else:
                    pdst = art / ident["file"]
                    shutil.copy2(pdb, pdst)
                    entry["pdb"] = {"kind": ident["kind"], "file": ident["file"], "artifact": str(pdst),
                                    "sha256": sha256_file(pdst), "size": pdst.stat().st_size}
                    if ident["kind"] == "pdb":
                        have = symbols.pdb_identity(pdst)
                        if (have["guid"], have["age"]) != (ident["guid"], ident["age"]):
                            errors.append(f"{pdb} carries {have['guid']} age {have['age']}, the image names "
                                          f"{ident['guid']} age {ident['age']}")
                        entry["pdb"].update(guid=ident["guid"], age=ident["age"])
            else:
                entry["pdb"] = {"kind": ident["kind"]}
            if o.get("authenticode") and entry.get("image_sha256"):
                entry["authenticode_sha256"] = entry["image_sha256"]
        if o.get("unsigned"):
            u = out / o["unsigned"].replace("/", os.sep)
            if not u.is_file():
                errors.append(f"the build wrote no {o['unsigned']}")
            else:
                udst = art / u.name
                shutil.copy2(u, udst)
                entry["unsigned"] = {"file": o["unsigned"], "artifact": str(udst), "sha256": sha256_file(udst)}
                if entry.get("image_sha256"):
                    entry["authenticode_sha256"] = entry["image_sha256"]
        done[o["file"]] = entry
        entries.append(entry)
    records = []
    for pattern in ["recipe.json"] + list(job.get("keep") or []):
        for f in sorted(out.glob(pattern)):
            if f.is_file():
                rel = f.relative_to(out)
                dst = art / "record" / rel
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(f, dst)
                records.append(str(rel).replace(os.sep, "/"))
    return {"outputs": entries, "records": records}, errors


# ---------------------------------------------------------------------------------------------------------------
# The rows.

def literal_args(build, version):
    values = {"jobs": str(build["jobs"]), "version": build.get("version") or version, "flavour": build["flavour"]}
    out = []
    for a in build["args"]:
        out.append(PLACEHOLDER.sub(lambda m: values[m.group(1)] if m.group(1) in LITERAL_PLACEHOLDERS else m.group(0), a))
    return out


# Fields of an output that only describe its row in release-sources.json, so a change of one keeps the artifact:
# texts, and 'authenticode', which the rows read from the image digest that every PE artifact carries.
OUTPUT_NOTES = ("pair_skew", "release_edits", "authenticode")


def build_key(build):
    """What decides the bytes of a build, so that an artifact of an earlier run stands for this manifest's row only
    while the row still says the same."""
    keys = ("repo", "commit", "recipe", "recipe_commit", "args", "inputs", "outputs", "compiler", "flavour", "version")
    key = {k: build.get(k) for k in keys}
    if isinstance(key["outputs"], list):
        key["outputs"] = [{k: v for k, v in o.items() if k not in OUTPUT_NOTES} if isinstance(o, dict) else o
                          for o in key["outputs"]]
    return json.dumps(key, sort_keys=True)


def make_rows(plan, results):
    """The release-sources.json rows of every output, and beside each row what the release does not read: the
    manifest row, whether its build claims determinism, its symbol file and the digests the comparison uses.

    results: id -> report entry of a build that ended well (this run or a kept earlier one), with its plan entry
    in 'definition'."""
    root = Path(plan["root"])
    rows, details, notes = [], [], []
    for rid in plan["rows_in_manifest"]:
        res = results.get(rid)
        if not res:
            if any(b["id"] == rid for b in plan["builds"]):
                notes.append(f"{rid}: the build did not end well, so it has no rows")
            continue
        b = res["definition"]
        by_path = {(o["path"], o["flavour"]): o for o in res["outputs"]}
        for o in b["outputs"]:
            flavour = o.get("flavour", b["flavour"])
            art = by_path[(o["path"], flavour)]
            built = {"repo": b["repo"], "commit": b["commit"]}
            if b["repo"] != plan["recipe_repository"] or b["recipe_commit"] != b["commit"]:
                built["recipe_commit"] = b["recipe_commit"]
            built.update({"recipe": b["recipe"], "args": literal_args(b, plan["version"])})
            if b.get("inputs"):
                built["inputs"] = b["inputs"]
            built["output"] = o["file"]
            if o.get("unsigned"):
                built["unsigned_output"] = o["unsigned"]
            row = {"component": o.get("component", b["component"]), "path": o["path"],
                   "source": Path(art["artifact"]).relative_to(root).as_posix(),
                   "sha256": art["sha256"], "built_from": built, "size": art["size"]}
            if art.get("unsigned"):
                row["unsigned_sha256"] = art["unsigned"]["sha256"]
            if art.get("authenticode_sha256"):
                row["authenticode_sha256"] = art["authenticode_sha256"]
            elif o.get("authenticode") and art.get("image_sha256"):
                row["authenticode_sha256"] = art["image_sha256"]
            for k in ("release_edits", "pair_skew"):
                if o.get(k):
                    row[k] = o[k]
            if b.get("role"):
                row["role"] = b["role"]
            if not b["deterministic"]:
                row["unverified"] = b["unverified"]
            row["flavour"] = flavour
            rows.append(row)
            details.append({"path": o["path"], "flavour": flavour, "row": rid, "deterministic": b["deterministic"],
                            "sha256": art["sha256"], "size": art["size"], "image_sha256": art.get("image_sha256"),
                            "unsigned_sha256": (art.get("unsigned") or {}).get("sha256"), "pdb": art.get("pdb")})
    return rows, details, notes


def compare_rows(details, sources_path):
    """Each output against the registered row of its path and flavour: match, match-image (the image without its
    signature is the registered one: the unsigned image or the Authenticode digest, as rebuild-check.ps1 reads
    them), differs, or no-row. An output whose build claims determinism must match."""
    registered = source_rows(sources_path)
    out, failed = [], 0
    for d in details:
        reg = registered.get((d["path"], "release" if d["flavour"] == "shared" else d["flavour"]))
        status = "no-row"
        if reg:
            if reg.get("sha256") == d["sha256"]:
                status = "match"
            elif ((reg.get("unsigned_sha256") and reg["unsigned_sha256"] == d.get("unsigned_sha256")) or
                  (reg.get("authenticode_sha256") and reg["authenticode_sha256"] == d.get("image_sha256"))):
                status = "match-image"
            else:
                status = "differs"
        ok = status in ("match", "match-image") or not d["deterministic"]
        failed += 0 if ok else 1
        out.append({"path": d["path"], "flavour": d["flavour"], "row": d["row"], "status": status,
                    "claimed": d["deterministic"], "sha256": d["sha256"], "registered": (reg or {}).get("sha256"),
                    "ok": ok})
    return out, failed


def merge_rows(rows, sources_path):
    """A copy of a release-sources.json with these rows in place of the rows of the same path and flavour."""
    data = load_json(sources_path)
    index = {(f.get("path"), f.get("flavour")): i for i, f in enumerate(data.get("files", []))}
    for r in rows:
        key = (r["path"], r["flavour"])
        if key in index:
            data["files"][index[key]] = r
        else:
            data["files"].append(r)
    return data


# The D3D11 deployment record (amdgpu_wddm_d3d11.config, tools/build/write-umd-config.py) pins the SHA-256 of the
# engine and the ICD beside it, so a rebuilt engine or ICD needs a new record. The first 68 bytes (header and the
# capability words) stay as they are: they are not measured again here, and the row says so.
CONFIG_NAME = "amdgpu_wddm_d3d11.config"
CONFIG_SIBLINGS = (("amdgpu_wddm_dxvk.dll", "engine_sha256"), ("amdgpu_wddm_radv.dll", "icd_sha256"))


def load_config_encoder(repo):
    spec = importlib.util.spec_from_file_location("write_umd_config", Path(repo) / "tools/build/write-umd-config.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.encode


def rebind_configs(data, root, version, out_dir, encode):
    """Rebind each D3D11 record whose engine or ICD changed to the hashes of the files the rows now name.

    Returns (receipts, errors). A record whose two siblings are unchanged keeps its row."""
    root, receipts, errors = Path(root), [], []
    files = data.get("files", [])
    for row in files:
        if not row["path"].endswith("/" + CONFIG_NAME):
            continue
        where = f"{row['path']} ({row['flavour']})"
        old_path = root / row["source"]
        old = old_path.read_bytes()
        caps = copy.deepcopy(((row.get("built_from") or {}).get("inputs") or {}).get("caps", {}).get("inline"))
        if not isinstance(caps, dict):
            errors.append(f"{where}: no inline caps input, so the record cannot be rebound")
            continue
        base = row["path"].rsplit("/", 1)[0]
        siblings, changed = [], False
        for name, key in CONFIG_SIBLINGS:
            match = [f for f in files if f["path"] == f"{base}/{name}" and f["flavour"] in (row["flavour"], "shared")]
            if len(match) != 1:
                errors.append(f"{where}: {len(match)} rows of {base}/{name}, 1 expected")
                break
            path = root / match[0]["source"]
            digest = sha256_file(path)
            if digest != match[0]["sha256"].upper():
                errors.append(f"{where}: {match[0]['source']} is {digest}, its row says {match[0]['sha256']}")
                break
            siblings.append(path)
            if caps[key].upper() != digest:
                caps[key], changed = digest, True
        else:
            if not changed:
                continue
            caps["carried_from"] = (caps.get("carried_from", "") + f"; {version}: engine and ICD hashes rebound to "
                                    f"the files of this release set by build-release-set.ps1, capabilities not "
                                    f"re-measured").lstrip("; ")
            encoded = encode(caps, *siblings)
            if encoded[:68] != old[:68]:
                errors.append(f"{where}: the rebound record changes the capability words, which this step must not")
                continue
            dst = Path(out_dir) / row["flavour"] / ("x86" if "/wow64/" in row["path"] else "x64") / CONFIG_NAME
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(encoded)
            before = row["sha256"]
            row["built_from"]["inputs"]["caps"]["inline"] = caps
            row.update(source=dst.resolve().relative_to(root.resolve()).as_posix(), sha256=sha256_file(dst),
                       size=len(encoded))
            row["role"] = (f"M14C deployment record rebound to the {version} engine and ICD. The first 68 bytes "
                           f"(header and capability words) are unchanged; capabilities were not re-measured.")
            receipts.append({"path": row["path"], "flavour": row["flavour"], "before_sha256": before,
                             "sha256": row["sha256"], "source": row["source"], "engine": caps["engine_sha256"],
                             "icd": caps["icd_sha256"]})
    return receipts, errors


# The summary texts of release-sources.json 'sources' name builds as "<flavour> <path>: <repo> <commit>", and the
# texts of provenance.PARSED_SOURCES start with "bc250-win <commit>" (release-baseline.py and the provenance gate read
# that commit). A registered row moves both along, so that no text names the build it replaced.
SOURCE_ENTRY = re.compile(r"\b(release|lab|shared) (payload/[^\s:]+): ([A-Za-z0-9_.-]+) ([0-9a-f]{40})\b")
SOURCE_HEAD = re.compile(r"^Current \S+ payload sources\.")


def refresh_sources(data, version, changed):
    """Rewrites the texts of data['sources'] that name a build of `changed` ((path, flavour) pairs) and returns
    their keys."""
    texts = data.get("sources")
    if not isinstance(texts, dict):
        return []
    release = version.split("-", 1)[1] if "-" in version else version
    rows = {(f.get("path"), f.get("flavour")): f for f in data.get("files", [])}

    def built(path, flavour):
        b = (rows.get((path, flavour)) or rows.get((path, "shared")) or {}).get("built_from") or {}
        return b.get("repo"), b.get("commit")

    def entry(m):
        repo, commit = built(m.group(2), m.group(1))
        if (m.group(2), m.group(1)) not in changed or not repo or not commit:
            return m.group(0)
        return f"{m.group(1)} {m.group(2)}: {repo} {commit}"

    keys = []
    for key, text in texts.items():
        if not isinstance(text, str):
            continue
        new = SOURCE_ENTRY.sub(entry, text)
        path = provenance.PARSED_SOURCES.get(key)
        if path and any(p == path for p, _ in changed):
            _, commit = built(path, "release")
            if commit:
                new = (f"bc250-win {commit} (KMD {data.get('kmd_version')}): the {release} KMD, both flavours built "
                       f"by the release set at that commit; the rows of {path} name the recipe and its arguments.")
        if new != text:
            texts[key] = SOURCE_HEAD.sub(f"Current {release} payload sources.", new)
            keys.append(key)
    return keys


def register(rows_doc, sources_path, root, repo, version, configs_out):
    """The registered release-sources.json with this set's rows and rebound D3D11 records, and what changed."""
    data = merge_rows(rows_doc["files"], sources_path)
    refresh_sources(data, version, {(r["path"], r["flavour"]) for r in rows_doc["files"]})
    receipts, errors = rebind_configs(data, root, version, configs_out, load_config_encoder(repo))
    for f in data.get("files", []):
        p = Path(root) / f["source"]
        if not p.is_file():
            errors.append(f"{f['path']} ({f.get('flavour')}): {f['source']} does not exist")
        elif sha256_file(p) != f["sha256"].upper():
            errors.append(f"{f['path']} ({f.get('flavour')}): {f['source']} does not match its sha256")
        elif "size" in f and p.stat().st_size != f["size"]:
            errors.append(f"{f['path']} ({f.get('flavour')}): {f['source']} does not match its size")
    return data, receipts, errors


# ---------------------------------------------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("plan")
    p.add_argument("--manifest", required=True)
    p.add_argument("--root", required=True)
    p.add_argument("--repo", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--work", required=True)
    p.add_argument("--sources")
    p.add_argument("--only", default="")
    p.add_argument("--version")
    p.add_argument("--max-parallel", type=int, default=3)
    p.add_argument("--jobs", type=int, default=0)
    p.add_argument("--logical", type=int)
    p.add_argument("--write", required=True)
    g = sub.add_parser("gates")
    g.add_argument("--plan", required=True)
    g.add_argument("--recipe-params", required=True)
    g.add_argument("--allow-unpushed", action="store_true")
    g.add_argument("--write", required=True)
    c = sub.add_parser("collect")
    c.add_argument("--job", required=True)
    r = sub.add_parser("rows")
    r.add_argument("--plan", required=True)
    r.add_argument("--report", required=True)
    r.add_argument("--write", required=True)
    r.add_argument("--compare")
    r.add_argument("--merge")
    r.add_argument("--merged")
    reg = sub.add_parser("register")
    reg.add_argument("--rows", required=True)
    reg.add_argument("--sources", required=True)
    reg.add_argument("--root", required=True)
    reg.add_argument("--repo", required=True)
    reg.add_argument("--configs-out", required=True)
    reg.add_argument("--receipt", required=True)
    args = parser.parse_args()
    if args.command == "register":
        rows_doc = load_json(args.rows)
        data, receipts, errors = register(rows_doc, args.sources, args.root, args.repo, rows_doc["version"],
                                          args.configs_out)
        for e in errors:
            print(f"FAIL {e}")
        if errors:
            return 1
        before = sha256_file(args.sources)
        Path(args.sources).write_bytes((json.dumps(data, indent=2) + "\n").encode("utf-8"))
        write_json(args.receipt, {"tool": "build-release-set", "version": rows_doc["version"],
                                  "sources": str(Path(args.sources).resolve()), "before_sha256": before,
                                  "sha256": sha256_file(args.sources), "rows": len(rows_doc["files"]),
                                  "rebound_configs": receipts})
        print(f"registered {len(rows_doc['files'])} rows and {len(receipts)} rebound D3D11 records in "
              f"{args.sources}")
        return 0
    if args.command == "plan":
        only = [x.strip() for x in args.only.split(",") if x.strip()]
        plan, errors = make_plan(args.manifest, args.root, args.repo, args.out, args.work, args.sources, only,
                                 args.version, args.max_parallel, args.jobs, args.logical)
        for e in errors:
            print(f"FAIL {e}")
        if errors:
            return 1
        write_json(args.write, plan)
        print(f"plan: {len(plan['builds'])} builds, {len(plan['kept'])} kept from an earlier run, "
              f"{len(plan['shared_trees'])} shared trees, {plan['jobs_per_slot']} jobs per slot")
        return 0
    if args.command == "gates":
        result = run_gates(load_json(args.plan), load_json(args.recipe_params), args.allow_unpushed)
        write_json(args.write, result)
        for gate in result["gates"]:
            print(f"gate {gate['name']}: {gate['status']}")
            for line in gate["lines"]:
                print(f"  {line}")
        return 0 if result["status"] == "PASS" else 1
    if args.command == "collect":
        job = load_json(args.job)
        result, errors = collect(job)
        result["errors"] = errors
        write_json(Path(job["log_dir"]) / "collect.json", result)
        for e in errors:
            print(f"FAIL {e}")
        for o in result["outputs"]:
            pdb = o.get("pdb") or {}
            print(f"  {o['sha256'][:8]} {o['size']:>10} {o['path']} ({o['flavour']})"
                  f"{'  pdb ' + pdb['file'] if pdb.get('file') else ''}")
        return 1 if errors else 0
    if args.command == "rows":
        plan, report = load_json(args.plan), load_json(args.report)
        results = {b["id"]: b for b in report.get("builds", []) if b.get("status") == "ok"}
        rows, details, notes = make_rows(plan, results)
        doc = {"tool": "build-release-set", "release": plan["release"], "version": plan["version"], "files": rows,
               "artifacts": details, "notes": notes}
        failed = 0
        if args.compare:
            doc["compare"], failed = compare_rows(details, args.compare)
            for c in doc["compare"]:
                tag = "OK  " if c["ok"] else "FAIL"
                print(f"{tag} {c['status']:<12} {c['sha256'][:8]} {c['path']} ({c['flavour']}, row {c['row']}"
                      f"{', claims a deterministic build' if c['claimed'] else ''}"
                      f"{', registered ' + c['registered'][:8] if c['registered'] and c['status'] == 'differs' else ''})")
            print(f"compare: {sum(c['ok'] for c in doc['compare'])} of {len(doc['compare'])} rows ok, {failed} fail")
        write_json(args.write, doc)
        if args.merge:
            write_json(args.merged, merge_rows(rows, args.merge))
            print(f"merged {len(rows)} rows into a copy of {args.merge}: {args.merged}")
        for n in notes:
            print(f"  note: {n}")
        return 1 if failed else 0
    return 2


if __name__ == "__main__":
    sys.exit(main())
