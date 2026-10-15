"""The symbol files of a release: archive them, record them, and refuse a package that is missing one.

A crash report from a tester, a dump from the lab and a debugger session all need the symbols of the exact
bytes that ran. The package itself never carries them (they are larger than the driver and they are of no use
on the tester's PC), so each release archives them next to its build and the manifest says which symbol file
belongs to which shipped byte: the GUID and age of the PE's own debug directory, which no other build repeats.

    python -I symbols.py archive --sources <flavour sources> --root <repo root> --package <package root>
                                 --out <archive directory> [--records <json>]
    python -I symbols.py check   --package <package root> --symbols <archive directory>

`archive` reads the staged package, finds the symbol file of each PE beside the file the release copied it
from, and writes the archive as one directory per payload directory, because three builds of the RADV ICD all
record the name `vulkan_radeon.pdb` and one flat directory cannot hold them. `check` is the gate: every PE of
the package has a symbol file in the archive, and that file carries the identity the PE names.

A staged DriverStore member answers for its input slot, as it does in the version gate: the release copies the
lane file under the name the DriverStore chose, so the row that pins it, and the exemption that may excuse it,
are the lane file's and not the renamed member's. Without that mapping every member of a DriverStore package
reads as a file the flavour's sources do not list.

A PE we do not build (the Visual C++ redistributable, the Vulkan SDK's own tools) cannot have a symbol file
here, so `symbols-exempt.json` lists each such path with the reason. The list is data and it is checked: an
exemption for a file that is not in the package, or a missing entry for a PE without debug information, both
fail the gate.
"""
import argparse
import json
import pathlib
import shutil
import struct
import sys

PE_SUFFIXES = (".dll", ".exe", ".sys")

# The PE side of this gate is already in the repository: packagecheck.PeImage reads the debug directory and
# gives (GUID, age, recorded path). check_pdb.py imports it the same way, for the KMD's submission symbols.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "packagecheck"))
from packagecheck import PeImage                                    # noqa: E402


def guid_text(g1, g2, g3, tail):
    """The GUID as the debuggers print it: 8-4-4-4-12. The same form PeImage prints for a PE."""
    return "%08X-%04X-%04X-%s-%s" % (g1, g2, g3, tail[:2].hex().upper(), tail[2:].hex().upper())


def pe_symbol_identity(path):
    """What a PE says about its symbols: the CodeView record, a GNU debug link, or neither."""
    pe = PeImage(path)
    if not pe.valid:
        raise ValueError("; ".join(pe.problems) or "not a PE")
    if pe.codeview:
        guid, age, recorded = pe.codeview
        return {"kind": "pdb", "guid": guid, "age": age,
                "file": recorded.replace("\\", "/").rsplit("/", 1)[-1]}
    for section in pe.sections:
        if section.name == ".gnu_debuglink":
            raw = pe.data[section.raw_off:section.raw_off + section.raw_size]
            return {"kind": "debuglink", "file": raw.split(b"\0")[0].decode("utf-8", "replace")}
    return {"kind": "none"}


def portable_pdb_identity(data):
    """A portable PDB (csc /debug:portable): the `#Pdb` stream holds the id whose GUID the PE records."""
    version_length = struct.unpack_from("<I", data, 12)[0]
    at = 16 + version_length + (-version_length % 4)
    streams = struct.unpack_from("<H", data, at + 2)[0]
    at += 4
    for _ in range(streams):
        offset, _size = struct.unpack_from("<II", data, at)
        at += 8
        end = data.index(b"\0", at)
        name = data[at:end].decode("ascii", "replace")
        at = end + 1
        at += -at % 4
        if name == "#Pdb":
            g1, g2, g3, g4 = struct.unpack_from("<I H H 8s", data, offset)
            return {"guid": guid_text(g1, g2, g3, g4), "age": 1}
    raise ValueError("the portable PDB has no #Pdb stream")


def pdb_identity(path):
    """The GUID and age the symbol file itself carries, so another build's file cannot pass for this one's."""
    data = pathlib.Path(path).read_bytes()
    if data[:4] == b"BSJB":
        return portable_pdb_identity(data)
    if not data.startswith(b"Microsoft C/C++ MSF 7.00"):
        raise ValueError("neither an MSF 7.0 PDB nor a portable PDB")
    block_size, _free, _blocks, dir_bytes, _unused, dir_block = struct.unpack_from("<IIIIII", data, 32)

    def block(n):
        return data[n * block_size:(n + 1) * block_size]

    n_dir_blocks = -(-dir_bytes // block_size)
    dir_map = struct.unpack_from("<%dI" % n_dir_blocks, block(dir_block), 0)
    directory = b"".join(block(n) for n in dir_map)[:dir_bytes]
    n_streams = struct.unpack_from("<I", directory, 0)[0]
    sizes = struct.unpack_from("<%dI" % n_streams, directory, 4)
    at = 4 + 4 * n_streams
    blocks = []
    for size in sizes:
        count = 0 if size in (0, 0xFFFFFFFF) else -(-size // block_size)
        blocks.append(struct.unpack_from("<%dI" % count, directory, at) if count else ())
        at += 4 * count
    if n_streams < 2:
        raise ValueError("no PDB stream")
    stream1 = b"".join(block(n) for n in blocks[1])[:sizes[1]]
    _version, _sig, age, g1, g2, g3, g4 = struct.unpack_from("<III I H H 8s", stream1, 0)
    return {"guid": guid_text(g1, g2, g3, g4), "age": age}


def exemptions(here, override=None):
    """The list of PE files that this release archives no symbol file for, with the reason for each.

    `override` is for the tool's own tests, which build a package of crafted files and need their own list."""
    path = pathlib.Path(override) if override else here / "symbols-exempt.json"
    if not path.is_file():
        return {}
    data = json.loads(path.read_text(encoding="utf-8"))
    return {row["path"]: row["reason"] for row in data["files"]}


def lane_slots(sources):
    """The lane path of every staged DriverStore member, by the path the member has in the package.

    An empty mapping for a package that stages nothing, and for a sources file the mapping cannot be read
    from: then every path answers for itself, which is what a release before the DriverStore did."""
    try:
        from driver_package import members
        return {row["path"]: row["source"] for row in members(sources)}
    except (ImportError, KeyError, ValueError):
        return {}


def package_pes(package):
    """Every PE of the staged package, by its path inside the package."""
    out = []
    for p in sorted(pathlib.Path(package).rglob("*")):
        if p.is_file() and p.suffix.lower() in PE_SUFFIXES:
            out.append((str(p.relative_to(package)).replace("\\", "/"), p))
    return out


def archive(args):
    here = pathlib.Path(__file__).resolve().parent
    exempt = exemptions(here, args.exempt)
    sources = json.loads(pathlib.Path(args.sources).read_text(encoding="utf-8"))
    source_of = {row["path"]: row["source"] for row in sources["files"]}
    slots = lane_slots(sources)
    out = pathlib.Path(args.out)
    records, missing = {}, []
    for rel, path in package_pes(args.package):
        # A staged member answers for its input slot, the lane file the release copied under the DriverStore's
        # name: that slot owns the row that pins it and the exemption that may excuse it.
        slot = slots.get(rel, rel)
        named = rel if slot == rel else "%s (%s)" % (rel, slot)
        # The exemption decides before anything is read from the file: a carried third-party binary may well
        # name a PDB of its own vendor's build (vc_redist.x64.exe names burn.pdb), and we have no such file.
        if slot in exempt:
            records[rel] = {"symbols": "none", "reason": exempt[slot]}
            print("  %-52s exempt: %s" % (named, exempt[slot]))
            continue
        said = pe_symbol_identity(path)
        if said["kind"] == "none":
            missing.append("%s: no debug information in the shipped file" % named)
            continue
        # The symbol file sits beside the file the release copied in, under the name the PE records.
        source = source_of.get(slot)
        if source is None:
            missing.append("%s: the flavour's sources do not list this file" % named)
            continue
        beside = (pathlib.Path(args.root) / source.replace("/", "\\")).parent / said["file"]
        if not beside.is_file():
            missing.append("%s: no %s beside %s" % (named, said["file"], source))
            continue
        if said["kind"] == "pdb":
            have = pdb_identity(beside)
            if have["guid"] != said["guid"] or have["age"] != said["age"]:
                missing.append("%s: %s belongs to another build (%s/%d, the file wants %s/%d)"
                               % (named, said["file"], have["guid"], have["age"], said["guid"], said["age"]))
                continue
        destination = out / pathlib.PurePosixPath(rel).parent / said["file"]
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(beside, destination)
        row = {"symbols": str(pathlib.PurePosixPath(rel).parent / said["file"]),
               "bytes": destination.stat().st_size}
        if said["kind"] == "pdb":
            row.update({"guid": said["guid"], "age": said["age"]})
        else:
            row["debuglink"] = said["file"]
        records[rel] = row
        print("  %-52s %s (%d bytes)" % (rel, said["file"], row["bytes"]))
    carried = {slots.get(rel, rel) for rel, _ in package_pes(args.package)}
    for path in sorted(exempt):
        if path not in carried:
            missing.append("%s: exempt but not in the package" % path)
    if args.records:
        pathlib.Path(args.records).write_text(json.dumps(records, indent=1), encoding="ascii")
    for line in missing:
        print("  MISSING %s" % line)
    return 1 if missing else 0


def check(args):
    here = pathlib.Path(__file__).resolve().parent
    exempt = exemptions(here, args.exempt)
    symbols = pathlib.Path(args.symbols)
    # With --sources a staged member answers for its input slot, as in `archive`: the exemption of a renamed
    # member is the lane file's. Without it every path answers for itself.
    slots = lane_slots(json.loads(pathlib.Path(args.sources).read_text(encoding="utf-8"))) if args.sources else {}
    bad = []
    for rel, path in package_pes(args.package):
        slot = slots.get(rel, rel)
        if slot in exempt:                        # the reason is in symbols-exempt.json, next to this tool
            continue
        said = pe_symbol_identity(path)
        if said["kind"] == "none":
            bad.append("%s: no debug information and no exemption" % rel)
            continue
        where = symbols / pathlib.PurePosixPath(rel).parent / said["file"]
        if not where.is_file():
            bad.append("%s: %s is not in the archive" % (rel, said["file"]))
            continue
        if said["kind"] == "pdb":
            try:
                have = pdb_identity(where)
            except (OSError, ValueError, struct.error) as e:
                bad.append("%s: %s cannot be read (%s)" % (rel, said["file"], e))
                continue
            if have["guid"] != said["guid"] or have["age"] != said["age"]:
                bad.append("%s: the archived %s is another build's (%s/%d, not %s/%d)"
                           % (rel, said["file"], have["guid"], have["age"], said["guid"], said["age"]))
    for line in bad:
        print("  %s" % line)
    print("symbols: %d PE files, %d exempt, %d problem(s)"
          % (len(package_pes(args.package)), len(exempt), len(bad)))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="action", required=True)
    a = sub.add_parser("archive")
    a.add_argument("--sources", required=True, help="the flavour's release sources (build-release.ps1 writes it)")
    a.add_argument("--root", required=True)
    a.add_argument("--package", required=True)
    a.add_argument("--out", required=True)
    a.add_argument("--records", help="write the manifest records here")
    a.add_argument("--exempt", help="another exemption list, for this tool's own tests")
    c = sub.add_parser("check")
    c.add_argument("--package", required=True)
    c.add_argument("--symbols", required=True)
    c.add_argument("--sources", help="the flavour's release sources, so a staged member answers for its slot")
    c.add_argument("--exempt", help="another exemption list, for this tool's own tests")
    args = ap.parse_args()
    return archive(args) if args.action == "archive" else check(args)


if __name__ == "__main__":
    sys.exit(main())
