#!/usr/bin/env python3
"""fwinfo - the headers of AMD amdgpu firmware containers, read on the host.

The files in amdgpu/cyan_skillfish2_*.bin are a header followed by a payload. The
driver reads four numbers out of that header (where the payload starts, how long it
is, and for the MEC where the jump table sits inside it) and hands the result to the
PSP. If any of those numbers is wrong, the PSP gets fed something that is not
microcode, which on this part is hard to tell apart from "the PSP hates us today".
This tool reads the same numbers here, on the development PC, before any of that.

Nic nie jest tak proste, jak sie wydaje - nothing is as simple as it looks.

The structure layouts are one table (STRUCTS): field name and width, base struct
first, exactly as the kernel nests them. No offsets are written down anywhere; an
offset is the sum of the widths before it.

Usage:
    fwinfo.py dump <file...> [--json] [--kind KIND]
    fwinfo.py check <file...> [--kind KIND] [--strict]
    fwinfo.py versions <dir>
    fwinfo.py compare <dir> <amdgpu_firmware_info.txt>

PROVENANCE: the structure definitions and the meaning of every field follow
drivers/gpu/drm/amd/amdgpu/amdgpu_ucode.h and amdgpu_ucode.c of the Linux kernel
(MIT, Copyright Advanced Micro Devices, Inc.), through ref/linux-src and the slice
already imported into driver/shim/include/amdgpu_ucode.h.
"""

import argparse
import json
import re
import sys
import zlib
from pathlib import Path

# ---- field widths -------------------------------------------------------------------

U16 = "u16"
U32 = "u32"
WIDTH = {U16: 2, U32: 4}

# Every struct below is a flat run of u16 and u32 with natural alignment, so a field's
# offset is the sum of the widths in front of it and a struct's size is their total.
# That holds for all of them; anything with a hole would need real alignment rules.


def _legacy_desc(prefix):
    """struct psp_fw_legacy_bin_desc, flattened under a name."""
    return (
        (prefix + ".fw_version", U32),
        (prefix + ".offset_bytes", U32),
        (prefix + ".size_bytes", U32),
    )


# struct psp_fw_bin_desc, the element of the v2.0 flexible arrays.
PSP_FW_BIN_DESC = (
    ("fw_type", U32),
    ("fw_version", U32),
    ("offset_bytes", U32),
    ("size_bytes", U32),
)

COMMON_FIELDS = (
    ("size_bytes", U32),
    ("header_size_bytes", U32),
    ("header_version_major", U16),
    ("header_version_minor", U16),
    ("ip_version_major", U16),
    ("ip_version_minor", U16),
    ("ucode_version", U32),
    ("ucode_size_bytes", U32),
    ("ucode_array_offset_bytes", U32),
    ("crc32", U32),
)

# name -> (base struct or None, own fields, flexible array or None).
# A flexible array is (count_field, element_prefix, element_fields).
STRUCTS = {
    "common_firmware_header": (None, COMMON_FIELDS, None),

    "gfx_firmware_header_v1_0": ("common_firmware_header", (
        ("ucode_feature_version", U32),
        ("jt_offset", U32),
        ("jt_size", U32),
    ), None),
    "gfx_firmware_header_v2_0": ("common_firmware_header", (
        ("ucode_feature_version", U32),
        ("ucode_size_bytes_v2", U32),        # shadows the common field in C; renamed here
        ("ucode_offset_bytes", U32),
        ("data_size_bytes", U32),
        ("data_offset_bytes", U32),
        ("ucode_start_addr_lo", U32),
        ("ucode_start_addr_hi", U32),
    ), None),

    "rlc_firmware_header_v1_0": ("common_firmware_header", (
        ("ucode_feature_version", U32),
        ("save_and_restore_offset", U32),
        ("clear_state_descriptor_offset", U32),
        ("avail_scratch_ram_locations", U32),
        ("master_pkt_description_offset", U32),
    ), None),
    "rlc_firmware_header_v2_0": ("common_firmware_header", (
        ("ucode_feature_version", U32),
        ("jt_offset", U32),
        ("jt_size", U32),
        ("save_and_restore_offset", U32),
        ("clear_state_descriptor_offset", U32),
        ("avail_scratch_ram_locations", U32),
        ("reg_restore_list_size", U32),
        ("reg_list_format_start", U32),
        ("reg_list_format_separate_start", U32),
        ("starting_offsets_start", U32),
        ("reg_list_format_size_bytes", U32),
        ("reg_list_format_array_offset_bytes", U32),
        ("reg_list_size_bytes", U32),
        ("reg_list_array_offset_bytes", U32),
        ("reg_list_format_separate_size_bytes", U32),
        ("reg_list_format_separate_array_offset_bytes", U32),
        ("reg_list_separate_size_bytes", U32),
        ("reg_list_separate_array_offset_bytes", U32),
    ), None),
    "rlc_firmware_header_v2_1": ("rlc_firmware_header_v2_0", (
        ("reg_list_format_direct_reg_list_length", U32),
        ("save_restore_list_cntl_ucode_ver", U32),
        ("save_restore_list_cntl_feature_ver", U32),
        ("save_restore_list_cntl_size_bytes", U32),
        ("save_restore_list_cntl_offset_bytes", U32),
        ("save_restore_list_gpm_ucode_ver", U32),
        ("save_restore_list_gpm_feature_ver", U32),
        ("save_restore_list_gpm_size_bytes", U32),
        ("save_restore_list_gpm_offset_bytes", U32),
        ("save_restore_list_srm_ucode_ver", U32),
        ("save_restore_list_srm_feature_ver", U32),
        ("save_restore_list_srm_size_bytes", U32),
        ("save_restore_list_srm_offset_bytes", U32),
    ), None),
    "rlc_firmware_header_v2_2": ("rlc_firmware_header_v2_1", (
        ("rlc_iram_ucode_size_bytes", U32),
        ("rlc_iram_ucode_offset_bytes", U32),
        ("rlc_dram_ucode_size_bytes", U32),
        ("rlc_dram_ucode_offset_bytes", U32),
    ), None),
    "rlc_firmware_header_v2_3": ("rlc_firmware_header_v2_2", (
        ("rlcp_ucode_version", U32),
        ("rlcp_ucode_feature_version", U32),
        ("rlcp_ucode_size_bytes", U32),
        ("rlcp_ucode_offset_bytes", U32),
        ("rlcv_ucode_version", U32),
        ("rlcv_ucode_feature_version", U32),
        ("rlcv_ucode_size_bytes", U32),
        ("rlcv_ucode_offset_bytes", U32),
    ), None),
    "rlc_firmware_header_v2_4": ("rlc_firmware_header_v2_3", (
        ("global_tap_delays_ucode_size_bytes", U32),
        ("global_tap_delays_ucode_offset_bytes", U32),
        ("se0_tap_delays_ucode_size_bytes", U32),
        ("se0_tap_delays_ucode_offset_bytes", U32),
        ("se1_tap_delays_ucode_size_bytes", U32),
        ("se1_tap_delays_ucode_offset_bytes", U32),
        ("se2_tap_delays_ucode_size_bytes", U32),
        ("se2_tap_delays_ucode_offset_bytes", U32),
        ("se3_tap_delays_ucode_size_bytes", U32),
        ("se3_tap_delays_ucode_offset_bytes", U32),
    ), None),

    "sdma_firmware_header_v1_0": ("common_firmware_header", (
        ("ucode_feature_version", U32),
        ("ucode_change_version", U32),
        ("jt_offset", U32),
        ("jt_size", U32),
    ), None),
    "sdma_firmware_header_v1_1": ("sdma_firmware_header_v1_0", (
        ("digest_size", U32),
    ), None),
    "sdma_firmware_header_v2_0": ("common_firmware_header", (
        ("ucode_feature_version", U32),
        ("ctx_ucode_size_bytes", U32),
        ("ctx_jt_offset", U32),
        ("ctx_jt_size", U32),
        ("ctl_ucode_offset", U32),
        ("ctl_ucode_size_bytes", U32),
        ("ctl_jt_offset", U32),
        ("ctl_jt_size", U32),
    ), None),
    "sdma_firmware_header_v3_0": ("common_firmware_header", (
        ("ucode_feature_version", U32),
        ("ucode_offset_bytes", U32),
        ("ucode_size_bytes_v3", U32),        # shadows the common field in C; renamed here
    ), None),

    # No cyan_skillfish PSP or TA file exists (see README); these come straight from the
    # kernel header and are exercised by synthetic containers in test_fwinfo.py only.
    "psp_firmware_header_v1_0": ("common_firmware_header", _legacy_desc("sos"), None),
    "psp_firmware_header_v1_1": ("psp_firmware_header_v1_0",
                                 _legacy_desc("toc") + _legacy_desc("kdb"), None),
    "psp_firmware_header_v1_2": ("psp_firmware_header_v1_0",
                                 _legacy_desc("res") + _legacy_desc("kdb"), None),
    "psp_firmware_header_v1_3": ("psp_firmware_header_v1_1",
                                 _legacy_desc("spl") + _legacy_desc("rl") +
                                 _legacy_desc("sys_drv_aux") + _legacy_desc("sos_aux"), None),
    "psp_firmware_header_v2_0": ("common_firmware_header", (
        ("psp_fw_bin_count", U32),
    ), ("psp_fw_bin_count", "psp_fw_bin", PSP_FW_BIN_DESC)),
    "psp_firmware_header_v2_1": ("common_firmware_header", (
        ("psp_fw_bin_count", U32),
        ("psp_aux_fw_bin_index", U32),
    ), ("psp_fw_bin_count", "psp_fw_bin", PSP_FW_BIN_DESC)),

    "ta_firmware_header_v1_0": ("common_firmware_header",
                                _legacy_desc("xgmi") + _legacy_desc("ras") +
                                _legacy_desc("hdcp") + _legacy_desc("dtm") +
                                _legacy_desc("securedisplay"), None),
    "ta_firmware_header_v2_0": ("common_firmware_header", (
        ("ta_fw_bin_count", U32),
    ), ("ta_fw_bin_count", "ta_fw_bin", PSP_FW_BIN_DESC)),
}

# (kind, header_version_major, header_version_minor) -> struct name. The container does
# not say which IP it belongs to; the file name does, the way amdgpu asks for it by name
# and then calls the matching amdgpu_ucode_print_*_hdr().
KIND_STRUCTS = {
    "gfx": {(1, 0): "gfx_firmware_header_v1_0", (2, 0): "gfx_firmware_header_v2_0"},
    "rlc": {(1, 0): "rlc_firmware_header_v1_0",
            (2, 0): "rlc_firmware_header_v2_0", (2, 1): "rlc_firmware_header_v2_1",
            (2, 2): "rlc_firmware_header_v2_2", (2, 3): "rlc_firmware_header_v2_3",
            (2, 4): "rlc_firmware_header_v2_4"},
    "sdma": {(1, 0): "sdma_firmware_header_v1_0", (1, 1): "sdma_firmware_header_v1_1",
             (2, 0): "sdma_firmware_header_v2_0", (3, 0): "sdma_firmware_header_v3_0"},
    "psp": {(1, 0): "psp_firmware_header_v1_0", (1, 1): "psp_firmware_header_v1_1",
            (1, 2): "psp_firmware_header_v1_2", (1, 3): "psp_firmware_header_v1_3",
            (2, 0): "psp_firmware_header_v2_0", (2, 1): "psp_firmware_header_v2_1"},
    "ta": {(1, 0): "ta_firmware_header_v1_0", (2, 0): "ta_firmware_header_v2_0"},
}

# file name suffix (after the last underscore) -> (kind, label in amdgpu_firmware_info).
# amdgpu asks for _asd.bin as a psp_firmware_header_v1_0 (amdgpu_psp.c psp_init_asd_microcode)
# and for _ta.bin as a ta_firmware_header (parse_ta_v1_microcode).
SUFFIX_KINDS = {
    "ce": ("gfx", "CE"),
    "pfp": ("gfx", "PFP"),
    "me": ("gfx", "ME"),
    "mec": ("gfx", "MEC"),
    "mec2": ("gfx", "MEC2"),
    "rlc": ("rlc", "RLC"),
    "sdma": ("sdma", "SDMA0"),
    "sdma1": ("sdma", "SDMA1"),
    "sos": ("psp", "SOS"),
    "asd": ("psp", "ASD"),
    "toc": ("psp", "TOC"),
    "ta": ("ta", None),
}

# Which field carries the feature version, per struct (inherited along the base chain).
# For the PSP containers amdgpu takes sos.fw_version as the feature version
# (amdgpu_psp.c: psp_init_asd_microcode).
FEATURE_FIELDS = {
    "gfx_firmware_header_v1_0": "ucode_feature_version",
    "gfx_firmware_header_v2_0": "ucode_feature_version",
    "rlc_firmware_header_v1_0": "ucode_feature_version",
    "rlc_firmware_header_v2_0": "ucode_feature_version",
    "sdma_firmware_header_v1_0": "ucode_feature_version",
    "sdma_firmware_header_v2_0": "ucode_feature_version",
    "sdma_firmware_header_v3_0": "ucode_feature_version",
    "psp_firmware_header_v1_0": "sos.fw_version",
}

# Jump table, in dwords relative to ucode_array_offset_bytes
# (amdgpu_ucode.c amdgpu_ucode_init_single_fw: jt_size * 4, jt_offset * 4).
JT_FIELDS = {
    "gfx_firmware_header_v1_0": (("jt_offset", "jt_size"),),
    "rlc_firmware_header_v2_0": (("jt_offset", "jt_size"),),
    "sdma_firmware_header_v1_0": (("jt_offset", "jt_size"),),
    "sdma_firmware_header_v2_0": (("ctx_jt_offset", "ctx_jt_size"),
                                  ("ctl_jt_offset", "ctl_jt_size")),
}

# Further (offset field, size field) pairs that must land inside the file. Offsets are
# from the start of the header, sizes in bytes, same as ucode_array_offset_bytes.
# Inherited along the base chain; a pair with size 0 is "not present" and is skipped.
REGION_FIELDS = {
    "rlc_firmware_header_v2_0": (
        ("reg_list_format_array_offset_bytes", "reg_list_format_size_bytes"),
        ("reg_list_array_offset_bytes", "reg_list_size_bytes"),
        ("reg_list_format_separate_array_offset_bytes", "reg_list_format_separate_size_bytes"),
        ("reg_list_separate_array_offset_bytes", "reg_list_separate_size_bytes"),
    ),
    "rlc_firmware_header_v2_1": (
        ("save_restore_list_cntl_offset_bytes", "save_restore_list_cntl_size_bytes"),
        ("save_restore_list_gpm_offset_bytes", "save_restore_list_gpm_size_bytes"),
        ("save_restore_list_srm_offset_bytes", "save_restore_list_srm_size_bytes"),
    ),
    "rlc_firmware_header_v2_2": (
        ("rlc_iram_ucode_offset_bytes", "rlc_iram_ucode_size_bytes"),
        ("rlc_dram_ucode_offset_bytes", "rlc_dram_ucode_size_bytes"),
    ),
    "rlc_firmware_header_v2_3": (
        ("rlcp_ucode_offset_bytes", "rlcp_ucode_size_bytes"),
        ("rlcv_ucode_offset_bytes", "rlcv_ucode_size_bytes"),
    ),
    "gfx_firmware_header_v2_0": (
        ("ucode_offset_bytes", "ucode_size_bytes_v2"),
        ("data_offset_bytes", "data_size_bytes"),
    ),
    "sdma_firmware_header_v3_0": (
        ("ucode_offset_bytes", "ucode_size_bytes_v3"),
    ),
    "psp_firmware_header_v1_0": (("sos.offset_bytes", "sos.size_bytes"),),
    "psp_firmware_header_v1_1": (("toc.offset_bytes", "toc.size_bytes"),
                                 ("kdb.offset_bytes", "kdb.size_bytes")),
    "psp_firmware_header_v1_2": (("res.offset_bytes", "res.size_bytes"),
                                 ("kdb.offset_bytes", "kdb.size_bytes")),
    "psp_firmware_header_v1_3": (("spl.offset_bytes", "spl.size_bytes"),
                                 ("rl.offset_bytes", "rl.size_bytes"),
                                 ("sys_drv_aux.offset_bytes", "sys_drv_aux.size_bytes"),
                                 ("sos_aux.offset_bytes", "sos_aux.size_bytes")),
    "ta_firmware_header_v1_0": (("xgmi.offset_bytes", "xgmi.size_bytes"),
                                ("ras.offset_bytes", "ras.size_bytes"),
                                ("hdcp.offset_bytes", "hdcp.size_bytes"),
                                ("dtm.offset_bytes", "dtm.size_bytes"),
                                ("securedisplay.offset_bytes", "securedisplay.size_bytes")),
}

COMMON_SIZE = sum(WIDTH[w] for _, w in COMMON_FIELDS)      # 32

# The header's crc32 is a plain CRC-32/ISO-HDLC (zlib) over everything after the common
# header, to the end of the file. The kernel only prints the field and never checks it
# (amdgpu_ucode.c:50), so the range is not written down there: it was found here, and all
# eight cyan_skillfish2 files agree on it across three different header sizes. See README.
CRC_START = COMMON_SIZE


# ---- struct machinery ---------------------------------------------------------------

class FormatError(Exception):
    """The bytes cannot be read as the header they claim to be."""


def struct_chain(name):
    """[base, ..., name], outermost last."""
    chain = []
    while name is not None:
        chain.insert(0, name)
        name = STRUCTS[name][0]
    return chain


def struct_fields(name):
    """Flat (field, width) list, base fields first, as the C struct lays them out."""
    fields = []
    for link in struct_chain(name):
        fields.extend(STRUCTS[link][1])
    return fields


def struct_array(name):
    """The flexible array of this struct or of a base, or None."""
    for link in reversed(struct_chain(name)):
        if STRUCTS[link][2] is not None:
            return STRUCTS[link][2]
    return None


def struct_fixed_size(name):
    """Size of the struct without its flexible array."""
    return sum(WIDTH[w] for _, w in struct_fields(name))


def _inherited(table, name):
    """Entries of `table` for this struct and all its bases, base first."""
    out = []
    for link in struct_chain(name):
        value = table.get(link)
        if value is None:
            continue
        out.extend(value if isinstance(value, tuple) else [value])
    return out


def read_fields(data, offset, fields):
    """(dict, next offset). Little endian: these files are, and so is every machine here."""
    values = {}
    for name, width in fields:
        end = offset + WIDTH[width]
        if end > len(data):
            raise FormatError("field %s ends at %d, past the end of the file (%d bytes)"
                              % (name, end, len(data)))
        values[name] = int.from_bytes(data[offset:end], "little")
        offset = end
    return values, offset


# ---- one file -----------------------------------------------------------------------

def kind_from_name(path):
    """('gfx', 'CE') from cyan_skillfish2_ce.bin, or (None, None)."""
    suffix = Path(path).stem.rsplit("_", 1)[-1].lower()
    return SUFFIX_KINDS.get(suffix, (None, None))


class Header:
    def __init__(self, path, data, kind):
        self.path = Path(path)
        self.file_size = len(data)
        self.data = data
        self.kind = kind
        self.label = None
        self.struct_name = None
        self.values = {}
        self.array = []                 # list of dicts, for the v2.0 flexible arrays
        self.order = []                 # field names in layout order

    # -- convenience --------------------------------------------------------------
    @property
    def version(self):
        return (self.values.get("header_version_major"), self.values.get("header_version_minor"))

    @property
    def feature_version(self):
        names = _inherited(FEATURE_FIELDS, self.struct_name) if self.struct_name else []
        return self.values.get(names[-1]) if names else None

    def payload(self):
        off = self.values["ucode_array_offset_bytes"]
        return self.data[off:off + self.values["ucode_size_bytes"]]

    def crc32_computed(self):
        return zlib.crc32(self.data[CRC_START:]) & 0xFFFFFFFF

    def to_dict(self):
        out = {
            "file": str(self.path),
            "file_size": self.file_size,
            "kind": self.kind,
            "struct": self.struct_name,
            "fields": {name: self.values[name] for name in self.order},
        }
        if self.array:
            out["array"] = self.array
        return out


def parse(path, kind=None):
    """Read one container. Raises FormatError when the bytes cannot be a header at all."""
    data = Path(path).read_bytes()
    if len(data) < COMMON_SIZE:
        raise FormatError("%d bytes, a common_firmware_header is %d" % (len(data), COMMON_SIZE))

    guessed_kind, label = kind_from_name(path)
    header = Header(path, data, kind or guessed_kind)
    header.label = label
    header.values, _ = read_fields(data, 0, COMMON_FIELDS)
    header.order = [name for name, _ in COMMON_FIELDS]
    if header.kind is None:
        return header                    # common header only; check() says so

    versions = KIND_STRUCTS.get(header.kind)
    if versions is None:
        raise FormatError("unknown kind %r, known: %s"
                          % (header.kind, ", ".join(sorted(KIND_STRUCTS))))
    name = versions.get(header.version)
    if name is None:
        raise FormatError("%s header version %d.%d is not one the kernel defines (%s)"
                          % (header.kind, header.version[0], header.version[1],
                             ", ".join("%d.%d" % v for v in sorted(versions))))
    header.struct_name = name

    fields = struct_fields(name)
    header.values, offset = read_fields(data, 0, fields)
    header.order = [field for field, _ in fields]

    array = struct_array(name)
    if array is not None:
        count_field, prefix, element = array
        count = header.values[count_field]
        stride = sum(WIDTH[w] for _, w in element)
        if offset + count * stride > len(data):
            raise FormatError("%s says %d entries of %d bytes at offset %d, past the end "
                              "of the file (%d bytes)"
                              % (count_field, count, stride, offset, len(data)))
        for index in range(count):
            values, offset = read_fields(data, offset, element)
            values["index"] = index
            values["name"] = "%s[%d]" % (prefix, index)
            header.array.append(values)
    return header


# ---- validation ---------------------------------------------------------------------

HEX_FIELDS = ("ucode_version", "crc32")


def validate(header):
    """([errors], [notes]). An error is a file the driver must not feed to the PSP."""
    errors, notes = [], []
    values = header.values
    size_bytes = values["size_bytes"]
    header_size = values["header_size_bytes"]
    ucode_off = values["ucode_array_offset_bytes"]
    ucode_size = values["ucode_size_bytes"]

    # The one rule the kernel enforces: amdgpu_ucode.c amdgpu_ucode_validate().
    if size_bytes != header.file_size:
        errors.append("size_bytes %d is not the file size %d" % (size_bytes, header.file_size))

    if header_size < COMMON_SIZE:
        errors.append("header_size_bytes %d is smaller than a common_firmware_header (%d)"
                      % (header_size, COMMON_SIZE))
    if header_size > header.file_size:
        errors.append("header_size_bytes %d is past the end of the file (%d bytes)"
                      % (header_size, header.file_size))

    if header.struct_name is None:
        notes.append("no kind for this file name; only the common header was read "
                     "(use --kind to say which IP it belongs to)")
    else:
        want = struct_fixed_size(header.struct_name)
        if struct_array(header.struct_name) is not None:
            count_field, _, element = struct_array(header.struct_name)
            want += values[count_field] * sum(WIDTH[w] for _, w in element)
        if header_size != want:
            errors.append("header_size_bytes %d, but %s of the declared version %d.%d is "
                          "%d bytes" % (header_size, header.struct_name,
                                        header.version[0], header.version[1], want))

    if ucode_size == 0:
        errors.append("ucode_size_bytes is 0")
    if ucode_off < header_size:
        errors.append("ucode_array_offset_bytes %d is inside the header (%d bytes)"
                      % (ucode_off, header_size))
    if ucode_off > header.file_size or ucode_size > header.file_size - min(ucode_off, header.file_size):
        errors.append("the payload (offset %d, %d bytes) does not fit in the file (%d bytes)"
                      % (ucode_off, ucode_size, header.file_size))

    if header.struct_name is not None:
        errors.extend(_check_jt(header))
        errors.extend(_check_regions(header))

    computed = header.crc32_computed()
    if computed != values["crc32"]:
        notes.append("crc32 0x%08X, computed 0x%08X over file[%d:] - the kernel never "
                     "checks this field" % (values["crc32"], computed, CRC_START))
    return errors, notes


def _check_jt(header):
    """Jump table inside the payload. jt_offset and jt_size are in dwords."""
    errors = []
    ucode_size = header.values["ucode_size_bytes"]
    for offset_field, size_field in _inherited(JT_FIELDS, header.struct_name):
        jt_offset = header.values[offset_field]
        jt_size = header.values[size_field]
        if jt_size == 0:
            continue                    # no jump table; the rlc of this part has none
        if jt_size > ucode_size // 4 or jt_offset > ucode_size // 4 - jt_size:
            errors.append("%s %d + %s %d dwords do not fit in the payload (%d bytes)"
                          % (offset_field, jt_offset, size_field, jt_size, ucode_size))
    return errors


def _check_regions(header):
    """The other (offset, size) pairs of the header, in bytes from the start of the file."""
    errors = []
    for offset_field, size_field in _inherited(REGION_FIELDS, header.struct_name):
        offset = header.values[offset_field]
        size = header.values[size_field]
        if size == 0:
            continue
        if offset < header.values["header_size_bytes"] or offset > header.file_size or \
                size > header.file_size - min(offset, header.file_size):
            errors.append("%s %d + %s %d does not fit in the file (%d bytes)"
                          % (offset_field, offset, size_field, size, header.file_size))
    for entry in header.array:
        offset, size = entry["offset_bytes"], entry["size_bytes"]
        if size == 0:
            continue
        if offset > header.file_size or size > header.file_size - offset:
            errors.append("%s offset %d + size %d does not fit in the file (%d bytes)"
                          % (entry["name"], offset, size, header.file_size))
    return errors


# ---- commands -------------------------------------------------------------------------

def format_value(name, value):
    if name in HEX_FIELDS:
        return "0x%08X (%d)" % (value, value)
    return str(value)


def cmd_dump(args):
    parsed, failed = [], 0
    for path in args.files:
        try:
            parsed.append(parse(path, args.kind))
        except (FormatError, OSError) as exc:
            failed += 1
            print("%s: %s" % (path, exc), file=sys.stderr)
    if args.json:
        print(json.dumps([header.to_dict() for header in parsed], indent=2))
        return 1 if failed else 0
    for index, header in enumerate(parsed):
        if index:
            print()
        print("%s  (%d bytes)" % (header.path.name, header.file_size))
        print("  kind: %s, struct: %s" % (header.kind or "?", header.struct_name or
                                          "common_firmware_header only"))
        width = max(len(name) for name in header.order)
        for name in header.order:
            print("  %-*s  %s" % (width, name, format_value(name, header.values[name])))
        for entry in header.array:
            print("  %s: fw_type %d, fw_version 0x%08X, offset %d, %d bytes"
                  % (entry["name"], entry["fw_type"], entry["fw_version"],
                     entry["offset_bytes"], entry["size_bytes"]))
        print("  crc32 computed over file[%d:]: 0x%08X" % (CRC_START, header.crc32_computed()))
    return 1 if failed else 0


def cmd_check(args):
    bad = 0
    for path in args.files:
        try:
            header = parse(path, args.kind)
        except (FormatError, OSError) as exc:
            print("%s: FAIL" % Path(path).name)
            print("  error: %s" % exc)
            bad += 1
            continue
        errors, notes = validate(header)
        if args.strict:
            errors, notes = errors + notes, []
        status = "FAIL" if errors else "OK"
        print("%s: %s (%s)" % (header.path.name, status,
                               header.struct_name or "common header only"))
        for message in errors:
            print("  error: %s" % message)
        for message in notes:
            print("  note:  %s" % message)
        if errors:
            bad += 1
    print("%d file(s), %d with errors" % (len(args.files), bad))
    return 1 if bad else 0


def firmware_files(directory):
    return sorted(p for p in Path(directory).iterdir() if p.is_file() and p.suffix == ".bin")


def cmd_versions(args):
    files = firmware_files(args.dir)
    if not files:
        print("no .bin files in %s" % args.dir, file=sys.stderr)
        return 1
    rows, bad = [], 0
    for path in files:
        try:
            header = parse(path)
        except (FormatError, OSError) as exc:
            rows.append((path.name, "-", "-", "-", "-", "-", str(exc)))
            bad += 1
            continue
        feature = header.feature_version
        rows.append((
            path.name,
            header.struct_name or "?",
            "%d.%d" % header.version,
            "0x%08X" % header.values["ucode_version"],
            "-" if feature is None else str(feature),
            "%d/%d" % (header.values["ucode_size_bytes"], header.file_size),
            "",
        ))
    heads = ("file", "struct", "hdr", "ucode_version", "feature", "ucode/file", "")
    widths = [max(len(str(row[i])) for row in rows + [heads]) for i in range(len(heads))]
    print("  ".join("%-*s" % (widths[i], heads[i]) for i in range(len(heads))).rstrip())
    for row in rows:
        print("  ".join("%-*s" % (widths[i], row[i]) for i in range(len(row))).rstrip())
    return 1 if bad else 0


_INFO_RE = re.compile(
    r"^(?P<label>.+?) feature version:\s*(?P<feature>0x[0-9a-fA-F]+|\d+)\s*,"
    r"(?:\s*program:\s*\d+\s*,)?\s*firmware version:\s*(?P<firmware>0x[0-9a-fA-F]+|\d+)")


def parse_firmware_info(path):
    """{label: (feature version, firmware version)} from a Linux amdgpu_firmware_info dump."""
    table = {}
    for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        match = _INFO_RE.match(line.strip())
        if match:
            table[match.group("label")] = (int(match.group("feature"), 0),
                                           int(match.group("firmware"), 0))
    return table


def cmd_compare(args):
    info = parse_firmware_info(args.info)
    if not info:
        print("no 'feature version' lines in %s" % args.info, file=sys.stderr)
        return 1
    rows, bad = [], 0
    for path in firmware_files(args.dir):
        _, label = kind_from_name(path)
        try:
            header = parse(path)
        except (FormatError, OSError) as exc:
            rows.append((path.name, label or "?", "-", "-", "PARSE: %s" % exc))
            bad += 1
            continue
        if label is None or label not in info:
            rows.append((path.name, label or "?", "0x%08X" % header.values["ucode_version"],
                         str(header.feature_version), "not in the dump"))
            continue
        want_feature, want_firmware = info[label]
        got_firmware = header.values["ucode_version"]
        got_feature = header.feature_version or 0
        verdict = "match"
        if got_firmware != want_firmware:
            verdict = "DIFFER: dump has firmware version 0x%08X" % want_firmware
        elif got_feature != want_feature:
            verdict = "DIFFER: dump has feature version %d" % want_feature
        if verdict != "match":
            bad += 1
        rows.append((path.name, label, "0x%08X" % got_firmware, str(got_feature), verdict))
    heads = ("file", "label", "ucode_version", "feature", "vs the dump")
    widths = [max(len(str(row[i])) for row in rows + [heads]) for i in range(len(heads))]
    print("  ".join("%-*s" % (widths[i], heads[i]) for i in range(len(heads))).rstrip())
    for row in rows:
        print("  ".join("%-*s" % (widths[i], row[i]) for i in range(len(row))).rstrip())
    print("%d file(s), %d disagreeing with %s" % (len(rows), bad, Path(args.info).name))
    return 1 if bad else 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    kinds = sorted(KIND_STRUCTS)
    dump = sub.add_parser("dump", help="every header field of the given files")
    dump.add_argument("files", nargs="+")
    dump.add_argument("--json", action="store_true")
    dump.add_argument("--kind", choices=kinds, help="override the kind taken from the file name")
    dump.set_defaults(func=cmd_dump)

    check = sub.add_parser("check", help="validate the headers against the file")
    check.add_argument("files", nargs="+")
    check.add_argument("--kind", choices=kinds)
    check.add_argument("--strict", action="store_true", help="notes count as errors")
    check.set_defaults(func=cmd_check)

    versions = sub.add_parser("versions", help="one line per firmware file in a directory")
    versions.add_argument("dir")
    versions.set_defaults(func=cmd_versions)

    compare = sub.add_parser("compare", help="versions against a Linux amdgpu_firmware_info dump")
    compare.add_argument("dir")
    compare.add_argument("info")
    compare.set_defaults(func=cmd_compare)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
