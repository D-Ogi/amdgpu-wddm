#!/usr/bin/env python3
"""Tests for packagecheck.

    python -m unittest discover -s tools/packagecheck

Every fixture is built here, in a scratch directory: a synthetic INF and synthetic PE images small enough
to read in one screen. The kit tools are switched off (`use_tools=False`) so that the suite runs on a
machine with no WDK unpacked and never shells out; the one test that touches the real packages skips
itself when they are not there.

Nothing is written to C:. The scratch root comes from packagecheck.scratch_root(), the same function the
tool uses for its Inf2Cat copy.
"""

import shutil
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import packagecheck as pc                                          # noqa: E402

REAL_BUILD = pc.WORKSPACE / "scratch" / "build" / "bc250kmd-074"


# ---------------------------------------------------------------------------------------------------
# fixtures
# ---------------------------------------------------------------------------------------------------

def align(value, boundary=0x200):
    return (value + boundary - 1) // boundary * boundary


def export_blob(base_rva, names, dll_name):
    """An IMAGE_EXPORT_DIRECTORY with its name table, laid out at `base_rva`."""
    names = sorted(names)
    n = len(names)
    funcs_off = 40
    names_off = funcs_off + 4 * n
    ords_off = names_off + 4 * n
    strings_off = ords_off + 2 * n

    strings = bytearray()
    dll_rva = base_rva + strings_off
    strings += dll_name.encode("ascii") + b"\0"
    name_rvas = []
    for name in names:
        name_rvas.append(base_rva + strings_off + len(strings))
        strings += name.encode("ascii") + b"\0"

    blob = bytearray(strings_off) + strings
    struct.pack_into("<IIHHIIIIIII", blob, 0, 0, 0, 0, 0, dll_rva, 1, n, n,
                     base_rva + funcs_off, base_rva + names_off, base_rva + ords_off)
    for i in range(n):
        struct.pack_into("<I", blob, funcs_off + 4 * i, 0x1000)    # every export points into .text
        struct.pack_into("<I", blob, names_off + 4 * i, name_rvas[i])
        struct.pack_into("<H", blob, ords_off + 2 * i, i)
    return bytes(blob)


def make_pe(path, machine=pc.IMAGE_FILE_MACHINE_AMD64, subsystem=pc.SUBSYSTEM_NATIVE,
            characteristics=0x0022, code=b"", pe32plus=True, exports=None,
            export_name="BC250UMD.dll"):
    """A minimal but real PE32+ image: DOS stub, COFF header, optional header, an executable .text and,
    when `exports` is given, an .rdata holding an export directory.

    Only the fields packagecheck reads are filled in. `code` lands inside .text, which is where the
    BC250_KMD_VERSION immediate lives in the driver proper.
    """
    e_lfanew = 0x80
    opt_size = 240 if pe32plus else 224
    nsections = 2 if exports is not None else 1
    sec_off = e_lfanew + 24 + opt_size
    raw_off = align(sec_off + 40 * nsections)
    body = code + b"\x90" * max(0, 0x40 - len(code))
    text_raw = align(len(body))
    rdata_rva = 0x2000
    rdata = export_blob(rdata_rva, exports, export_name) if exports is not None else b""
    rdata_raw = align(len(rdata)) if rdata else 0

    dos = bytearray(e_lfanew)
    dos[0:2] = b"MZ"
    struct.pack_into("<I", dos, 0x3C, e_lfanew)

    coff = struct.pack("<4sHHIIIHH", b"PE\0\0", machine, nsections, 0x68CFFFFF, 0, 0,
                       opt_size, characteristics)

    opt = bytearray(opt_size)
    struct.pack_into("<H", opt, 0, 0x20B if pe32plus else 0x10B)
    struct.pack_into("<I", opt, 16, 0x1000)                        # AddressOfEntryPoint
    struct.pack_into("<H", opt, 68, subsystem)
    dirs_off = (108 if pe32plus else 92)
    struct.pack_into("<I", opt, dirs_off, 16)                      # NumberOfRvaAndSizes
    if rdata:
        struct.pack_into("<II", opt, dirs_off + 4, rdata_rva, len(rdata))   # directory 0: exports
    # The other 15 data directories stay zero: no resources, no certificate table, no debug directory.

    table = struct.pack("<8sIIIIIIHHI", b".text", len(body), 0x1000, text_raw, raw_off,
                        0, 0, 0, 0, 0x60000020)
    if rdata:
        table += struct.pack("<8sIIIIIIHHI", b".rdata", len(rdata), rdata_rva, rdata_raw,
                             raw_off + text_raw, 0, 0, 0, 0, 0x40000040)

    blob = bytes(dos) + coff + bytes(opt) + table
    blob += b"\0" * (raw_off - len(blob)) + body + b"\0" * (text_raw - len(body))
    if rdata:
        blob += rdata + b"\0" * (rdata_raw - len(rdata))
    Path(path).write_bytes(blob)


def version_code(version):
    """A byte string holding the BC250_KMD_VERSION immediate the way the compiler emits it."""
    major, minor, rev = version
    return b"\xb8" + struct.pack("<I", pc.version_dword([major, minor, rev])) + b"\xc3"


INF_TEMPLATE = """\
; synthetic bc250kmd package for the packagecheck tests
[Version]
Signature   = "$Windows NT$"
Class       = Display
ClassGuid   = {{4d36e968-e325-11ce-bfc1-08002be10318}}
Provider    = %Provider%
CatalogFile = bc250kmd.cat
DriverVer   = 09/21/2026,{driver_ver}
PnpLockdown = 1

[DestinationDirs]
DefaultDestDir = 13
{umd_destdir}

[SourceDisksNames]
1 = %DiskName%

[SourceDisksFiles]
bc250kmd.sys = 1
{umd_source}

[Manufacturer]
%Provider% = Models,NTamd64

[Models.NTamd64]
%DeviceName% = Bc250_Install, PCI\\VEN_1002&DEV_13FE

[Bc250_Install]
FeatureScore = F8
CopyFiles    = {copy_files}
{umd_addreg}

[Bc250_Files]
bc250kmd.sys

{umd_sections}
[Bc250_Install.Services]
AddService = bc250kmd, 0x00000002, Bc250_Service

[Bc250_Service]
DisplayName   = %DeviceName%
ServiceType   = 1
StartType     = 3
ErrorControl  = 0
ServiceBinary = %13%\\bc250kmd.sys

[Strings]
Provider   = "BC-250 lab (D-Ogi)"
DiskName   = "bc250kmd installation media"
DeviceName = "BC-250 GPU (bc250kmd, lab build)"
"""

UMD_SECTIONS = """\
[Bc250_UmdFiles]
{dll}

[Bc250_UserModeDriver]
HKR,, UserModeDriverName, {flags}, {dll}, {dll}, {dll}

"""


def write_inf(path, umd=True, driver_ver="0.7.4.1", dll="bc250umd.dll", flags="0x00010000"):
    if umd:
        text = INF_TEMPLATE.format(
            driver_ver=driver_ver,
            umd_destdir="Bc250_UmdFiles = 11",
            umd_source="%s = 1" % dll,
            copy_files="Bc250_Files, Bc250_UmdFiles",
            umd_addreg="AddReg       = Bc250_UserModeDriver",
            umd_sections=UMD_SECTIONS.format(dll=dll, flags=flags))
    else:
        text = INF_TEMPLATE.format(driver_ver=driver_ver, umd_destdir="", umd_source="",
                                   copy_files="Bc250_Files", umd_addreg="", umd_sections="")
    Path(path).write_text(text, encoding="utf-8", newline="\r\n")


class PackageFixture:
    """A synthetic package directory. `check(...)` runs packagecheck over it with the kit tools off."""

    def __init__(self, root, umd=True, driver_ver="0.7.4.1", sys_version=(0, 7, 4),
                 dll="bc250umd.dll", write_dll=True, dll_machine=pc.IMAGE_FILE_MACHINE_AMD64,
                 flags="0x00010000", dll_exports=pc.UMD_REQUIRED_EXPORTS):
        self.dir = Path(root)
        self.dir.mkdir(parents=True, exist_ok=True)
        write_inf(self.dir / "bc250kmd.inf", umd=umd, driver_ver=driver_ver, dll=dll, flags=flags)
        make_pe(self.dir / "bc250kmd.sys", code=version_code(sys_version))
        if umd and write_dll:
            make_pe(self.dir / dll, machine=dll_machine, subsystem=pc.SUBSYSTEM_GUI,
                    characteristics=0x2022,
                    exports=list(dll_exports) if dll_exports is not None else None)
        (self.dir / "bc250kmd.cat").write_bytes(b"\x30\x82fake catalog")
        (self.dir / "bc250-lab-test.cer").write_bytes(b"\x30\x82fake certificate")

    def check(self, **kwargs):
        kwargs.setdefault("use_tools", False)
        check = pc.PackageCheck(self.dir, **kwargs)
        check.run_all()
        return check.report


def codes(report, level=None):
    return {f.code for f in report.findings if level is None or f.level == level}


# ---------------------------------------------------------------------------------------------------

class TempCaseMixin:
    @classmethod
    def setUpClass(cls):
        root = pc.scratch_root()
        root.mkdir(parents=True, exist_ok=True)
        cls.tmp = Path(tempfile.mkdtemp(prefix="packagecheck-test-", dir=str(root)))

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)


class InfParsingTests(TempCaseMixin, unittest.TestCase):
    def parse(self, text, encoding="utf-8"):
        path = self.tmp / "parse.inf"
        path.write_bytes(text.encode(encoding))
        return pc.InfFile(path)

    def test_sections_and_values(self):
        inf = self.parse("[Version]\r\nSignature = \"$Windows NT$\"\r\nDriverVer = 09/21/2026,0.7.4.0\r\n")
        self.assertEqual(inf.value("version", "Signature"), '$Windows NT$')
        self.assertEqual(inf.entry("version", "DriverVer").values, ["09/21/2026", "0.7.4.0"])

    def test_comments_are_stripped_but_not_inside_quotes(self):
        inf = self.parse('[A]\r\nHKR, "one;two", , 0x10   ; a comment\r\n')
        self.assertEqual(inf.get("a")[0].values, ["HKR", "one;two", "", "0x10"])

    def test_continuation_lines(self):
        inf = self.parse("[A]\r\nHKR,, Names, 0x00010000, one.dll, \\\r\n    two.dll\r\n")
        self.assertEqual(inf.get("a")[0].values[-2:], ["one.dll", "two.dll"])

    def test_string_substitution(self):
        inf = self.parse('[Models]\r\n%DeviceName% = Bc250_Install\r\n[Strings]\r\nDeviceName = "BC-250"\r\n')
        self.assertEqual(inf.subst("%DeviceName%"), "BC-250")
        self.assertEqual(inf.subst("%13%\\bc250kmd.sys"), "%13%\\bc250kmd.sys")   # a DIRID is left alone
        self.assertEqual(inf.subst("100%%"), "100%")

    def test_utf8_bom(self):
        inf = self.parse("﻿[Version]\r\nClass = Display\r\n")
        self.assertEqual(inf.encoding, "utf-8 (BOM)")
        self.assertEqual(inf.value("version", "Class"), "Display")

    def test_utf16_bom(self):
        path = self.tmp / "u16.inf"
        path.write_bytes("﻿[Version]\r\nClass = Display\r\n".encode("utf-16-le"))
        inf = pc.InfFile(path)
        self.assertEqual(inf.encoding, "utf-16-le (BOM)")
        self.assertEqual(inf.value("version", "Class"), "Display")

    def test_decorated_section_lookup(self):
        inf = self.parse("[Bc250_Install.NTamd64]\r\nCopyFiles = X\r\n"
                         "[Bc250_Install.NTamd64.Services]\r\nAddService = bc250kmd\r\n")
        self.assertEqual(inf.variants("Bc250_Install"), ["bc250_install.ntamd64"])
        self.assertEqual(inf.suffixed("Bc250_Install", "Services"), ["bc250_install.ntamd64.services"])


class PeParsingTests(TempCaseMixin, unittest.TestCase):
    def test_amd64_native_sys(self):
        path = self.tmp / "ok.sys"
        make_pe(path, code=version_code((0, 7, 4)))
        image = pc.PeImage(path)
        self.assertTrue(image.valid, image.problems)
        self.assertEqual(image.machine, pc.IMAGE_FILE_MACHINE_AMD64)
        self.assertEqual(image.subsystem, pc.SUBSYSTEM_NATIVE)
        self.assertTrue(image.pe32plus)
        self.assertFalse(image.has_certificate_table)

    def test_version_scan_finds_the_dword_in_code(self):
        path = self.tmp / "ver.sys"
        make_pe(path, code=version_code((0, 7, 4)))
        image = pc.PeImage(path)
        self.assertEqual(len(image.scan_executable(struct.pack("<I", 0x00070004))), 1)
        self.assertEqual(image.scan_executable(struct.pack("<I", 0x00070003)), [])

    def test_not_a_pe(self):
        path = self.tmp / "junk.sys"
        path.write_bytes(b"this is not a PE image at all")
        self.assertFalse(pc.PeImage(path).valid)

    def test_export_table(self):
        path = self.tmp / "exp.dll"
        make_pe(path, subsystem=pc.SUBSYSTEM_GUI, characteristics=0x2022,
                exports=list(pc.UMD_REQUIRED_EXPORTS))
        image = pc.PeImage(path)
        self.assertEqual(image.exports, sorted(pc.UMD_REQUIRED_EXPORTS))
        self.assertEqual(image.export_name, "BC250UMD.dll")

    def test_no_export_directory_reads_as_none(self):
        path = self.tmp / "noexp.dll"
        make_pe(path, subsystem=pc.SUBSYSTEM_GUI, characteristics=0x2022)
        self.assertIsNone(pc.PeImage(path).exports)


class GoodPackageTests(TempCaseMixin, unittest.TestCase):
    def test_full_package_passes(self):
        report = PackageFixture(self.tmp / "good-umd").check(model="full", expect_version="0.7.4")
        self.assertFalse(report.failed, [f.text for f in report.findings if f.level == pc.ERROR])
        self.assertIn("UMD005", codes(report, pc.OK))
        self.assertIn("VRS010", codes(report, pc.OK))
        self.assertIn("UMD015", codes(report, pc.NOTE))        # the deliberate missing Wow value

    def test_display_only_package_passes(self):
        report = PackageFixture(self.tmp / "good-plain", umd=False, driver_ver="0.7.4.0") \
            .check(model="display-only", expect_version="0.7.4")
        self.assertFalse(report.failed, [f.text for f in report.findings if f.level == pc.ERROR])
        self.assertIn("UMD002", codes(report, pc.NOTE))

    def test_model_is_inferred_from_a_dll(self):
        report = PackageFixture(self.tmp / "infer-full").check()
        self.assertEqual(report.facts["model"], "full")
        report = PackageFixture(self.tmp / "infer-plain", umd=False, driver_ver="0.7.4.0").check()
        self.assertEqual(report.facts["model"], "display-only")

    def test_umd_exports_are_verified_in_the_built_dll(self):
        report = PackageFixture(self.tmp / "good-exports").check(model="full")
        for name in pc.UMD_REQUIRED_EXPORTS:
            self.assertTrue(any(f.code == "EXP002" and name in f.text
                                for f in report.findings if f.level == pc.OK), name)

    def test_umd_build_field_one_is_accepted(self):
        report = PackageFixture(self.tmp / "buildfield").check(model="full", expect_version="0.7.4")
        self.assertIn("VRS003", codes(report, pc.OK))
        self.assertIn("VRS004", codes(report, pc.NOTE))


class NegativeTests(TempCaseMixin, unittest.TestCase):
    def test_no_umd_entry_with_model_full(self):
        """The E16 run 001/002 mistake: a plain package installed for a full WDDM run."""
        report = PackageFixture(self.tmp / "neg-noumd", umd=False, driver_ver="0.7.4.0") \
            .check(model="full", expect_version="0.7.4")
        self.assertTrue(report.failed)
        self.assertIn("UMD001", codes(report, pc.ERROR))

    def test_umd_dll_named_but_missing(self):
        report = PackageFixture(self.tmp / "neg-nodll", write_dll=False).check(model="full")
        self.assertTrue(report.failed)
        self.assertIn("UMD011", codes(report, pc.ERROR))       # named but not in the directory
        self.assertIn("CPY007", codes(report, pc.ERROR))       # and not copyable either

    def test_stale_sys(self):
        """A 0.7.3 binary inside a package labelled 0.7.4."""
        report = PackageFixture(self.tmp / "neg-stale", sys_version=(0, 7, 3)) \
            .check(model="full", expect_version="0.7.4")
        self.assertTrue(report.failed)
        self.assertIn("VRS011", codes(report, pc.ERROR))
        stale = [f for f in report.findings if f.code == "VRS011"][0]
        self.assertIn("0.7.3", stale.detail)                   # and it says which version it does look like

    def test_wrong_architecture_dll(self):
        report = PackageFixture(self.tmp / "neg-x86", dll_machine=pc.IMAGE_FILE_MACHINE_I386) \
            .check(model="full")
        self.assertTrue(report.failed)
        errors = [f for f in report.findings if f.code == "PE002"]
        self.assertEqual(len(errors), 1)
        self.assertIn("x86", errors[0].text)

    def test_file_not_referenced(self):
        fixture = PackageFixture(self.tmp / "neg-extra")
        make_pe(fixture.dir / "bc250old.sys", code=version_code((0, 7, 2)))
        report = fixture.check(model="full")
        self.assertTrue(report.failed)
        self.assertIn("FIL002", codes(report, pc.ERROR))

    def test_driver_ver_mismatch(self):
        report = PackageFixture(self.tmp / "neg-ver", driver_ver="0.7.3.1", sys_version=(0, 7, 3)) \
            .check(model="full", expect_version="0.7.4")
        self.assertTrue(report.failed)
        self.assertIn("VRS007", codes(report, pc.ERROR))

    def test_umd_missing_an_export(self):
        """The DLL builds and loads, and the D3D11 entry point the runtime wants is not there."""
        report = PackageFixture(self.tmp / "neg-export",
                                dll_exports=["OpenAdapter", "OpenAdapter10"]).check(model="full")
        self.assertTrue(report.failed)
        missing = [f for f in report.findings if f.code == "EXP004"]
        self.assertEqual(len(missing), 1)
        self.assertIn("OpenAdapter10_2", missing[0].text)

    def test_umd_export_is_decorated(self):
        """A C++ build, or an x86 __stdcall one: the name is there but not under the name the runtime
        asks for."""
        report = PackageFixture(self.tmp / "neg-decorated",
                                dll_exports=["_OpenAdapter@4", "OpenAdapter10", "OpenAdapter10_2"]) \
            .check(model="full")
        self.assertTrue(report.failed)
        decorated = [f for f in report.findings if f.code == "EXP003"]
        self.assertEqual(len(decorated), 1)
        self.assertIn("_OpenAdapter@4", decorated[0].text)

    def test_umd_has_no_export_directory(self):
        report = PackageFixture(self.tmp / "neg-noexports", dll_exports=None).check(model="full")
        self.assertTrue(report.failed)
        self.assertIn("EXP001", codes(report, pc.ERROR))

    def test_a_foreign_driver_is_not_called_stale(self):
        """The E05 kmdod package predates BC250_KMD_VERSION. Absence of the constant in a binary that
        never had one is a note, not a stale-binary error."""
        fixture = PackageFixture(self.tmp / "neg-foreign")
        for name in ("bc250kmd.inf",):
            path = fixture.dir / name
            path.write_text(path.read_text(encoding="utf-8").replace("bc250kmd.sys", "bc250kmdod.sys"),
                            encoding="utf-8", newline="")
        (fixture.dir / "bc250kmd.sys").rename(fixture.dir / "bc250kmdod.sys")
        make_pe(fixture.dir / "bc250kmdod.sys", code=b"\x90" * 16)      # no version immediate at all
        report = fixture.check(model="full", expect_version="0.7.4")
        self.assertIn("VRS012", codes(report, pc.NOTE))
        self.assertNotIn("VRS011", codes(report, pc.ERROR))

    def test_wrong_hardware_id(self):
        fixture = PackageFixture(self.tmp / "neg-hwid")
        path = fixture.dir / "bc250kmd.inf"
        path.write_text(path.read_text(encoding="utf-8").replace("DEV_13FE", "DEV_1234"),
                        encoding="utf-8", newline="")
        report = fixture.check(model="full")
        self.assertTrue(report.failed)
        self.assertIn("HW005", codes(report, pc.ERROR))

    def test_umd_name_as_reg_sz_is_a_warning_not_an_error(self):
        report = PackageFixture(self.tmp / "neg-regsz", flags="0x00000000").check(model="full")
        self.assertIn("UMD006", codes(report, pc.WARN))
        self.assertNotIn("UMD007", codes(report, pc.ERROR))

    def test_umd_name_with_a_nonsense_flag_is_an_error(self):
        report = PackageFixture(self.tmp / "neg-badflag", flags="0x00000007").check(model="full")
        self.assertTrue(report.failed)
        self.assertIn("UMD007", codes(report, pc.ERROR))

    def test_expect_commit_without_a_sidecar(self):
        report = PackageFixture(self.tmp / "neg-commit").check(model="full", expect_commit="83ce7bf")
        self.assertTrue(report.failed)
        self.assertIn("BLD001", codes(report, pc.ERROR))


class BuildInfoTests(TempCaseMixin, unittest.TestCase):
    def sidecar(self, directory, **fields):
        import json
        (Path(directory) / "BUILDINFO.json").write_text(json.dumps(fields), encoding="utf-8")

    def test_matching_sidecar(self):
        fixture = PackageFixture(self.tmp / "bld-ok")
        self.sidecar(fixture.dir, commit="83ce7bf", version="0.7.4", dirty=False,
                     files={"bc250kmd.sys": pc.sha256_file(fixture.dir / "bc250kmd.sys")})
        report = fixture.check(model="full", expect_commit="83ce7bf", expect_version="0.7.4")
        self.assertFalse(report.failed, [f.text for f in report.findings if f.level == pc.ERROR])
        self.assertIn("BLD013", codes(report, pc.OK))

    def test_sidecar_hash_mismatch(self):
        fixture = PackageFixture(self.tmp / "bld-stale")
        self.sidecar(fixture.dir, commit="83ce7bf", version="0.7.4", files={"bc250kmd.sys": "00" * 32})
        report = fixture.check(model="full", expect_commit="83ce7bf")
        self.assertTrue(report.failed)
        self.assertIn("BLD012", codes(report, pc.ERROR))

    def test_sidecar_commit_mismatch(self):
        fixture = PackageFixture(self.tmp / "bld-commit")
        self.sidecar(fixture.dir, commit="deadbee", version="0.7.4")
        report = fixture.check(model="full", expect_commit="83ce7bf")
        self.assertTrue(report.failed)
        self.assertIn("BLD008", codes(report, pc.ERROR))


class ReportTests(TempCaseMixin, unittest.TestCase):
    def test_json_and_markdown_are_produced(self):
        import io
        import json
        fixture = PackageFixture(self.tmp / "report")
        check = pc.PackageCheck(fixture.dir, model="full", expect_version="0.7.4", use_tools=False)
        check.run_all()
        doc = json.loads(json.dumps(pc.json_report(check)))
        self.assertEqual(doc["result"], "pass")
        self.assertEqual(doc["model"], "full")
        self.assertTrue(any(f["name"] == "bc250kmd.sys" for f in doc["facts"]["files"]))
        md = pc.markdown_report(check)
        self.assertIn("| level | code | group | finding |", md)
        self.assertIn("bc250umd.dll", md)
        buf = io.StringIO()
        pc.human_report(check, stream=buf)
        self.assertIn("PASS:", buf.getvalue())

    def test_manifest(self):
        fixture = PackageFixture(self.tmp / "manifest")
        check = pc.PackageCheck(fixture.dir, model="full", use_tools=False)
        check.run_all()
        text = pc.manifest_text(check)
        lines = [l for l in text.splitlines() if not l.startswith("#")]
        self.assertEqual(len(lines), len(list(fixture.dir.iterdir())))
        by_name = {l.split()[2]: l.split()[0] for l in lines}
        self.assertEqual(by_name["bc250umd.dll"], pc.sha256_file(fixture.dir / "bc250umd.dll"))
        self.assertEqual(by_name["bc250kmd.sys"], pc.sha256_file(fixture.dir / "bc250kmd.sys"))


class LoadTests(TempCaseMixin, unittest.TestCase):
    """The load step is never run against a synthetic image - those are not loadable and must not be
    handed to the loader. Only the two paths that decide nothing gets loaded are tested here; the real
    load is in RealPackageTests."""

    def test_load_is_off_by_default(self):
        report = PackageFixture(self.tmp / "load-off").check(model="full")
        self.assertEqual([f for f in report.findings if f.group == "load"], [])

    def test_load_on_a_package_without_a_dll(self):
        report = PackageFixture(self.tmp / "load-none", umd=False, driver_ver="0.7.4.0") \
            .check(model="display-only", load=True)
        self.assertIn("LDR001", codes(report, pc.NOTE))
        self.assertFalse(report.failed)


@unittest.skipUnless(REAL_BUILD.is_dir(), "no %s on this machine" % REAL_BUILD)
class RealPackageTests(unittest.TestCase):
    """The packages the lab actually installs. Kit tools stay off: this is about the parsing."""

    def test_umd_package_is_clean_as_full(self):
        report = pc.PackageCheck(REAL_BUILD / "package-umd", model="full", expect_version="0.7.4",
                                 use_tools=False)
        report.run_all()
        self.assertFalse(report.report.failed,
                         [f.text for f in report.report.findings if f.level == pc.ERROR])
        self.assertIn("UMD005", codes(report.report, pc.OK))
        self.assertIn("VRS010", codes(report.report, pc.OK))

    def test_plain_package_fails_as_full(self):
        report = pc.PackageCheck(REAL_BUILD / "package", model="full", expect_version="0.7.4",
                                 use_tools=False)
        report.run_all()
        self.assertTrue(report.report.failed)
        self.assertIn("UMD001", codes(report.report, pc.ERROR))

    def test_plain_package_is_clean_as_display_only(self):
        report = pc.PackageCheck(REAL_BUILD / "package", model="display-only", expect_version="0.7.4",
                                 use_tools=False)
        report.run_all()
        self.assertFalse(report.report.failed,
                         [f.text for f in report.report.findings if f.level == pc.ERROR])

    def test_real_stub_exports_the_three_names_undecorated(self):
        check = pc.PackageCheck(REAL_BUILD / "package-umd", model="full", use_tools=False)
        check.run_all()
        image = check.images["bc250umd.dll"]
        self.assertEqual(image.exports, sorted(pc.UMD_REQUIRED_EXPORTS))

    @unittest.skipUnless(sys.platform == "win32" and struct.calcsize("P") == 8,
                         "the stub is AMD64 and this Python is not 64-bit Windows")
    def test_real_stub_answers_e_notimpl(self):
        """Load the stub the lab would install and call its three entry points. It takes no lock,
        starts no thread and touches no file; packagecheck frees it again."""
        check = pc.PackageCheck(REAL_BUILD / "package-umd", model="full", use_tools=False, load=True)
        check.run_all()
        answered = [f for f in check.report.findings if f.code == "LDR008"]
        self.assertEqual(len(answered), len(pc.UMD_REQUIRED_EXPORTS))
        self.assertFalse(check.report.failed,
                         [f.text for f in check.report.findings if f.level == pc.ERROR])

    def test_stale_sys_is_caught_against_a_real_older_binary(self):
        """Take the 0.7.4 package, drop the 0.7.3 .sys into it: the version check must notice."""
        older = pc.WORKSPACE / "scratch" / "build" / "bc250kmd-073" / "package" / "bc250kmd.sys"
        if not older.is_file():
            self.skipTest("no bc250kmd-073 package on this machine")
        root = pc.scratch_root()
        root.mkdir(parents=True, exist_ok=True)
        work = Path(tempfile.mkdtemp(prefix="packagecheck-stale-", dir=str(root)))
        try:
            staging = work / "package-umd"
            shutil.copytree(REAL_BUILD / "package-umd", staging)
            shutil.copy2(older, staging / "bc250kmd.sys")
            check = pc.PackageCheck(staging, model="full", expect_version="0.7.4", use_tools=False)
            check.run_all()
            self.assertTrue(check.report.failed)
            self.assertIn("VRS011", codes(check.report, pc.ERROR))
        finally:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
