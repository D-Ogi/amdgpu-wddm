"""bc250kmd_cli names the driver's stages, so it repeats enum BC250_STAGE (driver/kmd/bc250kmd.h) in a table
of its own. This test parses both and fails if they drift apart. The monitor keeps a third copy and has its
own test next to it (tools/win/bc250mon/test_stages.py).

    python -m unittest discover -s tools/win/bc250kmd_cli
"""

import os
import re
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
HEADER = os.path.join(HERE, "..", "..", "..", "driver", "kmd", "bc250kmd.h")
TABLE = os.path.join(HERE, "bc250kmd_cli.c")


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def driver_stages():
    body = re.search(r"typedef enum _BC250_STAGE\s*\{(.*?)\}\s*BC250_STAGE;", read(HEADER), re.S)
    assert body, f"enum BC250_STAGE not found in {HEADER}"
    return {name: int(value) for name, value in re.findall(r"(\w+)\s*=\s*(\d+)\s*,", body.group(1))}


def cli_stages():
    body = re.search(r"g_Stages\[\]\s*=\s*\{(.*?)\n\};", read(TABLE), re.S)
    assert body, f"g_Stages not found in {TABLE}"
    return {name: int(value)
            for value, name in re.findall(r'\{\s*(\d+)\s*,\s*"(\w+)"', body.group(1))}


class StageTableTest(unittest.TestCase):
    def test_same_stages(self):
        self.assertEqual(driver_stages(), cli_stages(),
                         "driver/kmd/bc250kmd.h and bc250kmd_cli.c disagree about BC250_STAGE")

    def test_stages_are_not_empty(self):
        self.assertGreater(len(cli_stages()), 5)


if __name__ == "__main__":
    unittest.main()
