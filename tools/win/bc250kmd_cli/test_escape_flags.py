"""From KMD 0.7.184.1 the driver answers two escapes with NoAdapterSynchronization alone, so that a lab sampler's
reads no longer take the adapter lock a running game waits on: BC250_ESCAPE_GET_LOG and
BC250_ESCAPE_GET_PAGING_JOURNAL. The driver lists them in display.c (SoftwareReadEscape), and bc250kmd_cli lists the
commands it sends that way in SoftwareRead(). This test parses both lists and fails if they drift apart, if
LOG_SUMMARY (which walks state a stop frees and reads display registers) joins either, if the driver stops
insisting on HardwareAccess for the summary, or if a log or journal read in the CLI bypasses the read path.

From 0.7.185.1 it also holds BC250_ESCAPE_RUN_DPM_TUNE (the DPM governor's runtime thresholds and floor) to the same
rule as RUN_DPM: the driver accepts exactly {NoAdapterSynchronization}, refuses a non-administrator's write before it
takes the DPM lock, keeps the adapter context with rundown protection and is reached ahead of display.c's
NoAdapterSynchronization refusal; the CLI builds the request in one place and sends it that way.

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


DPM = os.path.join(HERE, "..", "..", "..", "driver", "kmd", "dpm.c")


def function_body(source, name):
    """The body of a top-level C function definition (up to the first closing brace at column 0)."""
    match = re.search(r"^\w[^\n;]*\b" + name + r"\s*\([^)]*\)\s*\{(.*?)^\}", source, re.S | re.M)
    return match.group(1) if match else None


def dpm_tune_problems(display_source, dpm_source, cli_source):
    """KMD 0.7.185.1 BC250_ESCAPE_RUN_DPM_TUNE: software state, so NoAdapterSynchronization alone, like RUN_DPM. The
    driver must insist on exactly that flag word, refuse a write from a non-administrator before it touches anything,
    and be reached ahead of display.c's NoAdapterSynchronization refusal; the CLI must send every tune request that way.
    Returns a list of sentences, empty when all of that holds."""
    found = []
    body = function_body(dpm_source, "DpmTuneRequest")
    if body is None:
        return ["dpm.c: DpmTuneRequest not found"]
    if not re.search(r"expectedFlags\.NoAdapterSynchronization\s*=\s*1\s*;", body) or \
       not re.search(r"EscapeFlags\s*!=\s*expectedFlags\.Value\)\s*return\s*;", body):
        found.append("dpm.c: DpmTuneRequest does not refuse every flag word but {NoAdapterSynchronization}")
    admin = re.search(r"if\s*\(\s*write\s*&&\s*!Admin\s*\)", body)
    lock = body.find("DpmLock(s)")
    if not admin or lock < 0 or admin.start() > lock:
        found.append("dpm.c: DpmTuneRequest does not refuse a non-administrator's write before it takes the DPM lock")
    if not re.search(r"ExAcquireRundownProtection\(&Device->StartHealth\.Readers\)", body):
        found.append("dpm.c: DpmTuneRequest takes no rundown protection on the adapter context")
    dispatch = re.search(r"if\s*\(\s*command\s*==\s*BC250_ESCAPE_RUN_DPM_TUNE\s*\)\s*\{(.*?)\}", display_source, re.S)
    refusal = display_source.find("data->Command!=BC250_ESCAPE_RUN_CLOCK")
    if not dispatch:
        found.append("display.c: no RUN_DPM_TUNE dispatch")
    else:
        if refusal < 0 or dispatch.start() > refusal:
            found.append("display.c: RUN_DPM_TUNE is dispatched after the NoAdapterSynchronization refusal")
        # 0.7.197.1: two exact sizes, ABI 2 and its ABI 1 prefix, and the size handed on to DpmTuneRequest.
        if not re.search(r"PrivateDriverDataSize\s*!=\s*sizeof\(BC250_ESCAPE_DPM_TUNE\)\s*&&\s*"
                         r"Escape->PrivateDriverDataSize\s*!=\s*BC250_DPM_TUNE_ABI1_SIZE\)", dispatch.group(1)) or \
           "Escape->Flags.Value" not in dispatch.group(1) or \
           not re.search(r"DpmTuneRequest\(device,[^;]*Escape->PrivateDriverDataSize,", dispatch.group(1)):
            found.append("display.c: RUN_DPM_TUNE dispatch without the exact size check or the flag word")
    # The ABI 1 size must never see an ABI 2 field: each one is read or written only inside an `if (abi2)` block, and
    # abi2 / abi1 pair each AbiVersion with its own size.
    if not re.search(r"abi2\s*=\s*abi\s*==\s*BC250_DPM_TUNE_ABI\s*&&\s*Size\s*==\s*sizeof\(BC250_ESCAPE_DPM_TUNE\)", body) or \
       not re.search(r"abi1\s*=\s*abi\s*==\s*BC250_DPM_TUNE_ABI_1\s*&&\s*Size\s*==\s*BC250_DPM_TUNE_ABI1_SIZE", body) or \
       not re.search(r"if\s*\(\s*!\(abi1\s*\|\|\s*abi2\)", body):
        found.append("dpm.c: DpmTuneRequest does not pair AbiVersion with the escape's size")
    guarded = [m.span() for m in re.finditer(r"if\s*\(abi2\)\s*\{[^{}]*\}", body)]
    for m in re.finditer(r"Data->(HotStepMs|SoftReleaseDeltaMc|SoftReleaseStepMs|DefaultHotStepMs|"
                         r"DefaultSoftReleaseDeltaMc|DefaultSoftReleaseStepMs|Reserved2)", body):
        if not any(a <= m.start() < b for a, b in guarded):
            found.append(f"dpm.c: DpmTuneRequest touches Data->{m.group(1)} outside an if (abi2) block")
            break
    query = function_body(cli_source, "TuneQuery")
    if query is None:
        found.append("bc250kmd_cli.c: TuneQuery not found")
    elif not re.search(r"SendEscapeFlags\(BC250_DEFAULT_HWID,\s*t,\s*size,\s*1,", query):
        found.append("bc250kmd_cli.c: TuneQuery does not send with NoAdapterSynchronization alone (softwareOnly 1)")
    # Exactly one request is built, in TuneQuery: no other path can send it with other flags.
    builders = re.findall(r"Command\s*=\s*BC250_ESCAPE_RUN_DPM_TUNE\s*;", cli_source)
    if len(builders) != 1 or (query is not None and not re.search(r"Command\s*=\s*BC250_ESCAPE_RUN_DPM_TUNE\s*;", query)):
        found.append(f"bc250kmd_cli.c: {len(builders)} RUN_DPM_TUNE requests built, expected one, in TuneQuery")
    return found


def dpm_request_problems(display_source, dpm_source, cli_source):
    """KMD 0.7.206.2 BC250_ESCAPE_RUN_DPM: ABI 2 (192 bytes, the idle state's setting and counters) beside the
    unchanged ABI 1 prefix (160 bytes). The same discipline RUN_DPM_TUNE has had since 0.7.197: the driver admits
    exactly the two sizes, pairs each with its own AbiVersion, hands the size on, and touches an ABI 2 field only
    inside an `if (abi2)` block - an unguarded one writes past the end of a 160-byte request, which is a kernel pool
    write out of bounds from an escape any caller may send. The CLI asks with ABI 2 and repeats with ABI 1, and
    prints the ABI 2 fields only when the answer carries them. Returns a list of sentences, empty when all of that
    holds."""
    found = []
    body = function_body(dpm_source, "DpmRequest")
    if body is None:
        return ["dpm.c: DpmRequest not found"]
    if not re.search(r"expectedFlags\.NoAdapterSynchronization\s*=\s*1\s*;", body) or \
       not re.search(r"EscapeFlags\s*!=\s*expectedFlags\.Value\)\s*return\s*;", body):
        found.append("dpm.c: DpmRequest does not refuse every flag word but {NoAdapterSynchronization}")
    if not re.search(r"abi2\s*=\s*abi\s*==\s*BC250_DPM_ABI\s*&&\s*Size\s*==\s*sizeof\(BC250_ESCAPE_DPM\)", body) or \
       not re.search(r"abi1\s*=\s*abi\s*==\s*BC250_DPM_ABI_1\s*&&\s*Size\s*==\s*BC250_DPM_ABI1_SIZE", body) or \
       not re.search(r"if\s*\(\s*!\(abi1\s*\|\|\s*abi2\)", body):
        found.append("dpm.c: DpmRequest does not pair AbiVersion with the escape's size")
    if not re.search(r"if\s*\(\s*confirm\s*&&\s*!Admin\s*\)", body):
        found.append("dpm.c: DpmRequest does not refuse a non-administrator's CONFIRM")
    if not re.search(r"ExAcquireRundownProtection\(&Device->StartHealth\.Readers\)", body):
        found.append("dpm.c: DpmRequest takes no rundown protection on the adapter context")
    # Every ABI 2 field, in or out, inside an `if (abi2) { ... }` block. The blocks hold no nested braces today.
    guarded = [m.span() for m in re.finditer(r"if\s*\(abi2\)\s*\{[^{}]*\}", body)]
    if not guarded:
        found.append("dpm.c: DpmRequest has no if (abi2) block")
    for m in re.finditer(r"Data->(IdleMHz|IdleHoldMs|IdleBusyPermille|IdleEntries|IdleExits|IdleRefusals|IdleMs)",
                         body):
        if not any(a <= m.start() < b for a, b in guarded):
            found.append(f"dpm.c: DpmRequest touches Data->{m.group(1)} outside an if (abi2) block")
            break
    dispatch = re.search(r"if\s*\(\s*data->Command\s*==\s*BC250_ESCAPE_RUN_DPM\s*\)\s*\{(.*?)\n    \}",
                         display_source, re.S)
    if not dispatch:
        found.append("display.c: no RUN_DPM dispatch")
    elif not re.search(r"PrivateDriverDataSize\s*!=\s*sizeof\(BC250_ESCAPE_DPM\)\s*&&\s*"
                       r"Escape->PrivateDriverDataSize\s*!=\s*BC250_DPM_ABI1_SIZE\)", dispatch.group(1)) or \
            not re.search(r"DpmRequest\(device,[^;]*Escape->PrivateDriverDataSize,", dispatch.group(1)):
        found.append("display.c: RUN_DPM dispatch without the exact size check or without handing the size on")
    # The CLI: one request builder, the ABI it asks with, the retry, and the guard before it reads ABI 2 fields.
    query = function_body(cli_source, "DpmQuery")
    if query is None:
        found.append("bc250kmd_cli.c: DpmQuery not found")
    else:
        if not re.search(r"size\s*=\s*g_DpmAbi\s*==\s*BC250_DPM_ABI\s*\?\s*\(unsigned\)sizeof\(\*d\)\s*:\s*"
                         r"BC250_DPM_ABI1_SIZE\s*;", query) or \
           not re.search(r"g_DpmAbi\s*=\s*BC250_DPM_ABI_1\s*;", query):
            found.append("bc250kmd_cli.c: DpmQuery does not send each ABI with its own size, or never falls back")
        if not re.search(r"SendEscapeFlags\(BC250_DEFAULT_HWID,\s*d,\s*size,\s*1,", query):
            found.append("bc250kmd_cli.c: DpmQuery does not send with NoAdapterSynchronization alone")
    idle = function_body(cli_source, "DpmPrintIdle")
    if idle is None:
        found.append("bc250kmd_cli.c: DpmPrintIdle not found")
    elif not re.search(r"if\s*\(d->AbiVersion\s*!=\s*BC250_DPM_ABI\)\s*\{", idle):
        found.append("bc250kmd_cli.c: DpmPrintIdle reads the ABI 2 fields without checking the answer's AbiVersion")
    builders = re.findall(r"Command\s*=\s*BC250_ESCAPE_RUN_DPM\s*;", cli_source)
    if len(builders) != 1 or (query is not None and not re.search(r"Command\s*=\s*BC250_ESCAPE_RUN_DPM\s*;", query)):
        found.append(f"bc250kmd_cli.c: {len(builders)} RUN_DPM requests built, expected one, in DpmQuery")
    return found


class DpmRequestAbiTest(unittest.TestCase):
    def test_driver_and_cli(self):
        self.assertEqual(dpm_request_problems(read(DISPLAY), read(DPM), read(CLI)), [])

    def test_negative_controls(self):
        display, dpm, cli = read(DISPLAY), read(DPM), read(CLI)
        request_at = dpm.find("void DpmRequest(")
        self.assertGreater(request_at, 0)
        mutations = [
            # An ABI 2 field written for every caller: 32 bytes past a 160-byte request.
            (display, dpm.replace("    Data->BusyPermille = Data->BusyAvgPermille = 0;\n",
                                  "    Data->BusyPermille = Data->BusyAvgPermille = 0;\n"
                                  "    Data->IdleMHz = 0;\n", 1), cli),
            # The ABI 1 size admitted without its pairing to AbiVersion 1.
            (display, dpm.replace("abi == BC250_DPM_ABI_1 && Size == BC250_DPM_ABI1_SIZE",
                                  "abi == BC250_DPM_ABI_1", 1), cli),
            # The flag word loosened in DpmRequest (DpmTuneRequest keeps the same line below it).
            (display, dpm[:request_at] + dpm[request_at:].replace("EscapeFlags != expectedFlags.Value) return;",
                                                                  "FALSE) return;", 1), cli),
            # The administrator check on CONFIRM dropped.
            (display, dpm.replace("if (confirm && !Admin)", "if (FALSE)", 1), cli),
            # The dispatch admitting any size at or above the ABI 1 prefix.
            (display.replace("Escape->PrivateDriverDataSize != BC250_DPM_ABI1_SIZE) return STATUS_INVALID_PARAMETER;",
                             "Escape->PrivateDriverDataSize < BC250_DPM_ABI1_SIZE) return STATUS_INVALID_PARAMETER;",
                             1), dpm, cli),
            # The CLI asking with ABI 2's size for an ABI 1 request.
            (display, dpm, cli.replace("size = g_DpmAbi == BC250_DPM_ABI ? (unsigned)sizeof(*d) : BC250_DPM_ABI1_SIZE;",
                                       "size = (unsigned)sizeof(*d);", 1)),
            # The CLI printing the ABI 2 fields whatever the driver answered.
            (display, dpm, cli.replace("    if (d->AbiVersion != BC250_DPM_ABI) {", "    if (0) {", 1)),
        ]
        for i, (d, k, c) in enumerate(mutations):
            with self.subTest(mutation=i):
                self.assertTrue((d, k, c) != (display, dpm, cli), "mutation did not apply")
                self.assertTrue(dpm_request_problems(d, k, c))


class DpmTuneFlagsTest(unittest.TestCase):
    def test_driver_and_cli(self):
        self.assertEqual(dpm_tune_problems(read(DISPLAY), read(DPM), read(CLI)), [])

    def test_negative_controls(self):
        display, dpm, cli = read(DISPLAY), read(DPM), read(CLI)
        tune_at = dpm.find("void DpmTuneRequest(")
        self.assertGreater(tune_at, 0)
        mutations = [
            # The flag word loosened, in DpmTuneRequest (DpmRequest has the same line above it).
            (display, dpm[:tune_at] + dpm[tune_at:].replace("EscapeFlags != expectedFlags.Value) return;",
                                                            "FALSE) return;", 1), cli),
            # The administrator check dropped.
            (display, re.sub(r"if \(write && !Admin\)", "if (FALSE)", dpm, count=1), cli),
            # The dispatch moved behind the refusal: drop it, as a stand-in.
            (display.replace("command == BC250_ESCAPE_RUN_DPM_TUNE", "command == 0xFFFFFFFFu", 1), dpm, cli),
            # The CLI sends with HardwareAccess.
            (display, dpm, cli.replace("SendEscapeFlags(BC250_DEFAULT_HWID, t, size, 1,",
                                       "SendEscapeFlags(BC250_DEFAULT_HWID, t, size, 0,", 1)),
            # 0.7.197.1: the ABI 1 size admitted without its pairing to AbiVersion 1.
            (display, dpm.replace("abi == BC250_DPM_TUNE_ABI_1 && Size == BC250_DPM_TUNE_ABI1_SIZE",
                                  "abi == BC250_DPM_TUNE_ABI_1", 1), cli),
            # An ABI 2 field written for every caller, the ABI 1 size included.
            (display, dpm.replace("    Data->FloorTicks = Data->Generation = 0;\n",
                                  "    Data->FloorTicks = Data->Generation = 0;\n    Data->HotStepMs = 0;\n", 1), cli),
            # The dispatch admitting any size.
            (display.replace("Escape->PrivateDriverDataSize != BC250_DPM_TUNE_ABI1_SIZE)",
                             "Escape->PrivateDriverDataSize < BC250_DPM_TUNE_ABI1_SIZE)", 1), dpm, cli),
        ]
        for i, (d, k, c) in enumerate(mutations):
            with self.subTest(mutation=i):
                self.assertTrue((d, k, c) != (display, dpm, cli), "mutation did not apply")
                self.assertTrue(dpm_tune_problems(d, k, c))


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
