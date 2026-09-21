#!/usr/bin/env python3
"""Tests for fwinfo. The containers are built here, byte by byte, so that the tests
need no firmware blob (and none is in this repository).

    python -m unittest discover -s tools/fwinfo

One test runs `check` over the real files instead, when BC250_FIRMWARE_DIR points at a
directory of them. It is skipped otherwise.
"""

import io
import json
import os
import struct
import tempfile
import unittest
import zlib
from contextlib import redirect_stdout, redirect_stderr
from pathlib import Path

import fwinfo


# ---- synthetic containers -------------------------------------------------------------

def build(struct_name, payload_size=256, header_size=None, size_bytes=None,
          ucode_offset=256, ucode_size=None, fields=None, payload=None, truncate=None,
          crc=None, array=()):
    """A container that is correct unless an argument says otherwise.

    Defaults mirror the real files: the payload starts at 256, the header is as long as
    the struct for its version, size_bytes is the file size and crc32 covers everything
    after the common header.
    """
    layout = fwinfo.struct_fields(struct_name)
    array_spec = fwinfo.struct_array(struct_name)
    values = {name: 0 for name, _ in layout}
    major, minor = struct_name.rsplit("_v", 1)[1].split("_")
    values["header_version_major"] = int(major)
    values["header_version_minor"] = int(minor)
    values["ip_version_major"] = 10
    values["ip_version_minor"] = 1
    values["ucode_version"] = 0x63
    if array_spec is not None:
        values[array_spec[0]] = len(array)
    element_size = 0 if array_spec is None else sum(fwinfo.WIDTH[w] for _, w in array_spec[2])
    natural_header = fwinfo.struct_fixed_size(struct_name) + len(array) * element_size

    values["header_size_bytes"] = natural_header if header_size is None else header_size
    values["ucode_array_offset_bytes"] = ucode_offset
    values["ucode_size_bytes"] = payload_size if ucode_size is None else ucode_size
    if fields:
        values.update(fields)

    blob = bytearray()
    for name, width in layout:
        blob += values[name].to_bytes(fwinfo.WIDTH[width], "little")
    for entry in array:
        for name, width in array_spec[2]:
            blob += entry.get(name, 0).to_bytes(fwinfo.WIDTH[width], "little")
    blob += b"\0" * max(0, ucode_offset - len(blob))
    body = payload if payload is not None else bytes(range(256)) * (payload_size // 256 + 1)
    blob += body[:payload_size]

    total = len(blob) if size_bytes is None else size_bytes
    struct.pack_into("<I", blob, 0, total)
    struct.pack_into("<I", blob, 28, 0)
    struct.pack_into("<I", blob, 28,
                     zlib.crc32(bytes(blob[fwinfo.CRC_START:])) & 0xFFFFFFFF if crc is None else crc)
    if truncate is not None:
        del blob[truncate:]
    return bytes(blob)


class Container:
    """A synthetic file on disk under a name fwinfo can take the kind from."""

    def __init__(self, case, name, data):
        self.path = Path(case.tmp.name) / name
        self.path.write_bytes(data)

    def __fspath__(self):
        return str(self.path)


class FwinfoTestCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="fwinfo-test-")
        self.addCleanup(self.tmp.cleanup)

    def write(self, name, data):
        return Container(self, name, data)

    def run_command(self, argv):
        """(exit code, stdout) of a fwinfo command line."""
        out = io.StringIO()
        with redirect_stdout(out), redirect_stderr(out):
            code = fwinfo.main(argv)
        return code, out.getvalue()


# ---- the layout table ------------------------------------------------------------------

class TestLayout(FwinfoTestCase):
    def test_common_header_is_32_bytes(self):
        self.assertEqual(fwinfo.COMMON_SIZE, 32)
        self.assertEqual(fwinfo.struct_fixed_size("common_firmware_header"), 32)

    def test_sizes_match_the_real_files(self):
        # The three header sizes the eight cyan_skillfish2 files carry.
        self.assertEqual(fwinfo.struct_fixed_size("gfx_firmware_header_v1_0"), 44)
        self.assertEqual(fwinfo.struct_fixed_size("sdma_firmware_header_v1_0"), 48)
        self.assertEqual(fwinfo.struct_fixed_size("rlc_firmware_header_v2_0"), 104)

    def test_every_struct_starts_with_the_common_header(self):
        for name in fwinfo.STRUCTS:
            if name == "common_firmware_header":
                continue
            head = fwinfo.struct_fields(name)[:len(fwinfo.COMMON_FIELDS)]
            self.assertEqual(tuple(head), fwinfo.COMMON_FIELDS, name)

    def test_no_struct_has_a_duplicate_field(self):
        for name in fwinfo.STRUCTS:
            names = [field for field, _ in fwinfo.struct_fields(name)]
            self.assertEqual(len(names), len(set(names)), name)

    def test_every_kind_entry_names_a_defined_struct(self):
        for kind, versions in fwinfo.KIND_STRUCTS.items():
            for version, name in versions.items():
                self.assertIn(name, fwinfo.STRUCTS, "%s %s" % (kind, version))

    def test_every_referenced_field_exists(self):
        for table in (fwinfo.FEATURE_FIELDS, fwinfo.JT_FIELDS, fwinfo.REGION_FIELDS):
            for name, entry in table.items():
                known = {field for field, _ in fwinfo.struct_fields(name)}
                flat = [entry] if isinstance(entry, str) else \
                    [f for pair in entry for f in pair]
                for field in flat:
                    self.assertIn(field, known, "%s.%s" % (name, field))


# ---- parsing --------------------------------------------------------------------------

class TestParse(FwinfoTestCase):
    def test_gfx_v1_0(self):
        blob = self.write("chip_me.bin", build("gfx_firmware_header_v1_0", payload_size=1024,
                                               fields={"ucode_feature_version": 32,
                                                       "jt_offset": 200, "jt_size": 24}))
        header = fwinfo.parse(blob)
        self.assertEqual(header.kind, "gfx")
        self.assertEqual(header.struct_name, "gfx_firmware_header_v1_0")
        self.assertEqual(header.version, (1, 0))
        self.assertEqual(header.feature_version, 32)
        self.assertEqual(header.values["jt_size"], 24)
        self.assertEqual(header.values["size_bytes"], header.file_size)
        self.assertEqual(header.crc32_computed(), header.values["crc32"])

    def test_kind_from_the_file_name(self):
        for name, kind, label in (("cyan_skillfish2_ce.bin", "gfx", "CE"),
                                  ("cyan_skillfish2_mec2.bin", "gfx", "MEC2"),
                                  ("cyan_skillfish2_rlc.bin", "rlc", "RLC"),
                                  ("cyan_skillfish2_sdma.bin", "sdma", "SDMA0"),
                                  ("cyan_skillfish2_sdma1.bin", "sdma", "SDMA1")):
            self.assertEqual(fwinfo.kind_from_name(name), (kind, label), name)
        self.assertEqual(fwinfo.kind_from_name("something_else.bin"), (None, None))

    def test_kind_override(self):
        blob = self.write("mystery.bin", build("sdma_firmware_header_v1_0"))
        self.assertIsNone(fwinfo.parse(blob).struct_name)         # name says nothing
        self.assertEqual(fwinfo.parse(blob, kind="sdma").struct_name,
                         "sdma_firmware_header_v1_0")

    def test_rlc_v2_1_reads_the_inherited_fields(self):
        blob = self.write("chip_rlc.bin", build(
            "rlc_firmware_header_v2_1", payload_size=512,
            fields={"ucode_feature_version": 7, "save_restore_list_cntl_ucode_ver": 3}))
        header = fwinfo.parse(blob)
        self.assertEqual(header.struct_name, "rlc_firmware_header_v2_1")
        self.assertEqual(header.feature_version, 7)               # from the v2.0 base
        self.assertEqual(header.values["save_restore_list_cntl_ucode_ver"], 3)

    def test_psp_v2_0_flexible_array(self):
        entries = [{"fw_type": 1, "fw_version": 0x11, "offset_bytes": 256, "size_bytes": 64},
                   {"fw_type": 4, "fw_version": 0x22, "offset_bytes": 320, "size_bytes": 64}]
        blob = self.write("chip_sos.bin", build("psp_firmware_header_v2_0", payload_size=128,
                                                array=entries))
        header = fwinfo.parse(blob)
        self.assertEqual(header.values["psp_fw_bin_count"], 2)
        self.assertEqual([e["fw_version"] for e in header.array], [0x11, 0x22])
        self.assertEqual([], fwinfo.validate(header)[0])

    def test_unknown_header_version_is_reported(self):
        blob = self.write("chip_me.bin", build("gfx_firmware_header_v1_0",
                                               fields={"header_version_major": 9}))
        with self.assertRaises(fwinfo.FormatError) as caught:
            fwinfo.parse(blob)
        self.assertIn("9.0", str(caught.exception))

    def test_file_shorter_than_the_common_header(self):
        blob = self.write("chip_me.bin", b"\0" * 16)
        with self.assertRaises(fwinfo.FormatError):
            fwinfo.parse(blob)


# ---- validation --------------------------------------------------------------------------

class TestValidate(FwinfoTestCase):
    def check(self, name, data, kind=None):
        header = fwinfo.parse(self.write(name, data), kind)
        return fwinfo.validate(header)

    def test_good_file_has_no_errors_and_no_notes(self):
        errors, notes = self.check("chip_me.bin", build(
            "gfx_firmware_header_v1_0", payload_size=1024,
            fields={"jt_offset": 200, "jt_size": 24}))
        self.assertEqual(errors, [])
        self.assertEqual(notes, [])

    def test_truncated_file(self):
        # A file cut short: size_bytes still claims the original length.
        errors, _ = self.check("chip_me.bin", build("gfx_firmware_header_v1_0",
                                                    payload_size=1024, truncate=600))
        self.assertTrue(any("size_bytes" in e for e in errors), errors)
        self.assertTrue(any("does not fit in the file" in e for e in errors), errors)

    def test_payload_past_the_end_of_the_file(self):
        errors, _ = self.check("chip_me.bin", build("gfx_firmware_header_v1_0",
                                                    payload_size=256, ucode_size=4096))
        self.assertTrue(any("does not fit in the file" in e for e in errors), errors)

    def test_payload_inside_the_header(self):
        errors, _ = self.check("chip_me.bin", build("gfx_firmware_header_v1_0",
                                                    ucode_offset=40, payload_size=256))
        self.assertTrue(any("inside the header" in e for e in errors), errors)

    def test_jt_past_the_end_of_the_payload(self):
        errors, _ = self.check("chip_mec.bin", build(
            "gfx_firmware_header_v1_0", payload_size=1024,
            fields={"jt_offset": 250, "jt_size": 16}))          # 250 + 16 > 1024/4
        self.assertTrue(any("jt_offset" in e for e in errors), errors)

    def test_jt_exactly_at_the_end_is_fine(self):
        errors, _ = self.check("chip_mec.bin", build(
            "gfx_firmware_header_v1_0", payload_size=1024,
            fields={"jt_offset": 240, "jt_size": 16}))          # 240 + 16 == 1024/4
        self.assertEqual(errors, [])

    def test_zero_jt_is_not_an_error(self):
        # The rlc of this part carries jt_offset 0, jt_size 0.
        errors, _ = self.check("chip_rlc.bin", build("rlc_firmware_header_v2_0",
                                                     payload_size=512))
        self.assertEqual(errors, [])

    def test_wrong_size_bytes(self):
        errors, _ = self.check("chip_me.bin", build("gfx_firmware_header_v1_0",
                                                    payload_size=256, size_bytes=999))
        self.assertTrue(any("is not the file size" in e for e in errors), errors)

    def test_header_size_not_the_struct_size(self):
        errors, _ = self.check("chip_me.bin", build("gfx_firmware_header_v1_0",
                                                    payload_size=256, header_size=40))
        self.assertTrue(any("gfx_firmware_header_v1_0" in e and "40" in e for e in errors),
                        errors)

    def test_header_size_smaller_than_the_common_header(self):
        errors, _ = self.check("chip_me.bin", build("gfx_firmware_header_v1_0",
                                                    payload_size=256, header_size=8))
        self.assertTrue(any("common_firmware_header" in e for e in errors), errors)

    def test_crc_mismatch_is_a_note_not_an_error(self):
        errors, notes = self.check("chip_me.bin", build("gfx_firmware_header_v1_0",
                                                        payload_size=256, crc=0xDEADBEEF))
        self.assertEqual(errors, [])
        self.assertTrue(any("crc32" in n for n in notes), notes)

    def test_crc_covers_the_bytes_after_the_common_header(self):
        # Flipping a byte of the version-specific header tail, not of the payload, must
        # still break the checksum: the field covers file[32:], not just the payload.
        data = bytearray(build("gfx_firmware_header_v1_0", payload_size=256))
        data[36] ^= 0x01                                        # jt_offset, past the common header
        errors, notes = self.check("chip_me.bin", bytes(data))
        self.assertEqual(errors, [])
        self.assertTrue(any("crc32" in n for n in notes), notes)

    def test_region_outside_the_file(self):
        errors, _ = self.check("chip_rlc.bin", build(
            "rlc_firmware_header_v2_0", payload_size=512,
            fields={"reg_list_array_offset_bytes": 400, "reg_list_size_bytes": 8192}))
        self.assertTrue(any("reg_list_array_offset_bytes" in e for e in errors), errors)

    def test_array_entry_outside_the_file(self):
        entries = [{"fw_type": 1, "fw_version": 1, "offset_bytes": 256, "size_bytes": 1 << 20}]
        errors, _ = self.check("chip_sos.bin", build("psp_firmware_header_v2_0",
                                                     payload_size=128, array=entries))
        self.assertTrue(any("psp_fw_bin[0]" in e for e in errors), errors)

    def test_unknown_kind_reads_the_common_header_and_says_so(self):
        errors, notes = self.check("mystery.bin", build("gfx_firmware_header_v1_0"))
        self.assertEqual(errors, [])
        self.assertTrue(any("no kind" in n for n in notes), notes)


# ---- the command line ----------------------------------------------------------------------

class TestCommands(FwinfoTestCase):
    def test_check_exit_codes(self):
        good = self.write("chip_me.bin", build("gfx_firmware_header_v1_0", payload_size=256))
        bad = self.write("chip_pfp.bin", build("gfx_firmware_header_v1_0",
                                               payload_size=256, ucode_size=99999))
        code, out = self.run_command(["check", os.fspath(good)])
        self.assertEqual(code, 0, out)
        self.assertIn("OK", out)
        code, out = self.run_command(["check", os.fspath(bad)])
        self.assertEqual(code, 1, out)
        self.assertIn("FAIL", out)

    def test_check_strict_turns_the_crc_note_into_an_error(self):
        path = self.write("chip_me.bin", build("gfx_firmware_header_v1_0",
                                               payload_size=256, crc=0x1234))
        code, _ = self.run_command(["check", os.fspath(path)])
        self.assertEqual(code, 0)
        code, out = self.run_command(["check", "--strict", os.fspath(path)])
        self.assertEqual(code, 1, out)

    def test_dump_json(self):
        path = self.write("chip_sdma.bin", build("sdma_firmware_header_v1_0",
                                                 payload_size=256,
                                                 fields={"ucode_feature_version": 50}))
        code, out = self.run_command(["dump", "--json", os.fspath(path)])
        self.assertEqual(code, 0, out)
        parsed = json.loads(out)
        self.assertEqual(parsed[0]["struct"], "sdma_firmware_header_v1_0")
        self.assertEqual(parsed[0]["fields"]["ucode_feature_version"], 50)

    def test_dump_human_readable_names_every_field(self):
        path = self.write("chip_me.bin", build("gfx_firmware_header_v1_0", payload_size=256))
        code, out = self.run_command(["dump", os.fspath(path)])
        self.assertEqual(code, 0, out)
        for field, _ in fwinfo.struct_fields("gfx_firmware_header_v1_0"):
            self.assertIn(field, out)

    def test_versions_lists_every_file(self):
        self.write("chip_me.bin", build("gfx_firmware_header_v1_0", payload_size=256,
                                        fields={"ucode_feature_version": 32,
                                                "ucode_version": 0x63}))
        self.write("chip_sdma.bin", build("sdma_firmware_header_v1_0", payload_size=256,
                                          fields={"ucode_feature_version": 50,
                                                  "ucode_version": 0x34}))
        code, out = self.run_command(["versions", self.tmp.name])
        self.assertEqual(code, 0, out)
        self.assertIn("chip_me.bin", out)
        self.assertIn("0x00000063", out)
        self.assertIn("chip_sdma.bin", out)
        self.assertIn("0x00000034", out)

    def _info_file(self, text):
        path = Path(self.tmp.name) / "amdgpu_firmware_info.txt"
        path.write_text(text, encoding="utf-8")
        return str(path)

    def test_compare_match_and_mismatch(self):
        self.write("chip_me.bin", build("gfx_firmware_header_v1_0", payload_size=256,
                                        fields={"ucode_feature_version": 32,
                                                "ucode_version": 0x63}))
        self.write("chip_sdma.bin", build("sdma_firmware_header_v1_0", payload_size=256,
                                          fields={"ucode_feature_version": 50,
                                                  "ucode_version": 0x34}))
        info = self._info_file(
            "ME feature version: 32, firmware version: 0x00000063\n"
            "SDMA0 feature version: 50, firmware version: 0x00000034\n"
            "SMC feature version: 0, program: 0, firmware version: 0x00580600 (88.6.0)\n"
            "VBIOS version: 113-AMDRBN-003\n")
        code, out = self.run_command(["compare", self.tmp.name, info])
        self.assertEqual(code, 0, out)
        self.assertEqual(out.count("match"), 2, out)

        info = self._info_file("ME feature version: 32, firmware version: 0x00000099\n"
                               "SDMA0 feature version: 50, firmware version: 0x00000034\n")
        code, out = self.run_command(["compare", self.tmp.name, info])
        self.assertEqual(code, 1, out)
        self.assertIn("DIFFER", out)

    def test_compare_parses_the_smc_and_ta_line_shapes(self):
        table = fwinfo.parse_firmware_info(self._info_file(
            "ME feature version: 32, firmware version: 0x00000063\n"
            "TA XGMI feature version: 0x00000000, firmware version: 0x00000000\n"
            "SMC feature version: 0, program: 0, firmware version: 0x00580600 (88.6.0)\n"
            "VBIOS version: 113-AMDRBN-003\n"))
        self.assertEqual(table["ME"], (32, 0x63))
        self.assertEqual(table["TA XGMI"], (0, 0))
        self.assertEqual(table["SMC"], (0, 0x00580600))
        self.assertNotIn("VBIOS", table)


# ---- the real files, when they are at hand ------------------------------------------------

@unittest.skipUnless(os.environ.get("BC250_FIRMWARE_DIR"),
                     "set BC250_FIRMWARE_DIR to a directory of amdgpu firmware files")
class TestRealFiles(unittest.TestCase):
    def test_check_passes_over_the_whole_directory(self):
        directory = Path(os.environ["BC250_FIRMWARE_DIR"])
        files = [str(p) for p in fwinfo.firmware_files(directory)]
        self.assertTrue(files, "no .bin files in %s" % directory)
        out = io.StringIO()
        with redirect_stdout(out), redirect_stderr(out):
            code = fwinfo.main(["check"] + files)
        self.assertEqual(code, 0, out.getvalue())


if __name__ == "__main__":
    unittest.main()
