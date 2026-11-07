"""Tiny content-identity fixtures only; no installed toolchain walk or compiler call."""
import os
from pathlib import Path
import subprocess
import tempfile
import types
import unittest
from unittest.mock import patch

import build_context as context


class ContextTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="build-context-", dir=os.environ.get("BC250_TEST_OUT"))
        self.root = Path(self.temp.name)
        self.repo = self.root / "repo"
        for helper in context.RUNTIME_HELPERS:
            self.write(self.repo / helper, helper.encode())
        self.tool = self.root / "toolchain/llvm/bin/compiler.exe"
        self.write(self.tool, b"compiler-A")

    def tearDown(self):
        self.temp.cleanup()

    @staticmethod
    def write(path, data):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def capture(self, **kwargs):
        options = dict(environ={}, executables={}, discover_vs=False)
        options.update(kwargs)
        return context.capture_context(self.root, self.repo, **options)

    def test_repeated_capture_is_deterministic_without_timestamps(self):
        first = self.capture()
        self.assertTrue(first["complete"])
        os.utime(self.tool, (1_700_000_000, 1_700_000_000))
        self.assertEqual(self.capture(), first)

    def test_same_size_same_mtime_content_replacement_changes_identity(self):
        first = self.capture()
        timestamp = self.tool.stat().st_mtime_ns
        self.tool.write_bytes(b"compiler-B")
        os.utime(self.tool, ns=(timestamp, timestamp))
        self.assertNotEqual(first["fingerprint"], self.capture()["fingerprint"])

    def test_tree_rename_changes_identity(self):
        first = self.capture()
        self.tool.rename(self.tool.with_name("other.exe"))
        self.assertNotEqual(first["fingerprint"], self.capture()["fingerprint"])

    def test_python_modules_and_explicit_mapping_are_content_inputs(self):
        external = self.root / "override/tool.exe"
        module = self.root / "scratch/py/package/module.py"
        self.write(external, b"tool-A")
        self.write(module, b"module-A")
        first = self.capture(tools={"special": str(external)})
        self.write(module, b"module-B")
        second = self.capture(tools={"special": str(external)})
        self.assertNotEqual(first["fingerprint"], second["fingerprint"])
        self.write(external, b"tool-B")
        self.assertNotEqual(second["fingerprint"], self.capture(tools={"special": str(external)})["fingerprint"])

    def test_helper_edit_invalidates_but_documentation_and_test_edits_do_not(self):
        first = self.capture()
        self.write(self.repo / "docs/build.md", b"docs")
        self.write(self.repo / "tools/release/test_build_context.py", b"test")
        self.assertEqual(first, self.capture())
        self.write(self.repo / context.RUNTIME_HELPERS[0], b"changed helper")
        self.assertNotEqual(first["fingerprint"], self.capture()["fingerprint"])

    def test_python_bytecode_is_excluded_but_source_changes_are_hashed(self):
        source = self.root / "scratch/py/package/module.py"
        self.write(source, b"source-A")
        first = self.capture()
        self.write(source.parent / "__pycache__/module.cpython-314.pyc", b"generated")
        self.write(source.with_suffix(".pyc"), b"legacy generated")
        self.assertEqual(first, self.capture())
        self.write(source, b"source-B")
        self.assertNotEqual(first["fingerprint"], self.capture()["fingerprint"])

    def test_environment_order_and_path_content_are_inputs(self):
        include = self.root / "include"
        self.write(include / "header.h", b"A")
        first = self.capture(environ={"PATH": "a;b", "INCLUDE": str(include), "CL": "/O2"})
        self.assertNotEqual(first, self.capture(environ={"PATH": "b;a", "INCLUDE": str(include), "CL": "/O2"}))
        self.write(include / "header.h", b"B")
        self.assertNotEqual(first, self.capture(environ={"PATH": "a;b", "INCLUDE": str(include), "CL": "/O2"}))

    def test_absent_explicit_mapping_or_helper_disables_reuse(self):
        receipt = self.capture(tools={"unknown": str(self.root / "missing")})
        self.assertFalse(receipt["complete"])
        self.assertEqual(receipt["inputs"]["tools"]["unknown"]["state"], "absent")
        self.assertIn({"path": str(self.root / "missing"), "state": "required-input-absent"},
                      receipt["incomplete_inputs"])
        (self.repo / context.RUNTIME_HELPERS[0]).unlink()
        self.assertFalse(self.capture()["complete"])

    def test_unresolved_optional_executable_is_recorded(self):
        receipt = self.capture(executables={"missing-tool": None})
        self.assertEqual(receipt["inputs"]["executables"]["missing-tool"], {"state": "not-resolved"})

    def test_same_input_is_read_only_once_per_capture(self):
        original = Path.open
        reads = []

        def opened(path, *args, **kwargs):
            if path == self.tool:
                reads.append(str(path))
            return original(path, *args, **kwargs)

        with patch.object(Path, "open", opened):
            self.capture(tools={"compiler": str(self.tool)}, executables={"compiler": str(self.tool)})
        self.assertEqual(len(reads), 1)

    def test_unreadable_input_is_not_silently_ignored(self):
        original = Path.open

        def opened(path, *args, **kwargs):
            if path == self.tool:
                raise PermissionError("fixture denied")
            return original(path, *args, **kwargs)

        with patch.object(Path, "open", opened):
            receipt = self.capture()
        self.assertFalse(receipt["complete"])
        self.assertEqual(receipt["inputs"]["defaults"]["toolchain"]["state"], "incomplete-directory")
        self.assertIn({"path": str(self.tool), "state": "unreadable", "error": "PermissionError"},
                      receipt["incomplete_inputs"])

    def test_reparse_input_is_refused_without_following_it(self):
        original = Path.lstat

        def inspected(path):
            found = original(path)
            if path == self.tool:
                return types.SimpleNamespace(st_mode=found.st_mode, st_file_attributes=0x400)
            return found

        with patch.object(Path, "lstat", inspected):
            receipt = self.capture()
        self.assertFalse(receipt["complete"])
        self.assertIn({"path": str(self.tool), "state": "unsupported-link"}, receipt["incomplete_inputs"])

    def test_vswhere_probe_is_bounded_and_hashes_selected_tool_directories(self):
        vs = self.root / "visual-studio"
        self.write(vs / "VC/bin/cl.exe", b"cl")
        self.write(vs / "MSBuild/msbuild.exe", b"msbuild")
        probe = self.root / "vswhere.exe"
        self.write(probe, b"probe fixture, never executed")
        completed = subprocess.CompletedProcess([], 0, str(vs) + "\n", "")
        with patch.object(context.subprocess, "run", return_value=completed) as run:
            first = self.capture(tools={"vswhere": str(probe)}, discover_vs=True)
            self.assertEqual(run.call_args.kwargs["timeout"], 15)
            self.assertIn("Microsoft.VisualStudio.Component.VC.Tools.x86.x64", run.call_args.args[0])
            self.assertTrue(first["complete"])
            self.write(vs / "VC/bin/cl.exe", b"changed compiler")
            second = self.capture(tools={"vswhere": str(probe)}, discover_vs=True)
            self.assertNotEqual(first["fingerprint"], second["fingerprint"])

    def test_failed_vswhere_discovery_disables_reuse(self):
        probe = self.root / "vswhere.exe"
        self.write(probe, b"not executed")
        with patch.object(context.subprocess, "run", side_effect=subprocess.TimeoutExpired("fixture", 15)):
            self.assertFalse(self.capture(tools={"vswhere": str(probe)}, discover_vs=True)["complete"])


if __name__ == "__main__":
    unittest.main()
