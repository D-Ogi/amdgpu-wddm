#!/usr/bin/env python3
"""The version resource of every PE file that a release ships (BD-166).

A game reads the user-mode driver version through DXGI CheckInterfaceSupport, and the kernel takes that number from
the file version of the first UserModeDriverName entry that is not "<>" (dxgkrnl DxgkpGetFileVersion: the file is
mapped as an image, LdrResFindResource(RT_VERSION, 1, language 0), then dwFileVersionMS/LS of VS_FIXEDFILEINFO). Our
DLLs had no version resource, so games read 0.0.0.0. The release build stamps every PE file of the package with one
resource, generated here from the package version, so the number is the release's and not a number that a component
build happened to carry:

  FILEVERSION, PRODUCTVERSION and the FileVersion string   (a + 40).b.c.d of the package version a.b.c.d (the
                                                           AMD-scheme number of BD-152: 0.7.216.102 -> 40.7.216.102)
  ProductVersion string                                    the package version text (0.7.216.102-tester.31)
  CompanyName, ProductName                                 the Provider string of the driver INF
  FileDescription                                          versioninfo.json, by file name
  InternalName, OriginalFilename                           the file name
  FILEFLAGS, PrivateBuild string                           release flavour: 0 and none; lab flavour:
                                                           VS_FF_PRIVATEBUILD and "lab" (--flavour, default release)

A release ships in two flavours (release and lab) with the same numbers; the lab flavour marks every file as a private
build, so the control app, the overlay and a bug report tell a lab file from a release file. The gate checks one
flavour at a time, so a file of the other flavour in a package fails it.

The stamp adds a resource section after the last section of an image that has none. An image whose resource section
is the last section (the .NET programs) gets that section written again: every resource except the old version
resource is kept. Nothing else of the image changes: the code, the data, the debug directory and the PDB identity stay
as they are, and the header changes only in the section count, SizeOfImage, SizeOfInitializedData, the resource
directory entry, the new section header and, when the image has one, the PE checksum. The result depends only on the
input file and the arguments, so a second stamp of the same source gives the same bytes (provenance.py relies on this).
Every stamp is checked before it is written: the old sections are byte for byte in place and the resource reads back.

  versionstamp.py amd-version <package version>
  versionstamp.py stamp --version <package version> --inf <bc250kmd.inf> [--flavour lab] <file>...
  versionstamp.py release --package <files dir> [--setup <setup.exe>] --version <v> --sources <release-sources.json> --inf <inf> [--flavour lab]
  versionstamp.py check --package-top <unpacked release> --version <v> --sources <release-sources.json> --inf <inf> [--signed] [--flavour lab] [--workspace <root>]
  versionstamp.py rc-header --version <v> --inf <inf> --name bc250kmd.sys --out <version-stamp.h> [--flavour lab | --lab]
  versionstamp.py show <file>...

`release` stamps the payload of a package in place (and the installer, with --setup) and writes the new engine and
ICD hashes into each M14C record (amdgpu_wddm_d3d11.config), after it has checked that the record pinned the unstamped
pair. `check` is the release gate: every PE file under the unpacked release carries the version resource of this
package, read back through the Windows image loader, unless release-sources.json marks it third_party or
versioninfo.json exempts it with a reason. The gate also checks the order of the release steps, stamp, then Inf2Cat,
then signtool: every embedded signature still covers the shipped bytes, every file that a catalog lists has the hash
the catalog gives it, and each M14C record pins the engine and the ICD it belongs to. With --signed (a release that
signs every PE file) an unsigned PE file, or no catalog entry at all, also fails, so the gate cannot run before those
steps.

The gate runs on a package in either state. Before DriverStore staging each M14C record sits beside the lane files it
pins by full-file SHA-256, the state `release` leaves behind. After staging the records are version 3, their pins are
Windows catalog image identities and the DriverStore has renamed the files, so a staged member is checked under the
name of the lane file it came from (amdgpu_wddm_radv_d3d11.dll answers for amdgpu_wddm_radv.dll) and the records
themselves are checked by tools/release/check_package_v3.py, which owns that mapping. With --workspace, pointing at
the root that holds the lane inputs release-sources.json pins, each staged record is also rebuilt from them, which
checks the measured capability fields as well as the pins.

A file that versioninfo.json lists under "built" (the KMD) is not stamped: its build compiles its own .rc with the
header that `rc-header` writes from the same numbers and strings, and the gate checks it like a stamped file. The
KMD's Authenticode digest is pinned in release-sources.json, so its resource has to be in the build, not added after.
"built" is therefore a table of what writes each resource and not an exemption list; the only exemption list is the
third-party one. Once a release names its stamping there, a shipped PE that the table does not claim is a package
error, because nothing stamps it: that is how a slot added without a build step behind it is caught.
"""
import argparse
import array
import hashlib
import json
import os
import re
import struct
import sys
from pathlib import Path, PurePosixPath

AMD_SCHEME_OFFSET = 40          # BD-152: product package a.b.c.d is (a + 40).b.c.d for Windows
FIELD_MAX = 65534               # BD-152: each field 0..65534, no wrap
RT_VERSION = 16
LANG_EN_US = 0x0409
CODEPAGE_UNICODE = 1200
VS_FFI_FILEFLAGSMASK = 0x3F
VS_FF_PRIVATEBUILD = 0x08
FLAVOURS = ("release", "lab")   # one release, two flavours: the same numbers, the lab one marked as a private build
SECTION_NAME = b".rsrc"
SECTION_FLAGS = 0x40000040      # IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ
TABLE = Path(__file__).with_name("versioninfo.json")
M14C_NAME = "amdgpu_wddm_d3d11.config"
M14C_PINS = (("amdgpu_wddm_dxvk.dll", 68), ("amdgpu_wddm_radv.dll", 100))


class StampError(Exception):
    pass


# ------------------------------------------------------------------ the number

def package_numbers(version):
    """The four numbers at the start of a package version ("0.7.216.102-tester.31" -> (0, 7, 216, 102))."""
    m = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)\.(\d+)(?:-[0-9A-Za-z][0-9A-Za-z.]*)?", version or "")
    if not m:
        raise StampError(f"package version {version!r} is not a.b.c.d or a.b.c.d-<label>")
    return tuple(int(g) for g in m.groups())


def amd_version(version):
    """The AMD-scheme number of a package version: (a + 40).b.c.d, every field at most 65534 (BD-152)."""
    a, b, c, d = package_numbers(version)
    fields = (a + AMD_SCHEME_OFFSET, b, c, d)
    if any(f > FIELD_MAX for f in fields):
        raise StampError(f"{version}: the AMD-scheme number {'.'.join(map(str, fields))} has a field above {FIELD_MAX}")
    return fields


def dotted(fields):
    return ".".join(str(f) for f in fields)


# ------------------------------------------------------------------ VS_VERSIONINFO

def _block(key, value=b"", value_length=0, text=True, children=()):
    out = bytearray(6) + (key + "\0").encode("utf-16-le")
    if value:
        out += b"\0" * (-len(out) % 4)
        out += value
    for child in children:
        out += b"\0" * (-len(out) % 4)
        out += child
    struct.pack_into("<HHH", out, 0, len(out), value_length, 1 if text else 0)
    return bytes(out)


def version_resource(file_version, product_version, strings, file_type, file_subtype=0, file_flags=0):
    """VS_VERSIONINFO with one string table, 0409 / Unicode, in the layout of rc.exe."""
    fv, pv = file_version, product_version
    fixed = struct.pack("<13I", 0xFEEF04BD, 0x00010000,
                        fv[0] << 16 | fv[1], fv[2] << 16 | fv[3], pv[0] << 16 | pv[1], pv[2] << 16 | pv[3],
                        VS_FFI_FILEFLAGSMASK, file_flags, 0x00040004, file_type, file_subtype, 0, 0)
    table = []
    for key, text in strings:
        if not text or any(ord(ch) < 0x20 for ch in text):
            raise StampError(f"version string {key} is empty or holds a control character")
        table.append(_block(key, (text + "\0").encode("utf-16-le"), len(text) + 1))
    string_info = _block("StringFileInfo", children=[_block("%04X%04X" % (LANG_EN_US, CODEPAGE_UNICODE), children=table)])
    var_info = _block("VarFileInfo", children=[
        _block("Translation", struct.pack("<HH", LANG_EN_US, CODEPAGE_UNICODE), 4, text=False)])
    return _block("VS_VERSION_INFO", fixed, len(fixed), text=False, children=[string_info, var_info])


def parse_version_resource(data):
    """(file version, product version, file type, {string: value}) of a VS_VERSIONINFO, or a StampError."""
    def walk(at, end):
        length, value_length, kind = struct.unpack_from("<HHH", data, at)
        if length < 6 or at + length > end:
            raise StampError("version resource: a block runs past its parent")
        k = at + 6
        key_end = k
        while data[key_end:key_end + 2] != b"\0\0":
            key_end += 2
        key = data[k:key_end].decode("utf-16-le")
        v = key_end + 2
        v += -v % 4
        size = value_length * 2 if kind == 1 else value_length
        value = data[v:v + size]
        c = v + size
        children = []
        while True:
            c += -c % 4
            if c >= at + length:
                break
            child = walk(c, at + length)
            children.append(child)
            c += child[3]
        return key, value, children, length

    key, value, children, _ = walk(0, len(data))
    if key != "VS_VERSION_INFO" or len(value) != 52:
        raise StampError("version resource: no VS_VERSION_INFO with a VS_FIXEDFILEINFO")
    f = struct.unpack("<13I", value)
    if f[0] != 0xFEEF04BD:
        raise StampError("version resource: VS_FIXEDFILEINFO has the wrong signature")
    file_version = (f[2] >> 16, f[2] & 0xFFFF, f[3] >> 16, f[3] & 0xFFFF)
    product_version = (f[4] >> 16, f[4] & 0xFFFF, f[5] >> 16, f[5] & 0xFFFF)
    strings = {}
    for ckey, _, tables, _ in children:
        if ckey == "StringFileInfo":
            for _, _, entries, _ in tables:
                for skey, svalue, _, _ in entries:
                    strings.setdefault(skey, svalue.decode("utf-16-le").rstrip("\0"))
    return file_version, product_version, f[9], strings


def file_flags(data):
    """dwFileFlags & dwFileFlagsMask of a VS_VERSIONINFO: its VS_FIXEDFILEINFO is the value at offset 40, after the
    header and the key "VS_VERSION_INFO" (parse_version_resource checks that layout)."""
    parse_version_resource(data)
    f = struct.unpack_from("<13I", data, 40)
    return f[7] & f[6]


# ------------------------------------------------------------------ PE image

class Image:
    def __init__(self, data):
        self.data = bytearray(data)
        d = self.data
        if d[:2] != b"MZ":
            raise StampError("not a PE image (no MZ)")
        self.pe = struct.unpack_from("<I", d, 0x3C)[0]
        if d[self.pe:self.pe + 4] != b"PE\0\0":
            raise StampError("not a PE image (no PE signature)")
        self.machine, self.nsections = struct.unpack_from("<HH", d, self.pe + 4)
        self.symbols = struct.unpack_from("<I", d, self.pe + 12)[0]
        self.opt_size = struct.unpack_from("<H", d, self.pe + 20)[0]
        self.characteristics = struct.unpack_from("<H", d, self.pe + 22)[0]
        self.opt = self.pe + 24
        magic = struct.unpack_from("<H", d, self.opt)[0]
        if magic not in (0x10B, 0x20B):
            raise StampError(f"unknown optional header magic {magic:#x}")
        self.plus = magic == 0x20B
        self.section_alignment, self.file_alignment = struct.unpack_from("<II", d, self.opt + 32)
        self.size_of_image, self.size_of_headers, self.checksum = struct.unpack_from("<III", d, self.opt + 56)
        rva_count_at = self.opt + (108 if self.plus else 92)
        self.ndirs = struct.unpack_from("<I", d, rva_count_at)[0]
        self.dirs_at = rva_count_at + 4
        self.table_at = self.opt + self.opt_size
        self.sections = []
        for i in range(self.nsections):
            at = self.table_at + 40 * i
            name = bytes(d[at:at + 8]).rstrip(b"\0")
            vsize, va, rsize, rptr = struct.unpack_from("<IIII", d, at + 8)
            self.sections.append({"name": name, "vsize": vsize, "va": va, "rsize": rsize, "rptr": rptr,
                                  "flags": struct.unpack_from("<I", d, at + 36)[0], "header": at})

    def directory(self, index):
        if index >= self.ndirs:
            return 0, 0
        return struct.unpack_from("<II", self.data, self.dirs_at + 8 * index)

    def offset(self, rva, size=0):
        for s in self.sections:
            if s["va"] <= rva and rva + size <= s["va"] + max(s["vsize"], s["rsize"]):
                if rva - s["va"] + size > s["rsize"]:
                    raise StampError(f"RVA {rva:#x} lies in the uninitialized tail of {s['name']!r}")
                return rva - s["va"] + s["rptr"]
        raise StampError(f"RVA {rva:#x} is outside every section")

    def end_of_sections(self):
        return max([s["rptr"] + s["rsize"] for s in self.sections if s["rsize"]] + [self.size_of_headers])


def align(value, alignment):
    return (value + alignment - 1) // alignment * alignment


def pe_checksum(data, checksum_at):
    """The PE CheckSum (imagehlp CheckSumMappedFile): the 16-bit one's complement sum of the file, then its length."""
    padded = bytes(data) + (b"\0" if len(data) % 2 else b"")
    words = array.array("H")
    words.frombytes(padded)
    if sys.byteorder != "little":
        words.byteswap()
    total = sum(words) - words[checksum_at // 2] - words[checksum_at // 2 + 1]
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return (total + len(data)) & 0xFFFFFFFF


# ------------------------------------------------------------------ resource tree

def read_resources(image):
    """[(type, name, language, codepage, data)] of the image; type and name are an int or a str."""
    rva, size = image.directory(2)
    if not rva:
        return []
    base = image.offset(rva, 16)
    d = image.data
    leaves = []

    def label(value):
        if value & 0x80000000:
            at = base + (value & 0x7FFFFFFF)
            n = struct.unpack_from("<H", d, at)[0]
            return bytes(d[at + 2:at + 2 + 2 * n]).decode("utf-16-le")
        return value & 0xFFFF

    def entries(at):
        named, ids = struct.unpack_from("<HH", d, at + 12)
        return [struct.unpack_from("<II", d, at + 16 + 8 * i) for i in range(named + ids)]

    for t, tchild in entries(base):
        if not tchild & 0x80000000:
            raise StampError("resource tree: a type entry is not a directory")
        for n, nchild in entries(base + (tchild & 0x7FFFFFFF)):
            if not nchild & 0x80000000:
                raise StampError("resource tree: a name entry is not a directory")
            for lang, lchild in entries(base + (nchild & 0x7FFFFFFF)):
                if lchild & 0x80000000:
                    raise StampError("resource tree: deeper than type, name and language")
                data_rva, data_size, codepage, _ = struct.unpack_from("<IIII", d, base + lchild)
                at = image.offset(data_rva, data_size)
                leaves.append((label(t), label(n), lang & 0xFFFF, codepage, bytes(d[at:at + data_size])))
    return leaves


def build_resources(leaves, section_rva):
    """The bytes of a resource section at section_rva that holds these leaves, in the layout of the linker."""
    def order(key):
        return (0, key, 0) if isinstance(key, str) else (1, "", key)

    tree = {}
    for t, n, lang, cp, data in leaves:
        slot = tree.setdefault(t, {}).setdefault(n, {})
        if lang in slot:
            raise StampError(f"resource {t}/{n}/{lang:#x} appears twice")
        slot[lang] = (cp, data)
    types = sorted(tree, key=order)
    dirs = [("root", types)]
    for t in types:
        names = sorted(tree[t], key=order)
        dirs.append(((t,), names))
        for n in names:
            dirs.append(((t, n), sorted(tree[t][n])))
    dir_at, at = {}, 0
    for key, children in dirs:
        dir_at[key] = at
        at += 16 + 8 * len(children)
    leaf_at = {}
    for t in types:
        for n in sorted(tree[t], key=order):
            for lang in sorted(tree[t][n]):
                leaf_at[(t, n, lang)] = at
                at += 16
    string_at = {}
    for t in types:
        for s in [t] + [n for n in tree[t]]:
            if isinstance(s, str) and s not in string_at:
                string_at[s] = at
                at += 2 + 2 * len(s)
    blob_at = {}
    for t in types:
        for n in sorted(tree[t], key=order):
            for lang in sorted(tree[t][n]):
                at = align(at, 8)
                blob_at[(t, n, lang)] = at
                at += len(tree[t][n][lang][1])
    out = bytearray(at)

    def entry_name(key):
        return 0x80000000 | string_at[key] if isinstance(key, str) else key

    def write_dir(offset, children, child_value):
        named = sum(1 for c in children if isinstance(c, str))
        struct.pack_into("<IIHHHH", out, offset, 0, 0, 0, 0, named, len(children) - named)
        for i, c in enumerate(children):
            struct.pack_into("<II", out, offset + 16 + 8 * i, entry_name(c), child_value(c))

    write_dir(dir_at["root"], types, lambda t: 0x80000000 | dir_at[(t,)])
    for t in types:
        names = sorted(tree[t], key=order)
        write_dir(dir_at[(t,)], names, lambda n, t=t: 0x80000000 | dir_at[(t, n)])
        for n in names:
            write_dir(dir_at[(t, n)], sorted(tree[t][n]), lambda lang, t=t, n=n: leaf_at[(t, n, lang)])
            for lang in sorted(tree[t][n]):
                cp, data = tree[t][n][lang]
                struct.pack_into("<IIII", out, leaf_at[(t, n, lang)], section_rva + blob_at[(t, n, lang)], len(data), cp, 0)
                out[blob_at[(t, n, lang)]:blob_at[(t, n, lang)] + len(data)] = data
    for s, offset in string_at.items():
        struct.pack_into("<H", out, offset, len(s))
        out[offset + 2:offset + 2 + 2 * len(s)] = s.encode("utf-16-le")
    return bytes(out)


# ------------------------------------------------------------------ the stamp

def load_table(path=TABLE):
    table = json.loads(Path(path).read_text(encoding="utf-8"))
    if table.get("schema") != 1 or not isinstance(table.get("files"), dict) or not isinstance(table.get("exempt"), dict) \
            or not isinstance(table.setdefault("built", {}), dict):
        raise StampError(f"{path}: expected schema 1 with 'files', 'exempt' and 'built'")
    for name in table["built"]:
        if name not in table["files"]:
            raise StampError(f"{path}: {name} is built with its resource but has no FileDescription in 'files'")
    # The release's own table says what writes every resource it describes: without that, leaving a file out
    # of 'built' would turn the refusal of an unclaimed shipped PE off instead of failing the release.
    for name in table["files"]:
        if name not in table["built"] and name not in table["exempt"]:
            raise StampError(f"{path}: nothing in 'built' writes {name}'s version resource; name what does, "
                             f"or give it a reason in 'exempt'")
    return table


def declares_stamping(table):
    """Does this table say what writes the version resource of every file it describes?

    versioninfo.json does, and load_table refuses to read it otherwise, so a release always does and the
    refusal in check_release is always on there. A host test that hands in a table of its own may name one
    built file and no more; the gate then checks the resources it can and accuses nobody of having no build
    step, because such a table does not claim to know."""
    claimed = set(table.get("built") or ()) | set(table.get("exempt") or ())
    return bool(table.get("built")) and set(table.get("files") or ()) <= claimed


def inf_provider(inf):
    """The Provider string of the driver INF ([Strings] Provider = "...")."""
    text = Path(inf).read_text(encoding="utf-8-sig", errors="strict")
    section = None
    for line in text.splitlines():
        line = line.split(";", 1)[0].strip()
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip().lower()
        elif section == "strings":
            m = re.fullmatch(r"Provider\s*=\s*\"([^\"]+)\"", line)
            if m:
                return m.group(1)
    raise StampError(f"{inf}: no Provider in [Strings]")


def check_flavour(flavour):
    if flavour not in FLAVOURS:
        raise StampError(f"flavour {flavour!r}: one of {', '.join(FLAVOURS)}")
    return flavour


def expected_resource(file_name, package_version, company, table, flavour="release"):
    """The version resource this release gives the file of that name, in that flavour of the release."""
    description = table["files"].get(file_name)
    if not description:
        raise StampError(f"{file_name}: no FileDescription in {TABLE.name}: add the file there, or exempt it with a reason")
    fields, strings, file_type, file_subtype, file_flags = resource_fields(file_name, package_version, company,
                                                                          description, flavour)
    return version_resource(fields, fields, strings, file_type, file_subtype, file_flags)


def resource_fields(file_name, package_version, company, description, flavour="release"):
    """(version fields, strings, file type, file subtype, file flags): one source for the stamp and for rc_header.
    Both flavours of a release carry the same numbers; the lab flavour marks itself as a private build
    (VS_FF_PRIVATEBUILD and PrivateBuild "lab"), so a lab file is told from a release file without a log."""
    fields = amd_version(package_version)
    lower = file_name.lower()
    if lower.endswith(".sys"):
        file_type, file_subtype = 3, 7                            # VFT_DRV, VFT2_DRV_SYSTEM, as bc250kmd.rc
    else:
        file_type, file_subtype = (1 if lower.endswith(".exe") else 2), 0   # VFT_APP, VFT_DLL
    strings = [("CompanyName", company), ("FileDescription", description), ("FileVersion", dotted(fields)),
               ("InternalName", os.path.splitext(file_name)[0]), ("OriginalFilename", file_name),
               ("ProductName", company), ("ProductVersion", package_version)]
    file_flags = 0
    if check_flavour(flavour) == "lab":
        strings.append(("PrivateBuild", "lab"))
        file_flags = VS_FF_PRIVATEBUILD
    return fields, strings, file_type, file_subtype, file_flags


def rc_header(file_name, package_version, company, table, flavour="release"):
    """version-stamp.h, which driver/kmd/bc250kmd.rc includes: the same numbers and strings that a stamp would write,
    so the release gate compares the built resource with the same expectation. The .rc writes InternalName and
    OriginalFilename itself, and PRODUCTVERSION is FILEVERSION. The lab flavour defines BC250_VER_LAB, which makes the
    .rc set VS_FF_PRIVATEBUILD and PrivateBuild "lab", as resource_fields does."""
    description = table["files"].get(file_name)
    if not description:
        raise StampError(f"{file_name}: no FileDescription in {TABLE.name}")
    fields, strings, _, _, _ = resource_fields(file_name, package_version, company, description, flavour)
    for key, text in strings:
        if not text or any(ord(ch) < 0x20 or ch in '"\\' for ch in text):
            raise StampError(f"version string {key} {text!r} is empty or holds a character that an .rc string cannot hold")
    names = {"CompanyName": "COMPANY", "FileDescription": "DESCRIPTION", "FileVersion": "FILEVERSION_STR",
             "ProductVersion": "PRODUCTVERSION_STR"}
    lines = [f"/* Written by tools/build/versionstamp.py rc-header for {file_name} of release {package_version}. */",
             "#pragma once",
             "#define BC250_VER_FILEVERSION " + ",".join(str(f) for f in fields)]
    lines += [f'#define BC250_VER_{names[key]} "{text}"' for key, text in strings if key in names]
    if flavour == "lab":
        lines.append("#define BC250_VER_LAB 1")
    return "\n".join(lines) + "\n"


def stamp_bytes(source, resource):
    """The image with this version resource in place of any it had (see the module text for what changes)."""
    image = Image(source)
    d = image.data
    if image.directory(4) != (0, 0):
        raise StampError("the image carries an Authenticode signature: a stamp would break it (stamp before signing)")
    if image.symbols:
        raise StampError("the image carries a COFF symbol table after its sections")
    end = image.end_of_sections()
    if len(d) != end:
        raise StampError(f"the image has {len(d) - end} bytes after its last section (an overlay): stamp it before they are added")
    if len(d) % image.file_alignment:
        raise StampError("the image size is not a multiple of FileAlignment")
    last = max(image.sections, key=lambda s: s["va"])
    if image.size_of_image != align(last["va"] + max(last["vsize"], 1), image.section_alignment):
        raise StampError("SizeOfImage is not the aligned end of the last section")
    if image.ndirs < 3:
        raise StampError("the image has no resource directory slot")
    res_rva, res_size = image.directory(2)
    leaves = [leaf for leaf in read_resources(image) if leaf[0] != RT_VERSION]
    leaves.append((RT_VERSION, 1, LANG_EN_US, 0, resource))
    if res_rva:
        # The resource section is written again in place: it must be the last section and hold nothing but resources.
        rsrc = [s for s in image.sections if s["va"] <= res_rva < s["va"] + max(s["vsize"], s["rsize"])]
        if not rsrc or rsrc[0] is not last or rsrc[0]["va"] != res_rva or last["rptr"] + last["rsize"] != len(d):
            raise StampError("the resource directory is not at the start of the last section")
        for i in range(image.ndirs):
            if i == 2:
                continue
            rva, size = image.directory(i)
            if rva and last["va"] <= rva < last["va"] + max(last["vsize"], last["rsize"]):
                raise StampError(f"data directory {i} points into the resource section")
        target = last
        header_at = last["header"]
        raw_ptr = last["rptr"]
        added = 0
    else:
        header_at = image.table_at + 40 * image.nsections
        first_raw = min([s["rptr"] for s in image.sections if s["rsize"]] + [image.size_of_headers])
        if header_at + 40 > min(image.size_of_headers, first_raw):
            raise StampError("no room for one more section header")
        if any(d[header_at:header_at + 40]):
            raise StampError("the slot after the section table is not empty")
        target = None
        raw_ptr = len(d)
        added = 1
    va = target["va"] if target else image.size_of_image
    body = build_resources(leaves, va)
    raw = align(len(body), image.file_alignment)
    out = bytearray(d[:raw_ptr]) + body + b"\0" * (raw - len(body))
    struct.pack_into("<8sIIIIIIHHI", out, header_at, SECTION_NAME, len(body), va, raw, raw_ptr, 0, 0, 0, 0, SECTION_FLAGS)
    struct.pack_into("<H", out, image.pe + 6, image.nsections + added)
    old_raw = target["rsize"] if target else 0
    init_at = image.opt + 8
    struct.pack_into("<I", out, init_at, (struct.unpack_from("<I", d, init_at)[0] + raw - old_raw) & 0xFFFFFFFF)
    struct.pack_into("<I", out, image.opt + 56, align(va + len(body), image.section_alignment))
    struct.pack_into("<II", out, image.dirs_at + 16, va, len(body))
    if image.checksum:
        struct.pack_into("<I", out, image.opt + 64, pe_checksum(out, image.opt + 64))
    verify_stamp(source, bytes(out), resource, leaves)
    return bytes(out)


def verify_stamp(source, stamped, resource, leaves):
    """The stamped image keeps every old section byte for byte and reads back the new resource tree."""
    old, new = Image(source), Image(stamped)
    res_rva = old.directory(2)[0]
    kept = [s for s in old.sections if not (res_rva and s["va"] == res_rva)]
    for s in kept:
        match = [n for n in new.sections if n["va"] == s["va"]]
        if not match or {k: match[0][k] for k in ("name", "vsize", "rsize", "rptr", "flags")} != \
                {k: s[k] for k in ("name", "vsize", "rsize", "rptr", "flags")}:
            raise StampError(f"section {s['name']!r} moved or changed its header")
        if stamped[s["rptr"]:s["rptr"] + s["rsize"]] != source[s["rptr"]:s["rptr"] + s["rsize"]]:
            raise StampError(f"section {s['name']!r} changed")
    for i in range(old.ndirs):
        if i != 2 and old.directory(i) != new.directory(i):
            raise StampError(f"data directory {i} changed")
    for at, size in ((old.opt + 16, 4), (old.opt + 24 if old.plus else old.opt + 28, 8 if old.plus else 4)):
        if source[at:at + size] != stamped[at:at + size]:
            raise StampError("the entry point or the image base changed")
    if sorted(read_resources(new), key=repr) != sorted(leaves, key=repr):
        raise StampError("the resource tree does not read back")
    found = [leaf for leaf in read_resources(new) if leaf[0] == RT_VERSION]
    if len(found) != 1 or found[0][4] != resource:
        raise StampError("the version resource does not read back")


def stamp_file(path, package_version, company, table, file_name=None, flavour="release"):
    path = Path(path)
    name = file_name or path.name
    source = path.read_bytes()
    stamped = stamp_bytes(source, expected_resource(name, package_version, company, table, flavour))
    path.write_bytes(stamped)
    return hashlib.sha256(source).hexdigest().upper(), hashlib.sha256(stamped).hexdigest().upper()


def is_pe(path):
    with open(path, "rb") as f:
        head = f.read(0x40)
        if len(head) < 0x40 or head[:2] != b"MZ":
            return False
        f.seek(struct.unpack_from("<I", head, 0x3C)[0])
        return f.read(4) == b"PE\0\0"


def repoint_m14c(record, engine_sha, icd_sha):
    """The M14C record with its engine and ICD pins set to these SHA-256 values; the capabilities are kept."""
    if len(record) != 132 or struct.unpack_from("<I", record, 0)[0] != 0x4334314D:
        raise StampError("not a 132-byte M14C record")
    out = bytearray(record)
    out[68:100] = bytes.fromhex(engine_sha)
    out[100:132] = bytes.fromhex(icd_sha)
    return bytes(out)


def m14c_pins(record):
    return record[68:100].hex().upper(), record[100:132].hex().upper()


# ------------------------------------------------------------------ the Windows read-back

def windows_read(path):
    """(file version, product version, strings) as a process reads them: the file mapped by the image loader
    (LOAD_LIBRARY_AS_IMAGE_RESOURCE, the mapping of the kernel's DxgkpGetFileVersion) and FindResource/LoadResource,
    then GetFileVersionInfo and VerQueryValue as a game's version query does. None off Windows."""
    if os.name != "nt":
        return None
    import ctypes
    from ctypes import wintypes
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    ver = ctypes.WinDLL("version", use_last_error=True)
    k32.LoadLibraryExW.restype = wintypes.HMODULE
    k32.LoadLibraryExW.argtypes = [wintypes.LPCWSTR, wintypes.HANDLE, wintypes.DWORD]
    k32.FindResourceW.restype = wintypes.HANDLE
    k32.FindResourceW.argtypes = [wintypes.HMODULE, ctypes.c_void_p, ctypes.c_void_p]
    k32.LoadResource.restype = wintypes.HANDLE
    k32.LoadResource.argtypes = [wintypes.HMODULE, wintypes.HANDLE]
    k32.SizeofResource.restype = wintypes.DWORD
    k32.SizeofResource.argtypes = [wintypes.HMODULE, wintypes.HANDLE]
    k32.LockResource.restype = ctypes.c_void_p
    k32.LockResource.argtypes = [wintypes.HANDLE]
    k32.FreeLibrary.argtypes = [wintypes.HMODULE]
    image = k32.LoadLibraryExW(str(path), None, 0x20 | 0x02)   # AS_IMAGE_RESOURCE | AS_DATAFILE
    if not image:
        raise StampError(f"the image loader refuses the file (error {ctypes.get_last_error()})")
    try:
        found = k32.FindResourceW(image, 1, RT_VERSION)
        if not found:
            raise StampError("the image loader finds no version resource")
        size = k32.SizeofResource(image, found)
        block = ctypes.string_at(k32.LockResource(k32.LoadResource(image, found)), size)
        loader_view = parse_version_resource(block)
    finally:
        k32.FreeLibrary(image)
    ver.GetFileVersionInfoSizeW.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(wintypes.DWORD)]
    ver.GetFileVersionInfoW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p]
    ver.VerQueryValueW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR, ctypes.POINTER(ctypes.c_void_p),
                                   ctypes.POINTER(wintypes.UINT)]
    n = ver.GetFileVersionInfoSizeW(str(path), None)
    if not n:
        raise StampError(f"GetFileVersionInfoSize finds no version resource (error {ctypes.get_last_error()})")
    buf = ctypes.create_string_buffer(n)
    if not ver.GetFileVersionInfoW(str(path), 0, n, buf):
        raise StampError(f"GetFileVersionInfo failed (error {ctypes.get_last_error()})")
    ptr, length = ctypes.c_void_p(), wintypes.UINT()
    if not ver.VerQueryValueW(buf, "\\", ctypes.byref(ptr), ctypes.byref(length)) or length.value < 52:
        raise StampError("VerQueryValue finds no VS_FIXEDFILEINFO")
    f = struct.unpack("<13I", ctypes.string_at(ptr, 52))
    api_file = (f[2] >> 16, f[2] & 0xFFFF, f[3] >> 16, f[3] & 0xFFFF)
    api_product = (f[4] >> 16, f[4] & 0xFFFF, f[5] >> 16, f[5] & 0xFFFF)
    strings = {}
    for key in ("CompanyName", "FileDescription", "FileVersion", "InternalName", "OriginalFilename",
                "ProductName", "ProductVersion", "PrivateBuild"):
        if ver.VerQueryValueW(buf, "\\StringFileInfo\\040904B0\\" + key, ctypes.byref(ptr), ctypes.byref(length)) and length.value:
            strings[key] = ctypes.wstring_at(ptr, length.value).rstrip("\0")
    if (api_file, api_product) != loader_view[:2]:
        raise StampError("GetFileVersionInfo and the image loader read different numbers")
    return api_file, api_product, strings


def _wintrust():
    import ctypes
    from ctypes import wintypes
    wt = ctypes.WinDLL("wintrust", use_last_error=True)
    wt.CryptCATOpen.restype = wintypes.HANDLE
    wt.CryptCATOpen.argtypes = [wintypes.LPWSTR, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD]
    wt.CryptCATClose.argtypes = [wintypes.HANDLE]
    wt.CryptCATEnumerateMember.restype = ctypes.c_void_p
    wt.CryptCATEnumerateMember.argtypes = [wintypes.HANDLE, ctypes.c_void_p]
    wt.CryptCATEnumerateAttr.restype = ctypes.c_void_p
    wt.CryptCATEnumerateAttr.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p]
    wt.CryptCATAdminAcquireContext2.argtypes = [ctypes.POINTER(wintypes.HANDLE), ctypes.c_void_p, wintypes.LPCWSTR,
                                                ctypes.c_void_p, wintypes.DWORD]
    wt.CryptCATAdminCalcHashFromFileHandle2.argtypes = [wintypes.HANDLE, wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD),
                                                        ctypes.c_void_p, wintypes.DWORD]
    wt.CryptCATAdminReleaseContext.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    wt.WinVerifyTrust.restype = ctypes.c_long
    wt.WinVerifyTrust.argtypes = [wintypes.HWND, ctypes.c_void_p, ctypes.c_void_p]
    return ctypes, wintypes, wt


def catalog_members(cat):
    """{file name in lower case: hash tag} of a driver catalog: each member's reference tag is the hexadecimal hash of
    the file (SHA-1 or SHA-256) and its "File" attribute is the file name (mscat.h CRYPTCATMEMBER, CRYPTCATATTRIBUTE)."""
    ctypes, wintypes, wt = _wintrust()

    class Member(ctypes.Structure):
        _fields_ = [("cbStruct", wintypes.DWORD), ("pwszReferenceTag", wintypes.LPWSTR)]

    class Attr(ctypes.Structure):
        _fields_ = [("cbStruct", wintypes.DWORD), ("pwszReferenceTag", wintypes.LPWSTR),
                    ("dwAttrTypeAndAction", wintypes.DWORD), ("cbValue", wintypes.DWORD), ("pbValue", ctypes.c_void_p)]

    handle = wt.CryptCATOpen(str(cat), 0x4, None, 0, 0)   # CRYPTCAT_OPEN_EXISTING
    if not handle or handle == wintypes.HANDLE(-1).value:
        raise StampError(f"{cat}: CryptCATOpen failed (error {ctypes.get_last_error()})")
    members = {}
    try:
        member = wt.CryptCATEnumerateMember(handle, None)
        while member:
            tag = Member.from_address(member).pwszReferenceTag.upper()
            attr = wt.CryptCATEnumerateAttr(handle, member, None)
            while attr:
                a = Attr.from_address(attr)
                if a.pwszReferenceTag.lower() == "file" and a.pbValue:
                    members[ctypes.wstring_at(a.pbValue, a.cbValue // 2).rstrip("\0").lower()] = tag
                attr = wt.CryptCATEnumerateAttr(handle, member, attr)
            member = wt.CryptCATEnumerateMember(handle, member)
    finally:
        wt.CryptCATClose(handle)
    return members


def catalog_hash(path, algorithm):
    """The hash that a catalog lists for this file (for a PE file its Authenticode hash), as an upper-case tag."""
    ctypes, wintypes, wt = _wintrust()
    admin = wintypes.HANDLE()
    if not wt.CryptCATAdminAcquireContext2(ctypes.byref(admin), None, algorithm, None, 0):
        raise StampError(f"CryptCATAdminAcquireContext2 {algorithm} failed (error {ctypes.get_last_error()})")
    try:
        with open(path, "rb") as f:
            import msvcrt
            handle = msvcrt.get_osfhandle(f.fileno())
            size = wintypes.DWORD(64)
            buf = ctypes.create_string_buffer(64)
            if not wt.CryptCATAdminCalcHashFromFileHandle2(admin, handle, ctypes.byref(size), buf, 0):
                raise StampError(f"{path}: CryptCATAdminCalcHashFromFileHandle2 failed (error {ctypes.get_last_error()})")
            return buf.raw[:size.value].hex().upper()
    finally:
        wt.CryptCATAdminReleaseContext(admin, 0)


def signature_digest(path):
    """'unsigned', 'bad digest', or 'covers the file': WinVerifyTrust checks the digest of an embedded signature
    before it builds the certificate chain, so TRUST_E_BAD_DIGEST says that the file changed after it was signed,
    whatever the trust of the signer on this computer. No revocation check, no network, no trust store change."""
    ctypes, wintypes, wt = _wintrust()

    class Guid(ctypes.Structure):
        _fields_ = [("d1", wintypes.DWORD), ("d2", wintypes.WORD), ("d3", wintypes.WORD), ("d4", ctypes.c_ubyte * 8)]

    class FileInfo(ctypes.Structure):
        _fields_ = [("cbStruct", wintypes.DWORD), ("pcwszFilePath", wintypes.LPCWSTR), ("hFile", wintypes.HANDLE),
                    ("pgKnownSubject", ctypes.c_void_p)]

    class Data(ctypes.Structure):
        _fields_ = [("cbStruct", wintypes.DWORD), ("pPolicyCallbackData", ctypes.c_void_p),
                    ("pSIPClientData", ctypes.c_void_p), ("dwUIChoice", wintypes.DWORD),
                    ("fdwRevocationChecks", wintypes.DWORD), ("dwUnionChoice", wintypes.DWORD),
                    ("pFile", ctypes.POINTER(FileInfo)), ("dwStateAction", wintypes.DWORD),
                    ("hWVTStateData", wintypes.HANDLE), ("pwszURLReference", wintypes.LPWSTR),
                    ("dwProvFlags", wintypes.DWORD), ("dwUIContext", wintypes.DWORD),
                    ("pSignatureSettings", ctypes.c_void_p)]

    action = Guid(0x00AAC56B, 0xCD44, 0x11D0, (ctypes.c_ubyte * 8)(0x8C, 0xC2, 0x00, 0xC0, 0x4F, 0xC2, 0x95, 0xEE))
    info = FileInfo(ctypes.sizeof(FileInfo), str(path), None, None)
    data = Data(cbStruct=ctypes.sizeof(Data), dwUIChoice=2, fdwRevocationChecks=0, dwUnionChoice=1,
                pFile=ctypes.pointer(info), dwStateAction=1, dwProvFlags=0x1000)   # WTD_UI_NONE, FILE, VERIFY, cache only
    result = wt.WinVerifyTrust(None, ctypes.byref(action), ctypes.byref(data)) & 0xFFFFFFFF
    data.dwStateAction = 2                                                       # WTD_STATEACTION_CLOSE
    wt.WinVerifyTrust(None, ctypes.byref(action), ctypes.byref(data))
    if result == 0x800B0100:                                                     # TRUST_E_NOSIGNATURE
        return "unsigned"
    if result == 0x80096010:                                                     # TRUST_E_BAD_DIGEST
        return "bad digest"
    return "covers the file"


def check_file(path, package_version, company, table, file_name=None, flavour="release"):
    """Problems of one shipped PE file against this release's version resource in that flavour (empty: it carries
    it). A file of the other flavour fails: its FILEFLAGS and its PrivateBuild string differ."""
    name = file_name or Path(path).name
    try:
        want_bytes = expected_resource(name, package_version, company, table, flavour)
        want = parse_version_resource(want_bytes)
        image = Image(Path(path).read_bytes())
        found = [leaf for leaf in read_resources(image) if leaf[0] == RT_VERSION]
        if not found:
            return ["no version resource"]
        if len(found) != 1:
            return [f"{len(found)} version resources"]
        have = parse_version_resource(found[0][4])
        problems = []
        if have[0] != want[0] or have[1] != want[1]:
            problems.append(f"FILEVERSION {dotted(have[0])} PRODUCTVERSION {dotted(have[1])}, this release is {dotted(want[0])}")
        for key, value in want[3].items():
            if have[3].get(key) != value:
                problems.append(f"{key} is {have[3].get(key)!r}, wanted {value!r}")
        for key in sorted(set(have[3]) - set(want[3])):
            problems.append(f"{key} is {have[3][key]!r}, the {flavour} flavour has no {key}")
        if file_flags(found[0][4]) != file_flags(want_bytes):
            problems.append(f"FILEFLAGS {file_flags(found[0][4]):#x}, the {flavour} flavour has {file_flags(want_bytes):#x}")
        api = windows_read(path)
        if api is not None and (api[0] != want[0] or api[1] != want[1] or api[2].get("ProductVersion") != package_version
                                or api[2].get("PrivateBuild") != want[3].get("PrivateBuild")):
            problems.append(f"Windows reads {dotted(api[0])} / {api[2].get('ProductVersion')!r} "
                            f"PrivateBuild {api[2].get('PrivateBuild')!r}")
        return problems
    except StampError as exc:
        return [str(exc)]


# ------------------------------------------------------------------ release steps

def third_party_paths(sources):
    data = json.loads(Path(sources).read_text(encoding="utf-8"))
    return {f["path"] for f in data["files"] if "third_party" in f}


def payload_pe_files(package):
    package = Path(package)
    for p in sorted((package / "payload").rglob("*")):
        if p.is_file() and is_pe(p):
            yield p.relative_to(package).as_posix(), p


def stamp_release(package, version, sources, inf, table, log=print, setup=None, flavour="release"):
    """Stamps the payload of a package folder in place, re-points its M14C records, and stamps the installer at setup
    when it is given, all in one flavour. Returns the rows."""
    package = Path(package)
    company = inf_provider(inf)
    check_flavour(flavour)
    third = third_party_paths(sources)
    before = {}
    rows = []
    for rel, p in payload_pe_files(package):
        name = p.name
        if rel in third:
            rows.append((rel, "third-party", None, None))
            continue
        if name in table["exempt"]:
            rows.append((rel, "exempt", None, None))
            continue
        if name in table.get("built", {}):
            rows.append((rel, "built", None, None))   # its own .rc; the gate checks it like a stamped file
            continue
        before[rel] = hashlib.sha256(p.read_bytes()).hexdigest().upper()
        old, new = stamp_file(p, version, company, table, flavour=flavour)
        rows.append((rel, "stamped", old, new))
    for cfg in sorted((package / "payload").rglob(M14C_NAME)):
        record = cfg.read_bytes()
        pins = m14c_pins(record)
        shas = []
        for (name, _), pin in zip(M14C_PINS, pins):
            rel = (cfg.parent / name).relative_to(package).as_posix()
            if before.get(rel) != pin:
                raise StampError(f"{cfg.relative_to(package)} pins {name} {pin[:8]}, the package had "
                                 f"{(before.get(rel) or 'no file')[:8]} before the stamp")
            shas.append(hashlib.sha256((cfg.parent / name).read_bytes()).hexdigest().upper())
        cfg.write_bytes(repoint_m14c(record, *shas))
        rows.append((cfg.relative_to(package).as_posix(), "re-pointed", pins[0], shas[0]))
    if setup is not None:
        old, new = stamp_file(setup, version, company, table, flavour=flavour)
        rows.append((Path(setup).name, "stamped", old, new))
    for rel, state, old, new in rows:
        log("  %-12s %s%s" % (state, rel, f"  {old[:8]} -> {new[:8]}" if old else ""))
    return rows


def release_side(name):
    """One module of tools/release, which owns the DriverStore mapping and the image identity of a pin."""
    directory = str(Path(__file__).resolve().parents[1] / "release")
    if directory not in sys.path:
        sys.path.insert(0, directory)
    return __import__(name)


def member_sources(sources, flavour):
    """Each staged DriverStore member by its input slot: payload/kmd/d3d11/amdgpu_wddm_radv_d3d11.dll came
    from payload/d3d11/amdgpu_wddm_radv.dll, so its version resource names the lane file and not the member.
    The mapping is followed in that direction only. It is empty when release-sources.json does not pin the
    lane inputs of the member set at all, which is a file that describes no such package: the legacy pins of
    a release before the two flavours, and the fixtures of the host tests. The package is then checked under
    the names it carries.

    A file that does pin them is a release manifest, and an invalid selection of it propagates: a flavour it
    does not describe, a malformed row or a slot missing from one flavour. A gate that answered such a file
    with "nothing to map" would check a release package under the wrong names and pass it."""
    data = sources if isinstance(sources, dict) else json.loads(Path(sources).read_text(encoding="utf-8-sig"))
    driver_package = release_side("driver_package")
    rows = data.get("files") if isinstance(data, dict) else None
    if not isinstance(rows, list) or not rows:
        return {}
    slots = {member["source"] for member in driver_package.members(data)}
    if not any(isinstance(row, dict) and row.get("path") in slots for row in rows):
        return {}
    selected = release_side("release_flavour").select_sources(data, flavour)
    return {member["path"]: member["source"] for member in driver_package.members(selected)}


def check_release(package_top, version, sources, inf, table, log=print, require_signed=False, flavour="release",
                  workspace=None):
    """The release gate: problems of every PE file under the unpacked release (setup.exe and files\\), in one
    flavour."""
    top = Path(package_top)
    package = top / "files"
    company = inf_provider(inf)
    third = third_party_paths(sources)
    staged = member_sources(sources, flavour)
    problems = []
    counts = {"carries": 0, "third-party": 0, "exempt": 0, "renamed": 0}
    files = [("setup.exe", top / "setup.exe")] if (top / "setup.exe").is_file() else []
    if not files:
        problems.append("setup.exe: not in the unpacked release")
    for p in sorted(package.rglob("*")):
        if p.is_file() and is_pe(p):
            files.append(("files/" + p.relative_to(package).as_posix(), p))
    checked = []
    for rel, p in files:
        # A staged member answers for its input slot: its exemptions, its description and the name in its
        # version resource are the lane file's, not the renamed member's.
        slot = staged.get(rel[6:]) if rel.startswith("files/") else None
        name = PurePosixPath(slot).name if slot else p.name
        if name != p.name:
            counts["renamed"] += 1
        if rel.startswith("files/") and (slot or rel[6:]) in third:
            counts["third-party"] += 1
            continue
        if name in table["exempt"]:
            counts["exempt"] += 1
            log(f"  exempt     {rel}: {table['exempt'][name]}")
            continue
        # "built" is a table of what writes each file's version resource, not an exemption list: a file
        # listed there is checked exactly like a release-stamped one. What makes the table mean anything is
        # this refusal: a shipped PE that nothing in it claims, and that is not pinned as third party, is a
        # slot added to the package with no build step behind it. A table that names no stamping at all
        # that does not claim to know what stamps everything it describes (the partial tables the host
        # tests hand in) checks resources alone.
        if declares_stamping(table) and name not in table["built"]:
            problems.append(f"{rel}: nothing stamps it; name what writes its version resource in "
                            f"{TABLE.name} under 'built', or pin it as third party")
            continue
        found = check_file(p, version, company, table, file_name=name, flavour=flavour)
        if found:
            problems.extend(f"{rel}: {x}" for x in found)
        else:
            counts["carries"] += 1
        checked.append((rel, p))
    want = dotted(amd_version(version))
    log(f"  {counts['carries']} PE files carry {want} / {version} ({flavour}), {counts['third-party']} third-party, "
        f"{counts['exempt']} exempt, {counts['renamed']} checked under their lane name")
    problems += order_problems(top, checked, log, require_signed)
    problems += record_problems(package, sources, flavour, staged, workspace, log)
    return problems


def record_problems(package, sources, flavour, staged, workspace, log=print):
    """The D3D11 configuration records of the package, in whichever state the package is in.

    A staged package (its DriverStore members are there, the lane inputs are gone) carries version 3 records
    whose pins are image identities, under names the DriverStore chose. Before staging the records are still
    the v1/v2 originals beside the lane files they pin by full-file SHA-256, which is what `release`
    re-points after stamping. tools/release/check_package_v3.py owns both checks, because it is also the
    module that owns the mapping and the image identity, and in both of them the records that must exist
    come from the source mapping and not from what the package happens to carry.

    Only a release-sources.json with no source rows at all leaves the records to be found by name: the
    legacy pins of a release before the two flavours, and the fixtures of the host tests."""
    v3 = release_side("check_package_v3")
    if staged:
        data = sources if isinstance(sources, dict) else json.loads(Path(sources).read_text(encoding="utf-8-sig"))
        if any((Path(package) / member).is_file() for member in staged):
            return v3.problems(package, data, flavour, workspace, log)
        return v3.prestage_problems(package, data, flavour, log)
    log(f"  release-sources.json carries no source rows: the records are checked under the names the "
        f"package carries")
    problems = []
    records = sorted(Path(package).rglob(M14C_NAME))
    for cfg in records:
        try:
            pins = v3.original_pins(cfg.read_bytes())
        except ValueError as exc:
            problems.append(f"files/{cfg.relative_to(package).as_posix()}: {exc}")
            continue
        for (name, _), pin in zip(M14C_PINS, pins):
            sibling = cfg.parent / name
            have = hashlib.sha256(sibling.read_bytes()).hexdigest().upper() if sibling.is_file() else "no file"
            if have != pin:
                problems.append(f"files/{cfg.relative_to(package).as_posix()}: pins {name} {pin[:8]}, "
                                f"the package has {have[:8]}")
    log(f"  {len(records)} D3D11 configuration records before staging, each pinned by full-file SHA-256")
    return problems


def order_problems(top, checked, log=print, require_signed=False):
    """The order of the release steps is stamp, catalog, signature. A file changed after Inf2Cat no longer has the hash
    that the catalog lists for its name, and a file changed after signtool no longer has the digest its signature
    covers: both are found here from the files themselves, so the gate does not depend on how the build script is
    written. Trust is not judged here (the signature gate does that); only whether each signature and catalog entry
    still belongs to the bytes of the stamped file. With require_signed (a release that signs every PE file, tester.31
    on) an unsigned file or a package without a catalog entry also fails: the gate then cannot run before those steps."""
    if os.name != "nt":
        log("  order: not checked off Windows")
        return []
    problems = []
    states = [(rel, signature_digest(p)) for rel, p in checked]
    for rel, state in states:
        if state == "bad digest":
            problems.append(f"{rel}: its signature does not cover the file as shipped (changed after signing)")
        elif state == "unsigned" and require_signed:
            problems.append(f"{rel}: not signed; with --signed every checked PE file is signed before this gate runs")
    signed = sum(1 for _, state in states if state != "unsigned")
    by_dir = {}
    for rel, p in checked:
        by_dir.setdefault(p.parent, []).append((rel, p))
    catalogs = sorted(Path(top).rglob("*.cat"))
    covered = 0
    for cat in catalogs:
        members = catalog_members(cat)
        for rel, p in [x for d, xs in by_dir.items() if d == cat.parent or cat.parent in d.parents for x in xs]:
            tag = members.get(p.name.lower())
            if tag is None:
                continue
            if catalog_hash(p, "SHA256" if len(tag) == 64 else "SHA1") != tag:
                problems.append(f"{rel}: the catalog {cat.name} lists another hash for it (changed after Inf2Cat)")
            else:
                covered += 1
    if require_signed and not covered:
        problems.append("no catalog under the package lists a checked PE file; with --signed the gate runs after Inf2Cat")
    log(f"  order: {signed} of {len(checked)} checked PE files signed over their shipped bytes, "
        f"{covered} listed with their shipped hash in {len(catalogs)} catalog(s)")
    return problems


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("amd-version")
    a.add_argument("version")
    s = sub.add_parser("stamp")
    s.add_argument("--version", required=True)
    s.add_argument("--inf", required=True)
    s.add_argument("--table", default=str(TABLE))
    s.add_argument("--name", help="the file name the resource names (default: the file's own name)")
    s.add_argument("--flavour", choices=FLAVOURS, default="release")
    s.add_argument("files", nargs="+")
    r = sub.add_parser("release")
    r.add_argument("--package", required=True)
    r.add_argument("--version", required=True)
    r.add_argument("--sources", required=True)
    r.add_argument("--inf", required=True)
    r.add_argument("--table", default=str(TABLE))
    r.add_argument("--setup", help="the installer exe outside the payload, stamped with it")
    r.add_argument("--flavour", choices=FLAVOURS, default="release")
    c = sub.add_parser("check")
    c.add_argument("--package-top", required=True)
    c.add_argument("--version", required=True)
    c.add_argument("--sources", required=True)
    c.add_argument("--inf", required=True)
    c.add_argument("--table", default=str(TABLE))
    c.add_argument("--signed", action="store_true",
                   help="the release signs every PE file and makes its catalog before this gate: refuse unsigned files")
    c.add_argument("--flavour", choices=FLAVOURS, default="release")
    c.add_argument("--workspace",
                   help="the workspace root that holds the lane inputs release-sources.json pins; with it the "
                        "D3D11 configuration records are rebuilt, so their capability fields are checked too")
    h = sub.add_parser("rc-header")
    h.add_argument("--version", required=True)
    h.add_argument("--inf", required=True)
    h.add_argument("--name", required=True, help="the file the .rc builds, for example bc250kmd.sys")
    h.add_argument("--out", required=True, help="the header, version-stamp.h for bc250kmd.rc")
    h.add_argument("--table", default=str(TABLE))
    h.add_argument("--flavour", choices=FLAVOURS, default="release")
    h.add_argument("--lab", action="store_true", help="the same as --flavour lab")
    w = sub.add_parser("show")
    w.add_argument("files", nargs="+")
    args = ap.parse_args(argv)
    try:
        if args.cmd == "amd-version":
            print(dotted(amd_version(args.version)))
            return 0
        if args.cmd == "show":
            for f in args.files:
                found = [leaf for leaf in read_resources(Image(Path(f).read_bytes())) if leaf[0] == RT_VERSION]
                if not found:
                    print(f"{f}: no version resource")
                for leaf in found:
                    fv, pv, kind, strings = parse_version_resource(leaf[4])
                    print(f"{f}: language {leaf[2]:#06x} FILEVERSION {dotted(fv)} PRODUCTVERSION {dotted(pv)} type {kind} {strings}")
            return 0
        table = load_table(args.table)
        if args.cmd == "rc-header":
            text = rc_header(args.name, args.version, inf_provider(args.inf), table,
                             "lab" if args.lab else args.flavour)
            Path(args.out).parent.mkdir(parents=True, exist_ok=True)
            Path(args.out).write_text(text, encoding="ascii", newline="\n")
            print(f"rc-header {args.out}: {args.name} {dotted(amd_version(args.version))} / {args.version}")
            return 0
        if args.cmd == "stamp":
            company = inf_provider(args.inf)
            for f in args.files:
                old, new = stamp_file(f, args.version, company, table, args.name, args.flavour)
                print(f"stamped {f} {dotted(amd_version(args.version))} / {args.version}: {old[:8]} -> {new}")
            return 0
        if args.cmd == "release":
            stamp_release(args.package, args.version, args.sources, args.inf, table, setup=args.setup,
                          flavour=args.flavour)
            return 0
        problems = check_release(args.package_top, args.version, args.sources, args.inf, table,
                                 require_signed=args.signed, flavour=args.flavour,
                                 workspace=args.workspace)
        for p in problems:
            print("FAIL " + p)
        print("version resources: " + ("FAIL" if problems else "PASS"))
        return 1 if problems else 0
    except StampError as exc:
        print(f"versionstamp: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
