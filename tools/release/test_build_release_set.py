"""Self-test of the release set build (tools/release/build-release-set.ps1 and build_release_set.py).

    python -m unittest discover -s tools/release -p test_build_release_set.py   # the 'release-build-set' gate of quick.ps1

Each test makes a throwaway workspace: a recipe repository with stub recipes and a bare "published" remote, a stub
size guard that reports the numbers the test gives it, and a release manifest. The unit tests run the schema, the plan
and the gates in process: the example manifest is valid, an unpushed commit, an AI agent named in a commit and an
implicit or refused compiler fail their gates, the names of the models this project runs on the GPU do not, and the
collection of a build's outputs takes the PDB that each image names, not another file of that name.
The end-to-end tests run build-release-set.ps1 (pwsh 7 on PATH; skipped without it): the scheduler keeps to -MaxParallel
and to the disk budget, -Plan builds nothing, the gates refuse an unpushed commit, an AI trailer and an implicit
compiler through the tool, and a failed build fails the release set and keeps its log while its tree goes.
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
from test_provenance import git, remove_tree
from test_symbols import msf_pdb_bytes, pe_bytes

OUT = os.environ.get("BC250_TEST_OUT") or None
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
        return build_release_set.make_plan(self.manifest_path, self.root, HERE.parents[1], self.out, self.work, **kw)

    def run(self, manifest, *extra, timeout=600):
        self.write(manifest)
        r = subprocess.run(["pwsh", "-NoProfile", "-File", str(TOOL), "-Manifest", str(self.manifest_path),
                            "-Out", str(self.out), "-Root", str(self.root), "-Work", str(self.work),
                            "-SizeGuard", str(self.guard_dir / "size-guard.py"), *extra],
                           capture_output=True, text=True, timeout=timeout)
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

    def test_only_needs_the_rows_it_reads(self):
        rows = [self.ws.row("a"), self.ws.row("b", extra=["-Payload", "{payload:payload/a.bin}"])]
        plan, errors = self.ws.plan(self.ws.manifest(rows), only=["b"])
        self.assertIsNone(plan)
        self.assertTrue(any("add a to --only" in e for e in errors), errors)

    def test_published_commits_pass_every_gate(self):
        result, gates = self.gates(self.ws.manifest([self.ws.row("a")]))
        self.assertEqual(result["status"], "PASS", json.dumps(result, indent=1))
        self.assertIn("1 commits, 0 hits", " ".join(gates["attribution"]["lines"]))

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
        log = self.ws.out / "logs" / "bad" / "build.log"
        self.assertIn("recipe bad fails on purpose", log.read_text(encoding="utf-8-sig", errors="replace"))
        self.assertIn("see " + str(log), next(b for b in report["builds"] if b["id"] == "bad")["error"])
        self.assertTrue((self.ws.out / "artifacts" / "release" / "a" / "a.bin").is_file())
        self.assertFalse((self.ws.out / "artifacts" / "release" / "bad").exists())
        rows_doc = json.loads((self.ws.out / "release-sources.rows.json").read_text(encoding="utf-8"))
        self.assertEqual([f["path"] for f in rows_doc["files"]], ["payload/a.bin"])
        self.assertTreesGone()
        # -Only rebuilds the failed row and keeps the artifact of the good one.
        rows[1] = self.ws.row("bad", sleep=0)
        code, out = self.ws.run(self.ws.manifest(rows), "-Only", "bad,c")
        self.assertEqual(code, 0, out)
        report = self.ws.report()
        self.assertEqual({b["id"]: (b["status"], bool(b.get("kept"))) for b in report["builds"]},
                         {"a": ("ok", True), "bad": ("ok", False), "c": ("ok", False)})


if __name__ == "__main__":
    unittest.main()
