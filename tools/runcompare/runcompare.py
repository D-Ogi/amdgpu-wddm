#!/usr/bin/env python3
"""runcompare - read two lab runs of bc250kmd and name the first difference.

Stage A of M7 is run over and over with one thing changed: a package, a gate, a DDI
answer. Every run leaves a driver log ring and a handful of state files, and the
question afterwards is always the same one - how far did dxgkrnl get this time, and
where exactly did this run stop agreeing with the last one. Answering it by eye means
reading two 35-line logs side by side and trusting yourself not to skip a line, which
is how "it behaved the same" gets written down about a run that did not.

    runcompare.py show <run-dir-or-log>
    runcompare.py diff <runA> <runB> [--json|--markdown]
    runcompare.py normalize <log>

A run is either an evidence directory (ring logs plus the target script's state files)
or a single ring log. With several rings in a directory the full-table instance - the
one whose log carries "gate: EnableFullWddm 1" - is taken, since that is the run under
test; --ring picks another.

This tool reads. It never writes into a run directory, and `evidence/` is immutable.

Name tables (QueryAdapterInfo types, NTSTATUS, CM_PROB, stages) come from
tables_generated.py, which gen_tables.py builds from the WDK, SDK and driver headers.
Nothing here is named from memory: an unknown code is printed as its raw hex.
"""

import argparse
import difflib
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from tables_generated import (CM_PROB_NAMES, CONTEXTINFO_CAPS, CREATECONTEXT_FLAGS,
                              CREATEDEVICE_FLAGS, CREATEPROCESS_FLAGS, HEADER_SOURCES,
                              LOG_CALLS, NTSTATUS_NAMES, QAI_TYPES, STAGE_NAMES)

# ---------------------------------------------------------------------------
# reading files the lab produces
# ---------------------------------------------------------------------------

# bc250kmd_cli writes UTF-16 through PowerShell redirection; the kept logs the driver
# itself writes at the stop are plain ASCII. Both turn up in one evidence directory.
def read_text(path):
    raw = Path(path).read_bytes()
    if raw.startswith(b"\xff\xfe"):
        return raw.decode("utf-16-le", errors="replace")[1:]
    if raw.startswith(b"\xfe\xff"):
        return raw.decode("utf-16-be", errors="replace")[1:]
    if raw.startswith(b"\xef\xbb\xbf"):
        return raw.decode("utf-8-sig", errors="replace")
    # No BOM: a UTF-16 file still gives itself away with a NUL in every other byte.
    head = raw[:512]
    if head.count(b"\x00") > len(head) // 4:
        return raw.decode("utf-16-le", errors="replace")
    return raw.decode("utf-8", errors="replace")


def read_lines(path):
    return [line.rstrip("\r\n") for line in read_text(path).splitlines()]


# ---------------------------------------------------------------------------
# name lookup
# ---------------------------------------------------------------------------

CM_PROB_BY_NAME = {name: code for code, name in CM_PROB_NAMES.items()}


def status_name(value):
    return NTSTATUS_NAMES.get(value, "0x%08X" % value)


def status_ok(value):
    """NT_SUCCESS(Status), which ntdef.h defines as (NTSTATUS)(Status) >= 0.

    So severity 0 (success) and 1 (informational) pass and the sign bit alone decides;
    a warning such as STATUS_BUFFER_OVERFLOW is not a success.
    """
    return (value & 0x80000000) == 0


def qai_name(kind):
    return QAI_TYPES.get(kind, "type %d, not in this Kit's enum" % kind)


def stage_name(number):
    return STAGE_NAMES.get(number)


def stage_label(number):
    name = stage_name(number)
    return "stage %d (%s)" % (number, name) if name else "stage %d" % number


def cm_prob_label(name):
    code = CM_PROB_BY_NAME.get(name)
    return "%s (Code %d)" % (name, code) if code is not None else name


def decode_bits(value, table):
    """0x5 over DXGK_CREATECONTEXTFLAGS -> "SystemContext | VirtualAddressing".

    A bit the Kit's header does not name is reported as `bit N`, never guessed at and
    never dropped: an unnamed bit in a flag word dxgkrnl passed is exactly the kind of
    thing a run is looked at for.
    """
    if value == 0:
        return "none set"
    names = []
    for bit in range(32):
        if value & (1 << bit):
            names.append(table.get(bit, "bit %d" % bit))
    return " | ".join(names)


# ---------------------------------------------------------------------------
# the ring log
# ---------------------------------------------------------------------------

# Written by the driver into its own log at DxgkDdiStopDevice, GuardLogKeep.
RE_HEAD_KEPT = re.compile(
    r"^bc250kmd\s+(0x[0-9A-Fa-f]+)\s+log kept at the stop:\s+(\d+) lines,"
    r"\s+(\d+) lost to the wrap,\s+(\d+) dropped above DISPATCH_LEVEL"
)
# Written by bc250kmd_cli when the ring is read through the escape while the driver runs.
RE_HEAD_LIVE = re.compile(
    r"^log\s+(\d+) lines since this driver load,\s+(\d+) lost to the wrap,"
    r"\s+(\d+) dropped above DISPATCH_LEVEL;\s+ring (\d+) lines of which the first (\d+) are kept;"
    r"\s+table (.+?)\s*$"
)
RE_BODY = re.compile(r"^\s*(\d+)\s+(\d+\.\d+)\s+(.*?)\s*$")
RE_TAIL = re.compile(r"^\s*(\d+) lines printed\s*$")


class Entry(object):
    """One line of the ring: its sequence number, its timestamp and what it says."""

    __slots__ = ("seq", "seconds", "text", "kind", "key", "data")

    def __init__(self, seq, seconds, text):
        self.seq = seq
        self.seconds = seconds
        self.text = text
        self.kind, self.key, self.data = classify(text)

    def __repr__(self):
        return "Entry(%d, %.3f, %r)" % (self.seq, self.seconds, self.text)


class RingLog(object):
    def __init__(self, path, lines):
        self.path = Path(path)
        self.driver_version = None
        self.kept = False
        self.table = None
        self.header_lines = None
        self.lost = None
        self.dropped = None
        self.ring_size = None
        self.header_text = None
        self.entries = []
        self.unparsed = []
        self._parse(lines)

    def _parse(self, lines):
        for line in lines:
            if not line.strip():
                continue
            m = RE_HEAD_KEPT.match(line)
            if m:
                self.header_text = line.strip()
                self.driver_version = int(m.group(1), 16)
                self.kept = True
                self.header_lines = int(m.group(2))
                self.lost = int(m.group(3))
                self.dropped = int(m.group(4))
                continue
            m = RE_HEAD_LIVE.match(line)
            if m:
                self.header_text = line.strip()
                self.header_lines = int(m.group(1))
                self.lost = int(m.group(2))
                self.dropped = int(m.group(3))
                self.ring_size = int(m.group(4))
                self.table = m.group(6)
                continue
            m = RE_BODY.match(line)
            if m:
                self.entries.append(Entry(int(m.group(1)), float(m.group(2)), m.group(3)))
                continue
            if RE_TAIL.match(line):
                continue
            self.unparsed.append(line.strip())

    @property
    def is_ring(self):
        return bool(self.entries)

    def gate(self, name="EnableFullWddm"):
        for entry in self.entries:
            if entry.kind == "gate" and entry.data["gate"] == name:
                return entry.data["value"]
        return None

    def sequence(self):
        """The entries of the last load, from its stage 10 on.

        A ring survives a device stop and start, so a log read through the escape can
        hold two starts. The run under test is the last one.
        """
        start = 0
        for index, entry in enumerate(self.entries):
            if entry.kind == "stage" and entry.data["stage"] == 10:
                start = index
        return self.entries[start:]


def load_ring(path):
    return RingLog(path, read_lines(path))


# ---------------------------------------------------------------------------
# classifying a log line
# ---------------------------------------------------------------------------

HEX = r"0x[0-9A-Fa-f]+"
RE_STAGE = re.compile(r"^stage (\d+)$")
RE_GATE = re.compile(r"^gate: (\w+) (\d+)$")
RE_POST = re.compile(r"^post display (\d+)x(\d+) pitch (\d+) format (\d+) target (\d+)$")
RE_INTERRUPT = re.compile(r"^interrupt resource (\d+): (.+?), vector (%s), flags (%s)$" % (HEX, HEX))
RE_MMIO = re.compile(r"^mmio: BAR5 at (%s) (.*)$" % HEX)
RE_VRAM = re.compile(r"^vram: (%s) \+ (%s), MC (%s), BAR0 (%s) \+ (%s), writes (\w+)$" % ((HEX,) * 5))
RE_CAPS = re.compile(r"^wddm: DRIVERCAPS (?:(\d+) of (\d+) bytes|into (\d+) bytes): (.*)$")
RE_QAI = re.compile(r"^wddm: QueryAdapterInfo type (\d+) in (\d+) out (\d+) -> (%s)$" % HEX)
RE_SEGMENT = re.compile(r"^wddm: segment (\d+) flags (%s) gpu (%s) cpu (%s) size (%s)$" % ((HEX,) * 4))
RE_CREATEPROCESS = re.compile(r"^wddm: CreateProcess flags (%s) pasids (\d+)$" % HEX)
RE_CREATEDEVICE = re.compile(r"^wddm: CreateDevice flags (%s) pasid (\d+)$" % HEX)
RE_CREATECONTEXT = re.compile(
    r"^wddm: CreateContext node (\d+) engine (%s) flags (%s) private (\d+)$" % (HEX, HEX))
RE_CONTEXTANSWER = re.compile(
    r"^wddm: CreateContext answered dma (\d+) bytes segment set (%s), lists (\d+)/(\d+), "
    r"caps (%s)$" % (HEX, HEX))
RE_STOP = re.compile(r"^wddm: stop, last completed fence (\d+), (\d+) vsync ticks, (\d+) objects freed")
RE_SUMMARY = re.compile(r"^wddm summary: (.*)$")
RE_TDR = re.compile(r"\*\*\*.*(ResetFromTimeout|RestartFromTimeout|ResetEngine|TDR)")
RE_DDI = re.compile(r"^wddm: ([A-Z]\w+)\b\s*(.*)$")
RE_CAPS_KV = re.compile(r"([A-Za-z][\w /]*?)\s+(%s|\d+)(?=\s|$)" % HEX)
# A 64-bit-looking literal in a line nothing else knows about: an address, not a flag word.
RE_LONG_HEX = re.compile(r"0x[0-9A-Fa-f]{9,}")

# (name in the log and in this tool, name in the stop summary's object line, create, destroy)
LIFECYCLE_KINDS = [
    ("process", "proc", "CreateProcess", "DestroyProcess"),
    ("device", "dev", "CreateDevice", "DestroyDevice"),
    ("context", "ctx", "CreateContext", "DestroyContext"),
    ("allocation", "alloc", "CreateAllocation", "DestroyAllocation"),
]
CREATE_DDIS = {create: kind for kind, _, create, _ in LIFECYCLE_KINDS}
DESTROY_DDIS = {destroy: kind for kind, _, _, destroy in LIFECYCLE_KINDS}

# What dxgkrnl is expected to ask a GpuMmu adapter for once it has built its system
# process, device and context, and `show` names the first of these a run never called.
#
# Sources: the DDI table driver/kmd/wddm.c builds (every name below is one it implements
# and counts, so "was it called" is answerable from the log alone) and the reasoning in
# the "What stage A is, and is not" section of driver/kmd/README.md - in particular why
# ControlInterrupt belongs at the end, since without VSync switched on a queued flip is
# never retired.
#
# THE ORDER HAS BEEN OBSERVED ON NO RUN YET. No run of E16 (001 to 004) has got past
# DestroyProcess, so this list is a reading of the driver's own table and of the WDK, not
# a measurement, and the tool says so wherever it prints a name from it. When a run does
# get further, correct the list from what that run shows, name the run here, and set
# EXPECTED_ORDER_OBSERVED to that run's id.
EXPECTED_AFTER_SYSTEM_CONTEXT = [
    ("GetRootPageTableSize", "how big a root page table the context's GPU VA space needs"),
    ("SetRootPageTable", "the root page table VidMm allocated, handed back to the driver"),
    ("CreateAllocation", "the first allocation, on this machine most likely DWM's primary"),
    ("BuildPagingBuffer", "the paging operations that map that allocation"),
    ("SetVidPnSourceAddress", "the VidPN path: a primary made visible to scanout"),
    ("ControlInterrupt", "VSync switched on, without which a queued flip never retires"),
]
EXPECTED_ORDER_OBSERVED = None      # the run that confirmed the order, once one does


def parse_caps_fields(text):
    """"wddm 8192 sched 0x45 mm 0x60 paging node 0" -> ordered [(key, value-as-text)].

    The keys are whatever the driver printed, so a build that adds a field (0.7.4 adds
    tdr, dflip and rot) is compared key by key instead of line against line.
    """
    fields = []
    for match in RE_CAPS_KV.finditer(text):
        fields.append((match.group(1).strip(), match.group(2)))
    return fields


def classify(text):
    """(kind, normalized key, data) for one ring line.

    The key is what `diff` aligns on: everything that says what the driver and dxgkrnl
    did, with what only says when or where it happened taken out. See README.md.
    """
    m = RE_STAGE.match(text)
    if m:
        return "stage", text, {"stage": int(m.group(1))}

    m = RE_GATE.match(text)
    if m:
        return "gate", text, {"gate": m.group(1), "value": int(m.group(2))}

    m = RE_POST.match(text)
    if m:
        # The target UID is the firmware's child, not an address: run 001 saw 0xFFFFFFFF
        # here and runs 002 and 003 our own child, which is a difference worth keeping.
        return "post", text, {"width": int(m.group(1)), "height": int(m.group(2)),
                              "pitch": int(m.group(3)), "format": int(m.group(4)),
                              "target": int(m.group(5))}

    m = RE_INTERRUPT.match(text)
    if m:
        return "interrupt", text, {"resources": int(m.group(1)), "how": m.group(2),
                                   "vector": m.group(3), "flags": m.group(4)}

    m = RE_MMIO.match(text)
    if m:
        return "mmio", "mmio: BAR5 at <addr> %s" % m.group(2), {"bar5": m.group(1)}

    m = RE_VRAM.match(text)
    if m:
        key = "vram: <addr> + %s, MC <addr>, BAR0 <addr> + %s, writes %s" % (
            m.group(2), m.group(5), m.group(6))
        return "vram", key, {"size": m.group(2), "bar0_size": m.group(5), "writes": m.group(6)}

    m = RE_CAPS.match(text)
    if m:
        size = int(m.group(2) or m.group(3))
        data = {"bytes": size, "fields": parse_caps_fields(m.group(4))}
        if m.group(1):
            data["written"] = int(m.group(1))
        # The wording of this line changed between builds and the field list grows; the
        # fields are compared as facts, so only the size takes part in the alignment.
        return "drivercaps", "wddm: DRIVERCAPS <%d bytes>" % size, data

    m = RE_QAI.match(text)
    if m:
        data = {
            "type": int(m.group(1)),
            "in": int(m.group(2)),
            "out": int(m.group(3)),
            "status": int(m.group(4), 16),
        }
        return "qai", text, data

    m = RE_SEGMENT.match(text)
    if m:
        key = "wddm: segment %s flags %s gpu <addr> cpu <addr> size %s" % (
            m.group(1), m.group(2), m.group(5))
        return "segment", key, {"flags": m.group(2), "size": m.group(5)}

    # The three objects dxgkrnl builds before it can do anything, and the one answer the
    # driver gives back. Every field is kept: the whole point of these lines is the fields.
    m = RE_CREATEPROCESS.match(text)
    if m:
        return "ddi", text, {"ddi": "CreateProcess", "flags": int(m.group(1), 16),
                             "pasids": int(m.group(2))}

    m = RE_CREATEDEVICE.match(text)
    if m:
        return "ddi", text, {"ddi": "CreateDevice", "flags": int(m.group(1), 16),
                             "pasid": int(m.group(2))}

    m = RE_CREATECONTEXT.match(text)
    if m:
        return "ddi", text, {"ddi": "CreateContext", "node": int(m.group(1)),
                             "engine": int(m.group(2), 16), "flags": int(m.group(3), 16),
                             "private": int(m.group(4))}

    m = RE_CONTEXTANSWER.match(text)
    if m:
        return "ddi_answer", text, {"ddi": "CreateContext", "dma": int(m.group(1)),
                                    "segment_set": int(m.group(2), 16),
                                    "allocation_list": int(m.group(3)),
                                    "patch_list": int(m.group(4)),
                                    "caps": int(m.group(5), 16)}

    m = RE_STOP.match(text)
    if m:
        return "stop", text, {"fence": int(m.group(1)), "vsync": int(m.group(2)),
                              "freed": int(m.group(3))}

    m = RE_SUMMARY.match(text)
    if m:
        body = re.sub(r"\s{2,}", " ", m.group(1)).strip()
        body = re.sub(r"first at \d+", "first at <seq>", body)
        return "summary", "wddm summary: " + body, {"body": m.group(1).strip()}

    if RE_TDR.search(text):
        return "tdr", RE_LONG_HEX.sub("<addr>", text), {}

    m = RE_DDI.match(text)
    if m:
        name, rest = m.group(1), m.group(2)
        kind = "ddi_answer" if rest.startswith("answered") else "ddi"
        return kind, RE_LONG_HEX.sub("<addr>", text), {"ddi": name}

    return "other", RE_LONG_HEX.sub("<addr>", text), {}


# Which entries make up "the sequence": what the driver and dxgkrnl did, in order. The
# stop summary is a report about that sequence, not a step of it, and its wording moves
# between builds, so it is read for its counters and left out of the alignment.
SEQUENCE_KINDS = {"stage", "gate", "post", "interrupt", "mmio", "vram", "drivercaps", "qai",
                  "segment", "ddi", "ddi_answer", "stop", "tdr", "other"}


def sequence_entries(ring):
    return [e for e in ring.sequence() if e.kind in SEQUENCE_KINDS]


# ---------------------------------------------------------------------------
# the state files of the target script
# ---------------------------------------------------------------------------

RE_ST_TIME = re.compile(r"^time\s+(\S+)\s+phase\s+(.+?)\s*$")
RE_ST_DEVICE = re.compile(r"^device\s+(.+?)\s+status\s+(\S+)\s+problem\s+(\S+)\s*$")
RE_ST_DRIVER = re.compile(r"^driver\s+(\S+)\s+\((\S+?)\)\s*$")
RE_ST_UMD = re.compile(r"^umd\s+(.+?)\s*$")
RE_ST_STAGES = re.compile(r"^stages\s+last\s+(\d+)\s+history\s+([\d ]*?)\s+unconfirmed\s+(\d+)\s*$")
RE_ST_KEEPLOG = re.compile(r"^keeplog\s+KeepLog\s+(\d+)\s+KeepStatus\s+(0x[0-9A-Fa-f]+)\s+files\s+(\d+)\s*$")
RE_ST_GATES = re.compile(r"^gates\s+(.+?)\s*$")
RE_ST_EVENTS = re.compile(r"^events\s+(.+?)\s+(\d+)\s*$")
RE_ST_REPORTS = re.compile(r"^reports\s+(.+?):\s*(\d+)\s*$")
RE_ST_VERSION = re.compile(r"^version\s+(0x[0-9A-Fa-f]+)\s*(.*)$")
RE_ST_LASTSTAGE = re.compile(r"^last stage\s+(\d+)\s*(.*)$")
RE_ST_PRESENTS = re.compile(r"^presents\s+(\d+)\s*$")
RE_ST_MODE = re.compile(r"^mode\s+(\S+)\s+pitch\s+(\d+)\s+format\s+(\d+)\s*$")
RE_ST_GATEPAIR = re.compile(r"(\w+)\s+(\d+)")


class StateFile(object):
    """gate-x-*.txt / state-*.txt / install-x-*.txt: what Windows said about the device."""

    def __init__(self, path, lines):
        self.path = Path(path)
        self.time = None
        self.phase = None
        self.device = None
        self.status = None
        self.problem = None
        self.driver = None
        self.inf = None
        self.umd = None
        self.last_stage = None
        self.stage_history = []
        self.unconfirmed = None
        self.keeplog = None
        self.gates = {}
        self.events = []
        self.reports = None
        self.escape_version = None
        self.escape_last_stage = None
        self.presents = None
        self.mode = None
        for line in lines:
            self._line(line)

    def _line(self, line):
        m = RE_ST_TIME.match(line)
        if m:
            self.time, self.phase = m.group(1), m.group(2)
            return
        m = RE_ST_DEVICE.match(line)
        if m:
            self.device, self.status, self.problem = m.group(1), m.group(2), m.group(3)
            return
        m = RE_ST_DRIVER.match(line)
        if m:
            self.driver, self.inf = m.group(1), m.group(2)
            return
        m = RE_ST_STAGES.match(line)
        if m:
            self.last_stage = int(m.group(1))
            self.stage_history = [int(v) for v in m.group(2).split()]
            self.unconfirmed = int(m.group(3))
            return
        m = RE_ST_KEEPLOG.match(line)
        if m:
            self.keeplog = {"KeepLog": int(m.group(1)), "KeepStatus": m.group(2),
                            "files": int(m.group(3))}
            return
        m = RE_ST_GATES.match(line)
        if m:
            self.gates = {k: int(v) for k, v in RE_ST_GATEPAIR.findall(m.group(1))}
            return
        m = RE_ST_EVENTS.match(line)
        if m:
            self.events.append((m.group(1).strip(), int(m.group(2))))
            return
        m = RE_ST_REPORTS.match(line)
        if m:
            self.reports = int(m.group(2))
            return
        m = RE_ST_VERSION.match(line)
        if m:
            self.escape_version = int(m.group(1), 16)
            return
        m = RE_ST_LASTSTAGE.match(line)
        if m:
            self.escape_last_stage = int(m.group(1))
            return
        m = RE_ST_PRESENTS.match(line)
        if m:
            self.presents = int(m.group(1))
            return
        m = RE_ST_MODE.match(line)
        if m:
            self.mode = "%s pitch %s format %s" % (m.group(1), m.group(2), m.group(3))
            return
        m = RE_ST_UMD.match(line)
        if m:
            self.umd = m.group(1)
            return


STATE_GLOBS = ("gate-x-*.txt", "state-*.txt", "install-x-*.txt", "diag-*.txt")


def load_states(directory):
    states = []
    for pattern in STATE_GLOBS:
        for path in sorted(directory.glob(pattern)):
            state = StateFile(path, read_lines(path))
            if state.device or state.gates:
                states.append(state)
    states.sort(key=lambda s: (s.time or "", s.path.name))
    return states


# ---------------------------------------------------------------------------
# a run: one ring plus the state file that belongs to it
# ---------------------------------------------------------------------------

class Run(object):
    def __init__(self, source, ring, state, states, ring_choices):
        self.source = Path(source)
        self.ring = ring
        self.state = state
        self.states = states
        self.ring_choices = ring_choices
        self.name = self.source.name

    # -- facts read straight off the two files -----------------------------

    @property
    def driver_version(self):
        if self.ring.driver_version is not None:
            return self.ring.driver_version
        return self.state.escape_version if self.state else None

    @property
    def package(self):
        if self.state and self.state.driver:
            return "%s (%s)" % (self.state.driver, self.state.inf)
        return None

    @property
    def umd(self):
        return self.state.umd if self.state else None

    @property
    def gates(self):
        return dict(self.state.gates) if self.state else {}

    @property
    def full_wddm(self):
        gate = self.ring.gate("EnableFullWddm")
        if gate is not None:
            return gate
        return self.gates.get("EnableFullWddm")

    @property
    def problem(self):
        return self.state.problem if self.state else None

    # -- facts derived from the sequence -----------------------------------

    @property
    def sequence(self):
        return sequence_entries(self.ring)

    @property
    def qai_calls(self):
        return [e for e in self.sequence if e.kind == "qai"]

    @property
    def caps(self):
        for entry in self.sequence:
            if entry.kind == "drivercaps":
                return entry
        return None

    @property
    def last_stage(self):
        stages = [e.data["stage"] for e in self.ring.sequence() if e.kind == "stage"]
        return stages[-1] if stages else None

    @property
    def stages(self):
        return [e.data["stage"] for e in self.ring.sequence() if e.kind == "stage"]

    def stage_time(self, number):
        for entry in self.ring.sequence():
            if entry.kind == "stage" and entry.data["stage"] == number:
                return entry.seconds
        return None

    @property
    def stop_after_start(self):
        """Seconds from stage 39 (StartDevice done) to stage 70 (StopDevice entered)."""
        start, stop = self.stage_time(39), self.stage_time(70)
        if start is None or stop is None:
            return None
        return stop - start

    @property
    def first_refusal(self):
        """The first entry that says no: a failed status, or a line that refuses."""
        for entry in self.sequence:
            if entry.kind == "qai" and not status_ok(entry.data["status"]):
                return entry
            if entry.kind in ("ddi", "ddi_answer", "other", "tdr"):
                if re.search(r"\brefused\b|\bfailed\b|must be zero", entry.text):
                    return entry
        return None

    @property
    def tdr_entries(self):
        """Only lines that report a TDR that happened.

        wddm.c writes one of two summary lines, and the difference is three stars: a
        "no TDR (...)" line is the driver saying it was never reset, not a TDR.
        """
        out = [e for e in self.ring.sequence() if e.kind == "tdr"]
        out += [e for e in self.ring.sequence()
                if e.kind == "summary" and "TDR" in e.text and "***" in e.text]
        return out

    @property
    def tdr_note(self):
        for entry in self.ring.sequence():
            if entry.kind == "summary" and entry.text.startswith("wddm summary: no TDR"):
                return entry.text[len("wddm summary: "):]
        return None

    @property
    def objects_summary(self):
        """The driver's own count, written into the log at the stop.

        One line up to KMD 0.7.207, two from 0.7.208 on: the nine counters at their widest did
        not fit the 160-byte log line, so the allocation pair moved to a second line of the same
        name (BD-070). The driver writes the allocation line first and the line that ends in
        "N alive" last, so one stop summary is one block of one or two lines.

        This reads the FIRST block and stops there, exactly where the one-line parser stopped. A
        ring can hold several stop summaries, from an adapter that was started and stopped more
        than once, and numbers from two of them must never be mixed into one row: merging the
        whole ring changed what 119 of the 1104 archived logs report.

        A block without its "N alive" line gives (pairs, None). A 0.7.207 line that the log
        truncated reads that way, so every caller has to handle the missing count.
        """
        pairs = {}
        for entry in self.ring.sequence():
            if entry.kind != "summary":
                continue
            m = re.search(r"objects created/destroyed: (.+)$", entry.text)
            if not m:
                continue
            body = m.group(1)
            tail = re.search(r", (\d+) alive$", body)
            if tail:
                body = body[:tail.start()]
            pairs.update({what: (int(a), int(b))
                          for what, a, b in re.findall(r"(\w+) (\d+)/(\d+)", body)})
            if tail:
                return (pairs or None), int(tail.group(1))
        return (pairs or None), None

    @property
    def summary_ddi_calls(self):
        """{DDI name: (count, first log line)} from the table the stop summary writes.

        Authoritative for "was this DDI ever entered": the log keeps only the first
        LOG_CALLS calls of each, the counters keep all of them.
        """
        out = {}
        for entry in self.ring.sequence():
            if entry.kind != "summary":
                continue
            m = re.match(r"^wddm summary: ([A-Za-z]\w*)\s+(\d+)\s+first at (\d+)$", entry.key)
            if m:
                out[m.group(1)] = (int(m.group(2)), int(m.group(3)))
        return out

    @property
    def lifecycle(self):
        return Lifecycle(self)

    @property
    def summary_qai_types(self):
        out = {}
        for entry in self.ring.sequence():
            if entry.kind != "summary":
                continue
            m = re.search(r"adapter info type\s+(\d+)\s+(\d+)\s+first at (\d+)", entry.text)
            if m:
                out[int(m.group(1))] = (int(m.group(2)), int(m.group(3)))
        return out


class Lifecycle(object):
    """The object graph dxgkrnl builds on an adapter, and how long it kept it.

    CreateProcess, CreateDevice and CreateContext followed by their Destroy counterparts
    is as far as every E16 run has got: dxgkrnl builds its system process, a device or
    two and one context, takes them all down again and stops the device. Which fields it
    asked for, what the driver answered and how long the whole graph lived are the only
    things those few milliseconds leave behind.

    The log carries no object handle, so this pairs creations and destructions by kind
    and by count, never one named object to another. Where the counts match, the graph's
    span is first creation to last destruction.
    """

    def __init__(self, run):
        self.run = run
        self.creates = {kind: [] for kind, _, _, _ in LIFECYCLE_KINDS}
        self.destroys = {kind: [] for kind, _, _, _ in LIFECYCLE_KINDS}
        self.answer = None              # the CreateContext answer line
        self.order = []                 # (kind, "create"|"destroy", entry, ordinal)
        for entry in run.sequence:
            name = entry.data.get("ddi")
            if entry.kind == "ddi_answer" and name == "CreateContext":
                self.answer = entry
                continue
            if entry.kind != "ddi":
                continue
            if name in CREATE_DDIS:
                kind = CREATE_DDIS[name]
                self.creates[kind].append(entry)
                self.order.append((kind, "create", entry, len(self.creates[kind])))
            elif name in DESTROY_DDIS:
                kind = DESTROY_DDIS[name]
                self.destroys[kind].append(entry)
                self.order.append((kind, "destroy", entry, len(self.destroys[kind])))

    def __bool__(self):
        return bool(self.order)

    __nonzero__ = __bool__

    @property
    def context_create(self):
        return self.creates["context"][0] if self.creates["context"] else None

    @property
    def counts(self):
        return {kind: (len(self.creates[kind]), len(self.destroys[kind]))
                for kind, _, _, _ in LIFECYCLE_KINDS}

    @property
    def symmetric(self):
        """Every object created was destroyed again, and the driver freed none itself."""
        if not self.order:
            return None
        if any(len(self.creates[k]) != len(self.destroys[k]) for k in self.creates):
            return False
        return self.alive in (0, None)

    @property
    def alive(self):
        _, alive = self.run.objects_summary
        return alive

    @property
    def span(self):
        """(first creation, last destruction, seconds between) for the whole graph."""
        starts = [e for entries in self.creates.values() for e in entries]
        ends = [e for entries in self.destroys.values() for e in entries]
        if not starts or not ends:
            return None, None, None
        first = min(starts, key=lambda e: e.seq)
        last = max(ends, key=lambda e: e.seq)
        return first, last, last.seconds - first.seconds

    @property
    def teardown_is_lifo(self):
        created = [kind for kind, what, _, _ in self.order if what == "create"]
        destroyed = [kind for kind, what, _, _ in self.order if what == "destroy"]
        if not created or len(created) != len(destroyed):
            return None
        return destroyed == list(reversed(created))

    @property
    def called_ddis(self):
        """Every DDI this run entered, from the counters and from the log lines."""
        names = set(self.run.summary_ddi_calls)
        for entry in self.run.sequence:
            if entry.kind in ("ddi", "ddi_answer") and entry.data.get("ddi"):
                names.add(entry.data["ddi"])
        return names

    @property
    def next_expected(self):
        """(name, why) of the first DDI of the next stage this run never called."""
        called = self.called_ddis
        for name, why in EXPECTED_AFTER_SYSTEM_CONTEXT:
            if name not in called:
                return name, why
        return None, None

    # -- decoding -----------------------------------------------------------

    @staticmethod
    def describe_create(entry):
        data = entry.data
        name = data.get("ddi")
        if name == "CreateProcess":
            return "flags %s (%s), %d pasid%s" % (
                "0x%08X" % data["flags"], decode_bits(data["flags"], CREATEPROCESS_FLAGS),
                data["pasids"], "" if data["pasids"] == 1 else "s")
        if name == "CreateDevice":
            return "flags %s (%s), pasid %d" % (
                "0x%08X" % data["flags"], decode_bits(data["flags"], CREATEDEVICE_FLAGS),
                data["pasid"])
        if name == "CreateContext":
            return "node %d, engine affinity 0x%X, flags %s (%s), private data %d bytes" % (
                data["node"], data["engine"], "0x%08X" % data["flags"],
                decode_bits(data["flags"], CREATECONTEXT_FLAGS), data["private"])
        return entry.text

    @property
    def answer_fields(self):
        """Every field of the CreateContext answer, decoded, as [(name, value)]."""
        if self.answer is None:
            return []
        data = self.answer.data
        segment = data["segment_set"]
        # DXGK_CONTEXTINFO.DmaBufferSegmentSet is a bit per segment id, 1-based.
        if segment == 0:
            segment_text = "0x%08X (no segment: the DMA buffer is not in video memory)" % segment
        else:
            ids = [str(bit + 1) for bit in range(32) if segment & (1 << bit)]
            segment_text = "0x%08X (segment %s)" % (segment, ", ".join(ids))
        return [
            ("DmaBufferSize", "%d bytes" % data["dma"]),
            ("DmaBufferSegmentSet", segment_text),
            ("AllocationListSize", str(data["allocation_list"])),
            ("PatchLocationListSize", str(data["patch_list"])),
            ("Caps", "0x%08X (%s)" % (data["caps"], decode_bits(data["caps"], CONTEXTINFO_CAPS))),
        ]

    # -- rendering ----------------------------------------------------------

    def event_lines(self):
        """The create/destroy calls in order, with the gap from the previous one."""
        lines, previous = [], None
        for kind, what, entry, ordinal in self.order:
            name = (CREATE_DDIS if what == "create" else DESTROY_DDIS)
            label = [n for n, k in name.items() if k == kind][0]
            if len(self.creates[kind]) > 1 or len(self.destroys[kind]) > 1:
                label += " #%d" % ordinal
            gap = "" if previous is None else "+%d ms" % round((entry.seconds - previous) * 1000)
            note = self.describe_create(entry) if what == "create" else ""
            lines.append(("%6.3f  %-17s %-7s %s" % (entry.seconds, label, gap, note)).rstrip())
            if what == "create" and kind == "context" and self.answer is not None:
                for index, (field, value) in enumerate(self.answer_fields):
                    lines.append("%6s  %-17s %-7s %-22s %s" % (
                        "%.3f" % self.answer.seconds if index == 0 else "",
                        "answered" if index == 0 else "", "", field, value))
            previous = entry.seconds
        return lines

    def balance_lines(self):
        lines = []
        for kind, _, _, _ in LIFECYCLE_KINDS:
            created, destroyed = self.creates[kind], self.destroys[kind]
            if not created and not destroyed:
                continue
            verdict = "symmetric" if len(created) == len(destroyed) else "NOT symmetric"
            when = ""
            if created and destroyed:
                when = ", %.3f s -> %.3f s (+%d ms)" % (
                    created[0].seconds, destroyed[-1].seconds,
                    round((destroyed[-1].seconds - created[0].seconds) * 1000))
            lines.append("%-12s %d created, %d destroyed - %s%s" % (
                kind, len(created), len(destroyed), verdict, when))
        return lines


def pick_ring(directory, explicit=None):
    """The ring of the run under test: the full-table one unless told otherwise."""
    if explicit:
        return load_ring(explicit), []
    candidates = []
    for path in sorted(directory.glob("ring-*.log")):
        ring = load_ring(path)
        if ring.is_ring:
            candidates.append(ring)
    if not candidates:
        raise SystemExit("runcompare: no readable ring-*.log in %s" % directory)
    full = [r for r in candidates if r.gate("EnableFullWddm") == 1]
    chosen = (full or candidates)[-1]
    return chosen, [r.path.name for r in candidates]


def pick_state(states, ring):
    """The state file taken while this ring's driver was the one running.

    A run directory holds the state before the gate, at the gate and after the gates are
    closed again. The one that belongs to a full-table ring is the one whose gates were
    open; among several, the last.
    """
    if not states:
        return None
    if ring.gate("EnableFullWddm") == 1:
        open_gate = [s for s in states if s.gates.get("EnableFullWddm") == 1]
        if open_gate:
            return open_gate[-1]
    return states[-1]


def load_run(source, ring_path=None, state_path=None):
    source = Path(source)
    if source.is_dir():
        ring, choices = pick_ring(source, ring_path)
        states = load_states(source)
        state = StateFile(state_path, read_lines(state_path)) if state_path else pick_state(states, ring)
        return Run(source, ring, state, states, choices)
    ring = load_ring(ring_path or source)
    if not ring.is_ring:
        raise SystemExit("runcompare: %s holds no ring lines" % (ring_path or source))
    if state_path:
        return Run(source, ring, StateFile(state_path, read_lines(state_path)), [], [])
    # A log named on its own still gets the state files lying next to it, if any.
    states = load_states(source.parent)
    return Run(source, ring, pick_state(states, ring), states, [])


# ---------------------------------------------------------------------------
# describing an entry in words
# ---------------------------------------------------------------------------

def describe(entry, short=False):
    if entry is None:
        return "the start of the log"
    if entry.kind == "stage":
        return stage_label(entry.data["stage"])
    if entry.kind == "qai":
        kind = entry.data["type"]
        if short:
            verdict = "answered" if status_ok(entry.data["status"]) else "refused"
            return "QueryAdapterInfo type %d (%s, %s)" % (kind, qai_name(kind), verdict)
        return "QueryAdapterInfo type %d (%s) -> %s" % (
            kind, qai_name(kind), status_name(entry.data["status"]))
    if entry.kind == "drivercaps":
        return "DRIVERCAPS (%d bytes)" % entry.data["bytes"]
    if entry.kind == "gate":
        return "gate %s %d" % (entry.data["gate"], entry.data["value"])
    if entry.kind in ("ddi", "ddi_answer"):
        name = entry.data.get("ddi", "?")
        return name if short else entry.key[len("wddm: "):]
    if entry.kind == "segment":
        return "the segment table (%s)" % entry.data["size"]
    if entry.kind == "stop":
        return "the stop line"
    return entry.key


def is_stop_entry(entry):
    return entry is not None and entry.kind == "stage" and entry.data["stage"] in (70, 79)


# ---------------------------------------------------------------------------
# show
# ---------------------------------------------------------------------------

EXPECTATION_SOURCE = ("order confirmed by %s" % EXPECTED_ORDER_OBSERVED if EXPECTED_ORDER_OBSERVED
                      else "order observed on no run yet; read from wddm.c's own DDI table "
                           "and the WDK, so it is a reading, not a measurement")


def expectation_note(name, why):
    """Always says where the expectation comes from, because it is not a measurement."""
    if name is None:
        return "none: this run called every DDI the expectation table lists"
    return "%s - %s" % (name, why)


def lifecycle_lines(run):
    life = run.lifecycle
    if not life:
        name, why = life.next_expected
        summary_objects, _ = run.objects_summary
        if summary_objects and any(v[0] for v in summary_objects.values()):
            return ["no create/destroy line in the log, but the stop summary counted "
                    "objects: %s" % ", ".join("%s %d/%d" % (k, v[0], v[1])
                                              for k, v in summary_objects.items())]
        return ["no object was created: dxgkrnl stopped the adapter before it built one",
                "",
                "%-24s %s" % ("next DDI expected", expectation_note(name, why)),
                "%-24s %s" % ("", EXPECTATION_SOURCE)]

    lines = list(life.event_lines())
    lines.append("")
    lines.extend(life.balance_lines())
    lines.append("")

    alive = life.alive
    if life.symmetric:
        lines.append("%-24s yes: every object created was destroyed again%s" % (
            "symmetric", ", %d alive at the stop" % alive if alive is not None else ""))
    elif life.symmetric is False:
        lines.append("%-24s NO: the counts do not match%s" % (
            "symmetric", ", %d alive at the stop" % alive if alive is not None else ""))

    lifo = life.teardown_is_lifo
    if lifo is not None:
        lines.append("%-24s %s" % ("teardown order", "LIFO, the reverse of the creation order"
                                   if lifo else "NOT the reverse of the creation order"))

    first, last, span = life.span
    if span is not None:
        lines.append("%-24s %.3f s (%s at %.3f s -> %s at %.3f s)" % (
            "object graph lived", span, first.data.get("ddi"), first.seconds,
            last.data.get("ddi"), last.seconds))

    name, why = life.next_expected
    lines.append("%-24s %s" % ("next DDI expected", expectation_note(name, why)))
    lines.append("%-24s %s" % ("", EXPECTATION_SOURCE))

    # The log keeps only the first LOG_CALLS calls of each DDI; the counters keep all.
    counted = run.summary_ddi_calls
    for ddi in sorted(set(CREATE_DDIS) | set(DESTROY_DDIS)):
        if ddi not in counted:
            continue
        logged = sum(1 for _, _, e, _ in life.order if e.data.get("ddi") == ddi)
        if counted[ddi][0] != logged:
            lines.append("%-24s %s counted %d times at the stop, %d in the log (the log keeps "
                         "the first %d calls of each DDI)" % (
                             "", ddi, counted[ddi][0], logged, LOG_CALLS))
    return lines


def show_report(run):
    """The facts of one run, as an ordered list of (heading, [lines])."""
    ring = run.ring
    out = []

    head = []
    head.append(("source", str(run.source)))
    head.append(("ring", "%s (%s)" % (ring.path.name, "kept at the stop" if ring.kept
                                      else "read live through the escape")))
    if run.ring_choices and len(run.ring_choices) > 1:
        head.append(("rings here", ", ".join(run.ring_choices) + "  (--ring picks another)"))
    version = run.driver_version
    if version is not None:
        head.append(("driver", "0x%08X (milestone %d revision %d)" % (
            version, (version >> 16) & 0xFF, version & 0xFFFF)))
    head.append(("log", "%s lines, %s lost to the wrap, %s dropped above DISPATCH_LEVEL" % (
        len(ring.entries), ring.lost, ring.dropped)))
    head.append(("package", run.package or "not recorded (no state file in this run)"))
    head.append(("umd", run.umd or "not recorded (no state file in this run)"))
    if run.state:
        head.append(("state file", run.state.path.name + "  (phase %s, %s)" % (
            run.state.phase, run.state.time)))
        head.append(("device", "status %s, problem %s" % (
            run.state.status, cm_prob_label(run.state.problem))))
        head.append(("gates", "  ".join("%s %d" % kv for kv in run.state.gates.items())
                     or "not recorded"))
        if run.state.last_stage is not None:
            head.append(("registry stages", "last %d, history %s, unconfirmed %s" % (
                run.state.last_stage,
                " ".join(str(s) for s in run.state.stage_history),
                run.state.unconfirmed)))
        events = ", ".join("%s %d" % e for e in run.state.events)
        if events:
            head.append(("event counts", events))
    else:
        head.append(("gates", "gate: EnableFullWddm %s (from the log)" % run.full_wddm))
    out.append(("run", head))

    seq = run.sequence
    lines = []
    for entry in seq:
        text = entry.text
        if entry.kind == "stage":
            name = stage_name(entry.data["stage"])
            if name:
                text = "%s  (%s)" % (text, name)
        if entry.kind == "qai":
            text = "%s  (%s, %s)" % (text, qai_name(entry.data["type"]),
                                     status_name(entry.data["status"]))
        lines.append("%6.3f  %s" % (entry.seconds, text))
    out.append(("PnP/DDI sequence after the last stage 10 (%d steps)" % len(seq), lines))

    qai_lines = []
    summary_types = run.summary_qai_types
    for entry in run.qai_calls:
        kind = entry.data["type"]
        qai_lines.append("type %3d  %-30s in %-5d out %-5d -> %s" % (
            kind, qai_name(kind), entry.data["in"], entry.data["out"],
            status_name(entry.data["status"])))
    unlogged = False
    for kind, (count, first) in sorted(summary_types.items()):
        logged = sum(1 for e in run.qai_calls if e.data["type"] == kind)
        if count != logged:
            unlogged = True
            qai_lines.append("type %3d  %-30s %d call(s) counted at the stop, %d in the log"
                             % (kind, qai_name(kind), count, logged))
    if unlogged:
        # Why a counted type can have no line, spelled out: it is not "because it
        # succeeded" but because the budget was already spent. wddm.c logs the first
        # LOG_CALLS calls of QueryAdapterInfo whatever they answered, and after that only
        # refusals - so a late refusal still gets a line and a late success does not.
        qai_lines.append("")
        qai_lines.append("a type counted here with no line of its own spent the log budget: "
                         "wddm.c logs the")
        qai_lines.append("first %d QueryAdapterInfo calls whatever they answer and, after "
                         "that, only refusals." % LOG_CALLS)
        qai_lines.append("The stop summary's own \"first at N\" for such a type is the ring's "
                         "sequence number when")
        qai_lines.append("the type was first noted, which is a line belonging to some other "
                         "call; it is not shown here.")
    out.append(("QueryAdapterInfo", qai_lines or ["none"]))

    caps = run.caps
    if caps is not None:
        fields = ["%-12s %s" % (k, v) for k, v in caps.data["fields"]]
        out.append(("DRIVERCAPS answered (%d bytes)" % caps.data["bytes"], fields))

    out.append(("object lifecycle", lifecycle_lines(run)))

    tail = []
    summary_objects, alive = run.objects_summary
    if summary_objects:
        # The live count can be missing: a truncated 0.7.207 line, or a ring that kept the
        # allocation line of a 0.7.208 pair and lost the other one. Say so instead of crashing.
        tail.append(("objects", "%s%s  (counted by the driver at the stop)" % (
            ", ".join("%s %d/%d" % (k, v[0], v[1]) for k, v in summary_objects.items()),
            ", %d alive" % alive if alive is not None else ", the live count is not in the log")))
    tail.append(("last stage", stage_label(run.last_stage) if run.last_stage is not None else "none"))
    tail.append(("stage history", " ".join(str(s) for s in run.stages)))

    refusal = run.first_refusal
    if refusal is not None:
        tail.append(("first refusal or error", "%s  (seq %d, %.3f s)" % (
            describe(refusal), refusal.seq, refusal.seconds)))
    elif run.problem and run.problem != "CM_PROB_NONE":
        tail.append(("first refusal or error", "none in the log - every answer succeeded and "
                                               "dxgkrnl failed the device anyway, %s"
                     % cm_prob_label(run.problem)))
    else:
        tail.append(("first refusal or error", "none"))

    stop = run.stop_after_start
    if stop is not None:
        tail.append(("StopDevice", "%s at %.3f s, %.3f s after %s" % (
            stage_label(70), run.stage_time(70), stop, stage_label(39))))
    elif run.stage_time(70) is not None:
        tail.append(("StopDevice", "%s at %.3f s (no stage 39 in this log)" % (
            stage_label(70), run.stage_time(70))))
    else:
        tail.append(("StopDevice", "not in this log"))

    tdr = run.tdr_entries
    if tdr:
        tail.append(("TDR", "\n".join(e.text for e in tdr)))
    else:
        tail.append(("TDR", run.tdr_note or "no TDR line in the log"))
    tail.append(("PnP problem", cm_prob_label(run.problem) if run.problem
                 else "not recorded (no state file in this run)"))
    if ring.unparsed:
        tail.append(("lines this tool did not recognise", "%d: %s" % (
            len(ring.unparsed), "; ".join(ring.unparsed[:3]))))
    out.append(("verdict", tail))
    return out


def render_show(report, markdown=False):
    lines = []
    for heading, body in report:
        lines.append("## " + heading if markdown else heading)
        lines.append("")
        for item in body:
            if isinstance(item, tuple):
                key, value = item
                first, _, rest = value.partition("\n")
                lines.append("  %-24s %s" % (key, first))
                for extra in rest.splitlines():
                    lines.append("  %-24s %s" % ("", extra))
            else:
                lines.append(("  " + item).rstrip())
        lines.append("")
    return "\n".join(lines)


def lifecycle_json(run):
    life = run.lifecycle
    name, why = life.next_expected
    first, last, span = life.span
    return {
        "objects": {kind: {"created": created, "destroyed": destroyed}
                    for kind, (created, destroyed) in life.counts.items()
                    if created or destroyed},
        "symmetric": life.symmetric,
        "alive_at_the_stop": life.alive,
        "teardown_is_lifo": life.teardown_is_lifo,
        "span_seconds": span,
        "events": [{"seconds": e.seconds, "kind": kind, "what": what, "ordinal": ordinal,
                    "decoded": life.describe_create(e) if what == "create" else None}
                   for kind, what, e, ordinal in life.order],
        "create_context": (life.describe_create(life.context_create)
                           if life.context_create else None),
        "create_context_answer": dict(life.answer_fields) or None,
        "next_expected_ddi": name,
        "next_expected_why": why,
        "expected_order_observed_on": EXPECTED_ORDER_OBSERVED,
    }


def show_json(run):
    caps = run.caps
    return {
        "source": str(run.source),
        "ring": run.ring.path.name,
        "kept_at_the_stop": run.ring.kept,
        "driver_version": ("0x%08X" % run.driver_version) if run.driver_version else None,
        "package": run.package,
        "umd": run.umd,
        "gates": run.gates,
        "full_wddm": run.full_wddm,
        "problem": run.problem,
        "problem_code": CM_PROB_BY_NAME.get(run.problem),
        "last_stage": run.last_stage,
        "stages": run.stages,
        "stop_after_start_seconds": run.stop_after_start,
        "sequence": [{"seconds": e.seconds, "kind": e.kind, "text": e.text, "key": e.key}
                     for e in run.sequence],
        "query_adapter_info": [
            {"type": e.data["type"], "name": qai_name(e.data["type"]),
             "in": e.data["in"], "out": e.data["out"],
             "status": "0x%08X" % e.data["status"],
             "status_name": status_name(e.data["status"])}
            for e in run.qai_calls],
        "drivercaps": {"bytes": caps.data["bytes"],
                       "fields": dict(caps.data["fields"])} if caps else None,
        "first_refusal": describe(run.first_refusal) if run.first_refusal else None,
        "tdr": [e.text for e in run.tdr_entries],
        "lifecycle": lifecycle_json(run),
    }


# ---------------------------------------------------------------------------
# diff
# ---------------------------------------------------------------------------

class Difference(object):
    def __init__(self, a, b):
        self.a = a
        self.b = b
        self.seq_a = a.sequence
        self.seq_b = b.sequence
        self.opcodes = difflib.SequenceMatcher(
            a=[e.key for e in self.seq_a], b=[e.key for e in self.seq_b], autojunk=False
        ).get_opcodes()
        self.context = None      # the last entry both runs share
        self.first_a = None      # what each run did at the point where they parted
        self.first_b = None
        self.identical = True
        self.same_facts = []
        for tag, i1, i2, j1, j2 in self.opcodes:
            if tag == "equal":
                if i2 > i1:
                    self.context = self.seq_a[i2 - 1]
                continue
            self.identical = False
            # Not "the first entry inside the differing block": when one run only adds
            # lines, the other run's next entry is the one it went to instead, and that
            # is what the sentence has to name (usually its stage 70).
            self.first_a = self.seq_a[i1] if i1 < len(self.seq_a) else None
            self.first_b = self.seq_b[j1] if j1 < len(self.seq_b) else None
            break

    # -- the one sentence --------------------------------------------------

    def _side(self, run, entry, other_entry):
        """What this run did at the point where the two stopped agreeing."""
        if entry is None:
            return "stopped (stage %d)" % run.last_stage if run.last_stage else "ended here"
        if is_stop_entry(entry) and not is_stop_entry(other_entry):
            return "stopped (stage %d)" % entry.data["stage"]
        return "went on to %s" % describe(entry)

    def sentence(self):
        if self.identical:
            return "No difference: the two sequences are identical after normalization."
        side_a = self._side(self.a, self.first_a, self.first_b)
        side_b = self._side(self.b, self.first_b, self.first_a)
        first, second = ("B", "A") if side_a.startswith("stopped") and not side_b.startswith("stopped") \
            else ("A", "B")
        texts = {"A": side_a, "B": side_b}
        return "First difference: after %s run %s %s; run %s %s." % (
            describe(self.context, short=True), first, texts[first], second, texts[second])

    # -- the side by side --------------------------------------------------

    def side_by_side(self, width=None):
        rows = []
        if width is None:
            longest = max([len(e.text) for e in self.seq_a + self.seq_b] + [24])
            width = min(longest, 74)
        for tag, i1, i2, j1, j2 in self.opcodes:
            if tag == "equal":
                for offset in range(i2 - i1):
                    left, right = self.seq_a[i1 + offset], self.seq_b[j1 + offset]
                    # Equal after normalization is not the same as equal on the page:
                    # ~ marks a pair the alignment kept together and whose fields are
                    # compared in the facts list instead (the DRIVERCAPS line does this).
                    rows.append(("=" if left.text == right.text else "~", left.text, right.text))
            else:
                for index in range(max(i2 - i1, j2 - j1)):
                    left = self.seq_a[i1 + index].text if i1 + index < i2 else ""
                    right = self.seq_b[j1 + index].text if j1 + index < j2 else ""
                    rows.append(("A" if not right else "B" if not left else "*", left, right))
        def fit(text):
            return text if len(text) <= width else text[:width - 3] + "..."

        lines = ["  %-3s %-*s %s" % ("", width, "A: " + self.a.name, "B: " + self.b.name)]
        for mark, left, right in rows:
            lines.append(("  %-3s %-*s %s" % (mark, width, fit(left), fit(right))).rstrip())
        return lines

    # -- everything else that differs --------------------------------------

    @staticmethod
    def _symmetry(life):
        if not life:
            return "no object was created"
        return {True: "yes", False: "no"}.get(life.symmetric, "unknown")

    def facts(self):
        """[(what, A, B)] for the facts that differ.

        A fact that matches is left out of the rows and its name is kept in same_facts,
        so that "not in the list" reads as "checked and equal" and not as "not looked at".
        """
        rows = []
        self.same_facts = []

        def compare(what, left, right):
            if left != right:
                rows.append((what, left, right))
            else:
                self.same_facts.append(what)

        unknown = "not recorded (no state file in this run)"
        compare("driver version",
                "0x%08X" % self.a.driver_version if self.a.driver_version else "unknown",
                "0x%08X" % self.b.driver_version if self.b.driver_version else "unknown")
        compare("package (DriverVer)", self.a.package or unknown, self.b.package or unknown)
        compare("UserModeDriverName", self.a.umd or unknown, self.b.umd or unknown)

        # Gate by gate only when both runs have a state file. With one side missing it,
        # seven rows of "not recorded" would read like seven differences.
        if self.a.gates and self.b.gates:
            for gate in sorted(set(self.a.gates) | set(self.b.gates)):
                compare("gate %s" % gate, self.a.gates.get(gate, "not in this run's state file"),
                        self.b.gates.get(gate, "not in this run's state file"))
        else:
            compare("gates", ("  ".join("%s %d" % kv for kv in self.a.gates.items())
                              or "%s; the log says EnableFullWddm %s" % (unknown, self.a.full_wddm)),
                    ("  ".join("%s %d" % kv for kv in self.b.gates.items())
                     or "%s; the log says EnableFullWddm %s" % (unknown, self.b.full_wddm)))

        caps_a = dict(self.a.caps.data["fields"]) if self.a.caps else {}
        caps_b = dict(self.b.caps.data["fields"]) if self.b.caps else {}
        if self.a.caps and self.b.caps:
            compare("DRIVERCAPS bytes", self.a.caps.data["bytes"], self.b.caps.data["bytes"])
        for key in sorted(set(caps_a) | set(caps_b)):
            compare("DRIVERCAPS %s" % key, caps_a.get(key, "not in this build's line"),
                    caps_b.get(key, "not in this build's line"))

        compare("last stage", stage_label(self.a.last_stage) if self.a.last_stage else "none",
                stage_label(self.b.last_stage) if self.b.last_stage else "none")
        compare("PnP problem",
                cm_prob_label(self.a.problem) if self.a.problem else unknown,
                cm_prob_label(self.b.problem) if self.b.problem else unknown)
        compare("QueryAdapterInfo types",
                " ".join(str(e.data["type"]) for e in self.a.qai_calls) or "none",
                " ".join(str(e.data["type"]) for e in self.b.qai_calls) or "none")

        objects_a, alive_a = self.a.objects_summary
        objects_b, alive_b = self.b.objects_summary
        if objects_a or objects_b:
            compare("objects created/destroyed",
                    ", ".join("%s %d/%d" % (k, v[0], v[1]) for k, v in (objects_a or {}).items()) or "none",
                    ", ".join("%s %d/%d" % (k, v[0], v[1]) for k, v in (objects_b or {}).items()) or "none")

        life_a, life_b = self.a.lifecycle, self.b.lifecycle
        if life_a or life_b:
            compare("object graph symmetric", self._symmetry(life_a), self._symmetry(life_b))
            compare("object graph lifetime",
                    "%.3f s" % life_a.span[2] if life_a.span[2] is not None else "no object",
                    "%.3f s" % life_b.span[2] if life_b.span[2] is not None else "no object")
            compare("CreateContext",
                    life_a.describe_create(life_a.context_create) if life_a.context_create
                    else "never called",
                    life_b.describe_create(life_b.context_create) if life_b.context_create
                    else "never called")
            fields_a = dict(life_a.answer_fields)
            fields_b = dict(life_b.answer_fields)
            for field in sorted(set(fields_a) | set(fields_b)):
                compare("CreateContext %s" % field, fields_a.get(field, "not answered"),
                        fields_b.get(field, "not answered"))
        compare("first DDI of the next stage not called",
                expectation_note(*self.a.lifecycle.next_expected),
                expectation_note(*self.b.lifecycle.next_expected))
        # Whether the scheduler timed the adapter out, not how the build worded it: the
        # "no TDR (...)" summary gained ResetEngine between 0.7.3 and today's source.
        compare("TDR",
                "; ".join(e.text for e in self.a.tdr_entries) or
                ("none" if self.a.tdr_note else "no TDR line in the log"),
                "; ".join(e.text for e in self.b.tdr_entries) or
                ("none" if self.b.tdr_note else "no TDR line in the log"))

        stop_a, stop_b = self.a.stop_after_start, self.b.stop_after_start
        compare("StopDevice after stage 39",
                "%.3f s" % stop_a if stop_a is not None else "not in this log",
                "%.3f s" % stop_b if stop_b is not None else "not in this log")
        return rows

    def to_json(self):
        return {
            "a": self.a.name,
            "b": self.b.name,
            "first_difference": self.sentence(),
            "identical": self.identical,
            "context": describe(self.context, short=True) if self.context else None,
            "a_next": describe(self.first_a) if self.first_a else None,
            "b_next": describe(self.first_b) if self.first_b else None,
            "differing_facts": [{"fact": f, "a": str(x), "b": str(y)} for f, x, y in self.facts()],
            "identical_facts": list(self.same_facts),
            "lifecycle_a": lifecycle_json(self.a),
            "lifecycle_b": lifecycle_json(self.b),
            "sequence_a": [e.key for e in self.seq_a],
            "sequence_b": [e.key for e in self.seq_b],
        }


def lifecycle_side_by_side(diff):
    """The object graph of the two runs next to each other, one row per fact."""
    life_a, life_b = diff.a.lifecycle, diff.b.lifecycle
    rows = []
    for kind, _, _, _ in LIFECYCLE_KINDS:
        created_a, destroyed_a = life_a.counts[kind]
        created_b, destroyed_b = life_b.counts[kind]
        if not (created_a or destroyed_a or created_b or destroyed_b):
            continue
        rows.append((kind, "%d created, %d destroyed" % (created_a, destroyed_a),
                     "%d created, %d destroyed" % (created_b, destroyed_b)))
    rows.append(("symmetric", diff._symmetry(life_a), diff._symmetry(life_b)))
    rows.append(("graph lived",
                 "%.3f s" % life_a.span[2] if life_a.span[2] is not None else "no object",
                 "%.3f s" % life_b.span[2] if life_b.span[2] is not None else "no object"))
    rows.append(("CreateContext flags",
                 life_a.describe_create(life_a.context_create) if life_a.context_create
                 else "never called",
                 life_b.describe_create(life_b.context_create) if life_b.context_create
                 else "never called"))
    fields_a, fields_b = dict(life_a.answer_fields), dict(life_b.answer_fields)
    for field in [name for name, _ in (life_a.answer_fields or life_b.answer_fields)]:
        rows.append(("answered " + field, fields_a.get(field, "not answered"),
                     fields_b.get(field, "not answered")))
    rows.append(("next DDI not called",
                 (life_a.next_expected[0] or "none left in the table"),
                 (life_b.next_expected[0] or "none left in the table")))

    label = max(len(what) for what, _, _ in rows)
    width = max([len(left) for _, left, _ in rows] + [len(diff.a.name) + 3])
    width = min(width, 46)

    def fit(text):
        return text if len(text) <= width else text[:width - 3] + "..."

    lines = ["  %-*s %-*s   %s" % (label, "", width, "A: " + diff.a.name, "B: " + diff.b.name)]
    for what, left, right in rows:
        mark = " " if left == right else "*"
        lines.append(("  %-*s %-*s %s %s" % (label, what, width, fit(left), mark, right)).rstrip())
    return lines


def render_diff(diff, markdown=False):
    lines = []
    if markdown:
        lines.append("# runcompare: %s vs %s" % (diff.a.name, diff.b.name))
        lines.append("")
        lines.append("**%s**" % diff.sentence())
        lines.append("")
        lines.append("## Aligned sequences")
        lines.append("")
        lines.append("`=` identical, `~` aligned but worded differently, `A`/`B` only that run, "
                     "`*` both and different.")
        lines.append("")
        lines.append("```")
        lines.extend(diff.side_by_side())
        lines.append("```")
        lines.append("")
        lines.append("## Object lifecycle")
        lines.append("")
        lines.append("`*` marks a row where the two runs differ.")
        lines.append("")
        lines.append("```")
        lines.extend(lifecycle_side_by_side(diff))
        lines.append("```")
        lines.append("")
        lines.append("The expected-DDI row comes from `EXPECTED_AFTER_SYSTEM_CONTEXT` in "
                     "`runcompare.py`, which is a reading of the driver's own DDI table and "
                     "the WDK, %s." % ("confirmed by " + EXPECTED_ORDER_OBSERVED
                                       if EXPECTED_ORDER_OBSERVED
                                       else "observed on no run yet"))
        lines.append("")
        lines.append("## Differing facts")
        lines.append("")
        rows = diff.facts()
        if not rows:
            lines.append("None: every fact below was compared and matched.")
        else:
            lines.append("| fact | A: %s | B: %s |" % (diff.a.name, diff.b.name))
            lines.append("|---|---|---|")
            for fact, left, right in rows:
                lines.append("| %s | %s | %s |" % (fact, left, right))
        lines.append("")
        lines.append("Compared and equal: %s." % ", ".join(diff.same_facts))
        lines.append("")
        return "\n".join(lines)

    lines.append(diff.sentence())
    lines.append("")
    lines.append("aligned sequences  (= identical, ~ aligned but worded differently, "
                 "A only, B only, * both and different)")
    lines.append("")
    lines.extend(diff.side_by_side())
    lines.append("")
    lines.append("object lifecycle  (* the two runs differ on this row)")
    lines.append("")
    lines.extend(lifecycle_side_by_side(diff))
    lines.append("")
    lines.append("differing facts")
    lines.append("")
    rows = diff.facts()
    if not rows:
        lines.append("  none: every fact below was compared and matched.")
    for fact, left, right in rows:
        lines.append("  %-28s A %s" % (fact, left))
        lines.append("  %-28s B %s" % ("", right))
    lines.append("")
    lines.append("  compared and equal: %s." % ", ".join(diff.same_facts))
    lines.append("")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# normalize
# ---------------------------------------------------------------------------

RE_NORM_HEAD_COUNT = re.compile(r"^(bc250kmd\s+0x[0-9A-Fa-f]+\s+log kept at the stop:\s+)\d+( lines,)")
RE_NORM_HEAD_LIVE = re.compile(r"^(log\s+)\d+( lines since this driver load,)")


def normalize_log(path):
    return normalize_log_lines(read_lines(path), name=path)


def normalize_log_lines(lines, name="ring.log"):
    """The log with everything that says when and where it happened taken out.

    What goes: the sequence number, the timestamp, the header's line count, addresses
    and pointer-like values, and the padding the driver's %-30s columns produce. What
    stays: every type, size, status, flag and count. See README.md for the full list.
    """
    ring = RingLog(name, lines)
    lines = []
    if ring.header_text:
        head = RE_NORM_HEAD_COUNT.sub(r"\1<n>\2", ring.header_text)
        head = RE_NORM_HEAD_LIVE.sub(r"\1<n>\2", head)
        lines.append(head)
    for entry in ring.entries:
        lines.append(entry.key)
    for line in ring.unparsed:
        lines.append(re.sub(r"\s{2,}", " ", line))
    return lines


# ---------------------------------------------------------------------------
# command line
# ---------------------------------------------------------------------------

def main(argv=None):
    parser = argparse.ArgumentParser(
        prog="runcompare",
        description="compare two lab runs of bc250kmd and name the first difference")
    parser.add_argument("--tables", action="store_true",
                        help="print which headers the name tables came from and exit")
    sub = parser.add_subparsers(dest="command")

    p_show = sub.add_parser("show", help="the facts of one run")
    p_show.add_argument("run")
    p_show.add_argument("--ring", help="use this ring log instead of the full-table one")
    p_show.add_argument("--state", help="use this state file instead of the matching one")
    p_show.add_argument("--json", action="store_true")
    p_show.add_argument("--markdown", action="store_true")

    p_diff = sub.add_parser("diff", help="the first difference between two runs")
    p_diff.add_argument("run_a")
    p_diff.add_argument("run_b")
    p_diff.add_argument("--ring-a")
    p_diff.add_argument("--ring-b")
    p_diff.add_argument("--json", action="store_true")
    p_diff.add_argument("--markdown", action="store_true")

    p_norm = sub.add_parser("normalize", help="the normalized log alone")
    p_norm.add_argument("log")

    args = parser.parse_args(argv)

    if args.tables:
        for name, where, version in HEADER_SOURCES:
            print("%-12s %s  (%s)" % (name, where, version))
        return 0

    if args.command == "show":
        run = load_run(args.run, args.ring, args.state)
        if args.json:
            print(json.dumps(show_json(run), indent=2))
        else:
            print(render_show(show_report(run), markdown=args.markdown))
        return 0

    if args.command == "diff":
        a = load_run(args.run_a, args.ring_a)
        b = load_run(args.run_b, args.ring_b)
        diff = Difference(a, b)
        if args.json:
            print(json.dumps(diff.to_json(), indent=2))
        else:
            print(render_diff(diff, markdown=args.markdown))
        return 0

    if args.command == "normalize":
        for line in normalize_log(args.log):
            print(line)
        return 0

    parser.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
