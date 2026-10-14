"""The raw diagnostic verbs of bc250kmd_cli (BD-167) are named in three places: the lab block of the CLI's own
dispatch (wmain, inside #ifdef AMDGPU_WDDM_LAB_FLAVOUR), the cases of verbs_test.c, which run each verb with a
valid argument shape against a fake adapter, and $labVerbs of test-flavour.ps1, which checks the usage text of a
built CLI. This test fails if the three lists drift apart, or if a verb of the lab block is also answered outside
it, where a release build would reach it.

    python -m unittest discover -s tools/win/bc250kmd_cli
"""

import os
import re
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
VERB = re.compile(r'_wcsicmp\(argv\[1\], L"([A-Za-z0-9-]+)"\)')


def read(path):
    with open(os.path.join(HERE, path), encoding="utf-8") as f:
        return f.read()


def dispatch(source):
    """(lab verbs, release verbs) of wmain: the verbs compared inside and outside #ifdef AMDGPU_WDDM_LAB_FLAVOUR."""
    start = source.index("\nint wmain(")
    body = source[start:]
    lab, release, inside = set(), set(), False
    for line in body.splitlines():
        stripped = line.strip()
        if stripped.startswith("#ifdef AMDGPU_WDDM_LAB_FLAVOUR"):
            inside = True
        elif stripped.startswith("#endif"):
            inside = False
        (lab if inside else release).update(VERB.findall(line))
        if line == "}":
            break
    return lab, release


class LabVerbsTest(unittest.TestCase):
    def setUp(self):
        self.lab, self.release = dispatch(read("bc250kmd_cli.c"))

    def test_dispatch_has_lab_verbs(self):
        self.assertGreaterEqual(len(self.lab), 18, sorted(self.lab))

    def test_no_lab_verb_outside_the_lab_block(self):
        self.assertEqual(self.lab & self.release, set())

    def test_verbs_test_runs_every_lab_verb(self):
        source = read("verbs_test.c")
        cases = set(re.findall(r'\{ \{ L"([a-z0-9]+)"', source))
        listed = re.search(r"g_verbs\[\] = \{(.*?)\};", source, re.S)
        self.assertIsNotNone(listed)
        self.assertEqual(cases, self.lab)
        self.assertEqual(set(re.findall(r'L"([a-z0-9]+)"', listed.group(1))), self.lab)

    def test_test_flavour_names_every_lab_verb(self):
        listed = re.search(r"\$labVerbs = @\((.*?)\)", read("test-flavour.ps1"), re.S)
        self.assertIsNotNone(listed)
        self.assertEqual(set(re.findall(r"'([a-z0-9]+)'", listed.group(1))), self.lab)


if __name__ == "__main__":
    unittest.main()
