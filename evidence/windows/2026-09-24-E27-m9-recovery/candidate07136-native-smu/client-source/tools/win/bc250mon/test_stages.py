"""The stage numbers exist twice: in the driver (enum BC250_STAGE, driver/kmd/bc250kmd.h) and in the monitor
(KmdStages.All, tools/win/bc250mon/src/KmdProvider.cs), because the monitor reads them out of the registry and
has to name them. This test parses both and fails if they ever drift apart.

    python -m unittest discover -s tools/win/bc250mon
"""

import os
import re
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
HEADER = os.path.join(HERE, "..", "..", "..", "driver", "kmd", "bc250kmd.h")
TABLE = os.path.join(HERE, "src", "KmdProvider.cs")


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def driver_stages():
    """{name: number} from enum BC250_STAGE."""
    body = re.search(r"typedef enum _BC250_STAGE\s*\{(.*?)\}\s*BC250_STAGE;", read(HEADER), re.S)
    assert body, f"enum BC250_STAGE not found in {HEADER}"
    return {name: int(value) for name, value in re.findall(r"(\w+)\s*=\s*(\d+)\s*,", body.group(1))}


def monitor_stages():
    """{name: number} from KmdStages.All."""
    return {name: int(value)
            for value, name in re.findall(r'new KmdStage\(\s*(\d+)\s*,\s*"(\w+)"', read(TABLE))}


def define(name):
    """A #define of a plain number from the driver header."""
    found = re.search(r"#define\s+%s\s+(\d+)" % re.escape(name), read(HEADER))
    assert found, f"#define {name} not found in {HEADER}"
    return int(found.group(1))


def constant(name):
    """A const int of KmdStages."""
    found = re.search(r"public const int %s\s*=\s*(\d+)" % re.escape(name), read(TABLE))
    assert found, f"KmdStages.{name} not found in {TABLE}"
    return int(found.group(1))


class StageTableTest(unittest.TestCase):
    def test_same_stages(self):
        self.assertEqual(driver_stages(), monitor_stages(),
                         "driver/kmd/bc250kmd.h and src/KmdProvider.cs disagree about BC250_STAGE")

    def test_stages_are_not_empty(self):
        self.assertGreater(len(driver_stages()), 5)

    def test_first_present_done(self):
        self.assertEqual(driver_stages()["StageFirstPresentDone"], constant("FirstPresentDone"))

    def test_max_unconfirmed_starts(self):
        self.assertEqual(define("BC250_MAX_UNCONFIRMED_STARTS"), constant("MaxUnconfirmedStarts"))


if __name__ == "__main__":
    unittest.main()
