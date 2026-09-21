#!/usr/bin/env python3
"""Debugger-free first look at a 64-bit kernel minidump (C:\\Windows\\Minidump\\*.dmp, "triage dump").

The lab has no kernel debugger, so after a bugcheck this answers the first three questions from the dump
alone: what was the bugcheck, which loaded module holds the faulting address (and at which offset), and which
modules does the stack point into. With the module's file as second argument it also prints the code bytes at
the faulting offset (triage dumps rarely carry code pages). No symbols: offsets only.

    python triage.py <minidump> [file of the faulting module]

Layout knowledge used (measured on a 22621 dump, guarded by sanity checks rather than trusted):
DUMP_HEADER64 bugcheck code at 0x38 and parameters at 0x40, dump type at 0xF98; TRIAGE_DUMP64 at 0x2000 as a
row of ULONG fields, with the signature "TRGD" at the offset its ValidOffset field names; driver entries of
0x90 bytes with the name's string-pool offset at +0, DllBase at +0x38, SizeOfImage at +0x48, CheckSum at +0x80,
TimeDateStamp at +0x88; pool strings are a ULONG character count followed by UTF-16. Every one of these is
checked before it is used: a dump whose layout differs makes the script stop, not invent.
"""
import struct
import sys

TRIAGE = 0x2000
FIELDS = ["ServicePackBuild", "SizeOfDump", "ValidOffset", "ContextOffset", "ExceptionOffset", "MmOffset",
          "UnloadedDriversOffset", "PrcbOffset", "ProcessOffset", "ThreadOffset", "CallStackOffset", "SizeOfCallStack",
          "DriverListOffset", "DriverCount", "StringPoolOffset", "StringPoolSize", "BrokenDriverOffset", "TriageOptions"]
ENTRY = 0x90
DUMP_TYPE_TRIAGE = 4
TRIAGE_VALID = 0x44475254  # "TRGD" at TRIAGE_DUMP64.ValidOffset
# Bugcheck codes whose parameter N (0-based) is the faulting instruction address.
FAULT_PARAM = {0x0A: 3, 0x1E: 1, 0x3B: 1, 0x50: 2, 0x7E: 1, 0x8E: 1, 0xD1: 3}
# Bugcheck codes known to carry no faulting address, so that silence about one is a statement, not a gap.
NO_FAULT_PARAM = {0x133, 0x139, 0x9F, 0xEF}


def main(argv):
    if not argv:
        sys.exit(__doc__)
    dump = open(argv[0], "rb").read()
    u32 = lambda o: struct.unpack_from("<I", dump, o)[0]
    u64 = lambda o: struct.unpack_from("<Q", dump, o)[0]
    if dump[:8] != b"PAGEDU64":
        sys.exit("not a 64-bit kernel dump")

    code = u32(0x38)
    params = [u64(0x40 + 8 * i) for i in range(4)]
    dump_type = u32(0xF98)
    print(f"build {u32(12)}  processors {u32(0x34)}  dump type {dump_type}")
    print(f"bugcheck {code:#x}  (" + ", ".join(f"{p:#x}" for p in params) + ")")
    if dump_type != DUMP_TYPE_TRIAGE:
        sys.exit(f"dump type {dump_type} is not a triage dump (4); this script only knows the triage layout")

    t = {name: u32(TRIAGE + 4 * i) for i, name in enumerate(FIELDS)}
    # Positive control for the whole layout: the field that says where the end marker is has to point at it.
    if not 0 < t["ValidOffset"] <= len(dump) - 4 or u32(t["ValidOffset"]) != TRIAGE_VALID:
        sys.exit("no TRGD marker where TRIAGE_DUMP64.ValidOffset points: this is not the layout this script reads")
    for field in ("CallStackOffset", "DriverListOffset", "StringPoolOffset"):
        if not 0 < t[field] < len(dump):
            sys.exit(f"TRIAGE_DUMP64.{field} ({t[field]:#x}) is outside the file: layout assumptions do not hold")
    if t["DriverListOffset"] + t["DriverCount"] * ENTRY > len(dump):
        sys.exit("the driver list runs past the end of the file: layout assumptions do not hold")
    modules = []
    for i in range(t["DriverCount"]):
        e = t["DriverListOffset"] + i * ENTRY
        name_at, base, size = u32(e), u64(e + 0x38), u32(e + 0x48)
        if base >> 47 != 0x1FFFF or base & 0xFFF or not 0 < name_at < len(dump) - 4 or not 0 < u32(name_at) < 300:
            sys.exit(f"driver entry {i} does not look like one: the layout assumptions of this script do not hold for this dump")
        name = dump[name_at + 4:name_at + 4 + 2 * u32(name_at)].decode("utf-16-le", "replace").split("\\")[-1]
        modules.append((base, size, name, u32(e + 0x80), u32(e + 0x88)))
    print(f"{len(modules)} loaded modules")

    def where(address):
        for base, size, name, _, _ in modules:
            if base <= address < base + size:
                return name, address - base
        return None

    if code in FAULT_PARAM:
        fault = params[FAULT_PARAM[code]]
        hit = where(fault)
        print(f"faulting address {fault:#x}: " + (f"{hit[0]}+{hit[1]:#x}" if hit else "in no loaded module (unloaded driver, pool, or user space)"))
        if hit and len(argv) > 1:
            code_bytes(argv[1], hit, [m for m in modules if m[2] == hit[0]][0])
    elif code in NO_FAULT_PARAM:
        print("this bugcheck carries no faulting instruction address in its parameters")
    else:
        print(f"bugcheck {code:#x} is not in this script's parameter table: read its parameters in the documentation")

    ours = [m for m in modules if "bc250" in m[2].lower()]
    print("our modules loaded: " + (", ".join(f"{m[2]} at {m[0]:#x}" for m in ours) if ours else "none"))

    print("stack values that point into modules (top first, repeats folded):")
    last, shown = None, 0
    for o in range(0, t["SizeOfCallStack"] - 7, 8):
        hit = where(u64(t["CallStackOffset"] + o))
        if hit and hit != last and shown < 48:
            print(f"  +{o:#06x}  {hit[0]}+{hit[1]:#x}")
            last, shown = hit, shown + 1


def code_bytes(path, hit, module):
    pe = open(path, "rb").read()
    nt = struct.unpack_from("<I", pe, 0x3C)[0]
    stamp, checksum = struct.unpack_from("<I", pe, nt + 8)[0], struct.unpack_from("<I", pe, nt + 24 + 64)[0]
    same = (stamp, checksum) == (module[4], module[3])
    print(f"  file {'matches' if same else 'DOES NOT match'} the loaded image (timestamp {stamp:#x}/{module[4]:#x}, checksum {checksum:#x}/{module[3]:#x})")
    sections = nt + 24 + struct.unpack_from("<H", pe, nt + 20)[0]
    for i in range(struct.unpack_from("<H", pe, nt + 6)[0]):
        s = sections + 40 * i
        vsize, va, rsize, raw = struct.unpack_from("<IIII", pe, s + 8)
        if va <= hit[1] < va + max(vsize, rsize):
            at = raw + hit[1] - va
            name = pe[s:s + 8].rstrip(bytes(1)).decode()
            if not 0 <= at < len(pe):
                print(f"  section {name}  offset {at:#x} is outside the file (uninitialised section?)")
                return
            print(f"  section {name}  16 bytes before: {pe[max(at - 16, 0):at].hex(' ')}")
            print(f"  {'':<{len(name) + 9}}  24 bytes at    : {pe[at:at + 24].hex(' ')}")
            return
    print(f"  offset {hit[1]:#x} falls in no section of the file")


if __name__ == "__main__":
    main(sys.argv[1:])
