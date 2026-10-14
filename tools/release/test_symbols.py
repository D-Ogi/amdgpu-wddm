"""The controls of the symbol gate (symbols.py).

The gate's claim is that a release cannot ship a PE whose symbols are missing, nor pass off another build's
symbol file as this one's. Both halves are tested here against crafted files, so the test needs no compiler:
a PE is written with a debug directory of our own making, and a symbol file in each of the two formats the
release produces (an MSF PDB from the linker, a portable PDB from csc).
"""
import json
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
TOOL = HERE / "symbols.py"
GUID = (0x11223344, 0x5566, 0x7788, bytes(range(0x99, 0xA1)))



def pe_bytes(pdb_name=None, guid=GUID, age=1):
    """A PE with one section, and a CodeView record in its debug directory when a name is given."""
    section_va, section_raw, section_size = 0x1000, 0x400, 0x400
    body = bytearray(section_size)
    debug_dir_at = 0x40                     # inside the section, so the data directory can point at it
    record_at = 0x80
    if pdb_name:
        name = pdb_name.encode("ascii") + b"\0"
        struct.pack_into("<I I H H 8s I", body, record_at, 0x53445352, guid[0], guid[1], guid[2], guid[3], age)
        body[record_at + 24:record_at + 24 + len(name)] = name
        struct.pack_into("<IIHHIIII", body, debug_dir_at, 0, 0, 0, 0, 2, 24 + len(name),
                         section_va + record_at, section_raw + record_at)
    header = bytearray(section_raw)
    header[0:2] = b"MZ"
    pe = 0x80
    struct.pack_into("<I", header, 0x3C, pe)
    header[pe:pe + 4] = b"PE\0\0"
    struct.pack_into("<HH", header, pe + 4, 0x8664, 1)           # machine, one section
    struct.pack_into("<H", header, pe + 20, 240)                 # optional header size (PE32+)
    opt = pe + 24
    struct.pack_into("<H", header, opt, 0x20B)
    struct.pack_into("<I", header, opt + 108, 16)                # number of data directories
    dirs = opt + 112
    if pdb_name:
        struct.pack_into("<II", header, dirs + 6 * 8, section_va + debug_dir_at, 28)
    at = opt + 240
    struct.pack_into("<8sIIII", header, at, b".rdata", section_size, section_va, section_size, section_raw)
    return bytes(header) + bytes(body)


def msf_pdb_bytes(guid=GUID, age=1):
    """A smallest MSF 7.0 file whose stream 1 carries the identity: superblock, directory, stream."""
    block = 512
    data = bytearray(block * 5)
    data[0:32] = b"Microsoft C/C++ MSF 7.00\r\n\x1aDS\0\0"
    struct.pack_into("<IIIIII", data, 32, block, 2, 5, 0, 0, 3)  # block size, free map, blocks, dir bytes, _, dir block
    # The stream directory: two streams, the second one is stream 1 in block 4.
    directory = bytearray()
    directory += struct.pack("<I", 2)
    directory += struct.pack("<II", 0, 28)                       # stream sizes
    directory += struct.pack("<I", 4)                            # stream 1 lives in block 4
    struct.pack_into("<I", data, 32 + 12, len(directory))        # directory byte count
    data[3 * block:3 * block + 4] = struct.pack("<I", 2)         # the directory's own block map
    data[2 * block:2 * block + len(directory)] = directory
    struct.pack_into("<III I H H 8s", data, 4 * block, 20000404, 0, age, guid[0], guid[1], guid[2], guid[3])
    return bytes(data)


def portable_pdb_bytes(guid=GUID):
    """A portable PDB: the BSJB root, a version string, and a `#Pdb` stream with the id."""
    version = b"PDB v1.0\0\0\0\0"
    head = bytearray()
    head += b"BSJB" + struct.pack("<HHI", 1, 1, 0) + struct.pack("<I", len(version)) + version
    head += struct.pack("<HH", 0, 1)
    name = b"#Pdb\0\0\0\0"
    offset = len(head) + 8 + len(name)
    head += struct.pack("<II", offset, 20) + name
    head += struct.pack("<I H H 8s", guid[0], guid[1], guid[2], guid[3]) + b"\0\0\0\0"
    return bytes(head)


class SymbolGate(unittest.TestCase):
    def setUp(self):
        self.dir = pathlib.Path(tempfile.mkdtemp(prefix="symbols-"))
        self.addCleanup(shutil.rmtree, self.dir, True)
        self.package = self.dir / "files"
        self.build = self.dir / "build"
        self.archive = self.dir / "symbols"
        for d in (self.package / "payload" / "kmd", self.build, self.archive):
            d.mkdir(parents=True, exist_ok=True)
        self.exempt = self.dir / "exempt.json"
        self.write_exempt([])

    def write_exempt(self, rows):
        self.exempt.write_text(json.dumps({"about": "test", "files": rows}), encoding="ascii")

    def ship(self, name="bc250kmd.sys", pdb_name="bc250kmd.pdb", **kw):
        (self.package / "payload/kmd" / name).write_bytes(pe_bytes(pdb_name, **kw))
        (self.build / name).write_bytes(pe_bytes(pdb_name, **kw))
        sources = {"files": [{"path": "payload/kmd/" + name, "source": "build/" + name,
                              "sha256": "0" * 64, "component": "kmd"}]}
        (self.dir / "sources.json").write_text(json.dumps(sources), encoding="ascii")

    def symbols_beside(self, name="bc250kmd.pdb", data=None):
        (self.build / name).write_bytes(data if data is not None else msf_pdb_bytes())

    def run_tool(self, *args):
        out = subprocess.run([sys.executable, "-I", str(TOOL)] + list(args),
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        return out.returncode, out.stdout.decode("utf-8", "replace")

    def archive_run(self):
        return self.run_tool("archive", "--sources", str(self.dir / "sources.json"), "--root", str(self.dir),
                             "--package", str(self.package), "--out", str(self.archive),
                             "--records", str(self.dir / "records.json"), "--exempt", str(self.exempt))

    def check_run(self):
        return self.run_tool("check", "--package", str(self.package), "--symbols", str(self.archive),
                             "--exempt", str(self.exempt))

    def test_the_right_symbol_file_is_archived_and_passes(self):
        self.ship()
        self.symbols_beside()
        code, text = self.archive_run()
        self.assertEqual(0, code, text)
        self.assertTrue((self.archive / "payload/kmd/bc250kmd.pdb").is_file(), text)
        records = json.loads((self.dir / "records.json").read_text(encoding="ascii"))
        self.assertEqual("payload/kmd/bc250kmd.pdb", records["payload/kmd/bc250kmd.sys"]["symbols"])
        self.assertEqual(1, records["payload/kmd/bc250kmd.sys"]["age"])
        code, text = self.check_run()
        self.assertEqual(0, code, text)

    def test_a_portable_pdb_passes(self):
        self.ship(name="amdgpu_wddm_control.exe", pdb_name="amdgpu_wddm_control.pdb")
        self.symbols_beside("amdgpu_wddm_control.pdb", portable_pdb_bytes())
        code, text = self.archive_run()
        self.assertEqual(0, code, text)
        code, text = self.check_run()
        self.assertEqual(0, code, text)

    def test_a_missing_symbol_file_fails(self):
        self.ship()
        code, text = self.archive_run()
        self.assertEqual(1, code, text)
        self.assertIn("no bc250kmd.pdb beside", text)
        code, text = self.check_run()
        self.assertEqual(1, code, text)
        self.assertIn("is not in the archive", text)

    def test_a_symbol_file_of_another_build_fails(self):
        self.ship()
        other = (GUID[0] ^ 0xFF, GUID[1], GUID[2], GUID[3])
        self.symbols_beside(data=msf_pdb_bytes(guid=other))
        code, text = self.archive_run()
        self.assertEqual(1, code, text)
        self.assertIn("belongs to another build", text)

    def test_an_archived_file_of_another_build_fails_the_gate(self):
        self.ship()
        self.symbols_beside()
        self.assertEqual(0, self.archive_run()[0])
        other = (GUID[0], GUID[1] ^ 0xFF, GUID[2], GUID[3])
        (self.archive / "payload/kmd/bc250kmd.pdb").write_bytes(msf_pdb_bytes(guid=other))
        code, text = self.check_run()
        self.assertEqual(1, code, text)
        self.assertIn("another build's", text)

    def test_a_shipped_pe_without_debug_information_fails(self):
        self.ship(pdb_name=None)
        code, text = self.archive_run()
        self.assertEqual(1, code, text)
        self.assertIn("no debug information", text)
        code, text = self.check_run()
        self.assertEqual(1, code, text)
        self.assertIn("no debug information and no exemption", text)

    def test_an_exempt_file_needs_no_symbols(self):
        self.ship(pdb_name=None)
        self.write_exempt([{"path": "payload/kmd/bc250kmd.sys", "reason": "carried unchanged, for this test"}])
        code, text = self.archive_run()
        self.assertEqual(0, code, text)
        self.assertIn("exempt:", text)
        self.assertEqual(0, self.check_run()[0])

    def test_an_exemption_for_a_file_the_package_does_not_hold_fails(self):
        self.ship()
        self.symbols_beside()
        self.write_exempt([{"path": "payload/kmd/not-shipped.dll", "reason": "stale entry"}])
        code, text = self.archive_run()
        self.assertEqual(1, code, text)
        self.assertIn("exempt but not in the package", text)


if __name__ == "__main__":
    unittest.main()
