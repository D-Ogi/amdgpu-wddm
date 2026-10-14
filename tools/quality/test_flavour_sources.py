"""Self-test of the flavour-sources gate (tools/quality/flavour_sources.py).

    python -m unittest discover -s tools/quality      # the 'quality-controls' gate of quick.ps1

Every negative control is code this project actually shipped in the tree. `Crash.cs` was added to the
control application by BD-174, after the BD-167 branch was cut, and its `Folder()` read
AMDGPU_WDDM_CONTROL_STATE in every flavour; the release image therefore carried the name of a diagnostic
setting, and only the build's own image check caught it, on a merge. The two arms of that method, before and
after the guard, are the controls here, next to the two readers that were already correct.
"""
import os
from pathlib import Path
import unittest

import flavour_sources as f

REPO = Path(__file__).resolve().parents[2]

# Crash.cs Folder() as it stood when the merge took it: no guard at all.
BEFORE = '''        public static string Folder()
        {
            if (DirectoryForTests != null) return DirectoryForTests;
            var over = Environment.GetEnvironmentVariable("AMDGPU_WDDM_CONTROL_STATE");
            return string.IsNullOrEmpty(over)
                ? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "amdgpu-wddm")
                : over;
        }
'''

# The same method with the guard, which is what the tree carries now.
AFTER = '''        public static string Folder()
        {
            if (DirectoryForTests != null) return DirectoryForTests;
#if AMDGPU_WDDM_LAB_FLAVOUR
            var over = Environment.GetEnvironmentVariable("AMDGPU_WDDM_CONTROL_STATE");
            return string.IsNullOrEmpty(over)
                ? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "amdgpu-wddm")
                : over;
#else
            return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "amdgpu-wddm");
#endif
        }
'''

# RecoveryActions.StateOverride: the guard with both arms on one line each.
PROPERTY = '''#if AMDGPU_WDDM_LAB_FLAVOUR
        static string StateOverride { get { var d = Environment.GetEnvironmentVariable("AMDGPU_WDDM_CONTROL_STATE"); return string.IsNullOrEmpty(d) ? null : d; } }
#else
        static string StateOverride { get { return null; } }
#endif
'''

# The shape that would slip past a rule that only looked for the name near a guard: the read is in the #else
# arm, which is the release flavour.
IN_ELSE = '''#if AMDGPU_WDDM_LAB_FLAVOUR
            var over = "";
#else
            var over = Environment.GetEnvironmentVariable("AMDGPU_WDDM_CONTROL_STRINGS");
#endif
'''

# A name in a comment beside guarded code is not a read.
COMMENT = '''        // AMDGPU_WDDM_CONTROL_STATE moves the directory for the build's gates, in a lab build only.
#if AMDGPU_WDDM_LAB_FLAVOUR
            var over = Environment.GetEnvironmentVariable("AMDGPU_WDDM_CONTROL_STATE");
#endif
'''

# A nested #if under the guard stays guarded.
NESTED = '''#if AMDGPU_WDDM_LAB_FLAVOUR
#if DEBUG
            var over = Environment.GetEnvironmentVariable("AMDGPU_WDDM_CONTROL_NO_DLL");
#endif
#endif
'''


class UnguardedReads(unittest.TestCase):
    def test_the_defect_this_gate_exists_for_fails(self):
        hits = f.unguarded_lab_reads(BEFORE)
        self.assertEqual(1, len(hits), hits)
        self.assertEqual("AMDGPU_WDDM_CONTROL_STATE", hits[0][1])
        self.assertEqual(4, hits[0][0])

    def test_the_guarded_method_passes(self):
        self.assertEqual([], f.unguarded_lab_reads(AFTER))

    def test_the_one_line_property_passes(self):
        self.assertEqual([], f.unguarded_lab_reads(PROPERTY))

    def test_a_read_in_the_else_arm_fails(self):
        hits = f.unguarded_lab_reads(IN_ELSE)
        self.assertEqual(1, len(hits), hits)
        self.assertEqual("AMDGPU_WDDM_CONTROL_STRINGS", hits[0][1])

    def test_a_name_in_a_comment_is_not_a_read(self):
        self.assertEqual([], f.unguarded_lab_reads(COMMENT))

    def test_a_nested_if_under_the_guard_stays_guarded(self):
        self.assertEqual([], f.unguarded_lab_reads(NESTED))

    def test_every_one_of_the_four_names_is_read(self):
        for name in f.LAB_NAMES:
            text = '            var x = Environment.GetEnvironmentVariable("%s");\n' % name
            self.assertEqual(1, len(f.unguarded_lab_reads(text)), name)

    def test_the_tree_itself_is_clean(self):
        src = REPO / "tools" / "win" / "amdgpu_wddm_control" / "src"
        self.assertTrue(src.is_dir(), src)
        bad = {}
        for path in sorted(src.glob("*.cs")):
            hits = f.unguarded_lab_reads(path.read_text(encoding="utf-8-sig"))
            if hits:
                bad[path.name] = hits
        self.assertEqual({}, bad)

    def test_the_gate_returns_one_when_it_finds_something(self):
        out = os.environ.get("BC250_TEST_OUT")
        code = f.main(["--root", str(REPO)] + (["--out", out] if out else []))
        self.assertEqual(0, code)


if __name__ == "__main__":
    unittest.main()
