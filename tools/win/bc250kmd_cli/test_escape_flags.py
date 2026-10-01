"""From KMD 0.7.184.1 the driver answers two escapes with NoAdapterSynchronization alone, so that a lab sampler's
reads no longer take the adapter lock a running game waits on: BC250_ESCAPE_GET_LOG and
BC250_ESCAPE_GET_PAGING_JOURNAL. The driver lists them in display.c (SoftwareReadEscape), and bc250kmd_cli lists the
commands it sends that way in SoftwareRead(). This test parses both lists and fails if they drift apart, if
LOG_SUMMARY (which walks state a stop frees and reads display registers) joins either, if the driver stops
insisting on HardwareAccess for the summary, or if a log or journal read in the CLI bypasses the read path.

    python -m unittest discover -s tools/win/bc250kmd_cli
"""

import os
import re
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
DISPLAY = os.path.join(HERE, "..", "..", "..", "driver", "kmd", "display.c")
CLI = os.path.join(HERE, "bc250kmd_cli.c")

SOFTWARE_READS = {"BC250_ESCAPE_GET_LOG", "BC250_ESCAPE_GET_PAGING_JOURNAL"}


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def switch_cases(source, function):
    """The case labels of the one switch in `function`, a static predicate that returns true for them."""
    body = re.search(r"static\s+\w+\s+" + function + r"\s*\([^)]*\)\s*\{(.*?)\n\}", source, re.S)
    assert body, f"{function} not found"
    switch = re.search(r"switch\s*\([^)]*\)\s*\{(.*?)default\s*:", body.group(1), re.S)
    assert switch, f"{function} has no switch with a default"
    return set(re.findall(r"case\s+(\w+)\s*:", switch.group(1)))


def driver_reads(display_source):
    return switch_cases(display_source, "SoftwareReadEscape")


def cli_reads(cli_source):
    return switch_cases(cli_source, "SoftwareRead")


def problems(display_source, cli_source):
    """Everything wrong with the pair of sources, as a list of sentences (empty when they agree)."""
    found = []
    try:
        driver = driver_reads(display_source)
    except AssertionError as e:
        driver = set()
        found.append(f"display.c: {e}")
    try:
        cli = cli_reads(cli_source)
    except AssertionError as e:
        cli = set()
        found.append(f"bc250kmd_cli.c: {e}")
    if driver != SOFTWARE_READS:
        found.append(f"display.c SoftwareReadEscape admits {sorted(driver)}, expected {sorted(SOFTWARE_READS)}")
    if cli != driver:
        found.append(f"bc250kmd_cli.c SoftwareRead {sorted(cli)} differs from display.c {sorted(driver)}")
    if "BC250_ESCAPE_LOG_SUMMARY" in driver | cli:
        found.append("LOG_SUMMARY must stay a HardwareAccess (Level Two) escape")
    # The software dispatch takes exactly NoAdapterSynchronization, nothing more.
    if not re.search(r"softwareRead\.NoAdapterSynchronization\s*=\s*1\s*;", display_source) or \
       not re.search(r"SoftwareReadEscape\(command\)\s*&&\s*Escape->Flags\.Value\s*==\s*softwareRead\.Value",
                     display_source):
        found.append("display.c: the software read is not tied to Flags.Value == {NoAdapterSynchronization}")
    # The summary keeps its own guard.
    if not re.search(r"summary\s*&&\s*\(\s*!Escape->Flags\.HardwareAccess\s*\|\|\s*"
                     r"Escape->Flags\.NoAdapterSynchronization\s*\)", display_source):
        found.append("display.c: LOG_SUMMARY is no longer refused without HardwareAccess")
    # Every log and journal read of the CLI goes through the read path, which picks the flags by command.
    for call in re.findall(r"SendEscape\w*\(BC250_DEFAULT_HWID,[^;]*;", cli_source):
        if re.search(r"\b(log|journal)\b", call) and not re.match(r"SendReadEscape|SendEscapeHeld", call):
            found.append(f"bc250kmd_cli.c: a log or journal read bypasses the read path: {call}")
    for name in ("SendReadEscape", "SendEscapeHeld"):
        if not re.search(r"\b" + name + r"\(BC250_DEFAULT_HWID,\s*\w+(->|\.)Command,", cli_source):
            found.append(f"bc250kmd_cli.c: {name} is not called with the request's own Command")
    return found


class EscapeFlagsTest(unittest.TestCase):
    def test_driver_and_cli_agree(self):
        self.assertEqual(problems(read(DISPLAY), read(CLI)), [])

    def test_lists_are_the_two_reads(self):
        self.assertEqual(driver_reads(read(DISPLAY)), SOFTWARE_READS)
        self.assertEqual(cli_reads(read(CLI)), SOFTWARE_READS)

    def test_negative_controls(self):
        display, cli = read(DISPLAY), read(CLI)
        # The summary added to the CLI's list.
        bad_cli = cli.replace("    case BC250_ESCAPE_GET_LOG:\n",
                              "    case BC250_ESCAPE_GET_LOG:\n    case BC250_ESCAPE_LOG_SUMMARY:\n", 1)
        bad_cli = bad_cli.replace("    case BC250_ESCAPE_GET_LOG:\r\n",
                                  "    case BC250_ESCAPE_GET_LOG:\r\n    case BC250_ESCAPE_LOG_SUMMARY:\r\n", 1)
        self.assertNotEqual(bad_cli, cli)
        self.assertTrue(problems(display, bad_cli))
        # The driver's summary guard removed.
        bad_display = display.replace("!Escape->Flags.HardwareAccess", "FALSE", 1)
        self.assertNotEqual(bad_display, display)
        self.assertTrue(problems(bad_display, cli))
        # The flags check loosened to "NoAdapterSynchronization set".
        bad_display = display.replace("Escape->Flags.Value == softwareRead.Value",
                                      "Escape->Flags.NoAdapterSynchronization", 1)
        self.assertNotEqual(bad_display, display)
        self.assertTrue(problems(bad_display, cli))
        # A journal read sent the old way.
        bad_cli = cli.replace("SendReadEscape(BC250_DEFAULT_HWID, journal.Command, &journal,",
                              "SendEscape(BC250_DEFAULT_HWID, &journal,", 1)
        self.assertNotEqual(bad_cli, cli)
        self.assertTrue(problems(display, bad_cli))


if __name__ == "__main__":
    unittest.main()
