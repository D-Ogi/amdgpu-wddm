"""BD-130: where bc250kmd_cli takes THIS boot's BootId from when it places the driver's stop record.

The driver writes SharedUserData->BootId into the record (driver/kmd/guard.c), which is
KUSER_SHARED_DATA.BootId. Windows keeps a second copy of the same counter in the PrefetchParameters registry
value, and that copy lags inside a boot: on 22631, 13 minutes after a boot, the page said 388 and the registry
387, so this reader called the fresh record of that boot "AN EARLIER BOOT". The reader must therefore read the
page, keep the registry value as the fallback only, name the source it used, and never call a HIGHER number in
the record an earlier boot.

    python -m unittest discover -s tools/win/bc250kmd_cli
"""

import os
import re
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(HERE, "bc250kmd_cli.c")


def read(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def reader_body(text):
    body = re.search(r"static int BootIdOfThisBoot\(([^\n]*)\)\s*\{(.*?)\n\}", text, re.S)
    assert body, "BootIdOfThisBoot not found in bc250kmd_cli.c"
    return body.group(1), body.group(2)


class BootIdSource(unittest.TestCase):
    def setUp(self):
        self.text = read(SOURCE)
        self.signature, self.body = reader_body(self.text)

    def test_the_page_address_is_the_offset_of_the_wdk_header(self):
        self.assertIn("0x7FFE02C4", self.text, "the shared page is read at KUSER_SHARED_DATA + 0x2C4")
        self.assertRegex(self.text, r"ntddk\.h[^\n]*8965|8965[^\n]*ntddk\.h|ntddk\.h")
        for cited in ("9568", "9569", "0x2C0", "0x2C8"):
            self.assertIn(cited, self.text, "the comment cites the header lines that bracket the field: " + cited)

    def test_the_page_answers_first_and_the_registry_is_the_fallback(self):
        page = self.body.index("BC250_SHARED_BOOT_ID")
        registry = self.body.index("ReadDword")
        self.assertLess(page, registry, "the shared page is read before the registry value")
        self.assertIn("source", self.signature, "the reader reports which source answered")
        self.assertIn('*source = "KUSER_SHARED_DATA"', self.body)
        self.assertRegex(self.body, r'\*source = "the PrefetchParameters registry value, which can lag')

    def test_the_verdict_names_this_boot_an_earlier_one_and_a_later_one(self):
        verdict = re.search(r"if \(haveBoot && haveNow\)(.*?)else if \(haveBoot\)", self.text, re.S)
        self.assertTrue(verdict, "the three-way verdict of the stop record was not found")
        line = verdict.group(1)
        self.assertIn("(from %s)", line, "the line prints the source of this boot's number")
        self.assertIn("boot == now", line)
        self.assertIn("boot < now", line, "a LOWER number in the record is the earlier boot")
        self.assertIn("AN EARLIER BOOT", line)
        self.assertIn("A LATER BOOT", line, "a HIGHER number is no earlier boot and says so")
        self.assertNotIn("boot != now", line, "no reading of two different counters as an earlier boot")


if __name__ == "__main__":
    unittest.main()
