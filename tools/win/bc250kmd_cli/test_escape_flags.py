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

From 0.7.213.1 the four escapes that had no gate here get one, as a table of what every operation admits and what it
refuses (FlagContractTest): BC250_ESCAPE_RUN_HWMON (27), BC250_ESCAPE_RUN_DPM_CURVE (28), BC250_ESCAPE_RUN_CPU (29)
and BC250_ESCAPE_RUN_START_HEALTH (21). BC250_ESCAPE_RUN_FAN (30), the case fan control, has the plain software
gate for every operation: its writes leave a request for the governor thread and touch no port. The product rule behind the table: no shipped component sends HardwareAccess
on a repeating schedule, and no HardwareAccess escape holds the GPU scheduler across a registry flush. So
RUN_START_HEALTH's CONFIRM and RUN_CPU's KEEP, which write the registry and touch no register, moved to
NoAdapterSynchronization and keep their old word admitted for one release; RUN_CPU's other writes send mailbox
messages and stay HardwareAccess. The schedule rule has no exception left: the last one was the overlay's graphics
panel, which polled `log summary only` every 5 s and now reads the ring with `log 0` and asks for a summary only
when an operator does (tools/win/bc250mon/src/GraphicsPipelineProvider.cs, docs/design/paging-journal.md). The client
half of that move is pinned here as well (LEGACY_RETRIES): a driver up to 0.7.212 refuses the new word, so every
sender of those two operations sends the request once more with the old word and remembers it for the process, which
is what keeps a start confirmed and a CPU trial kept while a release defers the device restart (C57). The behavioural
half of the RUN_START_HEALTH contract, every operation against
every flag word, is in driver/kmd/test/start_health_test.c, which builds the actual handler.

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
        # 0.7.197.1, 0.7.213: three exact sizes, ABI 3 and the ABI 2 and ABI 1 prefixes, and the size handed on to
        # DpmTuneRequest.
        if not re.search(r"PrivateDriverDataSize\s*!=\s*sizeof\(BC250_ESCAPE_DPM_TUNE\)\s*&&\s*"
                         r"Escape->PrivateDriverDataSize\s*!=\s*BC250_DPM_TUNE_ABI2_SIZE\s*&&\s*"
                         r"Escape->PrivateDriverDataSize\s*!=\s*BC250_DPM_TUNE_ABI1_SIZE\)", dispatch.group(1)) or \
           "Escape->Flags.Value" not in dispatch.group(1) or \
           not re.search(r"DpmTuneRequest\(device,[^;]*Escape->PrivateDriverDataSize,", dispatch.group(1)):
            found.append("display.c: RUN_DPM_TUNE dispatch without the exact size check or the flag word")
    # A shorter size must never see a longer ABI's field: each one is read or written only inside the `if` block of its
    # own ABI, and abi3 / abi2 / abi1 pair each AbiVersion with its own size. abi2 is true for an ABI 3 request too,
    # because ABI 3 contains ABI 2's fields; abi3 is the one that is not.
    if not re.search(r"abi3\s*=\s*abi\s*==\s*BC250_DPM_TUNE_ABI\s*&&\s*Size\s*==\s*sizeof\(BC250_ESCAPE_DPM_TUNE\)", body) or \
       not re.search(r"abi2\s*=\s*\(abi\s*==\s*BC250_DPM_TUNE_ABI_2\s*&&\s*Size\s*==\s*BC250_DPM_TUNE_ABI2_SIZE\)"
                     r"\s*\|\|\s*abi3", body) or \
       not re.search(r"abi1\s*=\s*abi\s*==\s*BC250_DPM_TUNE_ABI_1\s*&&\s*Size\s*==\s*BC250_DPM_TUNE_ABI1_SIZE", body) or \
       not re.search(r"if\s*\(\s*!\(abi1\s*\|\|\s*abi2\)", body):
        found.append("dpm.c: DpmTuneRequest does not pair AbiVersion with the escape's size")
    guarded = [m.span() for m in re.finditer(r"if\s*\(abi2\)\s*\{[^{}]*\}", body)]
    for m in re.finditer(r"Data->(HotStepMs|SoftReleaseDeltaMc|SoftReleaseStepMs|DefaultHotStepMs|"
                         r"DefaultSoftReleaseDeltaMc|DefaultSoftReleaseStepMs|Reserved2)", body):
        if not any(a <= m.start() < b for a, b in guarded):
            found.append(f"dpm.c: DpmTuneRequest touches Data->{m.group(1)} outside an if (abi2) block")
            break
    guarded3 = [m.span() for m in re.finditer(r"if\s*\(abi3\)\s*\{[^{}]*\}", body)]
    if not guarded3:
        found.append("dpm.c: DpmTuneRequest has no if (abi3) block")
    for m in re.finditer(r"Data->(ZoneDeltaMc|ZoneStepMs|ZoneLeadMs|DefaultZoneDeltaMc|DefaultZoneStepMs|"
                         r"DefaultZoneLeadMs|ZoneSlopeMs|Reserved3)", body):
        if not any(a <= m.start() < b for a, b in guarded3):
            found.append(f"dpm.c: DpmTuneRequest touches Data->{m.group(1)} outside an if (abi3) block")
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
    """KMD 0.7.207.1 BC250_ESCAPE_RUN_DPM: ABI 2 (192 bytes, the idle state's setting and counters) beside the
    unchanged ABI 1 prefix (160 bytes). The same discipline RUN_DPM_TUNE has had since 0.7.197: the driver admits
    exactly the two sizes, pairs each with its own AbiVersion, hands the size on, and touches an ABI 2 field only
    inside an `if (abi2)` block - an unguarded one writes past the end of a 160-byte request, which is a kernel pool
    write out of bounds from an escape any caller may send. The CLI asks with ABI 2 and repeats with ABI 1, and
    prints the ABI 2 fields only when the answer carries them. Returns a list of sentences, empty when all of that
    holds.

    KMD 0.7.215.1 adds ABI 3 (248 bytes: the ABI 2 structure and the SMU metrics tail). abi2 is also true for ABI 3,
    which contains the ABI 2 fields, and the tail is touched only inside `if (abi3)` statements. The CLI asks with
    ABI 3 first and falls back to ABI 2, then ABI 1; the export takes all three lengths."""
    found = []
    body = function_body(dpm_source, "DpmRequest")
    if body is None:
        return ["dpm.c: DpmRequest not found"]
    if not re.search(r"expectedFlags\.NoAdapterSynchronization\s*=\s*1\s*;", body) or \
       not re.search(r"EscapeFlags\s*!=\s*expectedFlags\.Value\)\s*return\s*;", body):
        found.append("dpm.c: DpmRequest does not refuse every flag word but {NoAdapterSynchronization}")
    if not re.search(r"abi3\s*=\s*abi\s*==\s*BC250_DPM_ABI_3\s*&&\s*Size\s*==\s*sizeof\(BC250_ESCAPE_DPM_EX\)", body) or \
       not re.search(r"abi2\s*=\s*\(abi\s*==\s*BC250_DPM_ABI\s*&&\s*Size\s*==\s*sizeof\(BC250_ESCAPE_DPM\)\)\s*\|\|\s*abi3\s*;",
                     body) or \
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
    # The ABI 3 tail, through ex, only inside `if (abi3)`: a one-line statement or a block without nested braces.
    tail = [m.span() for m in re.finditer(r"if\s*\(abi3\)\s*(\{[^{}]*\}|[^;{}]*;)", body)]
    if not tail:
        found.append("dpm.c: DpmRequest has no if (abi3) block")
    for m in re.finditer(r"ex->Metrics", body):
        if not any(a <= m.start() < b for a, b in tail):
            found.append("dpm.c: DpmRequest touches ex->Metrics outside an if (abi3) block")
            break
    dispatch = re.search(r"if\s*\(\s*data->Command\s*==\s*BC250_ESCAPE_RUN_DPM\s*\)\s*\{(.*?)\n    \}",
                         display_source, re.S)
    if not dispatch:
        found.append("display.c: no RUN_DPM dispatch")
    elif not re.search(r"PrivateDriverDataSize\s*!=\s*BC250_DPM_ABI3_SIZE\s*&&\s*"
                       r"Escape->PrivateDriverDataSize\s*!=\s*sizeof\(BC250_ESCAPE_DPM\)\s*&&\s*"
                       r"Escape->PrivateDriverDataSize\s*!=\s*BC250_DPM_ABI1_SIZE\)", dispatch.group(1)) or \
            not re.search(r"DpmRequest\(device,[^;]*Escape->PrivateDriverDataSize,", dispatch.group(1)):
        found.append("display.c: RUN_DPM dispatch without the exact size check or without handing the size on")
    # The CLI: one request builder, the ABI it asks with, the retry, and the guard before it reads ABI 2 fields.
    query = function_body(cli_source, "DpmQuery")
    if query is None:
        found.append("bc250kmd_cli.c: DpmQuery not found")
    else:
        if not re.search(r"size\s*=\s*g_DpmAbi\s*==\s*BC250_DPM_ABI_3\s*\?\s*\(unsigned\)sizeof\(\*x\)\s*:\s*"
                         r"g_DpmAbi\s*==\s*BC250_DPM_ABI\s*\?\s*\(unsigned\)sizeof\(\*d\)\s*:\s*"
                         r"BC250_DPM_ABI1_SIZE\s*;", query) or \
           not re.search(r"g_DpmAbi\s*=\s*g_DpmAbi\s*==\s*BC250_DPM_ABI_3\s*\?\s*BC250_DPM_ABI\s*:\s*"
                         r"BC250_DPM_ABI_1\s*;", query):
            found.append("bc250kmd_cli.c: DpmQuery does not send each ABI with its own size, or never falls back")
        if not re.search(r"SendEscapeFlags\(BC250_DEFAULT_HWID,\s*d,\s*size,\s*1,", query):
            found.append("bc250kmd_cli.c: DpmQuery does not send with NoAdapterSynchronization alone")
    idle = function_body(cli_source, "DpmPrintIdle")
    if idle is None:
        found.append("bc250kmd_cli.c: DpmPrintIdle not found")
    elif not re.search(r"if\s*\(d->AbiVersion\s*!=\s*BC250_DPM_ABI\s*&&\s*d->AbiVersion\s*!=\s*BC250_DPM_ABI_3\)\s*\{",
                       idle):
        found.append("bc250kmd_cli.c: DpmPrintIdle reads the ABI 2 fields without checking the answer's AbiVersion")
    # Bc250Dpm, the export bc250mon and the control application call through bc250control.dll. Its callers were
    # built against the ABI 1 layout and pass 160 bytes, so the ABI it asks with is the one its caller's length
    # names, and it must zero and send that length and no more. A check against sizeof(BC250_ESCAPE_DPM) alone
    # would refuse every deployed caller and stop the DLL compiling.
    export = function_body(cli_source, "Bc250Dpm")
    if export is None:
        found.append("bc250kmd_cli.c: Bc250Dpm not found")
    else:
        if not re.search(r"bytes\s*!=\s*BC250_DPM_ABI1_SIZE\s*&&\s*bytes\s*!=\s*sizeof\(\*data\)\s*&&\s*"
                         r"bytes\s*!=\s*BC250_DPM_ABI3_SIZE\)+\s*return", export):
            found.append("bc250kmd_cli.c: Bc250Dpm does not admit exactly the ABI 1 prefix, ABI 2 and ABI 3")
        if not re.search(r"abi\s*=\s*bytes\s*==\s*BC250_DPM_ABI1_SIZE\s*\?\s*BC250_DPM_ABI_1\s*:\s*"
                         r"bytes\s*==\s*BC250_DPM_ABI3_SIZE\s*\?\s*BC250_DPM_ABI_3\s*:\s*BC250_DPM_ABI\s*;", export):
            found.append("bc250kmd_cli.c: Bc250Dpm does not pair the caller's length with its AbiVersion")
        if not re.search(r"memset\(data,\s*0,\s*bytes\)\s*;", export) or \
           not re.search(r"TelemetryEscape\(data,\s*bytes\)\s*;", export):
            found.append("bc250kmd_cli.c: Bc250Dpm writes or sends more than the caller's length")
        if not re.search(r"data->AbiVersion\s*!=\s*abi\s*\|\|", export):
            found.append("bc250kmd_cli.c: Bc250Dpm does not check the answer's AbiVersion against the one it asked with")
    # Exactly two request builders, and each in its own function: the CLI command and the DLL export.
    builders = re.findall(r"Command\s*=\s*BC250_ESCAPE_RUN_DPM\s*;", cli_source)
    in_both = query is not None and export is not None and \
        re.search(r"Command\s*=\s*BC250_ESCAPE_RUN_DPM\s*;", query) and \
        re.search(r"Command\s*=\s*BC250_ESCAPE_RUN_DPM\s*;", export)
    if len(builders) != 2 or not in_both:
        found.append(f"bc250kmd_cli.c: {len(builders)} RUN_DPM requests built, expected two, in DpmQuery and Bc250Dpm")
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
            (display, dpm, cli.replace("g_DpmAbi == BC250_DPM_ABI ? (unsigned)sizeof(*d) : BC250_DPM_ABI1_SIZE;",
                                       "(unsigned)sizeof(*d);", 1)),
            # The CLI printing the ABI 2 fields whatever the driver answered.
            (display, dpm, cli.replace("    if (d->AbiVersion != BC250_DPM_ABI && d->AbiVersion != BC250_DPM_ABI_3) {",
                                       "    if (0) {", 1)),
            # The export asking with ABI 2 for a 160-byte caller, which reads 32 bytes of its stack.
            (display, dpm, cli.replace("    abi = bytes == BC250_DPM_ABI1_SIZE ? BC250_DPM_ABI_1 : "
                                       "bytes == BC250_DPM_ABI3_SIZE ? BC250_DPM_ABI_3 : BC250_DPM_ABI;",
                                       "    abi = BC250_DPM_ABI;", 1)),
            # The ABI 3 tail written for every caller: 56 bytes past a 192-byte request.
            (display, dpm.replace("    if (abi3) RtlZeroMemory(&ex->Metrics, sizeof(ex->Metrics));\n",
                                  "    RtlZeroMemory(&ex->Metrics, sizeof(ex->Metrics));\n", 1), cli),
            # ABI 3 admitted without its pairing to the 248-byte size.
            (display, dpm.replace("abi == BC250_DPM_ABI_3 && Size == sizeof(BC250_ESCAPE_DPM_EX)",
                                  "abi == BC250_DPM_ABI_3", 1), cli),
            # The dispatch without the ABI 3 size: every ABI 3 caller refused.
            (display.replace("Escape->PrivateDriverDataSize != BC250_DPM_ABI3_SIZE &&\n", "", 1), dpm, cli),
            # The CLI never falling back from ABI 3, so a driver before 0.7.215 answers nothing.
            (display, dpm, cli.replace("g_DpmAbi = g_DpmAbi == BC250_DPM_ABI_3 ? BC250_DPM_ABI : BC250_DPM_ABI_1;",
                                       "g_DpmAbi = BC250_DPM_ABI_1;", 1)),
            # The export zeroing the whole structure in a 160-byte caller's buffer.
            (display, dpm, cli.replace("    memset(data, 0, bytes);", "    memset(data, 0, sizeof(*data));", 1)),
            # The export back to one size, which refuses every deployed caller.
            (display, dpm, cli.replace("if (!data || (bytes != BC250_DPM_ABI1_SIZE && bytes != sizeof(*data) && "
                                       "bytes != BC250_DPM_ABI3_SIZE))", "if (!data || bytes != sizeof(*data))", 1)),
            # A third request builder, outside both functions.
            (display, dpm, cli.replace("static void DpmPrint(const BC250_ESCAPE_DPM *d)",
                                       "static void DpmStray(BC250_ESCAPE_DPM *d)\n"
                                       "{\n    d->Command = BC250_ESCAPE_RUN_DPM;\n}\n"
                                       "static void DpmPrint(const BC250_ESCAPE_DPM *d)", 1)),
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
            # 0.7.213: an ABI 3 field written for every caller, which runs 32 bytes past a 152-byte request.
            (display, dpm.replace("    Data->FloorTicks = Data->Generation = 0;\n",
                                  "    Data->FloorTicks = Data->Generation = 0;\n    Data->ZoneDeltaMc = 0;\n", 1), cli),
            # 0.7.213: the ABI 2 size admitted without its pairing to AbiVersion 2.
            (display, dpm.replace("abi == BC250_DPM_TUNE_ABI_2 && Size == BC250_DPM_TUNE_ABI2_SIZE",
                                  "abi == BC250_DPM_TUNE_ABI_2", 1), cli),
            # The dispatch admitting any size.
            (display.replace("Escape->PrivateDriverDataSize != BC250_DPM_TUNE_ABI1_SIZE)",
                             "Escape->PrivateDriverDataSize < BC250_DPM_TUNE_ABI1_SIZE)", 1), dpm, cli),
            # 0.7.213: the dispatch dropping the ABI 2 size, which refuses every tool built before this ABI.
            (display.replace("            Escape->PrivateDriverDataSize != BC250_DPM_TUNE_ABI2_SIZE &&\n", "", 1), dpm, cli),
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


# ---- the flag contract of commands 21, 27, 28 and 29 (0.7.213.1) ------------------------------------------------
#
# One table of what every operation of these four escapes admits, and therefore of what it refuses. The driver's
# gate is read out of its own source: each shape below is matched exactly, so the admitted set follows from the
# text and not from a second copy of the rule. A gate this test does not recognise is a failure, not a pass.

START_HEALTH = os.path.join(HERE, "..", "..", "..", "driver", "kmd", "start_health.c")
CPU = os.path.join(HERE, "..", "..", "..", "driver", "kmd", "cpu.c")
HWMON = os.path.join(HERE, "..", "..", "..", "driver", "kmd", "hwmon.c")
FAN = os.path.join(HERE, "..", "..", "..", "driver", "kmd", "fan.c")

HARDWARE = 1        # D3DDDI_ESCAPEFLAGS.HardwareAccess
NO_SYNC = 8         # D3DDDI_ESCAPEFLAGS.NoAdapterSynchronization
# Every flag word worth asking about: the two the driver uses, their union, the bits in between, and none at all.
FLAG_WORDS = (0, 1, 2, 3, 4, 8, 9, 10, 12, 16, 24)

# command -> operation -> the flag words admitted. Everything else in FLAG_WORDS is refused at the gate.
# CONFIRM of RUN_START_HEALTH and KEEP of RUN_CPU write the registry and touch no register, so they take
# NoAdapterSynchronization; both keep the HardwareAccess word of 0.7.212 admitted for one release, so that an
# older CLI, DLL or overlay still works against this driver.
FAN_OPS = ("READ", "BOARD", "CURVE", "FIXED", "RENEW")
CPU_MAILBOX_OPS = ("READBACK", "SET", "CANCEL", "RESET", "CORES", "SEARCH_BEGIN", "SEARCH_STEP")
FLAG_CONTRACT = {
    "BC250_ESCAPE_RUN_START_HEALTH": {"READ": {NO_SYNC}, "CONFIRM": {NO_SYNC, HARDWARE}},
    "BC250_ESCAPE_RUN_HWMON": {"READ": {NO_SYNC}},
    "BC250_ESCAPE_RUN_DPM_CURVE": dict.fromkeys(("READ", "SET", "KEEP", "CANCEL", "RESET"), {NO_SYNC}),
    "BC250_ESCAPE_RUN_CPU": dict({"READ": {NO_SYNC}, "KEEP": {NO_SYNC, HARDWARE}},
                                 **dict.fromkeys(CPU_MAILBOX_OPS, {HARDWARE})),
    "BC250_ESCAPE_RUN_FAN": dict.fromkeys(FAN_OPS, {NO_SYNC}),
}


def one_flag_word_gate(body, operations):
    """The admitted sets of a handler whose gate is the plain one: expectedFlags with NoAdapterSynchronization its
    only field, and any other flag word refused before anything is touched. None when that is not the shape."""
    if set(re.findall(r"expected\w*\.(\w+)\s*=\s*1\s*;", body)) != {"NoAdapterSynchronization"}:
        return None
    if not re.search(r"EscapeFlags\s*!=\s*expectedFlags\.Value\)\s*return\s*;", body):
        return None
    if len(re.findall(r"\bEscapeFlags\b", body)) != 1:      # no second, looser comparison
        return None
    return dict.fromkeys(operations, {NO_SYNC})


def start_health_gate(body):
    """RUN_START_HEALTH: NoAdapterSynchronization for both operations, and the HardwareAccess word of 0.7.212 for
    CONFIRM alone, for one release."""
    if not re.search(r"BOOLEAN\s+confirm\s*=\s*Data->Op\s*==\s*BC250_START_HEALTH_CONFIRM\s*;", body):
        return None
    if not re.search(r"expectedFlags\.NoAdapterSynchronization\s*=\s*1\s*;", body) or \
       not re.search(r"legacyFlags\.HardwareAccess\s*=\s*1\s*;", body) or \
       re.search(r"expectedFlags\.HardwareAccess\s*=", body):
        return None
    if not re.search(r"\(EscapeFlags!=expectedFlags\.Value\s*&&\s*"
                     r"!\(confirm\s*&&\s*EscapeFlags==legacyFlags\.Value\)\)\)\s*return\s*;", body):
        return None
    if len(re.findall(r"\bEscapeFlags\b", body)) != 2:
        return None
    return {"READ": {NO_SYNC}, "CONFIRM": {NO_SYNC, HARDWARE}}


def cpu_gate(body):
    """RUN_CPU: READ software only, KEEP software (with the old word for one release), every mailbox operation
    HardwareAccess alone."""
    if not re.search(r"write\s*=\s*op\s*!=\s*BC250_CPU_OP_READ\s*;", body) or \
       not re.search(r"keep\s*=\s*op\s*==\s*BC250_CPU_OP_KEEP\s*;", body):
        return None
    if not re.search(r"expectedRead\.NoAdapterSynchronization\s*=\s*1\s*;", body) or \
       not re.search(r"expectedWrite\.HardwareAccess\s*=\s*1\s*;", body):
        return None
    if not re.search(r"\(!write\s*&&\s*EscapeFlags\s*!=\s*expectedRead\.Value\)\s*\|\|\s*"
                     r"\(write\s*&&\s*!keep\s*&&\s*EscapeFlags\s*!=\s*expectedWrite\.Value\)\s*\|\|\s*"
                     r"\(keep\s*&&\s*EscapeFlags\s*!=\s*expectedRead\.Value\s*&&\s*"
                     r"EscapeFlags\s*!=\s*expectedWrite\.Value\)\)\s*return\s*;", body):
        return None
    if len(re.findall(r"\bEscapeFlags\b", body)) != 4:
        return None
    return dict({"READ": {NO_SYNC}, "KEEP": {NO_SYNC, HARDWARE}},
                **dict.fromkeys(CPU_MAILBOX_OPS, {HARDWARE}))


def driver_flag_contract(start_health_source, cpu_source, hwmon_source, dpm_source, fan_source=None):
    """What the five handlers admit, read out of their own gates: command -> operation -> flag words."""
    curve_ops = FLAG_CONTRACT["BC250_ESCAPE_RUN_DPM_CURVE"].keys()
    if fan_source is None:
        fan_source = read(FAN)
    gates = {
        "BC250_ESCAPE_RUN_START_HEALTH": (start_health_source, "StartHealthRequest", start_health_gate),
        "BC250_ESCAPE_RUN_HWMON": (hwmon_source, "HwmonRequest",
                                   lambda body: one_flag_word_gate(body, ("READ",))),
        "BC250_ESCAPE_RUN_DPM_CURVE": (dpm_source, "DpmCurveRequest",
                                       lambda body: one_flag_word_gate(body, curve_ops)),
        "BC250_ESCAPE_RUN_CPU": (cpu_source, "CpuRequest", cpu_gate),
        "BC250_ESCAPE_RUN_FAN": (fan_source, "FanRequest", lambda body: one_flag_word_gate(body, FAN_OPS)),
    }
    contract = {}
    for command, (source, name, gate) in gates.items():
        body = function_body(source, name)
        contract[command] = None if body is None else gate(body)
    return contract


def flag_matrix(contract):
    """The contract as the thing it decides: (command, operation, flag word) -> admitted. A command whose gate was
    not recognised answers None for every cell, which no expectation matches."""
    matrix = {}
    for command, operations in contract.items():
        for operation in FLAG_CONTRACT[command]:
            for word in FLAG_WORDS:
                matrix[(command, operation, word)] = \
                    None if operations is None else word in operations.get(operation, set())
    return matrix


# The two operations that moved to NoAdapterSynchronization in 0.7.213 are the two a client must still get through
# against the loaded 0.7.212 driver, because the installer defers the device restart: start health CONFIRM (the logon
# task, the overlay, the control application's Recovery action) and CPU KEEP (a trial that would otherwise be lost).
# A driver up to 0.7.212 answers the new word with Status REFUSED and NtStatus STATUS_INVALID_PARAMETER, which a
# well-formed request of this release gets from nothing else, so each of the three senders of those two operations
# must send the request once more with the old HardwareAccess word and must remember the answer for the process.
# Without this the start-confirm.ps1 fallback writes the boot-loop guard alone and the next start comes up at 24 CU
# and the floor clock (C57). The first attempt's flag argument is pinned by `senders` above; these patterns pin the
# fallback, so that deleting it is a test failure and not a silent regression.
LEGACY_RETRIES = {
    "Bc250StartHealth": (
        ("sticky old-word CONFIRM", r"if\(confirm && confirmLegacy\)\s*\{\s*"
                                    r"if\(SendEscapeFlags\(BC250_DEFAULT_HWID,data,sizeof\(\*data\),0,&status\)\)"),
        ("refused-CONFIRM resend", r"if\(confirm && NT_SUCCESS\(status\) &&\s*"
                                   r"data->Status==BC250_ESCAPE_STATUS_REFUSED && data->NtStatus==0xC000000Dul\)\s*\{\s*"
                                   r"confirmLegacy=1;\s*\*data=request;\s*"
                                   r"if\(SendEscapeFlags\(BC250_DEFAULT_HWID,data,sizeof\(\*data\),0,&status\)\)"),
    ),
    "Bc250Cpu": (
        ("sticky old-word KEEP", r"if \(op == BC250_CPU_OP_KEEP && keepLegacy\)\s*\{\s*"
                                 r"status = TelemetryEscapeFlags\(data, sizeof\(\*data\), 1\);"),
        ("refused-KEEP resend", r"if \(op == BC250_CPU_OP_KEEP && NT_SUCCESS\(status\) &&\s*"
                                r"data->Status == BC250_ESCAPE_STATUS_REFUSED && "
                                r"data->NtStatus == 0xC000000Dul\)\s*\{\s*"
                                r"keepLegacy = 1;\s*\*data = sent;\s*"
                                r"status = TelemetryEscapeFlags\(data, sizeof\(\*data\), 1\);"),
    ),
    "CpuQuery": (
        ("sticky old-word KEEP", r"if \(op == BC250_CPU_OP_KEEP && keepLegacy\)\s*\{\s*"
                                 r"if \(SendEscapeFlags\(BC250_DEFAULT_HWID, c, sizeof\(\*c\), 0, &status\)\)"),
        ("refused-KEEP resend", r"if \(op == BC250_CPU_OP_KEEP && NT_SUCCESS\(status\) &&\s*"
                                r"c->Status == BC250_ESCAPE_STATUS_REFUSED && "
                                r"c->NtStatus == 0xC000000Dul\)\s*\{\s*"
                                r"keepLegacy = 1;\s*\*c = sent;\s*"
                                r"if \(SendEscapeFlags\(BC250_DEFAULT_HWID, c, sizeof\(\*c\), 0, &status\)\)"),
    ),
}


def client_flag_problems(display_source, cli_source):
    """display.c's dispatch of the four commands, and the flags bc250kmd_cli.c and bc250control.dll send for them.
    Returns a list of sentences, empty when all of it holds."""
    found = []
    # The three software snapshots are dispatched ahead of display.c's NoAdapterSynchronization refusal, with the
    # exact request size and the caller's flag word handed to the handler.
    refusal = display_source.find("data->Command!=BC250_ESCAPE_RUN_CLOCK")
    if refusal < 0:
        found.append("display.c: the NoAdapterSynchronization refusal is gone")
    for command, handler, size in (
            ("BC250_ESCAPE_RUN_START_HEALTH", "StartHealthRequest", "sizeof(BC250_ESCAPE_START_HEALTH)"),
            ("BC250_ESCAPE_RUN_HWMON", "HwmonRequest", "sizeof(BC250_ESCAPE_HWMON)"),
            ("BC250_ESCAPE_RUN_DPM_CURVE", "DpmCurveRequest", "sizeof(BC250_ESCAPE_DPM_CURVE)"),
            ("BC250_ESCAPE_RUN_FAN", "FanRequest", "sizeof(BC250_ESCAPE_FAN)")):
        at = re.search(r"==\s*" + command + r"\s*\)\s*\{(.*?)\n    \}", display_source, re.S)
        if not at:
            found.append(f"display.c: no {command} dispatch")
            continue
        if refusal >= 0 and at.start() > refusal:
            found.append(f"display.c: {command} is dispatched after the NoAdapterSynchronization refusal")
        if ("PrivateDriverDataSize != " + size) not in at.group(1) and \
           ("PrivateDriverDataSize!=" + size) not in at.group(1):
            found.append(f"display.c: {command} dispatch without its exact size check")
        if not re.search(handler + r"\(device,[^;]*Escape->Flags\.Value", at.group(1)):
            found.append(f"display.c: {command} dispatch does not hand the flag word to {handler}")
    # RUN_CPU sends mailbox messages for most operations, so it stays behind the Level Two gate and the
    # power-phase check, and is named in the exemption that lets its software operations through.
    cpu_at = display_source.find("data->Command == BC250_ESCAPE_RUN_CPU")
    if cpu_at < 0 or (refusal >= 0 and cpu_at < refusal):
        found.append("display.c: RUN_CPU is no longer dispatched behind the power-phase check")
    if not re.search(r"data->Command!=BC250_ESCAPE_RUN_CPU\s*&&\s*\n?\s*Escape->Flags\.NoAdapterSynchronization",
                     display_source):
        found.append("display.c: RUN_CPU is not exempt from the NoAdapterSynchronization refusal")
    # The CLI and the DLL: one sender per command, with the flags of the contract.
    senders = {
        # function, the number of RUN_* builders expected in the whole file, the flag argument it must pass
        "Bc250StartHealth": (r"SendEscapeFlags\(BC250_DEFAULT_HWID,data,sizeof\(\*data\),1,", "BC250_ESCAPE_RUN_START_HEALTH", 1),
        "Bc250Hwmon": (r"TelemetryEscape\(data, sizeof\(\*data\)\)", "BC250_ESCAPE_RUN_HWMON", 1),
        "Bc250Fan": (r"TelemetryEscape\(data, sizeof\(\*data\)\)", "BC250_ESCAPE_RUN_FAN", 1),
        "Bc250DpmCurve": (r"TelemetryEscape\(data, sizeof\(\*data\)\)", "BC250_ESCAPE_RUN_DPM_CURVE", 2),
        "CurveQuery": (r"SendEscapeFlags\(BC250_DEFAULT_HWID, c, sizeof\(\*c\), 1,", "BC250_ESCAPE_RUN_DPM_CURVE", 2),
        "Bc250Cpu": (r"TelemetryEscapeFlags\(data, sizeof\(\*data\),\s*op != BC250_CPU_OP_READ "
                     r"&& op != BC250_CPU_OP_KEEP\)", "BC250_ESCAPE_RUN_CPU", 2),
        "CpuQuery": (r"SendEscapeFlags\(BC250_DEFAULT_HWID, c, sizeof\(\*c\),\s*"
                     r"op == BC250_CPU_OP_READ \|\| op == BC250_CPU_OP_KEEP,", "BC250_ESCAPE_RUN_CPU", 2),
    }
    for name, (pattern, command, builders) in senders.items():
        body = function_body(cli_source, name)
        if body is None:
            found.append(f"bc250kmd_cli.c: {name} not found")
            continue
        if not re.search(r"Command\s*=\s*" + command + r"\s*;", body):
            found.append(f"bc250kmd_cli.c: {name} does not build a {command} request")
        if not re.search(pattern, body):
            found.append(f"bc250kmd_cli.c: {name} does not send {command} with the flags of the contract")
        seen = len(re.findall(r"Command\s*=\s*" + command + r"\s*;", cli_source))
        if seen != builders:
            found.append(f"bc250kmd_cli.c: {seen} {command} requests built, expected {builders}")
        for what, retry in LEGACY_RETRIES.get(name, ()):
            if not re.search(retry, body, re.S):
                found.append(f"bc250kmd_cli.c: {name} has no {what} for a driver of 0.7.212 or older")
    return found


class FlagContractTest(unittest.TestCase):
    def sources(self):
        return (read(START_HEALTH), read(CPU), read(HWMON), read(DPM))

    def test_gates_are_the_contract(self):
        self.assertEqual(driver_flag_contract(*self.sources()), FLAG_CONTRACT)

    def test_every_flag_word_of_every_operation(self):
        """The admitted and the refused combinations, one cell at a time, so a failure names the cell."""
        matrix = flag_matrix(driver_flag_contract(*self.sources()))
        for (command, operation, word), admitted in sorted(matrix.items()):
            with self.subTest(command=command, op=operation, flags=word):
                self.assertEqual(admitted, word in FLAG_CONTRACT[command][operation])

    def test_dispatch_and_clients(self):
        self.assertEqual(client_flag_problems(read(DISPLAY), read(CLI)), [])

    def test_negative_controls(self):
        health, cpu, hwmon, dpm = self.sources()
        display, cli = read(DISPLAY), read(CLI)
        gate_mutations = [
            # CONFIRM back to HardwareAccess only, as it was up to 0.7.212: a logon task would again suspend the
            # GPU scheduler every 5.5 s for two minutes.
            (health.replace("expectedFlags.NoAdapterSynchronization=1;",
                            "if (confirm) expectedFlags.HardwareAccess=1;\n"
                            "    else expectedFlags.NoAdapterSynchronization=1;", 1), cpu, hwmon, dpm),
            # The legacy word admitted for READ as well.
            (health.replace("!(confirm && EscapeFlags==legacyFlags.Value)",
                            "!(EscapeFlags==legacyFlags.Value)", 1), cpu, hwmon, dpm),
            # The gate loosened to "NoAdapterSynchronization set", which also admits 9, 10, 12 and 24.
            (health.replace("EscapeFlags!=expectedFlags.Value &&",
                            "!(EscapeFlags&expectedFlags.Value) &&", 1), cpu, hwmon, dpm),
            # KEEP back to HardwareAccess only: seven flushed registry writes with the scheduler suspended.
            (health, cpu.replace("(write && !keep && EscapeFlags != expectedWrite.Value) ||",
                                 "(write && EscapeFlags != expectedWrite.Value) ||", 1), hwmon, dpm),
            # A mailbox operation admitted without HardwareAccess.
            (health, cpu.replace("const BOOLEAN keep = op == BC250_CPU_OP_KEEP;",
                                 "const BOOLEAN keep = op != BC250_CPU_OP_READ;", 1), hwmon, dpm),
            # The hardware monitor's snapshot admitted with HardwareAccess, which idles the GPU per sample.
            (health, cpu, hwmon.replace("expectedFlags.NoAdapterSynchronization = 1;",
                                        "expectedFlags.HardwareAccess = 1;", 1), dpm),
            # The V/F curve's gate dropped.
            (health, cpu, hwmon, dpm.replace("op > BC250_DPM_CURVE_OP_RESET || Data->Reserved[0] || "
                                             "Data->Reserved[1] ||\n        EscapeFlags != expectedFlags.Value)",
                                             "op > BC250_DPM_CURVE_OP_RESET || Data->Reserved[0] || "
                                             "Data->Reserved[1])", 1)),
        ]
        fan = read(FAN)
        fan_mutations = [
            # The fan control's requests admitted with HardwareAccess: each one would idle the GPU, for a request
            # that only leaves a note for the governor thread.
            fan.replace("expectedFlags.NoAdapterSynchronization = 1;", "expectedFlags.HardwareAccess = 1;", 1),
            # Its gate dropped.
            fan.replace("store > 1u || EscapeFlags != expectedFlags.Value) return;", "store > 1u) return;", 1),
        ]
        for i, mutant in enumerate(fan_mutations):
            with self.subTest(fan_mutation=i):
                self.assertNotEqual(mutant, fan, "mutation did not apply")
                self.assertNotEqual(driver_flag_contract(health, cpu, hwmon, dpm, mutant), FLAG_CONTRACT)
        for i, sources in enumerate(gate_mutations):
            with self.subTest(gate_mutation=i):
                self.assertNotEqual(sources, (health, cpu, hwmon, dpm), "mutation did not apply")
                self.assertNotEqual(driver_flag_contract(*sources), FLAG_CONTRACT)
        client_mutations = [
            # The DLL sends CONFIRM with HardwareAccess again.
            (display, cli.replace("SendEscapeFlags(BC250_DEFAULT_HWID,data,sizeof(*data),1,",
                                  "SendEscapeFlags(BC250_DEFAULT_HWID,data,sizeof(*data),"
                                  "op!=BC250_START_HEALTH_CONFIRM,", 1)),
            # The DLL sends CPU KEEP with HardwareAccess again.
            (display, cli.replace("op != BC250_CPU_OP_READ && op != BC250_CPU_OP_KEEP)",
                                  "op != BC250_CPU_OP_READ)", 1)),
            # The CLI sends CPU KEEP with HardwareAccess again.
            (display, cli.replace("op == BC250_CPU_OP_READ || op == BC250_CPU_OP_KEEP,",
                                  "op == BC250_CPU_OP_READ,", 1)),
            # The three legacy-word fallbacks deleted, one at a time: a client of this release would then leave a
            # start unconfirmed, or lose a CPU trial, against the still-loaded 0.7.212 driver (C57).
            (display, cli.replace("            confirmLegacy=1;\n"
                                  "            *data=request;\n"
                                  "            if(SendEscapeFlags(BC250_DEFAULT_HWID,data,sizeof(*data),0,&status))"
                                  "return status;\n", "", 1)),
            (display, cli.replace("            keepLegacy = 1;\n"
                                  "            *data = sent;\n"
                                  "            status = TelemetryEscapeFlags(data, sizeof(*data), 1);\n", "", 1)),
            (display, cli.replace("            keepLegacy = 1;\n"
                                  "            *c = sent;\n"
                                  "            if (SendEscapeFlags(BC250_DEFAULT_HWID, c, sizeof(*c), 0, &status)) "
                                  "return 1;\n", "", 1)),
            # The fallback fires but is forgotten, so every later CONFIRM of the process is refused again.
            (display, cli.replace("            confirmLegacy=1;\n", "", 1)),
            # The refusal that triggers the resend widened to any refusal, which would also resend a well-formed
            # request that the new driver refused for a reason of its own.
            (display, cli.replace("data->Status==BC250_ESCAPE_STATUS_REFUSED && data->NtStatus==0xC000000Dul",
                                  "data->Status==BC250_ESCAPE_STATUS_REFUSED", 1)),
            # A second place builds a start-health request, which no flag rule would cover.
            (display, cli.replace("static int StartHealth(int argc,wchar_t** argv)",
                                  "static void HealthStray(BC250_ESCAPE_START_HEALTH* d)\n"
                                  "{\n    d->Command=BC250_ESCAPE_RUN_START_HEALTH;\n}\n"
                                  "static int StartHealth(int argc,wchar_t** argv)", 1)),
            # The hardware monitor dispatched behind the power-phase check, where a snapshot does not belong.
            (display.replace("command == BC250_ESCAPE_RUN_HWMON", "command == 0xFFFFFFFFu", 1), cli),
            # The fan control's dispatch gone, or without its exact size check.
            (display.replace("command == BC250_ESCAPE_RUN_FAN", "command == 0xFFFFFFFDu", 1), cli),
            (display.replace("PrivateDriverDataSize != sizeof(BC250_ESCAPE_FAN)",
                             "PrivateDriverDataSize < sizeof(BC250_ESCAPE_FAN)", 1), cli),
            # A second place builds a fan request, which no flag rule would cover.
            (display, cli.replace("static int FanControl(int argc, WCHAR **argv)",
                                  "static void FanStray(BC250_ESCAPE_FAN *d)\n"
                                  "{\n    d->Command = BC250_ESCAPE_RUN_FAN;\n}\n"
                                  "static int FanControl(int argc, WCHAR **argv)", 1)),
            # RUN_CPU moved in front of the power-phase check, with its mailbox writes.
            (display.replace("data->Command == BC250_ESCAPE_RUN_CPU", "data->Command == 0xFFFFFFFEu", 1), cli),
        ]
        for i, (d, c) in enumerate(client_mutations):
            with self.subTest(client_mutation=i):
                self.assertNotEqual((d, c), (display, cli), "mutation did not apply")
                self.assertTrue(client_flag_problems(d, c))


if __name__ == "__main__":
    unittest.main()
