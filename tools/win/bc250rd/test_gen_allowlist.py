"""Run: python -m unittest discover -s tools/win/bc250rd

The allow-list generator must never put the UVD/VCN window into the driver's table. VCN 2.0.3 is
present on this part, its island is power- and clock-gated, and a read there is reported to wedge the
SoC (facts M787). The band itself is in `tools/diagusb/gen_probes.py`, computed from the IP base table.
"""

import sys
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[1] / "diagusb"))
sys.path.insert(0, str(HERE.parents[1] / "regcalc"))

import gen_allowlist  # noqa: E402
from gen_probes import denied, deny_range  # noqa: E402


class DenyWindow(unittest.TestCase):
    def test_the_band_is_the_uvd_window(self):
        self.assertEqual(deny_range(), (0x7800 * 4, 0x9000 * 4))

    def test_the_checked_in_list_holds_nothing_inside_the_band(self):
        offsets = []
        for line in (HERE / "reglist.txt").read_text(encoding="utf-8").splitlines():
            if line.strip():
                offsets.append(int(line.rsplit(" ", 1)[1], 16))
        self.assertTrue(offsets, "reglist.txt is empty")
        self.assertEqual([f"0x{off:05X}" for off in offsets if denied(off)], [])

    def test_a_sweep_entry_inside_the_band_is_dropped(self):
        low, _ = deny_range()
        sweep = [("GC", "GRBM_STATUS", 0x8900), ("UVD0", "UVD_FAKE_STATUS", low),
                 ("UVD0", "UVD_FAKE_CNTL", low + 8), ("THM", "THM_TCON_CUR_TMP", 0x59800)]
        written = {}

        class FakePath:
            """Collects what the generator writes, instead of touching the checked-in files."""

            def __init__(self, name=""):
                self.name = name

            def __truediv__(self, part):
                return FakePath(f"{self.name}/{part}" if self.name else part)

            def write_text(self, text, **kwargs):
                written[self.name] = text

        def reads(path, only_ip=None):
            return [row for row in sweep if only_ip is None or row[0] == only_ip]

        with mock.patch.object(gen_allowlist, "good_reads", reads), \
                mock.patch.object(gen_allowlist, "HERE", FakePath()):
            gen_allowlist.main()

        self.assertIn("UVD_FAKE", str(sweep))             # the input really held them
        self.assertNotIn("UVD_FAKE", written["reglist.txt"])
        self.assertNotIn(f"0x{low:05X}", written["driver/allowlist.h"])
        self.assertIn("GRBM_STATUS", written["reglist.txt"])


if __name__ == "__main__":
    unittest.main()
