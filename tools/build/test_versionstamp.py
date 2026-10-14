"""Host tests of versionstamp.py (BD-166) on images built here: no payload file, no device, no machine state."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import tempfile
import unittest

SCRIPT = Path(__file__).with_name("versionstamp.py")
spec = importlib.util.spec_from_file_location("versionstamp", SCRIPT)
vs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vs)

VERSION = "0.7.216.102-tester.31"
TABLE = {"schema": 1, "files": {"engine.dll": "test engine", "icd.dll": "test ICD", "tool.exe": "test tool",
                                "amdgpu_wddm_dxvk.dll": "test engine", "amdgpu_wddm_radv.dll": "test ICD"},
         "exempt": {"driver.sys": "a kernel driver of the test"}}


def align(v, a):
    return (v + a - 1) // a * a


def make_pe(plus=True, sections=((b".text", b"\xc3" * 16, 0x60000020),), leaves=None, checksum=False,
            headers=0x400, cert=False, overlay=b"", trailing=()):
    """A minimal DLL image: the given sections, a resource section with these leaves when there are any, then the
    trailing sections."""
    fa, sa = 0x200, 0x1000
    secs = []
    raw_at, va = headers, sa
    for name, data, flags in sections:
        secs.append([name, data, flags, va, raw_at])
        raw_at += align(len(data), fa)
        va += align(len(data), sa)
    if leaves:
        body = vs.build_resources(leaves, va)
        secs.append([b".rsrc", body, 0x40000040, va, raw_at])
        raw_at += align(len(body), fa)
        va += align(len(body), sa)
    for name, data, flags in trailing:
        secs.append([name, data, flags, va, raw_at])
        raw_at += align(len(data), fa)
        va += align(len(data), sa)
    last = secs[-1]
    size_of_image = align(last[3] + len(last[1]), sa)
    opt_size = 0xF0 if plus else 0xE0
    dirs = [(0, 0)] * 16
    if leaves:
        rsrc = [x for x in secs if x[0] == b".rsrc"][0]
        dirs[2] = (rsrc[3], len(rsrc[1]))
    if cert:
        dirs[4] = (raw_at, 8)
    if plus:
        opt = struct.pack("<HBBIIIIIQIIHHHHHHIIIIHHQQQQII", 0x20B, 14, 0, 0x200, 0, 0, 0, 0x1000, 0x180000000, sa, fa,
                          6, 0, 0, 0, 6, 0, 0, size_of_image, headers, 0, 2, 0x0100, 0x100000, 0x1000, 0x100000,
                          0x1000, 0, 16)
    else:
        opt = struct.pack("<HBBIIIIIIIIIHHHHHHIIIIHHIIIIII", 0x10B, 14, 0, 0x200, 0, 0, 0, 0x1000, 0, 0x10000000, sa,
                          fa, 6, 0, 0, 0, 6, 0, 0, size_of_image, headers, 0, 2, 0x0100, 0x100000, 0x1000, 0x100000,
                          0x1000, 0, 16)
    opt += b"".join(struct.pack("<II", *d) for d in dirs)
    assert len(opt) == opt_size
    machine, chars = (0x8664, 0x2022) if plus else (0x14C, 0x2102)
    out = bytearray(headers)
    out[0:2] = b"MZ"
    struct.pack_into("<I", out, 0x3C, 0x80)
    out[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", out, 0x84, machine, len(secs), 0, 0, 0, opt_size, chars)
    out[0x98:0x98 + opt_size] = opt
    for i, (name, data, flags, sva, sraw) in enumerate(secs):
        struct.pack_into("<8sIIIIIIHHI", out, 0x98 + opt_size + 40 * i, name, len(data), sva,
                         align(len(data), fa), sraw, 0, 0, 0, 0, flags)
    for name, data, flags, sva, sraw in secs:
        out += data + b"\0" * (align(len(data), fa) - len(data))
    if checksum:
        struct.pack_into("<I", out, 0x98 + 64, vs.pe_checksum(out, 0x98 + 64))
    if cert:
        out += b"\0" * 8
    return bytes(out) + overlay


def version_of(image_bytes):
    found = [leaf for leaf in vs.read_resources(vs.Image(image_bytes)) if leaf[0] == vs.RT_VERSION]
    assert len(found) == 1, found
    return found[0], vs.parse_version_resource(found[0][4])


class NumberTests(unittest.TestCase):
    def test_amd_scheme(self):
        self.assertEqual(vs.amd_version("0.7.216.102-tester.31"), (40, 7, 216, 102))
        self.assertEqual(vs.amd_version("0.7.216.102"), (40, 7, 216, 102))
        self.assertEqual(vs.amd_version("65494.0.0.65534"), (65534, 0, 0, 65534))

    def test_refusals(self):
        for bad in ("0.7.216", "0.7.216.102-", "v0.7.216.102", "65495.0.0.0", "0.7.216.65535", ""):
            with self.assertRaises(vs.StampError, msg=bad):
                vs.amd_version(bad)


class StampTests(unittest.TestCase):
    def resource(self, name="engine.dll", version=VERSION):
        return vs.expected_resource(name, version, "amdgpu-wddm", TABLE)

    def test_append_keeps_the_image(self):
        for plus in (True, False):
            source = make_pe(plus=plus, sections=((b".text", b"\xc3" * 300, 0x60000020),
                                                  (b".rdata", b"r" * 5000, 0x40000040)))
            stamped = vs.stamp_bytes(source, self.resource())
            old, new = vs.Image(source), vs.Image(stamped)
            self.assertEqual(new.nsections, old.nsections + 1)
            self.assertEqual(stamped[0x400:len(source)], source[0x400:])
            self.assertEqual(new.directory(2)[0], new.sections[-1]["va"])
            self.assertEqual(new.size_of_image, align(new.sections[-1]["va"] + new.sections[-1]["vsize"], 0x1000))
            leaf, (fv, pv, kind, strings) = version_of(stamped)
            self.assertEqual((leaf[1], leaf[2]), (1, 0x409))
            self.assertEqual((fv, pv, kind), ((40, 7, 216, 102), (40, 7, 216, 102), 2))
            self.assertEqual(strings, {"CompanyName": "amdgpu-wddm", "FileDescription": "test engine",
                                       "FileVersion": "40.7.216.102", "InternalName": "engine",
                                       "OriginalFilename": "engine.dll", "ProductName": "amdgpu-wddm",
                                       "ProductVersion": VERSION})

    def test_kernel_reading(self):
        # dxgkrnl DxgkpGetFileVersion: more than 0x5B bytes, "VS_VERSION_INFO" at 6, the file version at 0x30/0x34.
        block = self.resource()
        self.assertGreater(len(block), 0x5B)
        self.assertEqual(block[6:6 + 30].decode("utf-16-le"), "VS_VERSION_INFO")
        ms, ls = struct.unpack_from("<II", block, 0x30)
        self.assertEqual((ms, ls), (40 << 16 | 7, 216 << 16 | 102))

    def test_deterministic(self):
        source = make_pe()
        self.assertEqual(vs.stamp_bytes(source, self.resource()), vs.stamp_bytes(source, self.resource()))

    def test_restamp_replaces_and_keeps_other_resources(self):
        manifest = b"<assembly/>"
        old_version = vs.version_resource((0, 7, 0, 0), (0, 7, 0, 0), [("FileVersion", "0.7.0.0")], 1)
        source = make_pe(leaves=[(24, 1, 0, 0, manifest), (16, 1, 0, 0, old_version), (3, "NAMED", 0x409, 0, b"x" * 9)])
        stamped = vs.stamp_bytes(source, self.resource("tool.exe"))
        leaves = vs.read_resources(vs.Image(stamped))
        self.assertIn((24, 1, 0, 0, manifest), leaves)
        self.assertIn((3, "NAMED", 0x409, 0, b"x" * 9), leaves)
        self.assertEqual([leaf[:3] for leaf in leaves if leaf[0] == 16], [(16, 1, 0x409)])
        self.assertEqual(vs.Image(stamped).nsections, vs.Image(source).nsections)
        again = vs.stamp_bytes(stamped, self.resource("tool.exe", "0.7.216.103-tester.32"))
        self.assertEqual(version_of(again)[1][0], (40, 7, 216, 103))

    def test_checksum_is_written_again(self):
        source = make_pe(checksum=True)
        stamped = vs.stamp_bytes(source, self.resource())
        image = vs.Image(stamped)
        self.assertNotEqual(image.checksum, vs.Image(source).checksum)
        self.assertEqual(image.checksum, vs.pe_checksum(stamped, image.opt + 64))
        if os.name == "nt":
            import ctypes
            buf = ctypes.create_string_buffer(stamped, len(stamped))
            header, total = ctypes.c_uint32(), ctypes.c_uint32()
            ctypes.WinDLL("imagehlp").CheckSumMappedFile(buf, len(stamped), ctypes.byref(header), ctypes.byref(total))
            self.assertEqual((header.value, total.value), (image.checksum, image.checksum))

    def test_refusals(self):
        cases = {"signature": make_pe(cert=True), "overlay": make_pe(overlay=b"tail" * 128),
                 "no room": make_pe(headers=0x200, sections=[(b".a", b"1", 0x40000040), (b".b", b"2", 0x40000040),
                                                             (b".c", b"3", 0x40000040)]),
                 "resources not last": make_pe(leaves=[(24, 1, 0, 0, b"m")], trailing=[(b".reloc", b"r", 0x42000040)])}
        for name, data in cases.items():
            with self.assertRaises(vs.StampError, msg=name):
                vs.stamp_bytes(data, self.resource())

    def test_windows_reads_it(self):
        if os.name != "nt":
            self.skipTest("Windows image loader")
        with tempfile.TemporaryDirectory() as d:
            for plus in (True, False):
                p = Path(d) / f"engine-{plus}.dll"
                p.write_bytes(vs.stamp_bytes(make_pe(plus=plus), self.resource()))
                fv, pv, strings = vs.windows_read(p)
                self.assertEqual((fv, pv), ((40, 7, 216, 102), (40, 7, 216, 102)))
                self.assertEqual(strings["ProductVersion"], VERSION)
                self.assertEqual(strings["FileVersion"], "40.7.216.102")
            p = Path(d) / "tool.exe"
            p.write_bytes(vs.stamp_bytes(make_pe(leaves=[(24, 1, 0, 0, b"<assembly/>")]), self.resource("tool.exe")))
            self.assertEqual(vs.windows_read(p)[2]["OriginalFilename"], "tool.exe")


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.top = Path(self.temp.name) / "release"
        pkg = self.top / "files"
        d3d11 = pkg / "payload" / "d3d11"
        d3d11.mkdir(parents=True)
        (pkg / "payload" / "kmd").mkdir()
        (pkg / "payload" / "tools").mkdir()
        (d3d11 / "amdgpu_wddm_dxvk.dll").write_bytes(make_pe(sections=((b".text", b"E" * 64, 0x60000020),)))
        (d3d11 / "amdgpu_wddm_radv.dll").write_bytes(make_pe(sections=((b".text", b"I" * 64, 0x60000020),)))
        pins = [hashlib.sha256((d3d11 / n).read_bytes()).digest() for n in ("amdgpu_wddm_dxvk.dll", "amdgpu_wddm_radv.dll")]
        self.caps = struct.pack("<17I", 0x4334314D, 2, 132, 0, 0xC100, 1, 1, 1, 0, 0, 0, 3, 1, 1, 1, 3, 1)
        (d3d11 / vs.M14C_NAME).write_bytes(self.caps + pins[0] + pins[1])
        (pkg / "payload" / "kmd" / "driver.sys").write_bytes(make_pe())
        (pkg / "payload" / "tools" / "other.exe").write_bytes(make_pe(cert=True))
        (pkg / "payload" / "kmd" / "bc250kmd.inf").write_text('[Version]\nProvider = %Provider%\n\n[Strings]\nProvider    = "amdgpu-wddm"\n', encoding="ascii")
        (self.top / "setup.exe").write_bytes(make_pe())
        self.sources = Path(self.temp.name) / "release-sources.json"
        self.sources.write_text(json.dumps({"files": [{"path": "payload/tools/other.exe", "third_party": {}}]}), encoding="utf-8")
        self.inf = pkg / "payload" / "kmd" / "bc250kmd.inf"
        self.table = dict(TABLE, files=dict(TABLE["files"], **{"setup.exe": "test setup"}))

    def test_release_then_gate(self):
        quiet = lambda *_: None
        before = vs.check_release(self.top, VERSION, self.sources, self.inf, self.table, log=quiet)
        self.assertTrue(any("amdgpu_wddm_dxvk.dll: no version resource" in p for p in before))
        rows = vs.stamp_release(self.top / "files", VERSION, self.sources, self.inf, self.table, log=quiet,
                                setup=self.top / "setup.exe")
        states = {rel: state for rel, state, _, _ in rows}
        self.assertEqual(states["payload/tools/other.exe"], "third-party")
        self.assertEqual(states["payload/kmd/driver.sys"], "exempt")
        self.assertEqual(states["payload/d3d11/" + vs.M14C_NAME], "re-pointed")
        self.assertEqual(states["setup.exe"], "stamped")
        record = (self.top / "files" / "payload" / "d3d11" / vs.M14C_NAME).read_bytes()
        self.assertEqual(record[:68], self.caps)
        engine = (self.top / "files" / "payload" / "d3d11" / "amdgpu_wddm_dxvk.dll").read_bytes()
        self.assertEqual(record[68:100], hashlib.sha256(engine).digest())
        self.assertEqual(vs.check_release(self.top, VERSION, self.sources, self.inf, self.table, log=quiet), [])
        later = vs.check_release(self.top, "0.7.216.103-tester.32", self.sources, self.inf, self.table, log=quiet)
        self.assertTrue(any("this release is 40.7.216.103" in p for p in later))

    def test_record_of_another_pair_is_refused(self):
        cfg = self.top / "files" / "payload" / "d3d11" / vs.M14C_NAME
        cfg.write_bytes(self.caps + b"\1" * 64)
        with self.assertRaises(vs.StampError):
            vs.stamp_release(self.top / "files", VERSION, self.sources, self.inf, self.table, log=lambda *_: None)

    def test_file_without_a_description_is_refused(self):
        (self.top / "files" / "payload" / "tools" / "new.dll").write_bytes(make_pe())
        with self.assertRaises(vs.StampError):
            vs.stamp_release(self.top / "files", VERSION, self.sources, self.inf, self.table, log=lambda *_: None)

    def test_repository_table(self):
        table = vs.load_table()
        for name, text in table["files"].items():
            self.assertTrue(text.strip(), name)
        for name, reason in list(table["exempt"].items()) + list(table["built"].items()):
            self.assertGreater(len(reason.split()), 6, name)
        self.assertIn("bc250kmd.sys", table["built"])

    def test_built_file_is_checked_not_stamped(self):
        quiet = lambda *_: None
        table = dict(self.table, exempt={}, built={"driver.sys": "its own .rc in the test"},
                     files=dict(self.table["files"], **{"driver.sys": "test driver"}))
        sys_path = self.top / "files" / "payload" / "kmd" / "driver.sys"
        linked = sys_path.read_bytes()
        rows = vs.stamp_release(self.top / "files", VERSION, self.sources, self.inf, table, log=quiet,
                                setup=self.top / "setup.exe")
        self.assertEqual({rel: state for rel, state, _, _ in rows}["payload/kmd/driver.sys"], "built")
        self.assertEqual(sys_path.read_bytes(), linked)     # never stamped after the link
        problems = vs.check_release(self.top, VERSION, self.sources, self.inf, table, log=quiet)
        self.assertEqual(problems, ["files/payload/kmd/driver.sys: no version resource"])
        # The build's own resource (here the stamp's bytes stand in for rc.exe's; tools\rc-test.ps1 of BD-165 showed
        # that bc250kmd.rc compiles to these bytes): VFT_DRV, VFT2_DRV_SYSTEM.
        built = vs.expected_resource("driver.sys", VERSION, "amdgpu-wddm", table)
        sys_path.write_bytes(vs.stamp_bytes(linked, built))
        self.assertEqual(vs.check_release(self.top, VERSION, self.sources, self.inf, table, log=quiet), [])
        self.assertEqual(vs.parse_version_resource(built)[2], 3)
        self.assertEqual(struct.unpack_from("<13I", built, 40)[10], 7)
        # A resource with another number fails like a stamped file with it.
        sys_path.write_bytes(vs.stamp_bytes(linked, vs.expected_resource("driver.sys", "0.7.216.101", "amdgpu-wddm", table)))
        problems = vs.check_release(self.top, VERSION, self.sources, self.inf, table, log=quiet)
        self.assertTrue(any("driver.sys: FILEVERSION 40.7.216.101" in p for p in problems), problems)

    def test_lab_flavour(self):
        # One release, two flavours: the same numbers; the lab flavour is a private build (VS_FF_PRIVATEBUILD and
        # PrivateBuild "lab"). The gate checks one flavour, so a lab package fails the release gate and back.
        quiet = lambda *_: None
        vs.stamp_release(self.top / "files", VERSION, self.sources, self.inf, self.table, log=quiet,
                         setup=self.top / "setup.exe", flavour="lab")
        engine = self.top / "files" / "payload" / "d3d11" / "amdgpu_wddm_dxvk.dll"
        leaf, (fv, pv, kind, strings) = version_of(engine.read_bytes())
        self.assertEqual((fv, pv), ((40, 7, 216, 102), (40, 7, 216, 102)))
        self.assertEqual(strings["PrivateBuild"], "lab")
        self.assertEqual(vs.file_flags(leaf[4]), vs.VS_FF_PRIVATEBUILD)
        self.assertEqual(vs.file_flags(self.resource_of("engine.dll", "release")), 0)
        self.assertEqual(vs.check_release(self.top, VERSION, self.sources, self.inf, self.table, log=quiet,
                                          flavour="lab"), [])
        wrong = vs.check_release(self.top, VERSION, self.sources, self.inf, self.table, log=quiet)
        self.assertTrue(any("amdgpu_wddm_dxvk.dll: PrivateBuild is 'lab', the release flavour has no PrivateBuild" in p
                            for p in wrong), wrong)
        self.assertTrue(any("amdgpu_wddm_dxvk.dll: FILEFLAGS 0x8, the release flavour has 0x0" in p for p in wrong), wrong)
        if os.name == "nt":
            self.assertEqual(vs.windows_read(engine)[2]["PrivateBuild"], "lab")
        text = vs.rc_header("engine.dll", VERSION, "amdgpu-wddm", self.table, "lab")
        self.assertIn("#define BC250_VER_LAB 1\n", text)
        with self.assertRaises(vs.StampError):
            vs.expected_resource("engine.dll", VERSION, "amdgpu-wddm", self.table, "debug")

    def test_release_package_fails_the_lab_gate(self):
        quiet = lambda *_: None
        vs.stamp_release(self.top / "files", VERSION, self.sources, self.inf, self.table, log=quiet,
                         setup=self.top / "setup.exe")
        wrong = vs.check_release(self.top, VERSION, self.sources, self.inf, self.table, log=quiet, flavour="lab")
        self.assertTrue(any("setup.exe: PrivateBuild is None, wanted 'lab'" in p for p in wrong), wrong)

    def resource_of(self, name, flavour):
        return vs.expected_resource(name, VERSION, "amdgpu-wddm", self.table, flavour)

    def test_rc_header(self):
        table = dict(self.table, files=dict(self.table["files"], **{"driver.sys": "test driver"}))
        text = vs.rc_header("driver.sys", VERSION, "amdgpu-wddm", table)
        self.assertIn("#define BC250_VER_FILEVERSION 40,7,216,102\n", text)
        self.assertIn('#define BC250_VER_FILEVERSION_STR "40.7.216.102"\n', text)
        self.assertIn('#define BC250_VER_PRODUCTVERSION_STR "0.7.216.102-tester.31"\n', text)
        self.assertIn('#define BC250_VER_COMPANY "amdgpu-wddm"\n', text)
        self.assertIn('#define BC250_VER_DESCRIPTION "test driver"\n', text)
        self.assertNotIn("BC250_VER_LAB", text)
        self.assertIn("#define BC250_VER_LAB 1\n", vs.rc_header("driver.sys", VERSION, "amdgpu-wddm", table, "lab"))
        with self.assertRaises(vs.StampError):
            vs.rc_header("driver.sys", VERSION, 'quote"d', table)

    def test_gate_checks_m14c_after_the_stamp(self):
        quiet = lambda *_: None
        vs.stamp_release(self.top / "files", VERSION, self.sources, self.inf, self.table, log=quiet)
        vs.stamp_file(self.top / "setup.exe", VERSION, "amdgpu-wddm", self.table)
        cfg = self.top / "files" / "payload" / "d3d11" / vs.M14C_NAME
        cfg.write_bytes(cfg.read_bytes()[:68] + b"\2" * 64)
        problems = vs.check_release(self.top, VERSION, self.sources, self.inf, self.table, log=quiet)
        self.assertTrue(any("pins amdgpu_wddm_dxvk.dll" in p for p in problems), problems)


@unittest.skipUnless(os.environ.get("BC250_SIGNED_PACKAGE") and os.name == "nt",
                     "BC250_SIGNED_PACKAGE: a folder with a signed bc250kmd.sys and the signed bc250kmd.cat that lists it")
class OrderTests(unittest.TestCase):
    """The order gate on a real signed driver package: as built it passes; a byte changed in the image after Inf2Cat
    and signtool is found by both checks."""

    def test_signed_package(self):
        src = Path(os.environ["BC250_SIGNED_PACKAGE"])
        with tempfile.TemporaryDirectory() as d:
            top = Path(d)
            for name in ("bc250kmd.sys", "bc250kmd.cat"):
                (top / name).write_bytes((src / name).read_bytes())
            sys_path = top / "bc250kmd.sys"
            self.assertEqual(vs.order_problems(top, [("bc250kmd.sys", sys_path)], log=lambda *_: None), [])
            self.assertEqual(vs.signature_digest(sys_path), "covers the file")
            # --signed: the signed, listed driver passes; an unsigned PE file beside it, or no catalog, fails.
            quiet = lambda *_: None
            self.assertEqual(vs.order_problems(top, [("bc250kmd.sys", sys_path)], quiet, require_signed=True), [])
            other = top / "other.dll"
            other.write_bytes(make_pe())
            problems = vs.order_problems(top, [("bc250kmd.sys", sys_path), ("other.dll", other)], quiet, True)
            self.assertEqual(len(problems), 1, problems)
            self.assertIn("other.dll: not signed", problems[0])
            other.unlink()
            with tempfile.TemporaryDirectory() as bare:
                alone = Path(bare) / "bc250kmd.sys"
                alone.write_bytes(sys_path.read_bytes())
                problems = vs.order_problems(Path(bare), [("bc250kmd.sys", alone)], quiet, True)
                self.assertEqual(len(problems), 1, problems)
                self.assertIn("no catalog", problems[0])
            image = vs.Image(sys_path.read_bytes())
            data = bytearray(sys_path.read_bytes())
            text = next(s for s in image.sections if s["name"] == b".text")
            data[text["rptr"] + 0x10] ^= 0xFF
            sys_path.write_bytes(bytes(data))
            problems = vs.order_problems(top, [("bc250kmd.sys", sys_path)], log=lambda *_: None)
            self.assertEqual(len(problems), 2, problems)
            self.assertIn("changed after signing", problems[0])
            self.assertIn("changed after Inf2Cat", problems[1])


if __name__ == "__main__":
    unittest.main()
