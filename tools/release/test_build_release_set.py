"""Self-test of the release set build (tools/release/build-release-set.ps1 and build_release_set.py).

    python -m unittest discover -s tools/release -p test_build_release_set.py   # the 'release-build-set' gate of quick.ps1

Each test makes a throwaway workspace: a recipe repository with stub recipes and a bare "published" remote, a stub
size guard that reports the numbers the test gives it, and a release manifest. The unit tests run the schema, the plan
and the gates in process: the example manifest is valid, an unpushed commit, an AI agent named in a commit and an
implicit or refused compiler fail their gates, the names of the models this project runs on the GPU do not, and the
collection of a build's outputs takes the PDB that each image names, not another file of that name.
The end-to-end tests run build-release-set.ps1 (pwsh 7 on PATH; skipped without it): the scheduler keeps to -MaxParallel
and to the disk budget, -Plan builds nothing, the gates refuse an unpushed commit, an AI trailer and an implicit
compiler through the tool, and a failed build fails the release set and keeps its log while its tree goes. A tree
comes from the first checkout of its commit whose module stores hold every submodule commit, and -Plan fails the
commits gate when no checkout does (BD-285).
BC250_TEST_OUT, when set, holds the temporary trees (quick.ps1 keeps them off drive C:).
"""
import copy
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

import build_release_set
import build_context
from test_provenance import git, remove_tree
from test_symbols import msf_pdb_bytes, pe_bytes

OUT = os.environ.get("BC250_TEST_OUT") or None
# The fixture commits as test@example.com (test_provenance.git): that is the fork. A simulated upstream commit carries
# an identity of its own.
FORK_EMAIL = "test@example.com"
FORK_ID = f"test <{FORK_EMAIL}>"
UPSTREAM_ID = "Upstream Author <dev@upstream.example>"
HERE = Path(__file__).resolve().parent
TOOL = HERE / "build-release-set.ps1"
GB = 10 ** 9

# A recipe that sleeps, records when it ran, and writes one output. -Payload is a file of another row, whose hash
# goes into the output, so that the order of two builds shows in the bytes.
STUB = b"""param([string]$Out, [string]$Name, [double]$Sleep = 0, [switch]$Fail,
    [ValidateSet('clang-cl', 'msvc')][string]$Compiler, [string]$Events, [string]$Payload, [string]$Version)
"recipe $Name start, compiler $Compiler, version $Version"
if ($Events) { New-Item -ItemType Directory -Force $Events | Out-Null; [IO.File]::WriteAllText((Join-Path $Events "$Name.start"), [string][DateTime]::UtcNow.Ticks) }
Start-Sleep -Milliseconds ([int]($Sleep * 1000))
if ($Fail) { "recipe $Name fails on purpose"; exit 1 }
$text = "built $Name $Compiler $Version"
if ($Payload) { $text += ' from ' + (Get-FileHash -LiteralPath $Payload).Hash }
[IO.File]::WriteAllText((Join-Path $Out "$Name.bin"), $text)
if ($Events) { [IO.File]::WriteAllText((Join-Path $Events "$Name.end"), [string][DateTime]::UtcNow.Ticks) }
"""
# A recipe that writes a PE image and stamps the release version into it as its last step, as the real recipes do.
PESTUB = b"""param([string]$Out, [string]$Name, [ValidateSet('clang-cl', 'msvc')][string]$Compiler, [string]$Version,
    [string]$Image, [string]$Stamp, [string]$Inf, [string]$Python)
$dll = Join-Path $Out "$Name.dll"
Copy-Item -LiteralPath $Image $dll
& $Python -I $Stamp stamp --version $Version --inf $Inf --name amdgpu_wddm_d3d12.dll $dll
exit $LASTEXITCODE
"""
# A host test with two summary lines, as the RADV queue tests print them.
HOSTTEST = b"""param([string]$Build)
if (-not (Test-Path -LiteralPath $Build)) { "no build $Build"; exit 2 }
'case one: checks=3, failures=0, skipped=1'
'case two: checks=4, failures=0, skipped=0'
'PASSED: 0 failure(s)'
"""
# A recipe without -Compiler: its rows may name msvc (with a reason), never clang-cl.
PLAIN = b"""param([string]$Out)
[IO.File]::WriteAllText((Join-Path $Out 'plain.bin'), 'plain')
"""
GUARD = b"""import json, sys, time
from pathlib import Path
here = Path(__file__).resolve().parent
c = json.loads((here / 'config.json').read_text())
now = int(time.time())
(here / 'size-latest.json').write_text(json.dumps({
    'utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime(now)), 'epoch': now, 'counted_bytes': c['counted'],
    'warn_line_bytes': c['warn'], 'release_line_bytes': c['warn'], 'free_bytes': c['free'],
    'free_warn_bytes': c['free_warn'], 'free_floor_bytes': c['free_warn']}))
over = c['counted'] >= c['warn'] or c['free'] <= c['free_warn']
print('stub size %s: counted %d' % ('OVER' if over else 'ok', c['counted']))
sys.exit(1 if over else 0)
"""


class Workspace:
    def __init__(self, base):
        self.base = Path(base)
        self.root = self.base / "ws"
        self.remotes = self.base / "remotes"
        self.root.mkdir()
        self.remotes.mkdir()
        self.repo = self.root / "recipes"
        self.repo.mkdir()
        git(self.repo, "init", "-q")
        git(self.repo, "symbolic-ref", "HEAD", "refs/heads/main")
        git(self.repo, "config", "core.autocrlf", "false")
        (self.repo / "README.txt").write_bytes(b"recipes of a test release set\n")
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-q", "-m", "the previous release")
        self.base_commit = git(self.repo, "rev-parse", "HEAD")
        for name, data in (("stub.ps1", STUB), ("hosttest.ps1", HOSTTEST), ("plain.ps1", PLAIN)):
            (self.repo / name).write_bytes(data)
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-q", "-m", "stub recipes")
        self.head = git(self.repo, "rev-parse", "HEAD")
        self.remote = self.remotes / "recipes.git"
        git(self.remotes, "init", "-q", "--bare", str(self.remote))
        git(self.repo, "remote", "add", "origin", str(self.remote))
        git(self.repo, "push", "-q", "origin", "HEAD:refs/heads/main")
        git(self.repo, "fetch", "-q", "origin")
        self.guard_dir = self.base / "guard"
        self.guard_dir.mkdir()
        (self.guard_dir / "size-guard.py").write_bytes(GUARD)
        self.set_disk(counted=0, warn=1000 * GB, free=1000 * GB, free_warn=0)
        self.out = self.root / "out"
        self.work = self.base / "bt"
        self.events = self.root / "events"
        self.manifest_path = self.base / "release.json"

    def module_checkouts(self):
        """BD-285: one repository "mod" with a submodule "sub" that has a submodule "inner", in three checkouts of the
        same commit: mod-bare (a plain clone, no module store), mod-half (sub initialised, inner not) and mod-full
        (every module store). Returns the commit."""
        def repo(name, text):
            path = self.root / name
            path.mkdir()
            git(path, "init", "-q")
            git(path, "symbolic-ref", "HEAD", "refs/heads/main")
            (path / f"{name}.txt").write_text(text, encoding="utf-8")
            git(path, "add", "-A")
            git(path, "commit", "-q", "-m", text)
            return path
        repo("innersrc", "inner")
        sub = repo("subsrc", "sub")
        git(sub, "submodule", "add", "-q", str(self.root / "innersrc"), "inner")
        git(sub, "commit", "-q", "-m", "inner")
        full = repo("mod-full", "mod")
        git(full, "submodule", "add", "-q", str(sub), "sub")
        git(full, "submodule", "update", "-q", "--init", "--recursive")
        git(full, "commit", "-q", "-m", "sub")
        git(self.root, "clone", "-q", str(full), "mod-bare")
        git(self.root, "clone", "-q", str(full), "mod-half")
        git(self.root / "mod-half", "submodule", "update", "-q", "--init")
        return git(full, "rev-parse", "HEAD")

    def set_disk(self, **numbers):
        (self.guard_dir / "config.json").write_text(json.dumps(numbers), encoding="utf-8")

    def commit(self, message, name="change.txt"):
        (self.repo / name).write_text(message, encoding="utf-8")
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-q", "-m", message)
        return git(self.repo, "rev-parse", "HEAD")

    def row(self, rid, sleep=2, peak=1, extra=(), commit=None, **fields):
        r = {"id": rid, "component": "stub", "flavour": "release", "arch": "x64", "repo": "recipes",
             "commit": commit or self.head, "recipe": "stub.ps1",
             "args": ["-Out", "{out}", "-Name", rid, "-Sleep", str(sleep), "-Compiler", "clang-cl",
                      "-Events", "{root}/events", "-Version", "{version}", *extra],
             "compiler": "clang-cl", "kind": "user", "deterministic": True, "tree": "shared", "disk_peak_gb": peak,
             "outputs": [{"file": f"{rid}.bin", "path": f"payload/{rid}.bin"}]}
        r.update(fields)
        return r

    def manifest(self, rows, recipe_commit=None):
        return {"schema": 1, "release": "t1", "version": "1.2.3.4-test.1", "recipe_repository": "recipes",
                "recipe_commit": recipe_commit or self.head,
                "repositories": {"recipes": {"url": str(self.remote), "checkouts": ["recipes"],
                                             "base": self.base_commit}},
                "rows": rows}

    def write(self, manifest):
        self.manifest_path.write_text(json.dumps(manifest, indent=1), encoding="utf-8")
        return self.manifest_path

    def plan(self, manifest, **kw):
        self.write(manifest)
        kw.setdefault('reuse_context', build_context.capture_context(self.root, HERE.parents[1], environ={},
                                                                     executables={}, discover_vs=False))
        return build_release_set.make_plan(self.manifest_path, self.root, HERE.parents[1], self.out, self.work, **kw)

    def child_environment(self):
        # These recipes write tiny text files. No VS/SDK scan or user tool configuration belongs to this fixture.
        excluded = set(build_context.ENVIRONMENT) - {'PATH', 'PATHEXT'}
        excluded.add('PROGRAMFILES(X86)')
        return {k: v for k, v in os.environ.items() if k.upper() not in excluded and not k.upper().startswith('BC250_TOOL_')}

    def run(self, manifest, *extra, timeout=600):
        self.write(manifest)
        r = subprocess.run(["pwsh", "-NoProfile", "-File", str(TOOL), "-Manifest", str(self.manifest_path),
                            "-Out", str(self.out), "-Root", str(self.root), "-Work", str(self.work),
                            "-SizeGuard", str(self.guard_dir / "size-guard.py"), *extra],
                           capture_output=True, text=True, timeout=timeout, env=self.child_environment())
        return r.returncode, r.stdout + r.stderr

    def intervals(self):
        """id -> (start, end) in ticks, from the files the stub recipe wrote."""
        out = {}
        for start in self.events.glob("*.start"):
            end = start.with_suffix(".end")
            out[start.stem] = (int(start.read_text()), int(end.read_text()) if end.exists() else None)
        return out

    def report(self):
        return json.loads((self.out / "release-report.json").read_text(encoding="utf-8-sig"))


def most_at_once(intervals):
    points = sorted([(s, 1) for s, e in intervals.values()] + [(e, -1) for s, e in intervals.values()],
                    key=lambda p: (p[0], p[1]))
    now = best = 0
    for _, step in points:
        now += step
        best = max(best, now)
    return best


PARAMS = ["Out", "Name", "Sleep", "Fail", "Compiler", "Events", "Payload", "Version"]


# A commit message that attributes a rule to the repository owner with a date. The literal is split so that this
# file does not match the pattern it tests.
OWNER_QUOTE = "fix a thing\n\nThe driver sets the value (own" "er, 2026-10-08).\n"


class ReleaseSetPlanAndGates(unittest.TestCase):
    """build_release_set.py in process: no pwsh, no build."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="release-set-", dir=OUT)
        self.ws = Workspace(self.tmp)

    def tearDown(self):
        remove_tree(self.tmp)

    def gates(self, manifest, allow_unpushed=False):
        plan, errors = self.ws.plan(manifest)
        self.assertEqual(errors, [])
        params = {f"{r['commit']}:{r['recipe']}": {"params": PARAMS if r["recipe"] == "stub.ps1" else ["Out"]}
                  for r in plan["recipes"]}
        result = build_release_set.run_gates(plan, params, allow_unpushed)
        return result, {g["name"]: g for g in result["gates"]}

    def test_the_example_manifest_is_valid(self):
        manifest = build_release_set.load_json(HERE / "release-tester.32.json")
        self.assertEqual(build_release_set.check_manifest(manifest), [])

    def seed_success(self, manifest, **options):
        """Seed explicit byte receipts for planner tests; no compiler is needed."""
        plan, errors = self.ws.plan(manifest, **options)
        self.assertEqual(errors, [])
        entries, digests = [], {}
        for b in plan['builds']:
            outputs = []
            for o in b['outputs']:
                path = Path(b['artifact']) / Path(o['file']).name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(('fixture ' + b['id'] + o['path']).encode())
                digest = build_release_set.sha256_file(path)
                outputs.append(dict(o, artifact=str(path), flavour=o.get('flavour', b['flavour']), sha256=digest))
                digests[o['path']] = digest
            b['consumed_payload_sha256'] = {
                p: build_release_set.sha256_file(v['file']) if 'file' in v else digests[p]
                for p, v in b['payload_from'].items()}
            entries.append(dict(id=b['id'], status='ok', definition=b, outputs=outputs))
        build_release_set.write_json(self.ws.out / build_release_set.REPORT, {'builds': entries})
        return entries

    def test_default_reuses_unchanged_rows_and_rebuild_is_explicit(self):
        m = self.ws.manifest([self.ws.row('a'), self.ws.row('b', extra=['-Payload', '{payload:payload/a.bin}'])])
        self.seed_success(m)
        plan, errors = self.ws.plan(m)
        self.assertEqual(errors, [])
        self.assertEqual(plan['builds'], [])
        self.assertEqual(len(plan['kept']), 2)
        plan, errors = self.ws.plan(m, rebuild=True)
        self.assertEqual(errors, [])
        self.assertEqual([b['id'] for b in plan['builds']], ['a', 'b'])
        self.assertEqual(plan['kept'], [])
        for options in ({'only': ['a']}, {'keep_from': self.ws.out}):
            plan, errors = self.ws.plan(m, rebuild=True, **options)
            self.assertIsNone(plan)
            self.assertIn('cannot be combined', errors[0])

    def test_default_repairs_bad_artifact_and_consumers_but_keeps_unrelated_row(self):
        m = self.ws.manifest([self.ws.row('a'), self.ws.row('b', extra=['-Payload', '{payload:payload/a.bin}']),
                              self.ws.row('c')])
        entries = self.seed_success(m)
        path = Path(entries[0]['outputs'][0]['artifact'])
        for missing in (False, True):
            if missing:
                path.unlink()
            else:
                path.write_bytes(b'corrupt')
            plan, errors = self.ws.plan(m)
            self.assertEqual(errors, [])
            self.assertEqual([b['id'] for b in plan['builds']], ['a', 'b'])
            self.assertEqual([b['id'] for b in plan['kept']], ['c'])

    def test_default_checks_consumed_bytes_even_when_producer_receipt_is_valid(self):
        m = self.ws.manifest([self.ws.row('a'), self.ws.row('b', extra=['-Payload', '{payload:payload/a.bin}'])])
        entries = self.seed_success(m)
        o = entries[0]['outputs'][0]
        Path(o['artifact']).write_bytes(b'new producer from a separate partial build')
        o['sha256'] = build_release_set.sha256_file(o['artifact'])
        build_release_set.write_json(self.ws.out / build_release_set.REPORT, {'builds': entries})
        plan, errors = self.ws.plan(m)
        self.assertEqual(errors, [])
        self.assertEqual([b['id'] for b in plan['builds']], ['b'])
        self.assertIn('consumed payload', plan['changed']['b'])

    def test_default_requires_complete_outputs_and_test_identity(self):
        m = self.ws.manifest([self.ws.row('a', outputs=[{'file': 'a.bin', 'path': 'payload/a.bin'},
                                                       {'file': 'b.bin', 'path': 'payload/b.bin'}])])
        entries = self.seed_success(m)
        entries[0]['outputs'].pop()
        build_release_set.write_json(self.ws.out / build_release_set.REPORT, {'builds': entries})
        plan, errors = self.ws.plan(m)
        self.assertEqual(errors, [])
        self.assertIn('every declared output', plan['changed']['a'])
        self.seed_success(m)
        m['rows'][0]['tests'] = [{'name': 'new-check', 'run': '{recipe}/hosttest.ps1', 'args': []}]
        plan, errors = self.ws.plan(m)
        self.assertEqual(errors, [])
        self.assertIn('host test', plan['changed']['a'])

    def test_external_payload_identity_survives_repeated_reuse_and_detects_change(self):
        source = self.ws.root / 'external.bin'
        source.write_bytes(b'external first')
        sources = self.ws.root / 'sources.json'
        build_release_set.write_json(sources, {'files': [
            {'path': 'payload/ext.bin', 'flavour': 'release', 'source': 'external.bin'}]})
        m = self.ws.manifest([self.ws.row('a', extra=['-Payload', '{payload:payload/ext.bin}'])])
        self.seed_success(m, sources_path=sources)
        for _ in range(2):
            plan, errors = self.ws.plan(m, sources_path=sources)
            self.assertEqual(errors, [])
            self.assertEqual(plan['builds'], [])
            build_release_set.write_json(self.ws.out / build_release_set.REPORT, {'builds': plan['kept']})
        source.write_bytes(b'external second')
        plan, errors = self.ws.plan(m, sources_path=sources)
        self.assertEqual(errors, [])
        self.assertEqual([b['id'] for b in plan['builds']], ['a'])
        self.assertIn('external payload', plan['changed']['a'])

    def test_failed_and_running_rows_are_retried_while_successes_remain(self):
        m = self.ws.manifest([self.ws.row('a'), self.ws.row('b'), self.ws.row('c')])
        entries = self.seed_success(m)
        entries[1]['status'], entries[2]['status'] = 'failed', 'running'
        build_release_set.write_json(self.ws.out / build_release_set.REPORT, {'builds': entries})
        plan, errors = self.ws.plan(m)
        self.assertEqual(errors, [])
        self.assertEqual([b['id'] for b in plan['builds']], ['b', 'c'])
        self.assertEqual([b['id'] for b in plan['kept']], ['a'])

    def test_tool_bytes_change_or_missing_context_refuses_automatic_reuse(self):
        m = self.ws.manifest([self.ws.row('a')])
        tools = self.ws.root / 'toolchain'
        tools.mkdir()
        compiler = tools / 'compiler.bin'
        compiler.write_bytes(b'compiler one')
        entries = self.seed_success(m)
        compiler.write_bytes(b'compiler two')
        plan, errors = self.ws.plan(m)
        self.assertEqual(errors, [])
        self.assertIn('build tools', plan['changed']['a'])
        compiler.write_bytes(b'compiler one')
        del entries[0]['definition']['reuse_context']
        build_release_set.write_json(self.ws.out / build_release_set.REPORT, {'builds': entries})
        plan, errors = self.ws.plan(m)
        self.assertEqual(errors, [])
        self.assertIn('no recorded content identity', plan['changed']['a'])

    def test_default_tree_and_reused_rows_still_run_current_policy_gates(self):
        head = self.ws.commit('not published yet')
        row = self.ws.row('a', commit=head)
        del row['tree']
        m = self.ws.manifest([row], recipe_commit=head)
        self.seed_success(m)
        plan, errors = self.ws.plan(m)
        self.assertEqual(errors, [])
        self.assertEqual(plan['builds'], [])
        self.assertEqual(plan['kept'][0]['definition']['tree'], 'worktree')
        params = {f'{head}:stub.ps1': {'params': PARAMS}}
        result = build_release_set.run_gates(plan, params)
        gates = {g['name']: g for g in result['gates']}
        self.assertEqual(gates['published']['status'], 'FAIL')
        self.assertEqual(gates['compiler']['status'], 'PASS')
        # Compiler checks must also execute when no row needs compilation.
        result = build_release_set.run_gates(plan, {})
        self.assertEqual(next(g for g in result['gates'] if g['name'] == 'compiler')['status'], 'FAIL')

    def test_the_schema_refuses_an_unknown_field_and_a_missing_compiler(self):
        row = self.ws.row("a")
        row["colour"] = "red"
        del row["compiler"]
        errors = build_release_set.check_manifest(self.ws.manifest([row]))
        self.assertTrue(any("unknown field 'colour'" in e for e in errors), errors)
        self.assertTrue(any("needs 'compiler'" in e for e in errors), errors)

    def test_a_row_without_a_deterministic_build_says_why(self):
        errors = build_release_set.check_manifest(self.ws.manifest([self.ws.row("a", deterministic=False)]))
        self.assertTrue(any("says in 'unverified' why" in e for e in errors), errors)

    def test_the_plan_orders_by_payload_and_divides_the_cores(self):
        rows = [self.ws.row("b", extra=["-Payload", "{payload:payload/a.bin}"]), self.ws.row("a"),
                self.ws.row("k", slots=9)]
        plan, errors = self.ws.plan(self.ws.manifest(rows), max_parallel=3, logical=24)
        self.assertEqual(errors, [])
        builds = {b["id"]: b for b in plan["builds"]}
        self.assertEqual([b["id"] for b in plan["builds"]].index("a") <
                         [b["id"] for b in plan["builds"]].index("b"), True)
        self.assertEqual(builds["b"]["deps"], ["a"])
        self.assertEqual(builds["a"]["jobs"], 8)
        self.assertEqual((builds["k"]["slots"], builds["k"]["jobs"]), (3, 24))
        self.assertEqual(len(plan["shared_trees"]), 1)

    def test_a_kept_row_keeps_the_jobs_it_was_built_with(self):
        rows = [self.ws.row("a", extra=["-Jobs", "{jobs}"]), self.ws.row("b")]
        manifest = self.ws.manifest(rows)
        first, errors = self.ws.plan(manifest, max_parallel=3, logical=24)
        self.assertEqual(errors, [])
        a = next(b for b in first["builds"] if b["id"] == "a")
        self.assertEqual(a["jobs"], 8)
        # An earlier run in the output directory built 'a' with 8 jobs.
        art = self.ws.out / "artifacts" / "release" / "a" / "a.bin"
        art.parent.mkdir(parents=True, exist_ok=True)
        art.write_bytes(b"built a")
        entry = {"id": "a", "status": "ok", "definition": a,
                 "outputs": [{"file": "a.bin", "path": "payload/a.bin", "flavour": "release", "artifact": str(art),
                              "sha256": build_release_set.sha256_file(art), "size": art.stat().st_size}]}
        (self.ws.out / build_release_set.REPORT).write_text(json.dumps({"builds": [entry]}), encoding="utf-8")
        plan, errors = self.ws.plan(manifest, only=["b"], max_parallel=1, logical=24)
        self.assertEqual(errors, [])
        self.assertEqual(next(b for b in plan["builds"] if b["id"] == "b")["jobs"], 24)
        kept = next(k for k in plan["kept"] if k["id"] == "a")
        self.assertEqual(kept["definition"]["jobs"], 8)
        args = build_release_set.literal_args(kept["definition"], plan["version"])
        self.assertEqual(args[args.index("-Jobs") + 1], "8")

    def test_only_needs_the_rows_it_reads(self):
        rows = [self.ws.row("a"), self.ws.row("b", extra=["-Payload", "{payload:payload/a.bin}"])]
        plan, errors = self.ws.plan(self.ws.manifest(rows), only=["b"])
        self.assertIsNone(plan)
        self.assertTrue(any("add a to --only" in e for e in errors), errors)

    def test_published_commits_pass_every_gate(self):
        result, gates = self.gates(self.ws.manifest([self.ws.row("a")]))
        self.assertEqual(result["status"], "PASS", json.dumps(result, indent=1))
        self.assertIn("1 commits, 0 hits", " ".join(gates["attribution"]["lines"]))

    def mod_manifest(self, commit, checkouts):
        manifest = self.ws.manifest([self.ws.row("a", inputs={"mod": {"repo": "mod", "commit": commit}})])
        manifest["repositories"]["mod"] = {"url": "https://example.invalid/mod", "checkouts": checkouts}
        return manifest

    def test_a_tree_comes_from_the_first_checkout_with_every_module_store(self):
        # BD-285: locate gave the first checkout of the commit, New-Tree then found no module store there and the row
        # failed, while a later checkout held every module.
        import provenance
        commit = self.ws.module_checkouts()
        table = {"mod": {"url": "https://example.invalid/mod", "checkouts": ["mod-bare", "mod-half", "mod-full"]}}
        repos = provenance.Repositories(table, self.ws.root, self.ws.repo)
        self.assertEqual(repos.find("mod", commit), self.ws.root / "mod-bare", "the old answer: the first checkout")
        found, refused = repos.find_tree("mod", commit)
        self.assertEqual(found, self.ws.root / "mod-full")
        self.assertEqual(len(refused), 2, refused)
        self.assertIn("mod-bare", refused[0])
        self.assertIn("submodule sub has no module store", refused[0])
        self.assertIn("mod-half", refused[1])
        self.assertIn("submodule sub/inner has no module store", refused[1])
        # No checkout qualifies: each one is named with what it lacks.
        table["mod"]["checkouts"] = ["mod-bare", "mod-half"]
        found, refused = provenance.Repositories(table, self.ws.root, self.ws.repo).find_tree("mod", commit)
        self.assertIsNone(found)
        self.assertEqual(len(refused), 2, refused)
        # A module store that exists but lacks the gitlink commit is refused as well.
        full = self.ws.root / "mod-full"
        (self.ws.root / "subsrc" / "more.txt").write_text("more", encoding="utf-8")
        git(self.ws.root / "subsrc", "add", "-A")
        git(self.ws.root / "subsrc", "commit", "-q", "-m", "more")
        git(full / "sub", "fetch", "-q", "origin")
        git(full / "sub", "checkout", "-q", "--detach", "origin/main")
        git(full, "add", "sub")
        git(full, "commit", "-q", "-m", "sub moves")
        newer = git(full, "rev-parse", "HEAD")
        # Without --no-recurse-submodules git fetches the new sub commit into the populated module store as well.
        git(self.ws.root / "mod-half", "fetch", "-q", "--no-recurse-submodules", "origin")
        table["mod"]["checkouts"] = ["mod-half", "mod-full"]
        found, refused = provenance.Repositories(table, self.ws.root, self.ws.repo).find_tree("mod", newer)
        self.assertEqual(found, full)
        self.assertRegex(refused[0], r"submodule sub: module store .* has no commit [0-9a-f]{8}")

    def test_the_plan_refuses_a_tree_that_no_checkout_can_make(self):
        commit = self.ws.module_checkouts()
        _, gates = self.gates(self.mod_manifest(commit, ["mod-bare", "mod-full"]), allow_unpushed=True)
        self.assertEqual(gates["commits"]["status"], "PASS", gates["commits"])
        _, gates = self.gates(self.mod_manifest(commit, ["mod-bare", "mod-half"]), allow_unpushed=True)
        self.assertEqual(gates["commits"]["status"], "FAIL", gates["commits"])
        text = " ".join(gates["commits"]["lines"])
        self.assertIn("no checkout can make its tree with every submodule", text)
        self.assertIn("a input mod", text)
        self.assertIn("mod-bare", text)
        self.assertIn("mod-half", text)

    def test_the_state_history_gate(self):
        # A row that builds the state worker: driver/state/history at its commit holds the catalog of the previous
        # tester tag (BD-262). The tester.33 catalog of this tree is the previous release; then its file goes.
        t33 = "55ac2dcef76b92b5c52faf145d4a38e3fd31a77ed43b366bab38e33f3d6d1bf9"
        src, state = HERE.parents[1] / "driver" / "state", self.ws.repo / "driver" / "state"
        (state / "history").mkdir(parents=True)
        shutil.copyfile(src / "history" / f"{t33}.json", state / "catalog.json")
        git(self.ws.repo, "add", "-A")
        git(self.ws.repo, "commit", "-q", "-m", "the previous release")
        git(self.ws.repo, "tag", "v1.2.3.3-tester.0")
        shutil.copyfile(src / "catalog.json", state / "catalog.json")
        for f in (src / "history").glob("*.json"):
            shutil.copyfile(f, state / "history" / f.name)

        def gate(message):
            git(self.ws.repo, "add", "-A")
            git(self.ws.repo, "commit", "-q", "-m", message)
            commit = git(self.ws.repo, "rev-parse", "HEAD")
            git(self.ws.repo, "push", "-q", "origin", "HEAD:refs/heads/main")
            git(self.ws.repo, "fetch", "-q", "origin")
            row = self.ws.row("state-worker-rel", commit=commit, recipe="driver/state/installer-build.ps1")
            return self.gates(self.ws.manifest([row, self.ws.row("a")]))[1]["state-history"]

        g = gate("this release, with the history")
        self.assertEqual(g["status"], "PASS", g["lines"])
        self.assertIn("v1.2.3.3-tester.0: catalog 55ac2dce is in driver/state/history", " ".join(g["lines"]))
        (state / "history" / f"{t33}.json").unlink()
        g = gate("the tester.33 catalog removed")
        self.assertEqual(g["status"], "FAIL", g["lines"])
        self.assertIn(f"v1.2.3.3-tester.0: catalog {t33} is not in driver/state/history", " ".join(g["lines"]))
        _, gates = self.gates(self.ws.manifest([self.ws.row("a")]))
        self.assertEqual(gates["state-history"]["status"], "PASS")
        self.assertIn("no row builds the state worker", gates["state-history"]["lines"][0])

    def test_an_unpushed_commit_is_refused(self):
        commit = self.ws.commit("work that is not pushed")
        result, gates = self.gates(self.ws.manifest([self.ws.row("a", commit=commit)], recipe_commit=commit))
        self.assertEqual(gates["published"]["status"], "FAIL")
        self.assertEqual(result["status"], "FAIL")
        result, gates = self.gates(self.ws.manifest([self.ws.row("a", commit=commit)], recipe_commit=commit),
                                   allow_unpushed=True)
        self.assertEqual(gates["published"]["status"], "WARN")
        self.assertEqual(result["status"], "PASS")

    def test_an_ai_trailer_is_refused(self):
        commit = self.ws.commit("fix a thing\n\nCo-Authored-By: Claude <noreply@anthropic.com>\n")
        git(self.ws.repo, "push", "-q", "origin", "HEAD:refs/heads/main")
        git(self.ws.repo, "fetch", "-q", "origin")
        result, gates = self.gates(self.ws.manifest([self.ws.row("a", commit=commit)], recipe_commit=commit))
        self.assertEqual(gates["attribution"]["status"], "FAIL")
        words = {h["word"] for h in gates["attribution"]["hits"]}
        self.assertTrue({"Co-Authored-By", "Claude", "Anthropic"} <= words, words)

    def test_an_unpublished_owner_attribution_is_refused(self):
        commit = self.ws.commit(OWNER_QUOTE)
        _, gates = self.gates(self.ws.manifest([self.ws.row("a", commit=commit)], recipe_commit=commit),
                              allow_unpushed=True)
        self.assertEqual(gates["attribution"]["status"], "FAIL")
        self.assertIn("owner attribution", {h["word"] for h in gates["attribution"]["hits"]})

    def test_a_published_owner_attribution_is_reported_only(self):
        old = self.ws.commit(OWNER_QUOTE)
        git(self.ws.repo, "push", "-q", "origin", "HEAD:refs/heads/main")
        git(self.ws.repo, "fetch", "-q", "origin")
        commit = self.ws.commit("a later fix\n")
        git(self.ws.repo, "push", "-q", "origin", "HEAD:refs/heads/main")
        git(self.ws.repo, "fetch", "-q", "origin")
        _, gates = self.gates(self.ws.manifest([self.ws.row("a", commit=commit)], recipe_commit=commit))
        self.assertEqual(gates["attribution"]["status"], "WARN", gates["attribution"]["lines"])
        self.assertEqual(gates["attribution"]["hits"], [])
        self.assertEqual([h["commit"] for h in gates["attribution"]["published_owner_attributions"]], [old])

    def test_a_technical_owner_is_no_attribution(self):
        commit = self.ws.commit("kmd: the SMU owner keeps the owner lock across a reset\n")
        git(self.ws.repo, "push", "-q", "origin", "HEAD:refs/heads/main")
        git(self.ws.repo, "fetch", "-q", "origin")
        _, gates = self.gates(self.ws.manifest([self.ws.row("a", commit=commit)], recipe_commit=commit))
        self.assertEqual(gates["attribution"]["status"], "PASS", gates["attribution"]["lines"])

    def test_the_models_under_test_are_no_authors(self):
        commit = self.ws.commit("llm: Qwen3-1.7B and Llama 3 through llama.cpp on the GPU, gpt-oss refused\n")
        git(self.ws.repo, "push", "-q", "origin", "HEAD:refs/heads/main")
        git(self.ws.repo, "fetch", "-q", "origin")
        _, gates = self.gates(self.ws.manifest([self.ws.row("a", commit=commit)], recipe_commit=commit))
        # gpt-oss is a model name too: the gate names it, and the commit message has to say it some other way.
        self.assertEqual([h["word"] for h in gates["attribution"]["hits"]], ["GPT"])
        commit = self.ws.commit("llm: Qwen3-1.7B and Llama 3 through llama.cpp on the GPU\n")
        git(self.ws.repo, "push", "-q", "origin", "HEAD:refs/heads/main")
        git(self.ws.repo, "fetch", "-q", "origin")
        row = self.ws.row("a", commit=commit, base=git(self.ws.repo, "rev-parse", "HEAD~1"))
        manifest = self.ws.manifest([row], recipe_commit=commit)
        manifest["repositories"]["recipes"]["base"] = git(self.ws.repo, "rev-parse", "HEAD~1")
        _, gates = self.gates(manifest)
        self.assertEqual(gates["attribution"]["status"], "PASS", gates["attribution"])

    def publish(self):
        git(self.ws.repo, "push", "-q", "origin", "HEAD:refs/heads/main")
        git(self.ws.repo, "fetch", "-q", "origin")

    def commit_as(self, message, name="change.txt", author=UPSTREAM_ID, committer=UPSTREAM_ID):
        """A commit with the given 'Name <email>' author and committer (the fixture's git() is test@example.com)."""
        (self.ws.repo / name).write_text(f"{message}\n{author}\n{committer}\n", encoding="utf-8")
        git(self.ws.repo, "add", "-A")
        env = dict(os.environ)
        for role, ident in (("AUTHOR", author), ("COMMITTER", committer)):
            env[f"GIT_{role}_NAME"], env[f"GIT_{role}_EMAIL"] = ident.split(" <")[0], ident.split(" <")[1].rstrip(">")
        r = subprocess.run(["git", "-C", str(self.ws.repo), "-c", "core.autocrlf=false", "commit", "-q", "-m", message],
                           env=env, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return git(self.ws.repo, "rev-parse", "HEAD")

    def upstream_spec(self, up, url):
        return {"commit": up, "url": url, "fork_identities": [FORK_EMAIL, "alias@fork.example"]}

    def upstream_merge(self, message, author=UPSTREAM_ID, committer=UPSTREAM_ID, start=None):
        """A commit of an upstream project (published on its own remote, another URL, an upstream identity unless
        named) merged into main and pushed. Returns the upstream commit, the upstream URL and the merge."""
        remote = self.ws.remotes / "upstream.git"
        if not remote.exists():
            git(self.ws.remotes, "init", "-q", "--bare", str(remote))
            git(self.ws.repo, "remote", "add", "upstream", str(remote))
        git(self.ws.repo, "checkout", "-q", "-B", "up", start or self.ws.base_commit)
        up = self.commit_as(message, name="upstream.txt", author=author, committer=committer)
        git(self.ws.repo, "push", "-q", "upstream", "HEAD:refs/heads/main")
        git(self.ws.repo, "fetch", "-q", "upstream")
        git(self.ws.repo, "checkout", "-q", "main")
        git(self.ws.repo, "merge", "-q", "--no-ff", "-m", "merge the upstream project", "up")
        self.publish()
        return up, str(remote), git(self.ws.repo, "rev-parse", "HEAD")

    def test_an_upstream_trailer_is_left_out_and_counted(self):
        up, url, merge = self.upstream_merge("util: a faster path\n\nAssisted-by: Claude Code (Claude Opus 5.5)\n")
        manifest = self.ws.manifest([self.ws.row("a", commit=merge)], recipe_commit=merge)
        # Without the upstream commit, the trailer of the upstream author stops the release set.
        _, gates = self.gates(manifest)
        self.assertEqual(gates["attribution"]["status"], "FAIL")
        self.assertEqual({h["commit"] for h in gates["attribution"]["hits"]}, {up})
        manifest["repositories"]["recipes"]["upstream"] = self.upstream_spec(up, url)
        result, gates = self.gates(manifest)
        self.assertEqual(gates["attribution"]["status"], "PASS", gates["attribution"]["lines"])
        self.assertEqual(result["status"], "PASS")
        left = gates["attribution"]["upstream_left_out"]
        self.assertEqual([(x["upstream"], x["commits"], x["fork_identity_commits"]) for x in left], [(up, 1, [])])
        self.assertIn("1 commits of upstream", " ".join(gates["attribution"]["lines"]))

    def test_a_fork_trailer_fails_beside_an_upstream_merge(self):
        up, url, _ = self.upstream_merge("util: a faster path\n\nAssisted-by: Claude Code\n")
        fork = self.ws.commit("radv: a fix\n\nGenerated-by: Opus 5.5\n")
        self.publish()
        manifest = self.ws.manifest([self.ws.row("a", commit=fork)], recipe_commit=fork)
        manifest["repositories"]["recipes"]["upstream"] = self.upstream_spec(up, url)
        _, gates = self.gates(manifest)
        self.assertEqual(gates["attribution"]["status"], "FAIL", gates["attribution"]["lines"])
        self.assertEqual({(h["commit"], h["word"]) for h in gates["attribution"]["hits"]}, {(fork, "Opus")})
        # A fork commit named as the upstream hides nothing: the upstream remote does not hold it, and the fork's own
        # URL is no upstream URL.
        for spec in (self.upstream_spec(fork, url), self.upstream_spec(fork, str(self.ws.remote))):
            manifest["repositories"]["recipes"]["upstream"] = spec
            _, gates = self.gates(manifest)
            self.assertEqual(gates["attribution"]["status"], "FAIL", spec)
            self.assertEqual(gates["attribution"]["upstream_left_out"], [], spec)

    def test_a_model_name_that_is_also_a_word_needs_an_attribution_context(self):
        os_name = self.ws.commit("util: the condvar barrier that Windows, macOS and Haiku use\n\n"
                                 "The opus of this sonnet is no fable.\n")
        self.publish()
        _, gates = self.gates(self.ws.manifest([self.ws.row("a", commit=os_name)], recipe_commit=os_name))
        self.assertEqual(gates["attribution"]["status"], "PASS", gates["attribution"]["lines"])
        trailer = self.ws.commit("util: a fix\n\nAssisted-by: Haiku 4.5\n")
        self.publish()
        _, gates = self.gates(self.ws.manifest([self.ws.row("a", commit=trailer)], recipe_commit=trailer))
        self.assertEqual({(h["commit"], h["word"]) for h in gates["attribution"]["hits"]}, {(trailer, "Haiku")})

    def test_an_authorship_phrase_is_an_attribution_context(self):
        for text, word in (("Written by Opus", "Opus"), ("The tables were generated with Sonnet.", "Sonnet"),
                           ("authored by fable", "Fable"), ("Recorded by Haiku in the lab.", "Haiku"),
                           ("Assisted with Opus.", "Opus"), ("Created by Sonnet", "Sonnet")):
            commit = self.ws.commit(f"util: a fix\n\n{text}\n")
            self.publish()
            _, gates = self.gates(self.ws.manifest([self.ws.row("a", commit=commit)], recipe_commit=commit))
            self.assertEqual({(h["commit"], h["word"]) for h in gates["attribution"]["hits"]}, {(commit, word)}, text)
            self.ws.base_commit = commit  # the next case scans only its own commit
        # No authorship phrase: the operating system and the plain words stay no hit.
        commit = self.ws.commit("Haiku: fix the build\n\nThe file was written for Haiku by its port, "
                                "created on Windows.\n")
        self.publish()
        _, gates = self.gates(self.ws.manifest([self.ws.row("a", commit=commit)], recipe_commit=commit))
        self.assertEqual(gates["attribution"]["status"], "PASS", gates["attribution"]["lines"])

    def test_a_model_name_as_author_or_committer_is_always_a_hit(self):
        for word in ("Opus", "Sonnet", "Haiku", "Fable"):
            for field in ("author", "committer"):
                ident = f"{word} <model@example.invalid>"
                commit = self.commit_as(f"util: a fix of {field} {word}\n", author=ident if field == "author" else
                                        FORK_ID, committer=ident if field == "committer" else FORK_ID)
                self.publish()
                _, gates = self.gates(self.ws.manifest([self.ws.row("a", commit=commit)], recipe_commit=commit))
                self.assertEqual(gates["attribution"]["status"], "FAIL", (word, field))
                self.assertEqual({(h["commit"], h["field"], h["word"]) for h in gates["attribution"]["hits"]},
                                 {(commit, field, word)}, (word, field))
                self.ws.base_commit = commit  # the next case scans only its own commit

    def test_a_fork_identity_in_the_left_out_history_fails(self):
        # Author only, then committer only: an upstream-named commit with a fork identity is not left out quietly.
        for author, committer in ((FORK_ID, UPSTREAM_ID), (UPSTREAM_ID, "Alias <ALIAS@fork.example>")):
            up, url, merge = self.upstream_merge("util: a faster path\n", author=author, committer=committer)
            manifest = self.ws.manifest([self.ws.row("a", commit=merge)], recipe_commit=merge)
            manifest["repositories"]["recipes"]["upstream"] = self.upstream_spec(up, url)
            _, gates = self.gates(manifest)
            att = gates["attribution"]
            self.assertEqual(att["status"], "FAIL", att["lines"])
            field = "author" if author == FORK_ID else "committer"
            self.assertEqual([(m["commit"], m["field"]) for m in att["upstream_left_out"][0]["fork_identity_commits"]],
                             [(up, field)])
            self.assertIn("a fork identity", " ".join(att["lines"]))
            self.ws.base_commit = merge  # the next case starts after this one

    def test_a_fork_identity_before_the_release_range_is_not_read(self):
        # The upstream history holds a fork-identity commit that the previous release (the base) already reaches:
        # only (ancestors(commit) - ancestors(base)) & ancestors(upstream) is checked, so it is no finding.
        old, url, base = self.upstream_merge("util: an old path\n", author=FORK_ID)
        up, _, merge = self.upstream_merge("util: a faster path\n", start=old)
        manifest = self.ws.manifest([self.ws.row("a", commit=merge)], recipe_commit=merge)
        manifest["repositories"]["recipes"]["base"] = base
        manifest["repositories"]["recipes"]["upstream"] = self.upstream_spec(up, url)
        _, gates = self.gates(manifest)
        att = gates["attribution"]
        self.assertEqual(att["status"], "PASS", att["lines"])
        self.assertEqual([(x["commits"], x["fork_identity_commits"]) for x in att["upstream_left_out"]], [(1, [])])

    def test_an_upstream_entry_names_the_fork_identities(self):
        manifest = self.ws.manifest([self.ws.row("a")])
        manifest["repositories"]["recipes"]["upstream"] = {"commit": self.ws.head, "url": "https://example.org/u.git"}
        self.assertTrue(any("fork_identities" in e for e in build_release_set.check_manifest(manifest)))

    def test_collect_takes_the_pdb_of_the_image(self):
        # The KMD build leaves its image in package\ and bc250kmd.pdb elsewhere; a PDB of the same name from another
        # build must not pass for it.
        out, art = Path(self.tmp) / "o", Path(self.tmp) / "art"
        (out / "package").mkdir(parents=True)
        (out / "decoy").mkdir()
        mine, other = (0x01020304, 0x0506, 0x0708, bytes(8)), (0x0A0B0C0D, 0x0E0F, 0x1011, bytes(8))
        (out / "package" / "drv.sys").write_bytes(pe_bytes("C:\\build\\drv.pdb", guid=mine, age=3))
        (out / "drv.unsigned.sys").write_bytes(pe_bytes("C:\\build\\drv.pdb", guid=mine, age=3))
        (out / "decoy" / "drv.pdb").write_bytes(msf_pdb_bytes(guid=other, age=3))
        (out / "drv.pdb").write_bytes(msf_pdb_bytes(guid=mine, age=3))
        (out / "recipe.json").write_text("{}", encoding="utf-8")
        job = {"out": str(out), "artifact": str(art), "flavour": "release", "keep": [],
               "outputs": [{"file": "package/drv.sys", "path": "payload/kmd/drv.sys", "unsigned": "drv.unsigned.sys"}]}
        result, errors = build_release_set.collect(job)
        self.assertEqual(errors, [])
        entry = result["outputs"][0]
        self.assertEqual((entry["pdb"]["file"], entry["pdb"]["age"]), ("drv.pdb", 3))
        self.assertEqual((art / "drv.pdb").read_bytes(), (out / "drv.pdb").read_bytes())
        self.assertEqual(entry["authenticode_sha256"], entry["image_sha256"])
        self.assertEqual(entry["unsigned"]["sha256"], build_release_set.sha256_file(out / "drv.unsigned.sys"))
        self.assertEqual(result["records"], ["recipe.json"])
        (out / "drv.pdb").unlink()
        _, errors = build_release_set.collect(job)
        self.assertTrue(any("names the symbol file drv.pdb" in e for e in errors), errors)

    def test_an_implicit_or_refused_compiler_is_refused(self):
        implicit = self.ws.row("a")
        i = implicit["args"].index("-Compiler")
        del implicit["args"][i:i + 2]
        msvc = self.ws.row("b", compiler="msvc")
        msvc["args"][msvc["args"].index("clang-cl")] = "msvc"
        plain = self.ws.row("c", recipe="plain.ps1", args=["-Out", "{out}"],
                            outputs=[{"file": "plain.bin", "path": "payload/plain.bin"}])
        znver2 = self.ws.row("d", extra=["-Flags", "-march=znver2"])
        _, gates = self.gates(self.ws.manifest([implicit, msvc, plain, znver2]))
        text = "\n".join(gates["compiler"]["lines"])
        self.assertEqual(gates["compiler"]["status"], "FAIL")
        self.assertIn("a: stub.ps1 takes -Compiler and the row does not pass it", text)
        self.assertIn("b: a user-mode build with msvc needs 'compiler_reason'", text)
        self.assertIn("c: clang-cl must be named on the recipe's command line", text)
        self.assertIn("d: '-march=znver2'", text)
        reason = "the x86 meson build has no clang-cl route yet, see the lift work"
        ok = self.ws.row("b", compiler="msvc", compiler_reason=reason)
        ok["args"][ok["args"].index("clang-cl")] = "msvc"
        _, gates = self.gates(self.ws.manifest([ok]))
        self.assertEqual(gates["compiler"]["status"], "PASS", gates["compiler"])


@unittest.skipUnless(shutil.which("pwsh"), "pwsh 7 is not on PATH")
class ReleaseSetRegister(unittest.TestCase):
    """build_release_set.py register in process: the rows go in, and a D3D11 record follows its new engine."""

    CAPS = {"maximum_feature_level": 0xC100, "doubles": 1, "compute_raw_structured": 1, "logic_op": 1,
            "tile_based": 0, "pixel_min_precision": 2, "other_min_precision": 2, "tiled_resources_tier": 3,
            "typed_uav_loads": 1, "rovs": 1, "stencil_ref": 1, "conservative_tier": 3, "vp_rt_index_any_shader": 1,
            "carried_from": "measured"}

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="release-set-reg-", dir=OUT))
        self.encode = build_release_set.load_config_encoder(HERE.parents[1])

    def tearDown(self):
        remove_tree(self.tmp)

    def put(self, rel, data):
        path = self.tmp / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return {"source": rel, "sha256": build_release_set.sha256_file(path), "size": len(data)}

    def test_a_new_engine_rebinds_its_record_and_keeps_the_capability_words(self):
        engine = self.put("old/engine.dll", b"engine 1")
        icd = self.put("old/icd.dll", b"icd 1")
        caps = dict(self.CAPS, engine_sha256=engine["sha256"], icd_sha256=icd["sha256"])
        old = self.encode(caps, self.tmp / engine["source"], self.tmp / icd["source"])
        config = self.put("old/amdgpu_wddm_d3d11.config", old)
        rows = [dict(engine, component="d3d11-umd", path="payload/d3d11/amdgpu_wddm_dxvk.dll", flavour="release"),
                dict(icd, component="d3d11-umd", path="payload/d3d11/amdgpu_wddm_radv.dll", flavour="release"),
                dict(config, component="d3d11-umd", path="payload/d3d11/amdgpu_wddm_d3d11.config", flavour="release",
                     built_from={"inputs": {"caps": {"inline": caps}}})]
        sources = self.tmp / "release-sources.json"
        sources.write_text(json.dumps({"schema": 4, "files": rows}), encoding="utf-8")
        new_engine = self.put("new/engine.dll", b"engine 2")
        doc = {"version": "1.2.3.4-test.1", "files": [dict(new_engine, component="d3d11-umd", flavour="release",
                                                           path="payload/d3d11/amdgpu_wddm_dxvk.dll")]}
        data, receipts, errors = build_release_set.register(doc, sources, self.tmp, HERE.parents[1], doc["version"],
                                                            self.tmp / "configs")
        self.assertEqual(errors, [])
        self.assertEqual(len(receipts), 1)
        row = [f for f in data["files"] if f["path"].endswith(".config")][0]
        raw = (self.tmp / row["source"]).read_bytes()
        self.assertEqual(raw[:68], old[:68])
        self.assertEqual(raw[68:100].hex().upper(), new_engine["sha256"])
        self.assertEqual(raw[100:132].hex().upper(), icd["sha256"])
        self.assertIn("1.2.3.4-test.1", row["built_from"]["inputs"]["caps"]["inline"]["carried_from"])

    def test_an_unchanged_pair_keeps_its_record(self):
        engine = self.put("old/engine.dll", b"engine 1")
        icd = self.put("old/icd.dll", b"icd 1")
        caps = dict(self.CAPS, engine_sha256=engine["sha256"], icd_sha256=icd["sha256"])
        config = self.put("old/amdgpu_wddm_d3d11.config",
                          self.encode(caps, self.tmp / engine["source"], self.tmp / icd["source"]))
        rows = [dict(engine, component="d3d11-umd", path="payload/d3d11/amdgpu_wddm_dxvk.dll", flavour="release"),
                dict(icd, component="d3d11-umd", path="payload/d3d11/amdgpu_wddm_radv.dll", flavour="release"),
                dict(config, component="d3d11-umd", path="payload/d3d11/amdgpu_wddm_d3d11.config", flavour="release",
                     built_from={"inputs": {"caps": {"inline": caps}}})]
        sources = self.tmp / "release-sources.json"
        sources.write_text(json.dumps({"schema": 4, "files": rows}), encoding="utf-8")
        doc = {"version": "1.2.3.4-test.1", "files": [rows[0]]}
        data, receipts, errors = build_release_set.register(doc, sources, self.tmp, HERE.parents[1], doc["version"],
                                                            self.tmp / "configs")
        self.assertEqual((errors, receipts), ([], []))
        self.assertEqual([f for f in data["files"] if f["path"].endswith(".config")][0]["source"], config["source"])


class ReleaseSetSources(unittest.TestCase):
    """What a registered row changes beside itself: the 'sources' texts, and which edits keep an artifact."""

    OLD, NEW = "a" * 40, "b" * 40

    def test_register_moves_the_source_texts_to_the_new_build(self):
        data = {"kmd_version": "1.2.3.4", "sources": {
            "kmd": f"bc250-win {self.OLD} (old line): the old KMD",
            "d3d12_shell": f"Current old.1 payload sources. release payload/d3d12/x.dll: amdgpu-wddm {self.OLD}; "
                           f"lab payload/d3d12/x.dll: amdgpu-wddm {self.OLD}. Rows keep the rest.",
            "firmware": "linux-firmware 0123, downloaded by the installer"},
            "files": [{"path": "payload/kmd/bc250kmd.sys", "flavour": "release",
                       "built_from": {"repo": "amdgpu-wddm", "commit": self.NEW}},
                      {"path": "payload/d3d12/x.dll", "flavour": "release",
                       "built_from": {"repo": "amdgpu-wddm", "commit": self.NEW}},
                      {"path": "payload/d3d12/x.dll", "flavour": "lab",
                       "built_from": {"repo": "amdgpu-wddm", "commit": self.OLD}}]}
        changed = {("payload/kmd/bc250kmd.sys", "release"), ("payload/d3d12/x.dll", "release")}
        keys = build_release_set.refresh_sources(data, "1.2.3.4-new.2", changed)
        texts = data["sources"]
        self.assertEqual(sorted(keys), ["d3d12_shell", "kmd"])
        self.assertTrue(texts["kmd"].startswith(f"bc250-win {self.NEW} (KMD 1.2.3.4): the new.2 KMD"))
        self.assertEqual(texts["d3d12_shell"], f"Current new.2 payload sources. release payload/d3d12/x.dll: "
                                               f"amdgpu-wddm {self.NEW}; lab payload/d3d12/x.dll: amdgpu-wddm "
                                               f"{self.OLD}. Rows keep the rest.")
        self.assertEqual(texts["firmware"], "linux-firmware 0123, downloaded by the installer")

    def test_a_new_pair_skew_keeps_the_artifact_and_a_new_file_does_not(self):
        row = {"repo": "r", "commit": self.OLD, "outputs": [{"file": "a.dll", "path": "payload/a.dll"}]}
        noted = copy.deepcopy(row)
        noted["outputs"][0]["pair_skew"] = "a sentence that says why the two builds differ"
        moved = copy.deepcopy(row)
        moved["outputs"][0]["file"] = "b.dll"
        self.assertEqual(build_release_set.build_key(row), build_release_set.build_key(noted))
        self.assertNotEqual(build_release_set.build_key(row), build_release_set.build_key(moved))


class ReleaseSetRuns(unittest.TestCase):
    """build-release-set.ps1 end to end, on stub recipes."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="release-set-", dir=OUT)
        self.ws = Workspace(self.tmp)

    def tearDown(self):
        remove_tree(self.tmp)

    def assertTreesGone(self):
        self.assertFalse(self.ws.work.exists() and any(self.ws.work.iterdir()), list(self.ws.work.glob("*")))
        self.assertEqual(len(git(self.ws.repo, "worktree", "list").splitlines()), 1)

    def test_the_scheduler_keeps_to_max_parallel(self):
        test = {"name": "unit", "run": "hosttest.ps1", "args": ["-Build", "{out}"],
                "summary": r"checks=(?<checks>\d+), failures=(?<failures>\d+), skipped=(?<skipped>\d+)",
                "pass": "^PASSED: 0 failure"}
        rows = [self.ws.row("a", tests=[test]), self.ws.row("b"), self.ws.row("c"),
                self.ws.row("d", extra=["-Payload", "{payload:payload/a.bin}"])]
        code, out = self.ws.run(self.ws.manifest(rows), "-MaxParallel", "2")
        self.assertEqual(code, 0, out)
        spans = self.ws.intervals()
        self.assertEqual(sorted(spans), ["a", "b", "c", "d"], out)
        self.assertEqual(most_at_once(spans), 2, (spans, out))
        self.assertGreaterEqual(spans["d"][0], spans["a"][1], "d reads the output of a, so it starts after a ends")
        report = self.ws.report()
        self.assertEqual(report["status"], "ok")
        a = next(b for b in report["builds"] if b["id"] == "a")
        self.assertEqual(a["tests"][0]["status"], "pass")
        self.assertEqual(a["tests"][0]["summary"], {"checks": 7, "failures": 0, "skipped": 1})
        rows_doc = json.loads((self.ws.out / "release-sources.rows.json").read_text(encoding="utf-8"))
        files = {f["path"]: f for f in rows_doc["files"]}
        self.assertEqual(sorted(files), ["payload/a.bin", "payload/b.bin", "payload/c.bin", "payload/d.bin"])
        art = self.ws.root / files["payload/d.bin"]["source"]
        self.assertEqual(build_release_set.sha256_file(art), files["payload/d.bin"]["sha256"])
        a_hash = files["payload/a.bin"]["sha256"]
        self.assertIn(a_hash, art.read_text())
        self.assertEqual(files["payload/a.bin"]["built_from"]["args"][-2:], ["-Version", "1.2.3.4-test.1"])
        self.assertIn("{payload:payload/a.bin}", files["payload/d.bin"]["built_from"]["args"])
        self.assertTreesGone()

    def test_the_scheduler_keeps_to_the_disk_budget(self):
        # Room for two builds of 1 GB under the warning line, with four slots.
        self.ws.set_disk(counted=int(490 * GB), warn=int(492.5 * GB), free=1000 * GB, free_warn=0)
        rows = [self.ws.row(x, sleep=3) for x in "abcd"]
        code, out = self.ws.run(self.ws.manifest(rows), "-MaxParallel", "4")
        self.assertEqual(code, 0, out)
        spans = self.ws.intervals()
        self.assertEqual(sorted(spans), ["a", "b", "c", "d"], out)
        self.assertEqual(most_at_once(spans), 2, (spans, out))
        # A build that can never fit fails the release set with the disk's own message, and nothing else starts.
        rows = [self.ws.row("big", peak=5), self.ws.row("e")]
        shutil.rmtree(self.ws.events)
        code, out = self.ws.run(self.ws.manifest(rows), "-MaxParallel", "4")
        self.assertEqual(code, 1, out)
        self.assertIn("FAIL disk: big needs 5.0 GB", out)
        self.assertFalse(self.ws.events.exists(), out)
        self.assertTreesGone()

    def test_plan_builds_nothing(self):
        rows = [self.ws.row("a"), self.ws.row("b", extra=["-Payload", "{payload:payload/a.bin}"])]
        code, out = self.ws.run(self.ws.manifest(rows), "-Plan")
        self.assertEqual(code, 0, out)
        self.assertIn("-Name a -Sleep 2 -Compiler clang-cl", out)
        self.assertIn(str(self.ws.out / "artifacts" / "release" / "a" / "a.bin"), out)
        self.assertIn("-> payload/b.bin (release)", out)
        self.assertIn("nothing built", out)
        self.assertFalse(self.ws.events.exists())
        self.assertFalse((self.ws.out / "artifacts").exists())
        self.assertFalse((self.ws.out / "logs").exists())
        self.assertFalse((self.ws.out / "release-report.json").exists())
        self.assertFalse(self.ws.work.exists())
        self.assertTreesGone()

    def test_the_gates_refuse_before_any_build(self):
        unpushed = self.ws.commit("work that is not pushed")
        code, out = self.ws.run(self.ws.manifest([self.ws.row("a", commit=unpushed)], recipe_commit=unpushed))
        self.assertEqual(code, 1, out)
        self.assertIn("gate published: FAIL", out)
        self.assertIn("nothing is built", out)
        self.assertFalse(self.ws.events.exists())
        self.assertEqual(self.ws.report()["status"], "gates-failed")
        code, out = self.ws.run(self.ws.manifest([self.ws.row("a", commit=unpushed)], recipe_commit=unpushed),
                                "-AllowUnpushed", "-Plan")
        self.assertEqual(code, 0, out)
        self.assertIn("gate published: WARN", out)
        trailer = self.ws.commit("a fix\n\nCo-Authored-By: GPT-5 <noreply@openai.com>\n")
        git(self.ws.repo, "push", "-q", "origin", "HEAD:refs/heads/main")
        git(self.ws.repo, "fetch", "-q", "origin")
        implicit = self.ws.row("b", commit=trailer)
        i = implicit["args"].index("-Compiler")
        del implicit["args"][i:i + 2]
        code, out = self.ws.run(self.ws.manifest([implicit], recipe_commit=trailer), "-Plan")
        self.assertEqual(code, 1, out)
        self.assertIn("gate attribution: FAIL", out)
        self.assertIn("'Co-Authored-By'", out)
        self.assertIn("gate compiler: FAIL", out)
        self.assertIn("b: stub.ps1 takes -Compiler and the row does not pass it", out)
        self.assertFalse(self.ws.events.exists())
        self.assertTreesGone()

    def test_a_failed_build_fails_the_set_and_keeps_its_log(self):
        rows = [self.ws.row("a", sleep=0), self.ws.row("bad", sleep=0, extra=["-Fail"]), self.ws.row("c", sleep=0)]
        code, out = self.ws.run(self.ws.manifest(rows), "-MaxParallel", "1")
        self.assertEqual(code, 1, out)
        report = self.ws.report()
        status = {b["id"]: b["status"] for b in report["builds"]}
        self.assertEqual(status, {"a": "ok", "bad": "failed", "c": "not-started"}, out)
        self.assertEqual(report["status"], "failed")
        log = Path(next(b for b in report['builds'] if b['id'] == 'bad')['log_dir']) / 'build.log'
        self.assertIn("recipe bad fails on purpose", log.read_text(encoding="utf-8-sig", errors="replace"))
        self.assertIn("see " + str(log), next(b for b in report["builds"] if b["id"] == "bad")["error"])
        self.assertTrue((self.ws.out / "artifacts" / "release" / "a" / "a.bin").is_file())
        self.assertFalse((self.ws.out / "artifacts" / "release" / "bad").exists())
        rows_doc = json.loads((self.ws.out / "release-sources.rows.json").read_text(encoding="utf-8"))
        self.assertEqual([f["path"] for f in rows_doc["files"]], ["payload/a.bin"])
        self.assertTreesGone()
        # -Only rebuilds the failed row and keeps the artifact of the good one, whose row takes the note that the
        # manifest gave it since.
        rows[1] = self.ws.row("bad", sleep=0)
        rows[0]["outputs"][0]["pair_skew"] = "a sentence that the kept row takes from the manifest of this run"
        code, out = self.ws.run(self.ws.manifest(rows), "-Only", "bad,c")
        self.assertEqual(code, 0, out)
        report = self.ws.report()
        self.assertEqual({b["id"]: (b["status"], bool(b.get("kept"))) for b in report["builds"]},
                         {"a": ("ok", True), "bad": ("ok", False), "c": ("ok", False)})
        rows_doc = json.loads((self.ws.out / "release-sources.rows.json").read_text(encoding="utf-8"))
        a = next(f for f in rows_doc["files"] if f["path"] == "payload/a.bin")
        self.assertEqual(a.get("pair_skew"), rows[0]["outputs"][0]["pair_skew"])

    def test_a_deleted_work_folder_leaves_no_registration_that_stops_the_next_run(self):
        # BD-264: -Keep leaves <Work>\_<hash> registered; after the Work folder went as a folder, the next run
        # stopped at its start. A killed job leaves the registration of its row tree the same way.
        rows = [self.ws.row("a", sleep=0), self.ws.row("b", sleep=0, tree="worktree")]
        manifest = self.ws.manifest(rows)
        code, out = self.ws.run(manifest, "-Keep")
        self.assertEqual(code, 0, out)
        self.assertGreater(len(git(self.ws.repo, "worktree", "list").splitlines()), 1, "-Keep keeps the shared tree")
        shutil.rmtree(self.ws.work)
        git(self.ws.repo, "worktree", "add", "--detach", str(self.ws.work / "b" / "s"), "HEAD")
        other = self.ws.base / "other-lane" / "b" / "s"
        git(self.ws.repo, "worktree", "add", "--detach", str(other), "HEAD")
        shutil.rmtree(self.ws.work)
        shutil.rmtree(other)
        # Exercise worktree creation explicitly. The default correctly needs no tree for already built rows.
        code, out = self.ws.run(manifest, '-Rebuild')
        self.assertEqual(code, 0, out)
        self.assertIn("removed the registration of " + str(self.ws.work / "b" / "s"), out)
        listed = git(self.ws.repo, "worktree", "list", "--porcelain").replace("\\", "/")
        self.assertIn(str(other).replace("\\", "/"), listed, "another lane's registration stays")
        git(self.ws.repo, "worktree", "prune")
        self.assertTreesGone()

    def test_a_tree_input_comes_from_a_later_checkout_with_the_module_stores(self):
        # BD-285 end to end: the first checkout of the commit has no module store. The tree comes from the second,
        # with sub/inner checked out (the stub hashes a file of it into its output); with no checkout that has every
        # store, -Plan already fails the commits gate and nothing is built.
        commit = self.ws.module_checkouts()

        def manifest(checkouts):
            row = self.ws.row("a", sleep=0, extra=["-Payload", "{input:mod}/sub/inner/innersrc.txt"],
                              inputs={"mod": {"repo": "mod", "commit": commit}})
            m = self.ws.manifest([row])
            m["repositories"]["mod"] = {"url": "https://example.invalid/mod", "checkouts": checkouts}
            return m
        code, out = self.ws.run(manifest(["mod-bare", "mod-full"]), "-AllowUnpushed")
        self.assertEqual(code, 0, out)
        inner = build_release_set.sha256_file(self.ws.root / "innersrc" / "innersrc.txt")
        self.assertIn(inner, (self.ws.out / "artifacts" / "release" / "a" / "a.bin").read_text())
        self.assertEqual(len(git(self.ws.root / "mod-bare", "worktree", "list").splitlines()), 1)
        self.assertEqual(len(git(self.ws.root / "mod-full", "worktree", "list").splitlines()), 1)
        shutil.rmtree(self.ws.events)
        code, out = self.ws.run(manifest(["mod-bare", "mod-half"]), "-AllowUnpushed", "-Plan")
        self.assertEqual(code, 1, out)
        self.assertIn("gate commits: FAIL", out)
        self.assertIn("no checkout can make its tree with every submodule", out)
        self.assertFalse(self.ws.events.exists())

    def builds(self):
        return {b["id"]:(b["status"], bool(b.get("kept")), bool(b.get("carried"))) for b in self.ws.report()["builds"]}

    def test_a_gate_failed_run_keeps_the_earlier_builds(self):
        # BD-244: a run that stopped at the size gate wrote a report with no builds, and the next -Only run in the
        # same -Out had to build every row again.
        rows = [self.ws.row("a", sleep=0), self.ws.row("b", sleep=0, extra=["-Payload", "{payload:payload/a.bin}"]),
                self.ws.row("c", sleep=0)]
        manifest = self.ws.manifest(rows)
        code, out = self.ws.run(manifest)
        self.assertEqual(code, 0, out)
        self.ws.set_disk(counted=int(500 * GB), warn=int(490 * GB), free=1000 * GB, free_warn=0)
        code, out = self.ws.run(manifest, "-Only", "b", "-GuardMaxAge", "0")
        self.assertEqual(code, 1, out)
        self.assertIn("gate size: FAIL", out)
        self.assertEqual(self.ws.report()["status"], "gates-failed")
        self.assertEqual(self.builds(), {"a": ("ok", True, False), "c": ("ok", True, False),
                                         "b": ("ok", False, True)}, out)
        # The next plan of the same manifest keeps the same rows, and a run with another -Only still finds b.
        self.ws.set_disk(counted=0, warn=1000 * GB, free=1000 * GB, free_warn=0)
        plan, errors = self.ws.plan(manifest, only=["b"])
        self.assertEqual(errors, [])
        self.assertEqual(sorted(k["id"] for k in plan["kept"]), ["a", "c"])
        code, out = self.ws.run(manifest, "-Only", "c", "-GuardMaxAge", "0")
        self.assertEqual(code, 0, out)
        self.assertEqual(self.builds(), {"a": ("ok", True, False), "b": ("ok", True, False),
                                         "c": ("ok", False, False)}, out)
        rows_doc = json.loads((self.ws.out / "release-sources.rows.json").read_text(encoding="utf-8"))
        self.assertEqual(sorted(f["path"] for f in rows_doc["files"]), ["payload/a.bin", "payload/b.bin", "payload/c.bin"])
        self.assertTreesGone()

    def test_keep_from_imports_the_builds_of_an_earlier_round(self):
        # BD-244, second shape: a row built in an earlier round directory was unknown to the next round.
        rows = [self.ws.row("a", sleep=0), self.ws.row("b", sleep=0, extra=["-Payload", "{payload:payload/a.bin}"])]
        manifest = self.ws.manifest(rows)
        code, out = self.ws.run(manifest)
        self.assertEqual(code, 0, out)
        round1 = self.ws.root / "round1"
        self.ws.out.rename(round1)
        code, out = self.ws.run(manifest, "-Only", "b")
        self.assertEqual(code, 1, out)
        self.assertIn("needs the outputs of row a", out)
        code, out = self.ws.run(manifest, "-Only", "b", "-KeepFrom", str(round1))
        self.assertEqual(code, 0, out)
        self.assertIn("imported from", out)
        report = self.ws.report()
        a = next(b for b in report["builds"] if b["id"] == "a")
        self.assertTrue(a["kept"])
        self.assertEqual(a["imported_from"]["out"], str(round1))
        copied = Path(a["outputs"][0]["artifact"])
        self.assertTrue(copied.is_relative_to(self.ws.out / "artifacts"), copied)
        self.assertEqual(build_release_set.sha256_file(copied), a["outputs"][0]["sha256"])
        rows_doc = json.loads((self.ws.out / "release-sources.rows.json").read_text(encoding="utf-8"))
        files = {f["path"]: f for f in rows_doc["files"]}
        self.assertEqual(files["payload/a.bin"]["source"], copied.relative_to(self.ws.root).as_posix())
        self.assertIn(files["payload/a.bin"]["sha256"], (self.ws.root / files["payload/b.bin"]["source"]).read_text())
        # A changed file at the source is refused before any build.
        shutil.rmtree(self.ws.out)
        (round1 / "artifacts" / "release" / "a" / "a.bin").write_text("not the build", encoding="utf-8")
        shutil.rmtree(self.ws.events)
        code, out = self.ws.run(manifest, "-Only", "b", "-KeepFrom", str(round1))
        self.assertEqual(code, 1, out)
        self.assertIn("is gone or changed", out)
        self.assertFalse(self.ws.events.exists(), out)
        code, out = self.ws.run(manifest, "-KeepFrom", str(round1))
        self.assertEqual(code, 0, out)
        self.assertIn('an earlier artifact is missing or changed', out)
        self.assertEqual({b['id']: bool(b.get('kept')) for b in self.ws.report()['builds']}, {'a': False, 'b': False})
        rebuilt = next(b for b in self.ws.report()['builds'] if b['id'] == 'a')
        self.assertEqual(rebuilt['outputs'][0]['sha256'], a['outputs'][0]['sha256'])
        self.assertEqual((round1 / 'artifacts/release/a/a.bin').read_text(), 'not the build')
        self.assertTreesGone()

    def test_keep_from_without_only_builds_only_the_changed_rows(self):
        # tester.38 built all 60 rows for a change of 6. -KeepFrom alone imports every row whose earlier build
        # stands for it and builds the changed row and the rows that take its payload.
        rows = [self.ws.row("a", sleep=0), self.ws.row("b", sleep=0, extra=["-Payload", "{payload:payload/a.bin}"]),
                self.ws.row("c", sleep=0)]
        code, out = self.ws.run(self.ws.manifest(rows))
        self.assertEqual(code, 0, out)
        round1 = self.ws.root / "round1"
        self.ws.out.rename(round1)
        moved = self.ws.commit("a moves", name="a-moves.txt")
        git(self.ws.repo, "push", "-q", "origin", "HEAD:refs/heads/main")
        rows[0] = self.ws.row("a", sleep=0, commit=moved)
        code, out = self.ws.run(self.ws.manifest(rows), "-KeepFrom", str(round1))
        self.assertEqual(code, 0, out)
        self.assertIn("build a: the manifest row changed", out)
        self.assertIn("build b: it takes a payload of a, which this run builds", out)
        report = self.ws.report()
        kept = {b["id"]: bool(b.get("kept")) for b in report["builds"]}
        self.assertEqual(kept, {"a": False, "b": False, "c": True}, out)
        c = next(b for b in report["builds"] if b["id"] == "c")
        self.assertEqual(c["imported_from"]["out"], str(round1))
        # Nothing changed: nothing is built.
        round2 = self.ws.root / "round2"
        self.ws.out.rename(round2)
        shutil.rmtree(self.ws.events, ignore_errors=True)
        code, out = self.ws.run(self.ws.manifest(rows), "-KeepFrom", str(round2))
        self.assertEqual(code, 0, out)
        self.assertNotIn("  build ", out)
        self.assertFalse(self.ws.events.exists(), out)
        self.assertTreesGone()

    def test_keep_from_stamps_an_earlier_release_build_with_the_new_version(self):
        # Every recipe stamps the release version into its files as its last step, so a new release version made
        # every row stand for nothing and the whole set was built again. The import now stamps the copy with the
        # new version: the bytes must be those that a build for the new version gives.
        import sys
        sys.path.insert(0, str(HERE.parents[1] / "tools" / "build"))
        import test_versionstamp
        image = self.ws.root / "fixture" / "image.dll"
        image.parent.mkdir(parents=True)
        image.write_bytes(test_versionstamp.make_pe())
        (self.ws.repo / "pestub.ps1").write_bytes(PESTUB)
        head = self.ws.commit("a recipe that stamps a PE image", name="pestub-note.txt")
        git(self.ws.repo, "push", "-q", "origin", "HEAD:refs/heads/main")
        repo = HERE.parents[1]
        rows = [self.ws.row("p", commit=head, recipe="pestub.ps1",
                            args=["-Out", "{out}", "-Name", "p", "-Compiler", "clang-cl", "-Version", "{version}",
                                  "-Image", str(image), "-Stamp", str(repo / "tools/build/versionstamp.py"),
                                  "-Inf", str(repo / "driver/kmd/bc250kmd.inf"), "-Python", sys.executable],
                            outputs=[{"file": "p.dll", "path": "payload/d3d12/p.dll"}])]

        def manifest(version):
            m = self.ws.manifest(rows, recipe_commit=head)
            m["version"] = version
            return m

        code, out = self.ws.run(manifest("1.2.3.4-test.1"))
        self.assertEqual(code, 0, out)
        round1 = self.ws.root / "round1"
        self.ws.out.rename(round1)
        code, out = self.ws.run(manifest("1.2.3.5-test.2"), "-KeepFrom", str(round1))
        self.assertEqual(code, 0, out)
        self.assertNotIn("  build p", out)
        self.assertIn("restamped p: 1.2.3.4-test.1 -> 1.2.3.5-test.2", out)
        restamped = next(b for b in self.ws.report()["builds"] if b["id"] == "p")
        self.assertTrue(restamped["kept"])
        art = Path(restamped["outputs"][0]["artifact"])
        self.assertEqual(build_release_set.sha256_file(art), restamped["outputs"][0]["sha256"])
        self.assertNotEqual(restamped["outputs"][0]["restamped_from_sha256"], restamped["outputs"][0]["sha256"])
        rows_doc = json.loads((self.ws.out / "release-sources.rows.json").read_text(encoding="utf-8"))
        self.assertEqual(rows_doc["files"][0]["sha256"], restamped["outputs"][0]["sha256"])
        # A build for the new version gives the same bytes.
        stamped = self.ws.root / "restamped"
        self.ws.out.rename(stamped)
        code, out = self.ws.run(manifest("1.2.3.5-test.2"))
        self.assertEqual(code, 0, out)
        fresh = next(b for b in self.ws.report()["builds"] if b["id"] == "p")
        self.assertFalse(fresh.get("kept"))
        self.assertEqual(fresh["outputs"][0]["sha256"], restamped["outputs"][0]["sha256"])
        self.assertEqual(fresh["outputs"][0]["image_sha256"], restamped["outputs"][0]["image_sha256"])

    def test_a_moved_recipe_commit_keeps_the_build_when_the_recipe_is_the_same(self):
        # BD-244: the recipe commit of the manifest moved; a hand edit set the old one per row after an empty
        # git diff. The tool now makes that check itself and keeps the commit that built the row in its rows.
        (self.ws.repo / "sub").mkdir()
        (self.ws.repo / "sub" / "stub.ps1").write_bytes(STUB)
        c1 = self.ws.commit("a recipe in a directory of its own", name="sub/README.txt")
        git(self.ws.repo, "push", "-q", "origin", "HEAD:refs/heads/main")
        git(self.ws.repo, "fetch", "-q", "origin")

        def rows(recipe_commit):
            return [self.ws.row("a", sleep=0, recipe="sub/stub.ps1", recipe_commit=recipe_commit),
                    self.ws.row("b", sleep=0, extra=["-Payload", "{payload:payload/a.bin}"])]
        code, out = self.ws.run(self.ws.manifest(rows(c1), recipe_commit=c1))
        self.assertEqual(code, 0, out)
        c2 = self.ws.commit("a change outside the recipe directory")
        git(self.ws.repo, "push", "-q", "origin", "HEAD:refs/heads/main")
        git(self.ws.repo, "fetch", "-q", "origin")
        code, out = self.ws.run(self.ws.manifest(rows(c2), recipe_commit=c2), "-Only", "b")
        self.assertEqual(code, 0, out)
        self.assertIn(f"recipe commit {c2[:8]} in the manifest, {c1[:8]} built it", out)
        rows_doc = json.loads((self.ws.out / "release-sources.rows.json").read_text(encoding="utf-8"))
        a = next(f for f in rows_doc["files"] if f["path"] == "payload/a.bin")
        self.assertEqual(a["built_from"]["recipe_commit"], c1)
        c3 = self.ws.commit("# a change of the recipe\n", name="sub/stub.ps1")
        git(self.ws.repo, "push", "-q", "origin", "HEAD:refs/heads/main")
        git(self.ws.repo, "fetch", "-q", "origin")
        code, out = self.ws.run(self.ws.manifest(rows(c3), recipe_commit=c3), "-Only", "b")
        self.assertEqual(code, 1, out)
        self.assertIn(f"sub differ between the recipe commits {c1[:8]} and {c3[:8]}", out)
        self.assertTreesGone()


if __name__ == "__main__":
    unittest.main()
