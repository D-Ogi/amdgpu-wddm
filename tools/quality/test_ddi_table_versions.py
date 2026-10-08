"""Self-test of the ddi-table-versions gate (tools/quality/ddi_table_versions.py).

    python -m unittest discover -s tools/quality      # the 'quality-controls' gate of quick.ps1

The gate says that the newer DDI table inherits every entry of the table below it, so that the shell
may copy that table and replace only what changed. Each case below breaks that agreement in one way
and asks the gate to refuse it: an inherited entry renamed, an inherited entry retyped, an appended
entry missing, an entry named as changed that did not change, and a builder that assigns one entry too
many or one too few. The headers of the kit are read as well, which is the gate's own live case.
"""
from pathlib import Path
import unittest

import ddi_table_versions as g

REPO = Path(__file__).resolve().parents[2]

OLD_STRUCT = """
typedef struct D3DOLDDI_DEVICEFUNCS
{
    PFN_A   pfnOne;    // a comment
    PFN_B   pfnTwo;
#ifdef D3D10PSGP
    PFN_X   pfnPsgpOnly;
#endif
    PFN_C   pfnThree;
} D3DOLDDI_DEVICEFUNCS;
"""
NEW_STRUCT = """
typedef struct D3DNEWDI_DEVICEFUNCS
{
    PFN_A   pfnOne;
    PFN_B2  pfnTwo;
    PFN_C   pfnThree;
    PFN_D   pfnFour;
} D3DNEWDI_DEVICEFUNCS;
"""
RETYPED = {"pfnTwo": "PFN_B2"}
APPENDED = ["pfnFour"]
BUILDER = """
D3DNEWDI_DEVICEFUNCS make_new_table() {
    D3DNEWDI_DEVICEFUNCS t{};
    std::memcpy(&t,&base(),sizeof(base()));
    t.pfnTwo=two_new;
    t.pfnFour=four;
    return t;
}
"""


def entries(text, name):
    return g.device_entries(text, name)


class TableVersions(unittest.TestCase):
    def compare(self, old_text=OLD_STRUCT, new_text=NEW_STRUCT, retyped=None, appended=None):
        failures = []
        g.compare("device", entries(old_text, "D3DOLDDI_DEVICEFUNCS"), entries(new_text, "D3DNEWDI_DEVICEFUNCS"),
                  3, 4, RETYPED if retyped is None else retyped,
                  APPENDED if appended is None else appended, failures)
        return failures

    def test_agreeing_pair_passes(self):
        self.assertEqual(self.compare(), [])

    def test_psgp_entries_are_dropped(self):
        names = [name for _, name in entries(OLD_STRUCT, "D3DOLDDI_DEVICEFUNCS")]
        self.assertEqual(names, ["pfnOne", "pfnTwo", "pfnThree"])

    def test_last_declaration_wins(self):
        first = OLD_STRUCT.replace("PFN_C   pfnThree;", "")
        body = g.struct_body(first + OLD_STRUCT, "D3DOLDDI_DEVICEFUNCS")
        self.assertIn("pfnThree", body)

    def test_inherited_entry_renamed(self):
        failures = self.compare(new_text=NEW_STRUCT.replace("pfnThree", "pfnThreeRenamed"))
        self.assertTrue(any("pfnThreeRenamed" in f for f in failures), failures)

    def test_inherited_entry_retyped(self):
        failures = self.compare(new_text=NEW_STRUCT.replace("PFN_C   pfnThree", "PFN_C2  pfnThree"))
        self.assertTrue(any("PFN_C2" in f for f in failures), failures)

    def test_appended_entry_missing(self):
        failures = self.compare(new_text=NEW_STRUCT.replace("    PFN_D   pfnFour;\n", ""))
        self.assertTrue(failures)

    def test_entry_named_as_changed_did_not_change(self):
        failures = self.compare(retyped={"pfnTwo": "PFN_B2", "pfnThree": "PFN_C"})
        self.assertTrue(any("pfnThree" in f for f in failures), failures)

    def test_builder_assigns_an_inherited_entry(self):
        failures = []
        g.check_builder("fake.cpp", BUILDER.replace("t.pfnTwo=two_new;", "t.pfnTwo=two_new; t.pfnOne=one;"),
                        "make_new_table", {"pfnTwo", "pfnFour"}, failures)
        self.assertTrue(any("pfnOne" in f for f in failures), failures)

    def test_builder_leaves_a_changed_entry(self):
        failures = []
        g.check_builder("fake.cpp", BUILDER.replace("t.pfnFour=four;", ""),
                        "make_new_table", {"pfnTwo", "pfnFour"}, failures)
        self.assertTrue(any("pfnFour" in f for f in failures), failures)

    def test_agreeing_builder_passes(self):
        failures = []
        g.check_builder("fake.cpp", BUILDER, "make_new_table", {"pfnTwo", "pfnFour"}, failures)
        self.assertEqual(failures, [])

    def test_repository_tables_agree_with_the_kit(self):
        import os
        import subprocess
        import sys
        roots = [Path(os.environ["BC250_ROOT"])] if os.environ.get("BC250_ROOT") else []
        roots.append(REPO.parent)
        kits = [root / "toolchain" / "nuget" for root in roots if (root / "toolchain" / "nuget").is_dir()]
        if not kits:
            self.skipTest("no WDK/SDK kit beside this checkout")
        result = subprocess.run([sys.executable, str(Path(g.__file__)), "--kits", str(kits[0])],
                                cwd=str(REPO), capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(result.stdout.startswith("PASS"), result.stdout)


if __name__ == "__main__":
    unittest.main()
