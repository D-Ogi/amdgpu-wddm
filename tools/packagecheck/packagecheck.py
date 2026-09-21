#!/usr/bin/env python3
"""packagecheck - read a finished bc250kmd driver package and say whether it is the package we think it is.

Run it on the package directory before every install on the lab. It answers the questions that cost us
lab runs, in the order in which they bite:

    is the INF the one we meant to ship (hardware id, class, service, DriverVer),
    does a full WDDM package register a user-mode driver at all (facts M64),
    does that user-mode driver export the three names the Direct3D runtimes look up, and - with
        --load - does it actually answer E_NOTIMPL when they are called,
    is every file the INF names actually there, and is every file there named by the INF,
    is the .sys inside this package the .sys of this version, or a stale one from two builds ago,
    is the catalog this package's catalog.

E16 runs 001 and 002 were spent measuring a package whose INF had no UserModeDriverName. dxgkrnl cannot
start a full (non compute-only) WDDM adapter without it, so that package could never have passed, and the
two runs measured nothing. `--model full` makes that a hard error here instead of a day on the bench.

Usage:
    python tools/packagecheck/packagecheck.py <package-dir>
                                              [--model full|display-only]
                                              [--expect-version 0.7.4] [--expect-commit <hash>]
                                              [--load]
                                              [--json out.json] [--markdown out.md] [--manifest out.txt]
                                              [--kits DIR] [--no-tools] [-v]

Exit code: 0 when nothing failed, 1 when any check produced an error, 2 on a usage or I/O problem.

Nothing here writes into the package. Inf2Cat, which can only work by producing a .cat, is run on a copy
in a scratch directory (never on C:, see the workspace rules), and only its output is compared.

Python 3 standard library only. The PE and INF parsing is done here, by hand, on purpose: the tool has to
run on a machine with nothing installed but Python and the unpacked NuGet kits.
"""

import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

TOOL_VERSION = "1.0"

REPO = Path(__file__).resolve().parents[2]          # ...\bc250-win
WORKSPACE = REPO.parent                             # ...\BC-250

# What a bc250kmd package must say about itself. These are not guesses: the class GUID is the Windows
# display class, the hardware id is the one line in docs/hardware.md that identifies our part.
DISPLAY_CLASS = "display"
DISPLAY_CLASS_GUID = "{4d36e968-e325-11ce-bfc1-08002be10318}"
HARDWARE_ID = r"PCI\VEN_1002&DEV_13FE"

REG_SZ = 0x00000000
REG_MULTI_SZ = 0x00010000

# The three entry points the Direct3D runtimes look up by name in a user-mode display driver, in the order
# the INF's REG_MULTI_SZ lists them: D3D9, D3D10, D3D11. driver\umd-stub\bc250umd.c exports them through
# driver\umd-stub\bc250umd.def (the linker's /DEF: switch - the source has no __declspec(dllexport)), and
# the runtime does GetProcAddress by exactly these strings, so a decorated name is a name it cannot find.
UMD_REQUIRED_EXPORTS = ("OpenAdapter", "OpenAdapter10", "OpenAdapter10_2")
E_NOTIMPL = 0x80004001

# The argument block passed to the entry points by --load. Both D3DDDIARG_OPENADAPTER (d3dumddi.h) and
# D3D10DDIARG_OPENADAPTER (d3d10umddi.h) are well under 128 bytes on x64; a zeroed page is far more than
# either, so the stub - which ignores its argument entirely - cannot read past the end of it.
OPENADAPTER_ARG_BYTES = 4096

IMAGE_FILE_MACHINE_AMD64 = 0x8664
IMAGE_FILE_MACHINE_I386 = 0x014C
IMAGE_FILE_MACHINE_ARM64 = 0xAA64
MACHINE_NAMES = {IMAGE_FILE_MACHINE_AMD64: "AMD64", IMAGE_FILE_MACHINE_I386: "x86",
                 IMAGE_FILE_MACHINE_ARM64: "ARM64"}
IMAGE_FILE_DLL = 0x2000
SUBSYSTEM_NATIVE = 1
SUBSYSTEM_GUI = 2
SUBSYSTEM_CUI = 3
SUBSYSTEM_NAMES = {1: "NATIVE", 2: "WINDOWS_GUI", 3: "WINDOWS_CUI"}
IMAGE_SCN_MEM_EXECUTE = 0x20000000

# Files a package may hold that the INF does not name. The .cat is named by CatalogFile but is not a copied
# file; the .cer is the test certificate, carried for the lab's convenience and deliberately not catalogued.
KNOWN_EXTRAS = {".cat", ".cer", ".json"}

ERROR, WARN, NOTE, OK = "error", "warn", "note", "ok"
LEVEL_RANK = {ERROR: 0, WARN: 1, NOTE: 2, OK: 3}
LEVEL_TAG = {ERROR: "ERROR", WARN: "WARN ", NOTE: "note ", OK: "ok   "}


# ---------------------------------------------------------------------------------------------------
# findings
# ---------------------------------------------------------------------------------------------------

@dataclass
class Finding:
    group: str
    level: str
    code: str
    text: str
    detail: str = ""

    def as_dict(self):
        d = {"group": self.group, "level": self.level, "code": self.code, "text": self.text}
        if self.detail:
            d["detail"] = self.detail
        return d


class Report:
    """Findings in the order they were made, plus the facts the report prints."""

    def __init__(self):
        self.findings = []
        self.facts = {}

    def add(self, group, level, code, text, detail=""):
        self.findings.append(Finding(group, level, code, text, detail))

    def error(self, group, code, text, detail=""):
        self.add(group, ERROR, code, text, detail)

    def warn(self, group, code, text, detail=""):
        self.add(group, WARN, code, text, detail)

    def note(self, group, code, text, detail=""):
        self.add(group, NOTE, code, text, detail)

    def ok(self, group, code, text, detail=""):
        self.add(group, OK, code, text, detail)

    def count(self, level):
        return sum(1 for f in self.findings if f.level == level)

    @property
    def failed(self):
        return self.count(ERROR) > 0


# ---------------------------------------------------------------------------------------------------
# INF parsing
# ---------------------------------------------------------------------------------------------------

def decode_inf(raw):
    """Decode an INF the way setupapi does: BOM first, ANSI when there is none.

    Returns (encoding label, text, ascii_only). A UTF-8 INF without a BOM is read by setupapi as ANSI,
    so non-ASCII bytes in one are worth a note even though our own INF only puts them in comments.
    """
    if raw.startswith(b"\xef\xbb\xbf"):
        return "utf-8 (BOM)", raw[3:].decode("utf-8", errors="replace"), raw[3:].isascii()
    if raw.startswith(b"\xff\xfe") and not raw.startswith(b"\xff\xfe\x00\x00"):
        return "utf-16-le (BOM)", raw[2:].decode("utf-16-le", errors="replace"), True
    if raw.startswith(b"\xfe\xff"):
        return "utf-16-be (BOM)", raw[2:].decode("utf-16-be", errors="replace"), True
    try:
        return "utf-8 (no BOM)", raw.decode("utf-8"), raw.isascii()
    except UnicodeDecodeError:
        return "cp1252 (no BOM)", raw.decode("cp1252", errors="replace"), False


def strip_comment(line):
    """Remove an INF comment. A ';' inside double quotes is not a comment."""
    quoted = False
    for i, ch in enumerate(line):
        if ch == '"':
            quoted = not quoted
        elif ch == ";" and not quoted:
            return line[:i]
    return line


def split_fields(text):
    """Split an INF value list on commas that are not inside double quotes."""
    out, cur, quoted = [], "", False
    for ch in text:
        if ch == '"':
            quoted = not quoted
            cur += ch
        elif ch == "," and not quoted:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    out.append(cur.strip())
    return out


def unquote(value):
    if len(value) >= 2 and value.startswith('"') and value.endswith('"'):
        return value[1:-1]
    return value


@dataclass
class InfLine:
    section: str
    number: int                 # 1-based, the first physical line of a continued line
    key: str                    # text left of '=', '' when the line has no '='
    values: list = field(default_factory=list)
    raw: str = ""


class InfFile:
    """Enough of an INF parser for a driver package: sections, continuations, comments, %strings%."""

    def __init__(self, path):
        self.path = Path(path)
        raw = self.path.read_bytes()
        self.encoding, text, self.ascii_only = decode_inf(raw)
        self.sections = {}                  # lower-case name -> list[InfLine]
        self.section_names = {}             # lower-case name -> name as written
        self.nonascii_lines = []
        self.parse(text)
        self.strings = self._strings()

    def parse(self, text):
        physical = text.replace("\r\n", "\n").replace("\r", "\n").split("\n")
        section = ""
        i = 0
        while i < len(physical):
            start = i + 1
            body = strip_comment(physical[i]).rstrip()
            # Continuation: a backslash as the last non-blank character joins the next line.
            while body.endswith("\\") and i + 1 < len(physical):
                i += 1
                body = body[:-1] + " " + strip_comment(physical[i]).strip()
                body = body.rstrip()
            if not physical[start - 1].isascii():
                self.nonascii_lines.append(start)
            i += 1
            body = body.strip()
            if not body:
                continue
            if body.startswith("[") and body.endswith("]"):
                name = body[1:-1].strip()
                section = name.lower()
                self.sections.setdefault(section, [])
                self.section_names.setdefault(section, name)
                continue
            if "=" in body:
                key, _, rest = body.partition("=")
                line = InfLine(section, start, key.strip(), [unquote(v) for v in split_fields(rest)], body)
            else:
                line = InfLine(section, start, "", [unquote(v) for v in split_fields(body)], body)
            self.sections.setdefault(section, []).append(line)

    def _strings(self):
        out = {}
        for name, lines in self.sections.items():
            if name == "strings" or name.startswith("strings."):
                for line in lines:
                    if line.key:
                        out[line.key.lower()] = line.values[0] if line.values else ""
        return out

    def subst(self, text):
        """%Token% from [Strings]; %% is a literal percent; anything else (a DIRID) is left alone."""
        def repl(m):
            if m.group(0) == "%%":
                return "%"
            token = m.group(1)
            return self.strings.get(token.lower(), m.group(0))
        return re.sub(r"%%|%([^%\n]+)%", repl, text)

    # -- section lookup ------------------------------------------------------------------------------

    def get(self, name):
        return self.sections.get(name.lower(), [])

    def has(self, name):
        return name.lower() in self.sections

    def entry(self, section, key):
        """The last value list for `key` in `section`, or None. Last wins, as setupapi does."""
        found = None
        for line in self.get(section):
            if line.key.lower() == key.lower():
                found = line
        return found

    def value(self, section, key, index=0):
        line = self.entry(section, key)
        if line is None or index >= len(line.values):
            return None
        return self.subst(line.values[index])

    def variants(self, base):
        """`base` and its platform decorations, without the .Services / .HW style suffixes."""
        base = base.lower()
        tail = ("services", "hw", "coinstallers", "interfaces", "logconfigoverride", "wmi", "factdef")
        out = []
        for name in self.sections:
            if name == base:
                out.append(name)
            elif name.startswith(base + "."):
                if name.rsplit(".", 1)[-1] not in tail:
                    out.append(name)
        return out

    def suffixed(self, base, suffix):
        """`base[.decoration].suffix` sections, e.g. Bc250_Install.NTamd64.Services."""
        pattern = re.compile(r"^" + re.escape(base.lower()) + r"(\.[^.]+)*\." + re.escape(suffix.lower()) + r"$")
        return [n for n in self.sections if pattern.match(n)]


# ---------------------------------------------------------------------------------------------------
# PE parsing
# ---------------------------------------------------------------------------------------------------

@dataclass
class PeSection:
    name: str
    vaddr: int
    vsize: int
    raw_off: int
    raw_size: int
    characteristics: int

    @property
    def executable(self):
        return bool(self.characteristics & IMAGE_SCN_MEM_EXECUTE)


class PeImage:
    """The few PE fields a package check needs. Anything malformed lands in .problems, never an exception."""

    def __init__(self, path):
        self.path = Path(path)
        self.data = self.path.read_bytes()
        self.problems = []
        self.valid = False
        self.machine = None
        self.timestamp = None
        self.characteristics = 0
        self.pe32plus = None
        self.subsystem = None
        self.sections = []
        self.directories = []
        self.codeview = None            # (guid string, age, pdb path)
        self.file_version = None        # from VS_FIXEDFILEINFO, when the build embeds one
        self.product_version = None
        self.exports = None             # None when there is no export directory, else a sorted list
        self.export_name = None         # the library name the export directory carries
        try:
            self._parse()
        except (struct.error, IndexError, ValueError) as exc:
            self.problems.append("PE parse failed: %s" % exc)

    def _parse(self):
        d = self.data
        if len(d) < 0x40 or d[:2] != b"MZ":
            self.problems.append("no MZ signature: not a PE image")
            return
        off = struct.unpack_from("<I", d, 0x3C)[0]
        if off + 24 > len(d) or d[off:off + 4] != b"PE\0\0":
            self.problems.append("no PE signature at e_lfanew")
            return
        self.machine, nsections, self.timestamp = struct.unpack_from("<HHI", d, off + 4)
        opt_size = struct.unpack_from("<H", d, off + 20)[0]
        self.characteristics = struct.unpack_from("<H", d, off + 22)[0]
        opt = off + 24
        magic = struct.unpack_from("<H", d, opt)[0]
        self.pe32plus = magic == 0x20B
        if magic not in (0x10B, 0x20B):
            self.problems.append("optional header magic 0x%04X is neither PE32 nor PE32+" % magic)
            return
        self.subsystem = struct.unpack_from("<H", d, opt + (68 if self.pe32plus else 68))[0]
        dir_count_off = opt + (108 if self.pe32plus else 92)
        ndirs = struct.unpack_from("<I", d, dir_count_off)[0]
        dirs_off = dir_count_off + 4
        for i in range(min(ndirs, 16)):
            rva, size = struct.unpack_from("<II", d, dirs_off + 8 * i)
            self.directories.append((rva, size))
        sec_off = opt + opt_size
        for i in range(nsections):
            base = sec_off + 40 * i
            if base + 40 > len(d):
                self.problems.append("section table truncated")
                break
            name = d[base:base + 8].rstrip(b"\0").decode("ascii", errors="replace")
            vsize, vaddr, raw_size, raw_off = struct.unpack_from("<IIII", d, base + 8)
            chars = struct.unpack_from("<I", d, base + 36)[0]
            self.sections.append(PeSection(name, vaddr, vsize, raw_off, raw_size, chars))
        self.valid = True
        self._read_debug_directory()
        self._read_version_resource()
        self._read_exports()

    # -- helpers -------------------------------------------------------------------------------------

    def rva_to_offset(self, rva):
        for s in self.sections:
            if s.vaddr <= rva < s.vaddr + max(s.vsize, s.raw_size):
                delta = rva - s.vaddr
                if delta < s.raw_size:
                    return s.raw_off + delta
        return None

    def directory(self, index):
        if index < len(self.directories):
            return self.directories[index]
        return (0, 0)

    @property
    def is_dll(self):
        return bool(self.characteristics & IMAGE_FILE_DLL)

    @property
    def has_certificate_table(self):
        # Directory 4 is a file offset, not an RVA: an embedded Authenticode signature.
        return self.directory(4)[1] > 0

    @property
    def machine_name(self):
        return MACHINE_NAMES.get(self.machine, "0x%04X" % (self.machine or 0))

    @property
    def subsystem_name(self):
        return SUBSYSTEM_NAMES.get(self.subsystem, str(self.subsystem))

    @property
    def timestamp_utc(self):
        if not self.timestamp:
            return None
        try:
            return datetime.datetime.fromtimestamp(self.timestamp, datetime.timezone.utc)
        except (OverflowError, OSError, ValueError):
            return None

    def _string_at_rva(self, rva, limit=512):
        off = self.rva_to_offset(rva)
        if off is None:
            return None
        end = self.data.find(b"\0", off, off + limit)
        if end < 0:
            return None
        return self.data[off:end].decode("ascii", errors="replace")

    def _read_exports(self):
        """The export name table. Names only: what the Direct3D runtime does is GetProcAddress by name,
        so the ordinals and addresses do not decide whether a lookup succeeds."""
        rva, size = self.directory(0)
        if not size:
            return
        off = self.rva_to_offset(rva)
        if off is None or off + 40 > len(self.data):
            return
        (_chars, _stamp, _major, _minor, name_rva, _base,
         _n_funcs, n_names, _funcs_rva, names_rva, _ords_rva) = struct.unpack_from("<IIHHIIIIIII", self.data, off)
        self.export_name = self._string_at_rva(name_rva)
        names = []
        table = self.rva_to_offset(names_rva)
        if table is None:
            self.exports = []
            return
        for i in range(min(n_names, 4096)):
            if table + 4 * i + 4 > len(self.data):
                break
            entry = struct.unpack_from("<I", self.data, table + 4 * i)[0]
            text = self._string_at_rva(entry)
            if text is not None:
                names.append(text)
        self.exports = sorted(names)

    def _read_debug_directory(self):
        rva, size = self.directory(6)
        if not size:
            return
        off = self.rva_to_offset(rva)
        if off is None:
            return
        for i in range(size // 28):
            base = off + 28 * i
            if base + 28 > len(self.data):
                return
            dtype = struct.unpack_from("<I", self.data, base + 12)[0]
            data_size, data_rva, data_off = struct.unpack_from("<III", self.data, base + 16)
            if dtype != 2:                      # IMAGE_DEBUG_TYPE_CODEVIEW
                continue
            cv = self.data[data_off:data_off + data_size]
            if len(cv) < 25 or cv[:4] != b"RSDS":
                continue
            d1, d2, d3 = struct.unpack_from("<IHH", cv, 4)
            rest = cv[12:20]
            guid = "%08X-%04X-%04X-%s-%s" % (d1, d2, d3, rest[:2].hex().upper(), rest[2:].hex().upper())
            age = struct.unpack_from("<I", cv, 20)[0]
            pdb = cv[24:].split(b"\0")[0].decode("utf-8", errors="replace")
            self.codeview = (guid, age, pdb)
            return

    def _read_version_resource(self):
        """VS_FIXEDFILEINFO, when the build embeds a version resource. Ours does not (no rc.exe in
        build.ps1), so this stays None and the report says so rather than pretending the check ran."""
        rva, size = self.directory(2)
        if not size:
            return
        root = self.rva_to_offset(rva)
        if root is None:
            return
        try:
            data_rva = self._find_resource(root, root, [16, None, None])   # RT_VERSION
        except (struct.error, IndexError):
            return
        if data_rva is None:
            return
        off = self.rva_to_offset(data_rva)
        if off is None:
            return
        blob = self.data[off:off + 4096]
        sig = blob.find(struct.pack("<I", 0xFEEF04BD))
        if sig < 0:
            return
        ms, ls, pms, pls = struct.unpack_from("<IIII", blob, sig + 8)
        self.file_version = "%d.%d.%d.%d" % (ms >> 16, ms & 0xFFFF, ls >> 16, ls & 0xFFFF)
        self.product_version = "%d.%d.%d.%d" % (pms >> 16, pms & 0xFFFF, pls >> 16, pls & 0xFFFF)

    def _find_resource(self, root, node, path):
        """Walk the three resource levels; `path` entries are an id to match or None for 'the first'."""
        nnamed, nid = struct.unpack_from("<HH", self.data, node + 12)
        for i in range(nnamed + nid):
            base = node + 16 + 8 * i
            name, offset = struct.unpack_from("<II", self.data, base)
            want = path[0]
            if want is not None and not (name & 0x80000000) and name != want:
                continue
            if offset & 0x80000000:
                child = root + (offset & 0x7FFFFFFF)
                found = self._find_resource(root, child, path[1:] if len(path) > 1 else [None])
                if found is not None:
                    return found
            else:
                return struct.unpack_from("<I", self.data, root + offset)[0]
        return None

    def scan_executable(self, needle):
        """Offsets of `needle` inside executable sections. How the version dword is found: it is an
        immediate operand in code, not a string, so only .text/INIT/PAGE are worth searching."""
        hits = []
        for s in self.sections:
            if not s.executable:
                continue
            blob = self.data[s.raw_off:s.raw_off + s.raw_size]
            i = blob.find(needle)
            while i >= 0:
                hits.append(s.raw_off + i)
                i = blob.find(needle, i + 1)
        return hits


# ---------------------------------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------------------------------

def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def version_dword(parts):
    """The BC250_KMD_VERSION encoding: 0.7.4 is 0x00070004 (bc250kmd_escape.h)."""
    major, minor, rev = parts[0], parts[1], parts[2]
    if major > 0xFF or minor > 0xFF or rev > 0xFFFF:
        return None
    return (major << 24) | (minor << 16) | rev


def parse_version(text):
    """'0.7.4.1' -> [0, 7, 4, 1]; None when it is not a dotted number."""
    if not text or not re.fullmatch(r"\d+(\.\d+)*", text):
        return None
    return [int(p) for p in text.split(".")]


def scratch_root(explicit=None):
    """Where a temporary copy may go. Never C: - the workspace rules keep the development drive free."""
    if explicit:
        return Path(explicit)
    env = os.environ.get("BC250_TMP")
    if env:
        return Path(env)
    candidate = WORKSPACE / "scratch" / "tmp"
    if candidate.is_dir():
        return candidate
    return Path(tempfile.gettempdir())


def find_tool(kits, name, fast_paths):
    """A kit tool, the way driver\\kmd\\build.ps1 finds one: a known path first, a search as fallback."""
    kits = Path(kits)
    if not kits.is_dir():
        return None
    for pattern in fast_paths:
        matches = sorted(kits.glob(pattern))
        if matches:
            return matches[-1]
    matches = sorted(kits.rglob(name))
    return matches[-1] if matches else None


def run(argv, timeout=180):
    try:
        proc = subprocess.run([str(a) for a in argv], capture_output=True, text=True,
                              errors="replace", timeout=timeout)
    except (OSError, subprocess.SubprocessError) as exc:
        return None, str(exc)
    out = (proc.stdout or "") + (proc.stderr or "")
    return proc.returncode, out.strip()


def catalog_member_names(path):
    """File names a .cat carries, read out of its UTF-16LE member tags. Informational: the authoritative
    membership test is signtool, which checks hashes. This one still catches 'the catalog was not rebuilt'."""
    try:
        data = Path(path).read_bytes()
    except OSError:
        return set()
    names = set()
    for run_bytes in re.findall(rb"(?:[\x20-\x7e]\x00){4,}", data):
        text = run_bytes.decode("utf-16-le")
        if re.fullmatch(r"[\w.+\- ]+\.[A-Za-z0-9]{1,8}", text):
            names.add(text.lower())
    return names


# ---------------------------------------------------------------------------------------------------
# the checks
# ---------------------------------------------------------------------------------------------------

class PackageCheck:
    def __init__(self, directory, model=None, expect_version=None, expect_commit=None,
                 kits=None, use_tools=True, tmp=None, load=False):
        self.dir = Path(directory).resolve()
        self.model = model
        self.expect_version = expect_version
        self.expect_commit = expect_commit
        self.kits = Path(kits) if kits else (WORKSPACE / "toolchain" / "nuget")
        self.use_tools = use_tools
        self.tmp = tmp
        self.load = load
        self.report = Report()
        self.inf = None
        self.inf_path = None
        self.files = {}                 # lower-case name -> Path
        self.copy_files = {}            # lower-case name -> (section, dirid)
        self.source_disks = {}          # lower-case name -> disk id
        self.referenced = set()         # lower-case names the INF names as files
        self.images = {}                # lower-case name -> PeImage
        self.inf_version = None         # [a, b, c, d] from DriverVer
        self.buildinfo = None

    # -- entry point ---------------------------------------------------------------------------------

    def run_all(self):
        r = self.report
        if not self.dir.is_dir():
            r.error("package", "PKG001", "not a directory: %s" % self.dir)
            return r
        self.collect_files()
        if not self.find_inf():
            return r
        self.check_version_section()
        self.check_hardware_id()
        self.check_service()
        self.check_copyfiles()
        self.decide_model()
        self.check_umd()
        self.check_unreferenced()
        self.check_pe()
        self.check_exports()
        self.check_load()
        self.check_driver_version()
        self.check_buildinfo()
        self.check_catalog()
        self.check_infverif()
        self.check_inf2cat()
        return r

    # -- files ---------------------------------------------------------------------------------------

    def collect_files(self):
        entries = sorted(p for p in self.dir.iterdir() if p.is_file())
        for p in entries:
            self.files[p.name.lower()] = p
        self.report.facts["files"] = [
            {"name": p.name, "size": p.stat().st_size, "sha256": sha256_file(p)} for p in entries
        ]
        subdirs = [p.name for p in self.dir.iterdir() if p.is_dir()]
        if subdirs:
            self.report.note("package", "PKG002",
                             "the package directory has subdirectories; nothing here looks inside them",
                             ", ".join(sorted(subdirs)))
        self.report.facts["package"] = str(self.dir)

    def find_inf(self):
        infs = [p for n, p in sorted(self.files.items()) if n.endswith(".inf")]
        if not infs:
            self.report.error("inf", "INF001", "no .inf in the package directory")
            return False
        if len(infs) > 1:
            self.report.error("inf", "INF002", "more than one .inf in the package directory",
                              ", ".join(p.name for p in infs))
            return False
        self.inf_path = infs[0]
        try:
            self.inf = InfFile(self.inf_path)
        except OSError as exc:
            self.report.error("inf", "INF003", "cannot read %s: %s" % (self.inf_path.name, exc))
            return False
        self.referenced.add(self.inf_path.name.lower())
        self.report.facts["inf"] = self.inf_path.name
        self.report.facts["inf_encoding"] = self.inf.encoding
        self.report.ok("inf", "INF004", "%s parsed, %d sections, %s"
                       % (self.inf_path.name, len(self.inf.sections), self.inf.encoding))
        if not self.inf.ascii_only and "BOM" not in self.inf.encoding:
            self.report.note("inf", "INF005",
                             "non-ASCII bytes and no BOM: setupapi reads this file as ANSI",
                             "lines " + ", ".join(str(n) for n in self.inf.nonascii_lines[:10]))
        return True

    # -- [Version] -----------------------------------------------------------------------------------

    def check_version_section(self):
        inf, r = self.inf, self.report
        if not inf.has("version"):
            r.error("inf", "VER001", "no [Version] section")
            return
        signature = inf.value("version", "Signature")
        if signature is None:
            r.error("inf", "VER002", "[Version] has no Signature")
        elif unquote(signature).lower() != "$windows nt$":
            r.error("inf", "VER003", "Signature is %r, expected \"$Windows NT$\"" % signature)

        klass = inf.value("version", "Class")
        guid = inf.value("version", "ClassGuid")
        if klass is None or klass.lower() != DISPLAY_CLASS:
            r.error("inf", "VER004", "Class is %r, expected Display" % klass)
        if guid is None or guid.strip().lower() != DISPLAY_CLASS_GUID:
            r.error("inf", "VER005", "ClassGuid is %r, expected %s" % (guid, DISPLAY_CLASS_GUID))
        if klass and guid and klass.lower() == DISPLAY_CLASS and guid.strip().lower() == DISPLAY_CLASS_GUID:
            r.ok("inf", "VER006", "Class Display %s" % DISPLAY_CLASS_GUID)

        catalog = inf.value("version", "CatalogFile")
        if not catalog:
            r.error("inf", "VER007", "[Version] has no CatalogFile: the package cannot be signed as a package")
        else:
            self.referenced.add(catalog.lower())
            r.facts["catalog"] = catalog
            if catalog.lower() not in self.files:
                r.error("inf", "VER008", "CatalogFile %s is not in the directory" % catalog)
            else:
                r.ok("inf", "VER009", "CatalogFile %s present" % catalog)

        driver_ver = inf.entry("version", "DriverVer")
        if driver_ver is None:
            r.error("inf", "VER010", "[Version] has no DriverVer")
        else:
            fields = driver_ver.values
            date = fields[0] if fields else ""
            version = fields[1] if len(fields) > 1 else ""
            if not re.fullmatch(r"\d{2}/\d{2}/\d{4}", date.strip()):
                r.error("inf", "VER011", "DriverVer date %r is not MM/DD/YYYY" % date)
            self.inf_version = parse_version(version.strip())
            if self.inf_version is None:
                r.error("inf", "VER012", "DriverVer has no a.b.c.d version: %r" % driver_ver.raw)
            else:
                r.facts["driver_ver"] = "%s,%s" % (date.strip(), version.strip())
                r.ok("inf", "VER013", "DriverVer %s,%s" % (date.strip(), version.strip()))

        lockdown = inf.value("version", "PnpLockdown")
        if lockdown != "1":
            r.note("inf", "VER014", "PnpLockdown is %r, not 1" % lockdown)
        provider = inf.value("version", "Provider")
        if provider:
            r.facts["provider"] = provider

    # -- hardware id and the install section -----------------------------------------------------------

    def check_hardware_id(self):
        inf, r = self.inf, self.report
        manufacturer = inf.get("manufacturer")
        if not manufacturer:
            r.error("inf", "HW001", "no [Manufacturer] section")
            return
        models_sections = []
        for line in manufacturer:
            if not line.values:
                continue
            base = line.values[0]
            for dec in line.values[1:] or [""]:
                name = (base + "." + dec) if dec else base
                if inf.has(name):
                    models_sections.append(name)
            if inf.has(base) and base.lower() not in [m.lower() for m in models_sections]:
                models_sections.append(base)
        if not models_sections:
            r.error("inf", "HW002", "[Manufacturer] names no models section that exists in this INF")
            return
        r.facts["models_sections"] = models_sections

        hardware_ids = []
        installs = []
        for section in models_sections:
            for line in inf.get(section):
                if len(line.values) < 2:
                    r.warn("inf", "HW003", "[%s] line %d names no hardware id" % (section, line.number),
                           line.raw)
                    continue
                installs.append(line.values[0])
                hardware_ids.extend(v for v in line.values[1:])
        r.facts["hardware_ids"] = hardware_ids
        r.facts["install_sections"] = installs
        if any(h.strip().lower() == HARDWARE_ID.lower() for h in hardware_ids):
            r.ok("inf", "HW004", "hardware id %s present" % HARDWARE_ID)
        else:
            r.error("inf", "HW005", "hardware id %s is not in the models section" % HARDWARE_ID,
                    "found: " + (", ".join(hardware_ids) or "none"))
        extra = [h for h in hardware_ids if h.strip().lower() != HARDWARE_ID.lower()]
        if extra:
            r.warn("inf", "HW006", "the INF also matches %d other device(s)" % len(extra), ", ".join(extra))
        self.installs = installs

    def install_section_names(self):
        """Every section an install directive may live in: the DDInstall section and its decorations."""
        out = []
        for base in getattr(self, "installs", []):
            out.extend(self.inf.variants(base))
        return out

    # -- service -------------------------------------------------------------------------------------

    def check_service(self):
        inf, r = self.inf, self.report
        services = []
        for base in getattr(self, "installs", []):
            services.extend(inf.suffixed(base, "services"))
        if not services:
            r.error("inf", "SRV001", "no <install>.Services section: nothing installs a driver service")
            return
        for section in services:
            add = [l for l in inf.get(section) if l.key.lower() == "addservice"]
            if not add:
                r.error("inf", "SRV002", "[%s] has no AddService" % inf.section_names[section])
                continue
            for line in add:
                name = line.values[0] if line.values else ""
                flags = line.values[1] if len(line.values) > 1 else ""
                install = line.values[2] if len(line.values) > 2 else ""
                r.facts["service"] = name
                try:
                    flag_value = int(flags, 0)
                except ValueError:
                    flag_value = None
                if flag_value is not None and not flag_value & 0x00000002:
                    r.warn("inf", "SRV003",
                           "AddService %s flags %s without SPSVCINST_ASSOCSERVICE (0x2)" % (name, flags))
                if not install or not inf.has(install):
                    r.error("inf", "SRV004", "AddService %s names service-install section %r, which is absent"
                            % (name, install))
                    continue
                self.check_service_install(name, install)

    def check_service_install(self, service, section):
        inf, r = self.inf, self.report
        binary = inf.value(section, "ServiceBinary")
        service_type = inf.value(section, "ServiceType")
        start_type = inf.value(section, "StartType")
        r.facts["service_binary"] = binary
        if not binary:
            r.error("inf", "SRV005", "[%s] has no ServiceBinary" % inf.section_names[section])
            return
        m = re.match(r"^%(\d+)%\\(.+)$", binary.strip())
        if not m:
            r.warn("inf", "SRV006", "ServiceBinary %r is not of the form %%dirid%%\\file" % binary)
            name = Path(binary.replace("\\", "/")).name
            dirid = None
        else:
            dirid, name = m.group(1), m.group(2)
        self.service_binary = name.lower()
        self.referenced.add(name.lower())
        if dirid and dirid not in ("12", "13"):
            r.warn("inf", "SRV007", "ServiceBinary dirid is %s, not 12 (drivers) or 13 (driver store)" % dirid)
        if name.lower() not in self.files:
            r.error("inf", "SRV008", "ServiceBinary names %s, which is not in the directory" % name)
        else:
            r.ok("inf", "SRV009", "ServiceBinary %s present (dirid %s)" % (name, dirid))
        if service_type not in (None, "1"):
            r.note("inf", "SRV010", "ServiceType is %s, not 1 (SERVICE_KERNEL_DRIVER)" % service_type)
        if start_type is not None:
            r.facts["start_type"] = start_type
        if service and self.service_binary and Path(self.service_binary).stem != service.lower():
            r.note("inf", "SRV011", "service name %r and binary %s do not share a stem"
                   % (service, self.service_binary))

    # -- CopyFiles / SourceDisksFiles ------------------------------------------------------------------

    def check_copyfiles(self):
        inf, r = self.inf, self.report

        # [SourceDisksNames] / [SourceDisksFiles]
        disks = set()
        for name in list(inf.sections):
            if name == "sourcedisksnames" or name.startswith("sourcedisksnames."):
                disks.update(l.key for l in inf.get(name))
        for name in list(inf.sections):
            if name == "sourcedisksfiles" or name.startswith("sourcedisksfiles."):
                for line in inf.get(name):
                    if not line.key:
                        continue
                    self.source_disks[line.key.lower()] = line.values[0] if line.values else ""
                    disk = line.values[0] if line.values else ""
                    if disks and disk and disk not in disks:
                        r.error("inf", "CPY001", "[SourceDisksFiles] %s names disk %s, absent from "
                                "[SourceDisksNames]" % (line.key, disk))
        if not self.source_disks:
            r.warn("inf", "CPY002", "no [SourceDisksFiles]: the package lists no source files")

        # dirids per CopyFiles section
        dest = {}
        for line in inf.get("destinationdirs"):
            if line.key:
                dest[line.key.lower()] = line.values[0] if line.values else ""
        default_dest = dest.get("defaultdestdir", "")
        r.facts["default_dest_dir"] = default_dest

        # CopyFiles= directives in every install section variant
        copy_sections = []
        for section in self.install_section_names():
            for line in inf.get(section):
                if line.key.lower() != "copyfiles":
                    continue
                for value in line.values:
                    if value.startswith("@"):
                        # CopyFiles=@file: a single file, no section
                        self.copy_files[value[1:].lower()] = (inf.section_names[section], default_dest)
                        continue
                    copy_sections.append((inf.section_names[section], value))
        r.facts["copyfiles_sections"] = [c[1] for c in copy_sections]
        if not copy_sections and not self.copy_files:
            r.error("inf", "CPY003", "no CopyFiles directive in any install section")

        for owner, section in copy_sections:
            if not inf.has(section):
                r.error("inf", "CPY004", "[%s] CopyFiles names section %r, which is absent" % (owner, section))
                continue
            dirid = dest.get(section.lower(), default_dest)
            for line in inf.get(section):
                name = (line.key or (line.values[0] if line.values else "")).strip()
                if not name:
                    continue
                self.copy_files[name.lower()] = (section, dirid)
            if not inf.get(section):
                r.warn("inf", "CPY005", "[%s] is empty" % section)

        for name, (section, dirid) in sorted(self.copy_files.items()):
            self.referenced.add(name)
            if name not in self.source_disks:
                r.error("inf", "CPY006", "%s is copied by [%s] but not listed in [SourceDisksFiles]"
                        % (name, section))
            if name not in self.files:
                r.error("inf", "CPY007", "%s is copied by [%s] but is not in the directory" % (name, section))
            else:
                r.ok("inf", "CPY008", "%s copied by [%s] to dirid %s, present" % (name, section, dirid or "?"))
        for name in sorted(self.source_disks):
            self.referenced.add(name)
            if name not in self.files:
                r.error("inf", "CPY009", "[SourceDisksFiles] lists %s, which is not in the directory" % name)
            if name not in self.copy_files:
                r.warn("inf", "CPY010", "%s is a source file that no CopyFiles section copies" % name)
        if getattr(self, "service_binary", None) and self.service_binary not in self.copy_files:
            r.error("inf", "CPY011", "ServiceBinary %s is not in any CopyFiles section: the service would "
                    "point at a file the install never copies" % self.service_binary)

    # -- model and the user-mode driver ------------------------------------------------------------------

    def decide_model(self):
        dlls = sorted(n for n in self.files if n.endswith(".dll"))
        if self.model is None:
            self.model = "full" if dlls else "display-only"
            self.report.note("umd", "MOD001", "model not given, inferred %s (%s)"
                             % (self.model, ("DLL in the directory: " + ", ".join(dlls)) if dlls
                                else "no DLL in the directory"))
        self.report.facts["model"] = self.model
        self.report.facts["model_inferred"] = dlls if dlls else []

    def umd_entries(self):
        """AddReg entries named UserModeDriverName*, with the section that carries them.

        Returns list of (value_name, flags, values, addreg_section, host_section). `host_section` is the
        install section variant whose AddReg it is: the software key for a plain DDInstall AddReg, the
        hardware key for a .HW one. dxgkrnl reads the software key, so where it sits matters.
        """
        inf = self.inf
        out = []
        hosts = []
        for base in getattr(self, "installs", []):
            hosts.extend(inf.variants(base))
            hosts.extend(inf.suffixed(base, "hw"))
        for host in hosts:
            for line in inf.get(host):
                if line.key.lower() != "addreg":
                    continue
                for section in line.values:
                    if not inf.has(section):
                        self.report.error("inf", "REG001", "[%s] AddReg names section %r, which is absent"
                                          % (inf.section_names[host], section))
                        continue
                    for entry in inf.get(section):
                        fields = ([entry.key] + entry.values) if entry.key else entry.values
                        if len(fields) < 3:
                            continue
                        value_name = fields[2].strip()
                        if not value_name.lower().startswith("usermodedrivername"):
                            continue
                        flags = fields[3].strip() if len(fields) > 3 else ""
                        values = [v for v in fields[4:] if v.strip()]
                        out.append((value_name, flags, values, section, host))
        return out

    def check_umd(self):
        r, inf = self.report, self.inf
        entries = self.umd_entries()
        names = [e for e in entries if e[0].lower() == "usermodedrivername"]
        wow = [e for e in entries if e[0].lower() == "usermodedrivernamewow"]
        r.facts["umd_entries"] = [
            {"value": e[0], "flags": e[1], "names": e[2], "section": e[3], "host": e[4]} for e in entries
        ]

        if not names:
            if self.model == "full":
                r.error("umd", "UMD001",
                        "no UserModeDriverName in the software key: a full WDDM adapter cannot start "
                        "(facts M64). This is the E16 run 001/002 package.")
            else:
                r.note("umd", "UMD002",
                       "no UserModeDriverName; correct for a display-only package, fatal for a full one")
            return

        for value_name, flags, values, section, host in names:
            is_hw = host.rsplit(".", 1)[-1] == "hw"
            if is_hw:
                r.error("umd", "UMD003",
                        "UserModeDriverName is written from [%s], a .HW AddReg: that is the device "
                        "hardware key, dxgkrnl reads the software key" % inf.section_names[host])
            try:
                flag_value = int(flags, 0) if flags else 0
            except ValueError:
                flag_value = None
            if flag_value is None:
                r.error("umd", "UMD004", "UserModeDriverName flags %r are not a number" % flags)
            elif flag_value == REG_MULTI_SZ:
                r.ok("umd", "UMD005", "UserModeDriverName is REG_MULTI_SZ (0x00010000) with %d name(s): %s"
                     % (len(values), ", ".join(values)))
            elif flag_value == REG_SZ:
                r.warn("umd", "UMD006",
                       "UserModeDriverName is written as REG_SZ (flags 0). Every shipping WDDM display INF "
                       "writes REG_MULTI_SZ here; bc250kmd.inf keeps REG_SZ as a documented fallback only")
            else:
                r.error("umd", "UMD007", "UserModeDriverName flags 0x%08X are neither REG_SZ nor REG_MULTI_SZ"
                        % flag_value)
            if not values:
                r.error("umd", "UMD008", "UserModeDriverName is present but names no DLL")
            # The three entries of a REG_MULTI_SZ are D3D9, D3D10 and D3D11 and usually name the same file;
            # each distinct name is checked once, so the report does not say the same thing three times.
            seen = set()
            for dll in values:
                leaf = Path(dll.replace("\\", "/")).name.lower()
                if leaf in seen:
                    continue
                seen.add(leaf)
                self.referenced.add(leaf)
                if leaf not in self.copy_files:
                    r.error("umd", "UMD009", "UserModeDriverName names %s, which no CopyFiles section copies"
                            % dll)
                if leaf not in self.source_disks:
                    r.error("umd", "UMD010", "UserModeDriverName names %s, absent from [SourceDisksFiles]"
                            % dll)
                if leaf not in self.files:
                    r.error("umd", "UMD011", "UserModeDriverName names %s, which is not in the directory"
                            % dll)
                else:
                    r.ok("umd", "UMD012", "user-mode driver %s present" % dll)
                dirid = self.copy_files.get(leaf, (None, None))[1]
                if dirid and dirid not in ("11", "13"):
                    r.note("umd", "UMD013", "%s is copied to dirid %s, not 11 (System32) or 13 (driver store)"
                           % (dll, dirid))

        if self.model == "display-only":
            r.note("umd", "UMD014", "this display-only package registers a user-mode driver; harmless, and "
                   "the display-only table does start with one (facts M64)")
        if not wow:
            r.note("umd", "UMD015", "no UserModeDriverNameWow: deliberate, the stub is 64-bit only "
                   "(bc250kmd.inf explains why a Wow value pointing at the 64-bit DLL would be worse)")
        else:
            entry = wow[0]
            r.warn("umd", "UMD016", "UserModeDriverNameWow is present: %s" % ", ".join(entry[2]),
                   "our INF omits it on purpose; a 32-bit process would be pointed at a file that is not "
                   "in SysWOW64")

    def check_unreferenced(self):
        r = self.report
        for name, path in sorted(self.files.items()):
            if name in self.referenced:
                continue
            if path.suffix.lower() in KNOWN_EXTRAS:
                r.note("files", "FIL001", "%s is in the package but not named by the INF (known extra)" % path.name)
                continue
            r.error("files", "FIL002",
                    "%s is in the package but nothing in the INF refers to it" % path.name,
                    "an unreferenced file is either a leftover from an older build or a file somebody "
                    "forgot to add to CopyFiles")

    # -- PE ------------------------------------------------------------------------------------------

    def check_pe(self):
        r = self.report
        summaries = []
        for name in sorted(self.copy_files):
            path = self.files.get(name)
            if path is None:
                continue
            if path.suffix.lower() not in (".sys", ".dll", ".exe"):
                continue
            image = PeImage(path)
            self.images[name] = image
            summaries.append(self.describe_pe(name, image))
        r.facts["images"] = summaries

    def describe_pe(self, name, image):
        r = self.report
        summary = {"name": name}
        if not image.valid:
            r.error("pe", "PE001", "%s is not a valid PE image" % name, "; ".join(image.problems))
            summary["valid"] = False
            return summary
        summary.update({
            "valid": True,
            "machine": image.machine_name,
            "subsystem": image.subsystem_name,
            "pe32plus": image.pe32plus,
            "timestamp": image.timestamp,
            "timestamp_utc": image.timestamp_utc.isoformat() if image.timestamp_utc else None,
            "is_dll": image.is_dll,
            "signed": image.has_certificate_table,
            "file_version": image.file_version,
            "pdb": image.codeview[2] if image.codeview else None,
            "pdb_guid": ("%s+%d" % (image.codeview[0], image.codeview[1])) if image.codeview else None,
        })
        suffix = Path(name).suffix.lower()
        if image.machine != IMAGE_FILE_MACHINE_AMD64:
            r.error("pe", "PE002", "%s is %s, not AMD64: it cannot load on this machine"
                    % (name, image.machine_name))
        else:
            r.ok("pe", "PE003", "%s is AMD64%s" % (name, " PE32+" if image.pe32plus else ""))
        if suffix == ".sys":
            if image.subsystem != SUBSYSTEM_NATIVE:
                r.error("pe", "PE004", "%s subsystem is %s, a kernel driver must be NATIVE"
                        % (name, image.subsystem_name))
            else:
                r.ok("pe", "PE005", "%s subsystem NATIVE" % name)
            if not image.has_certificate_table:
                r.warn("pe", "PE006", "%s carries no embedded signature" % name,
                       "build.ps1 signs the .sys; an unsigned one means the package was assembled by hand")
        if suffix == ".dll":
            if not image.is_dll:
                r.error("pe", "PE007", "%s has no IMAGE_FILE_DLL characteristic: it is not a DLL" % name)
            if image.subsystem not in (SUBSYSTEM_GUI, SUBSYSTEM_CUI):
                r.note("pe", "PE008", "%s subsystem is %s" % (name, image.subsystem_name))
            if not image.has_certificate_table:
                r.note("pe", "PE009", "%s has no embedded signature; it is catalog-signed only, which is "
                       "how driver\\kmd\\build.ps1 builds it" % name)
        when = image.timestamp_utc
        if when:
            r.note("pe", "PE010", "%s linked %s UTC" % (name, when.strftime("%Y-%m-%d %H:%M:%S")))
            if when > datetime.datetime.now(datetime.timezone.utc) + datetime.timedelta(days=1):
                r.warn("pe", "PE011", "%s link timestamp is in the future (%s): a reproducible-build hash "
                       "rather than a time?" % (name, when.isoformat()))
        if image.file_version:
            r.note("pe", "PE012", "%s version resource: file %s, product %s"
                   % (name, image.file_version, image.product_version))
        else:
            r.note("pe", "PE013", "%s has no version resource" % name,
                   "driver\\kmd\\build.ps1 runs no rc.exe, so no binary in this package carries one; the "
                   "version check below reads the BC250_KMD_VERSION immediate out of the code instead")
        if image.codeview:
            r.note("pe", "PE014", "%s debug id %s age %d" % (name, image.codeview[0], image.codeview[1]),
                   "pdb: %s" % image.codeview[2])
        return summary

    # -- the user-mode driver's exports, and optionally its answers ---------------------------------------

    def umd_dll_names(self):
        """The DLLs UserModeDriverName names, as leaf names present in the package directory."""
        out = []
        for entry in self.report.facts.get("umd_entries", []):
            if entry["value"].lower() != "usermodedrivername":
                continue
            for dll in entry["names"]:
                leaf = Path(dll.replace("\\", "/")).name.lower()
                if leaf not in out and leaf in self.files:
                    out.append(leaf)
        return out

    def check_exports(self):
        """The built DLL must export the three names undecorated. Verified against the PE export table,
        not against the .def file: what ships is what the runtime will call GetProcAddress on."""
        r = self.report
        for name in self.umd_dll_names():
            image = self.images.get(name)
            if image is None or not image.valid:
                continue
            if image.exports is None:
                r.error("exports", "EXP001", "%s has no export directory at all: the runtime can look up "
                        "nothing in it" % name)
                continue
            r.facts.setdefault("exports", {})[name] = image.exports
            have = set(image.exports)
            for wanted in UMD_REQUIRED_EXPORTS:
                if wanted in have:
                    r.ok("exports", "EXP002", "%s exports %s" % (name, wanted))
                    continue
                # A missing name is worth more than "missing" when a decorated form of it is there:
                # _OpenAdapter@4 (a __stdcall x86 build) or ?OpenAdapter@@... (compiled as C++).
                decorated = [e for e in image.exports if wanted in e and e != wanted]
                if decorated:
                    r.error("exports", "EXP003",
                            "%s exports %s but not the undecorated %s" % (name, ", ".join(decorated), wanted),
                            "the runtime does GetProcAddress by the plain name; a decorated export is not "
                            "found. A C++ build or an x86 __stdcall build does this")
                else:
                    r.error("exports", "EXP004", "%s does not export %s" % (name, wanted),
                            "driver\\umd-stub\\bc250umd.def lists the three names and build.ps1 passes it "
                            "to the linker with /DEF:; this DLL was built without it or the name was renamed")
            extra = sorted(have - set(UMD_REQUIRED_EXPORTS))
            if extra:
                r.note("exports", "EXP005", "%s also exports: %s" % (name, ", ".join(extra)))
            if image.export_name and image.export_name.lower() != name:
                r.note("exports", "EXP006", "%s carries the library name %s in its export directory"
                       % (name, image.export_name))

    def check_load(self):
        """Load the stub in this process and call its three entry points. Off unless --load is given.

        This runs our own code on the development PC, so it is deliberately narrow: only the DLL that
        UserModeDriverName names, only from inside the package directory given on the command line, only
        when it is AMD64 and this Python is 64-bit, and the library is freed again afterwards. The stub
        takes no lock, starts no thread and touches no file (driver\\umd-stub\\bc250umd.c).
        """
        r = self.report
        if not self.load:
            return
        dlls = self.umd_dll_names()
        if not dlls:
            r.note("load", "LDR001", "--load: no user-mode driver in this package to load")
            return
        if sys.platform != "win32":
            r.note("load", "LDR002", "--load: not Windows, skipped")
            return
        if struct.calcsize("P") != 8:
            r.note("load", "LDR003", "--load: this Python is 32-bit and the stub is AMD64, skipped")
            return
        import ctypes
        for name in dlls:
            path = self.files[name].resolve()
            # Never load anything but a DLL that sits inside the directory that was given to us.
            if path.parent != self.dir:
                r.error("load", "LDR004", "%s resolves outside the package directory; not loaded" % name)
                continue
            image = self.images.get(name)
            if image is None or not image.valid or image.machine != IMAGE_FILE_MACHINE_AMD64:
                r.note("load", "LDR005", "--load: %s is not an AMD64 image, skipped" % name)
                continue
            handle = None
            try:
                dll = ctypes.WinDLL(str(path))
                handle = dll._handle
                r.ok("load", "LDR006", "%s loaded in-process" % name)
                block = ctypes.create_string_buffer(OPENADAPTER_ARG_BYTES)
                for export in UMD_REQUIRED_EXPORTS:
                    try:
                        fn = getattr(dll, export)
                    except AttributeError:
                        r.error("load", "LDR007", "%s: GetProcAddress(%s) failed in a real load"
                                % (name, export))
                        continue
                    fn.restype = ctypes.c_long
                    fn.argtypes = [ctypes.c_void_p]
                    ctypes.memset(block, 0, OPENADAPTER_ARG_BYTES)
                    result = fn(ctypes.cast(block, ctypes.c_void_p)) & 0xFFFFFFFF
                    if result == E_NOTIMPL:
                        r.ok("load", "LDR008", "%s!%s returned E_NOTIMPL (0x%08X)" % (name, export, result))
                    else:
                        r.error("load", "LDR009", "%s!%s returned 0x%08X, expected E_NOTIMPL (0x%08X)"
                                % (name, export, result, E_NOTIMPL),
                                "the stub must refuse every entry point; anything else means the DLL in "
                                "this package is not the stub this run expects")
            except OSError as exc:
                r.error("load", "LDR010", "%s could not be loaded: %s" % (name, exc))
            except Exception as exc:                            # noqa: BLE001 - a fault here is a finding
                r.error("load", "LDR011", "%s raised %s while being called: %s"
                        % (name, type(exc).__name__, exc))
            finally:
                if handle is not None:
                    try:
                        ctypes.windll.kernel32.FreeLibrary(ctypes.c_void_p(handle))
                    except Exception:                           # noqa: BLE001
                        r.note("load", "LDR012", "%s stayed loaded in this process" % name)

    # -- version agreement ------------------------------------------------------------------------------

    def check_driver_version(self):
        r = self.report
        expected = parse_version(self.expect_version) if self.expect_version else None
        if self.expect_version and expected is None:
            r.error("version", "VRS001", "--expect-version %r is not a dotted number" % self.expect_version)
            return
        if self.inf_version is None:
            r.warn("version", "VRS002", "no usable DriverVer: the version checks cannot run")
            return

        if expected is not None:
            n = len(expected)
            if self.inf_version[:n] == expected:
                build = self.inf_version[3] if len(self.inf_version) > 3 else 0
                if n <= 3 and build:
                    r.ok("version", "VRS003", "DriverVer %s matches --expect-version %s (build field %d)"
                         % (".".join(map(str, self.inf_version)), self.expect_version, build))
                    if build == 1 and self.model == "full":
                        r.note("version", "VRS004", "build field 1: the -UmdStub package convention, so that "
                               "this package outranks the plain one on the same hardware id and date")
                    elif build != 1:
                        r.note("version", "VRS005", "build field is %d, not the 0 (plain) or 1 (-UmdStub) "
                               "this build script produces" % build)
                else:
                    r.ok("version", "VRS006", "DriverVer %s matches --expect-version %s"
                         % (".".join(map(str, self.inf_version)), self.expect_version))
            else:
                r.error("version", "VRS007", "DriverVer is %s, --expect-version says %s"
                        % (".".join(map(str, self.inf_version)), self.expect_version))

        # The .sys against the version the INF claims. BC250_KMD_VERSION is compiled in as an immediate
        # operand (bc250kmd_escape.h, returned in BC250_ESCAPE.Version), so it appears in .text as the
        # little-endian dword and nowhere else - there is no version resource to read.
        target = expected[:3] if expected and len(expected) >= 3 else self.inf_version[:3]
        if len(target) < 3:
            r.note("version", "VRS008", "version %s has fewer than three fields: no dword to look for"
                   % ".".join(map(str, target)))
            return
        dword = version_dword(target)
        if dword is None or dword < 0x00010000:
            r.note("version", "VRS009", "version dword 0x%08X is too small to search for without false "
                   "positives" % (dword or 0))
            return
        needle = struct.pack("<I", dword)
        for name, image in sorted(self.images.items()):
            if not image.valid or Path(name).suffix.lower() != ".sys":
                continue
            hits = image.scan_executable(needle)
            if hits:
                r.ok("version", "VRS010", "%s carries BC250_KMD_VERSION 0x%08X (%s): %d occurrence(s) in code"
                     % (name, dword, ".".join(map(str, target)), len(hits)))
                continue
            others = self.other_versions(image, target)
            if not others and name != "bc250kmd.sys":
                # A different driver (the E05 kmdod experiment, say) has no BC250_KMD_VERSION at all.
                # Saying "stale" about a binary that never carried the constant would be crying wolf.
                r.note("version", "VRS012", "%s carries no BC250_KMD_VERSION constant, so its version "
                       "cannot be read out of the image" % name)
                continue
            detail = ("the image looks like " + ", ".join("%s (x%d)" % (v, c) for v, c in others)) if others \
                else ("no version constant of this shape is in the code at all, which for bc250kmd.sys "
                      "means either a binary older than the constant or a build that optimised it away")
            r.error("version", "VRS011",
                    "%s does not contain BC250_KMD_VERSION 0x%08X, the version its INF claims (%s)"
                    % (name, dword, ".".join(map(str, target))),
                    detail + ". A stale .sys in a package labelled with a newer version is exactly the "
                             "mistake this check exists for")

    def other_versions(self, image, target):
        """Which BC250_KMD_VERSION a .sys does look like: same major, minor within 3, any revision."""
        found = []
        major, minor, _ = target
        for m in range(max(minor - 3, 0), minor + 4):
            for rev in range(0, 64):
                cand = version_dword([major, m, rev])
                if cand is None or cand < 0x00010000:
                    continue
                hits = image.scan_executable(struct.pack("<I", cand))
                if hits:
                    found.append(("%d.%d.%d" % (major, m, rev), len(hits)))
        return found

    # -- BUILDINFO.json -----------------------------------------------------------------------------------

    def check_buildinfo(self):
        r = self.report
        path = self.files.get("buildinfo.json")
        if path is None:
            if self.expect_commit:
                r.error("buildinfo", "BLD001",
                        "--expect-commit was given but this package has no BUILDINFO.json",
                        "nothing in a bc250kmd package ties a binary to a commit today: build.ps1 runs no "
                        "rc.exe and embeds no commit string. See tools/packagecheck/README.md for the "
                        "three-line change to build.ps1 that would produce the sidecar")
            else:
                r.note("buildinfo", "BLD002", "no BUILDINFO.json: this package cannot be tied to a commit",
                       "the .sys carries only its PDB path and debug id (%s)"
                       % (self.images.get("bc250kmd.sys").codeview[2]
                          if self.images.get("bc250kmd.sys") and self.images["bc250kmd.sys"].codeview
                          else "none"))
            return
        try:
            info = json.loads(path.read_text(encoding="utf-8-sig"))
        except (OSError, ValueError) as exc:
            r.error("buildinfo", "BLD003", "BUILDINFO.json is unreadable: %s" % exc)
            return
        if not isinstance(info, dict):
            r.error("buildinfo", "BLD004", "BUILDINFO.json is not an object")
            return
        self.buildinfo = info
        r.facts["buildinfo"] = info
        commit = str(info.get("commit", "")).strip()
        if commit:
            r.ok("buildinfo", "BLD005", "BUILDINFO.json commit %s%s"
                 % (commit, " (working tree dirty)" if info.get("dirty") else ""))
            if info.get("dirty"):
                r.warn("buildinfo", "BLD006", "the build was made from a dirty working tree: the commit "
                       "does not describe what is in this package")
        if self.expect_commit:
            want = self.expect_commit.strip().lower()
            have = commit.lower()
            if have and (have.startswith(want) or want.startswith(have)):
                r.ok("buildinfo", "BLD007", "commit matches --expect-commit %s" % self.expect_commit)
            else:
                r.error("buildinfo", "BLD008", "BUILDINFO.json commit %r, --expect-commit %r"
                        % (commit, self.expect_commit))
        version = str(info.get("version", "")).strip()
        if version and self.inf_version:
            want = parse_version(version)
            if want is None or self.inf_version[:len(want)] != want:
                r.error("buildinfo", "BLD009", "BUILDINFO.json says version %s, the INF says %s"
                        % (version, ".".join(map(str, self.inf_version))))
        files = info.get("files")
        if not isinstance(files, dict) or not files:
            r.warn("buildinfo", "BLD010", "BUILDINFO.json lists no file hashes: it says nothing about the "
                   "binaries next to it, and a sidecar nobody checks is a sidecar that goes stale")
            return
        actual = {f["name"].lower(): f["sha256"] for f in r.facts.get("files", [])}
        for name, want in sorted(files.items()):
            have = actual.get(name.lower())
            if have is None:
                r.error("buildinfo", "BLD011", "BUILDINFO.json lists %s, which is not in the package" % name)
            elif have.lower() != str(want).lower():
                r.error("buildinfo", "BLD012", "%s does not match its BUILDINFO.json hash" % name,
                        "package %s, BUILDINFO %s" % (have, want))
            else:
                r.ok("buildinfo", "BLD013", "%s matches its BUILDINFO.json hash" % name)

    # -- catalog and kit tools ---------------------------------------------------------------------------

    def catalog_path(self):
        name = self.report.facts.get("catalog")
        if not name:
            return None
        return self.files.get(name.lower())

    def check_catalog(self):
        r = self.report
        cat = self.catalog_path()
        if cat is None:
            return
        members = catalog_member_names(cat)
        r.facts["catalog_members"] = sorted(members)
        expected = set(self.copy_files) | {self.inf_path.name.lower()}
        if members:
            missing = sorted(expected - members)
            extra = sorted(members - expected)
            if missing:
                r.error("catalog", "CAT001", "%s names no member for: %s" % (cat.name, ", ".join(missing)),
                        "the catalog was not rebuilt after the INF changed")
            else:
                r.ok("catalog", "CAT002", "%s names all %d installed file(s)" % (cat.name, len(expected)))
            if extra:
                r.warn("catalog", "CAT003", "%s names files the INF does not install: %s"
                       % (cat.name, ", ".join(extra)))
        else:
            r.note("catalog", "CAT004", "no member names could be read out of %s" % cat.name)

        if not self.use_tools:
            return
        signtool = find_tool(self.kits, "signtool.exe",
                             ["microsoft.windows.sdk.cpp/c/bin/*/x64/signtool.exe",
                              "*/c/bin/*/x64/signtool.exe"])
        if signtool is None:
            r.note("catalog", "CAT005", "signtool.exe not found under %s: catalog membership not verified "
                   "by hash" % self.kits)
            return
        r.facts["signtool"] = str(signtool)
        for name in sorted(expected):
            path = self.files.get(name)
            if path is None:
                continue
            code, out = run([signtool, "verify", "/pa", "/c", str(cat), str(path)])
            if code is None:
                r.error("catalog", "CAT006", "signtool could not be run: %s" % out)
                return
            # signtool wraps its messages across lines, so match against the text with its whitespace
            # collapsed: "terminated in a root\n\tcertificate which is not trusted" is one sentence.
            flat = re.sub(r"\s+", " ", out).lower()
            if code == 0:
                r.ok("catalog", "CAT007", "%s is in %s and the chain is trusted here" % (name, cat.name))
            elif "not found in the specified catalog" in flat:
                r.error("catalog", "CAT008", "%s is NOT a member of %s (hash mismatch or stale catalog)"
                        % (name, cat.name), out)
            elif "root certificate which is not trusted" in flat:
                # Expected: the lab certificate is a test certificate and this development PC does not
                # trust it. The file was found in the catalog and its hash matched - that is the check.
                r.note("catalog", "CAT009", "%s is a member of %s; the chain ends in an untrusted root, "
                       "which is what a test certificate does on this PC" % (name, cat.name))
            else:
                r.error("catalog", "CAT010", "signtool refused %s against %s" % (name, cat.name), out)

    def check_infverif(self):
        r = self.report
        if not self.use_tools:
            return
        infverif = find_tool(self.kits, "infverif.exe",
                             ["microsoft.windows.wdk.x64/c/tools/*/x64/infverif.exe",
                              "*/c/tools/*/x64/infverif.exe"])
        if infverif is None:
            r.note("tools", "IVF001", "infverif.exe not found under %s" % self.kits)
            return
        r.facts["infverif"] = str(infverif)
        # No mode switch: plain syntax and consistency. /w, /u and /h are WHQL and Windows Driver rule sets,
        # which this package fails on purpose (dirid 13 without a TargetOSVersion decoration, and the UMD
        # DLL going to System32); those are design decisions, not defects, and they belong in a note.
        code, out = run([infverif, str(self.inf_path)])
        if code is None:
            r.error("tools", "IVF002", "infverif could not be run: %s" % out)
            return
        warnings = [l for l in out.splitlines() if l.strip().upper().startswith("WARNING")]
        errors = [l for l in out.splitlines() if l.strip().upper().startswith("ERROR")]
        if code == 0 and not errors:
            r.ok("tools", "IVF003", "infverif: syntax clean, %d warning(s)" % len(warnings))
        else:
            r.error("tools", "IVF004", "infverif rejected the INF (exit %d)" % code, out)
        for line in warnings:
            r.note("tools", "IVF005", "infverif: " + line.strip())

    def check_inf2cat(self):
        """Regenerate the catalog in a scratch copy and compare its membership with the shipped one.

        Inf2Cat has no read-only mode - it works by writing a .cat - so the package is never given to it.
        The copy goes under scratch (P:\\BC-250\\scratch\\tmp by default), never on C:.
        """
        r = self.report
        if not self.use_tools:
            return
        inf2cat = find_tool(self.kits, "Inf2Cat.exe",
                            ["microsoft.windows.wdk.x64/c/bin/*/x86/Inf2Cat.exe", "*/c/bin/*/x86/Inf2Cat.exe"])
        if inf2cat is None:
            r.note("tools", "I2C001", "Inf2Cat.exe not found under %s" % self.kits)
            return
        r.facts["inf2cat"] = str(inf2cat)
        root = scratch_root(self.tmp)
        try:
            root.mkdir(parents=True, exist_ok=True)
            work = Path(tempfile.mkdtemp(prefix="packagecheck-", dir=str(root)))
        except OSError as exc:
            r.note("tools", "I2C002", "no scratch directory for the Inf2Cat copy (%s): check skipped" % exc)
            return
        try:
            staging = work / "package"
            staging.mkdir()
            for name, path in self.files.items():
                if path.suffix.lower() == ".cat":
                    continue            # Inf2Cat would catalogue a stale .cat into the new one
                shutil.copy2(path, staging / path.name)
            code, out = run([inf2cat, "/driver:%s" % staging, "/os:10_X64", "/uselocaltime"])
            if code is None:
                r.error("tools", "I2C003", "Inf2Cat could not be run: %s" % out)
                return
            if code != 0:
                r.error("tools", "I2C004", "Inf2Cat rejected this INF (exit %d)" % code, out)
                return
            built = sorted(staging.glob("*.cat"))
            if not built:
                r.error("tools", "I2C005", "Inf2Cat reported success but produced no .cat", out)
                return
            fresh = catalog_member_names(built[0])
            shipped = set(self.report.facts.get("catalog_members", []))
            if not shipped:
                r.note("tools", "I2C006", "Inf2Cat rebuild succeeded; nothing to compare it with")
            elif fresh == shipped:
                r.ok("tools", "I2C007", "Inf2Cat rebuild lists the same members as the shipped catalog")
            else:
                r.error("tools", "I2C008", "the shipped catalog and a fresh Inf2Cat run disagree",
                        "shipped only: %s; rebuilt only: %s"
                        % (", ".join(sorted(shipped - fresh)) or "-", ", ".join(sorted(fresh - shipped)) or "-"))
        finally:
            shutil.rmtree(work, ignore_errors=True)


# ---------------------------------------------------------------------------------------------------
# output
# ---------------------------------------------------------------------------------------------------

GROUP_ORDER = ["package", "inf", "files", "umd", "pe", "exports", "load", "version", "buildinfo",
               "catalog", "tools"]
GROUP_TITLE = {
    "package": "package", "inf": "INF", "files": "files", "umd": "user-mode driver",
    "pe": "binaries", "exports": "UMD exports", "load": "UMD load test",
    "version": "version agreement", "buildinfo": "BUILDINFO.json",
    "catalog": "catalog", "tools": "kit tools",
}


def ordered(findings):
    for group in GROUP_ORDER:
        rows = [f for f in findings if f.group == group]
        if rows:
            yield group, sorted(rows, key=lambda f: LEVEL_RANK[f.level])
    seen = set(GROUP_ORDER)
    for f in findings:
        if f.group not in seen:
            seen.add(f.group)
            yield f.group, [g for g in findings if g.group == f.group]


def human_report(check, verbose=False, stream=sys.stdout):
    r = check.report
    facts = r.facts
    print("packagecheck %s  %s" % (TOOL_VERSION, facts.get("package", check.dir)), file=stream)
    head = []
    if facts.get("driver_ver"):
        head.append("DriverVer %s" % facts["driver_ver"])
    head.append("model %s" % facts.get("model", "?"))
    if facts.get("service"):
        head.append("service %s" % facts["service"])
    print("  " + ", ".join(head), file=stream)
    for group, rows in ordered(r.findings):
        shown = [f for f in rows if verbose or f.level != OK]
        if not shown:
            continue
        print("\n[%s]" % GROUP_TITLE.get(group, group), file=stream)
        for f in shown:
            print("  %s %s  %s" % (LEVEL_TAG[f.level], f.code, f.text), file=stream)
            if f.detail and (verbose or f.level in (ERROR, WARN)):
                for line in f.detail.splitlines():
                    print("         %s" % line.strip(), file=stream)
    print("\n[contents]", file=stream)
    for entry in facts.get("files", []):
        print("  %10d  %s  %s" % (entry["size"], entry["sha256"], entry["name"]), file=stream)
    print("", file=stream)
    verdict = "FAIL" if r.failed else "PASS"
    print("%s: %d error(s), %d warning(s), %d note(s), %d check(s) passed"
          % (verdict, r.count(ERROR), r.count(WARN), r.count(NOTE), r.count(OK)), file=stream)


def manifest_text(check):
    """A SHA-256 manifest of the package, so that a run's evidence can name the bytes it measured.

    Every file in the directory, because the whole directory is what gets pushed to the lab. Two comment
    lines, then `<sha256>  <size>  <name>` - close enough to sha256sum that the eye reads it the same way.
    """
    facts = check.report.facts
    lines = ["# packagecheck manifest: %s" % facts.get("package", check.dir),
             "# %s UTC, model %s, DriverVer %s"
             % (datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M:%S"),
                facts.get("model", "?"), facts.get("driver_ver", "?"))]
    for entry in facts.get("files", []):
        lines.append("%s  %9d  %s" % (entry["sha256"], entry["size"], entry["name"]))
    return "\n".join(lines) + "\n"


def json_report(check):
    r = check.report
    return {
        "tool": "packagecheck",
        "tool_version": TOOL_VERSION,
        "generated": datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0).isoformat(),
        "package": str(check.dir),
        "model": r.facts.get("model"),
        "expect_version": check.expect_version,
        "expect_commit": check.expect_commit,
        "result": "fail" if r.failed else "pass",
        "counts": {level: r.count(level) for level in (ERROR, WARN, NOTE, OK)},
        "facts": r.facts,
        "findings": [f.as_dict() for f in r.findings],
    }


def markdown_report(check):
    r = check.report
    facts = r.facts
    out = []
    out.append("# packagecheck: %s" % Path(facts.get("package", check.dir)).name)
    out.append("")
    out.append("- package: `%s`" % facts.get("package", check.dir))
    out.append("- checked: %s UTC" % datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M:%S"))
    out.append("- model: %s" % facts.get("model", "?"))
    if facts.get("driver_ver"):
        out.append("- DriverVer: `%s`" % facts["driver_ver"])
    if facts.get("hardware_ids"):
        out.append("- hardware id: `%s`" % "`, `".join(facts["hardware_ids"]))
    if facts.get("service"):
        out.append("- service: `%s`, binary `%s`" % (facts["service"], facts.get("service_binary", "?")))
    out.append("- verdict: **%s** (%d errors, %d warnings, %d notes)"
               % ("FAIL" if r.failed else "PASS", r.count(ERROR), r.count(WARN), r.count(NOTE)))
    out.append("")
    out.append("## Findings")
    out.append("")
    out.append("| level | code | group | finding |")
    out.append("|---|---|---|---|")
    for group, rows in ordered(r.findings):
        for f in rows:
            text = f.text.replace("|", "\\|")
            if f.detail and f.level in (ERROR, WARN):
                text += " - " + f.detail.replace("|", "\\|").replace("\n", " ")
            out.append("| %s | %s | %s | %s |" % (f.level, f.code, group, text))
    out.append("")
    out.append("## Files")
    out.append("")
    out.append("| size | SHA-256 | name |")
    out.append("|---:|---|---|")
    for entry in facts.get("files", []):
        out.append("| %d | `%s` | `%s` |" % (entry["size"], entry["sha256"], entry["name"]))
    images = facts.get("images") or []
    if images:
        out.append("")
        out.append("## Binaries")
        out.append("")
        out.append("| file | machine | subsystem | linked (UTC) | signed | version resource |")
        out.append("|---|---|---|---|---|---|")
        for img in images:
            if not img.get("valid"):
                out.append("| `%s` | - | - | - | - | not a PE image |" % img["name"])
                continue
            out.append("| `%s` | %s | %s | %s | %s | %s |"
                       % (img["name"], img["machine"], img["subsystem"],
                          (img.get("timestamp_utc") or "?").replace("T", " ").replace("+00:00", ""),
                          "yes" if img.get("signed") else "no", img.get("file_version") or "none"))
    for name, names in sorted((facts.get("exports") or {}).items()):
        out.append("")
        out.append("`%s` exports: %s" % (name, ", ".join("`%s`" % n for n in names) or "nothing"))
    out.append("")
    return "\n".join(out)


# ---------------------------------------------------------------------------------------------------

def build_parser():
    p = argparse.ArgumentParser(
        prog="packagecheck",
        description="Validate a finished bc250kmd driver package before it is installed on the lab.")
    p.add_argument("package", help="the package directory (the one holding the .inf)")
    p.add_argument("--model", choices=["full", "display-only"],
                   help="what the package is for; default: inferred from the presence of a DLL")
    p.add_argument("--expect-version", metavar="A.B.C",
                   help="the version this package should be, e.g. 0.7.4 (the .1 build field of a "
                        "-UmdStub package is accepted)")
    p.add_argument("--expect-commit", metavar="HASH",
                   help="the commit this package should have been built from; needs a BUILDINFO.json")
    p.add_argument("--kits", help="the unpacked WDK/SDK NuGet root (default: <workspace>\\toolchain\\nuget)")
    p.add_argument("--tmp", help="scratch directory for the Inf2Cat copy (default: "
                                "<workspace>\\scratch\\tmp, never C:)")
    p.add_argument("--no-tools", action="store_true", help="skip signtool, infverif and Inf2Cat")
    p.add_argument("--load", action="store_true",
                   help="load the package's user-mode driver in this process and call its three entry "
                        "points; they must all return E_NOTIMPL. Runs our own stub on this PC, so it is "
                        "off by default")
    p.add_argument("--json", metavar="FILE", help="write the full result as JSON")
    p.add_argument("--markdown", metavar="FILE", help="write a Markdown report for a run's evidence")
    p.add_argument("--manifest", metavar="FILE",
                   help="write a SHA-256 manifest of the package: hash, size, name per file")
    p.add_argument("-v", "--verbose", action="store_true", help="also print the checks that passed")
    return p


def main(argv=None):
    args = build_parser().parse_args(argv)
    check = PackageCheck(args.package, model=args.model, expect_version=args.expect_version,
                         expect_commit=args.expect_commit, kits=args.kits,
                         use_tools=not args.no_tools, tmp=args.tmp, load=args.load)
    try:
        check.run_all()
    except OSError as exc:
        print("packagecheck: %s" % exc, file=sys.stderr)
        return 2
    human_report(check, verbose=args.verbose)
    if args.json:
        Path(args.json).write_text(json.dumps(json_report(check), indent=2) + "\n", encoding="utf-8")
    if args.markdown:
        Path(args.markdown).write_text(markdown_report(check), encoding="utf-8")
    if args.manifest:
        Path(args.manifest).write_text(manifest_text(check), encoding="utf-8")
    return 1 if check.report.failed else 0


if __name__ == "__main__":
    sys.exit(main())
