"""Compare two PE images byte for byte, apart from the fields a non-/Brepro MSVC link writes afresh on every build.

    python tools/build/pe_compare.py <reference.dll> <rebuilt.dll>

Those fields are the COFF header TimeDateStamp, the TimeDateStamp of every debug directory entry, and the GUID and
age of the CodeView (RSDS) record. Everything else, the PE CheckSum included, must be equal. Prints each differing
byte range with the field it belongs to; exit 0 when the only differences are in those fields (or none), 1
otherwise. Used to show that the committed router source (driver/umd/router) rebuilds the registered binary.
"""
import hashlib
import struct
import sys


def volatile_fields(data):
    """[(offset, length, name)] of the per-build fields of a PE image."""
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    if data[pe:pe + 4] != b'PE\0\0':
        raise ValueError('not a PE image')
    fields = [(pe + 8, 4, 'COFF TimeDateStamp')]
    sections = struct.unpack_from('<H', data, pe + 6)[0]
    optional_size = struct.unpack_from('<H', data, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from('<H', data, opt)[0]
    directories = opt + (112 if magic == 0x20B else 96)
    debug_rva, debug_size = struct.unpack_from('<II', data, directories + 6 * 8)
    table = []
    at = opt + optional_size
    for _ in range(sections):
        virtual_size, va, raw_size, raw_ptr = struct.unpack_from('<IIII', data, at + 8)
        table.append((va, max(virtual_size, raw_size), raw_ptr))
        at += 40

    def file_offset(rva):
        for va, size, raw in table:
            if va <= rva < va + size:
                return rva - va + raw
        raise ValueError('RVA %#x outside every section' % rva)

    if debug_size:
        base = file_offset(debug_rva)
        for i in range(debug_size // 28):
            entry = base + i * 28
            fields.append((entry + 4, 4, 'debug entry %d TimeDateStamp' % i))
            kind, size, _, pointer = struct.unpack_from('<IIII', data, entry + 12)
            if kind == 2 and data[pointer:pointer + 4] == b'RSDS':
                fields.append((pointer + 4, 16, 'RSDS GUID'))
                fields.append((pointer + 20, 4, 'RSDS age'))
    return fields


def main(reference_path, rebuilt_path):
    reference = open(reference_path, 'rb').read()
    rebuilt = open(rebuilt_path, 'rb').read()
    if len(reference) != len(rebuilt):
        print('size differs: %d against %d' % (len(reference), len(rebuilt)))
        return 1
    fields = volatile_fields(reference)
    if fields != volatile_fields(rebuilt):
        print('the per-build fields sit at different offsets')
        return 1
    ranges, start = [], None
    for i in range(len(reference) + 1):
        differs = i < len(reference) and reference[i] != rebuilt[i]
        if differs and start is None:
            start = i
        elif not differs and start is not None:
            ranges.append((start, i))
            start = None
    # A range is explained when every byte of it lies in some per-build field: one contiguous range can span two
    # adjacent fields (RSDS GUID followed by RSDS age).
    covered = set()
    for off, ln, _ in fields:
        covered.update(range(off, off + ln))
    unexplained = 0
    for a, b in ranges:
        names = [n for off, ln, n in fields if off < b and a < off + ln]
        explained = all(i in covered for i in range(a, b))
        if not explained:
            unexplained += 1
        print('0x%05X-0x%05X  %s  %s -> %s' % (a, b - 1, ' + '.join(names) if explained else 'UNEXPLAINED',
                                               reference[a:b].hex(), rebuilt[a:b].hex()))
    print('%d bytes, %d differing range(s), %d outside the per-build fields' % (len(reference), len(ranges), unexplained))
    # The rebuilt image with the reference's per-build fields copied in: equal to the reference exactly when every
    # difference sits in those fields. Printed as evidence, never written back.
    patched = bytearray(rebuilt)
    for off, ln, _ in fields:
        patched[off:off + ln] = reference[off:off + ln]
    print('SHA-256 reference                        %s' % hashlib.sha256(reference).hexdigest().upper())
    print('SHA-256 rebuilt                          %s' % hashlib.sha256(rebuilt).hexdigest().upper())
    print('SHA-256 rebuilt, reference fields copied %s' % hashlib.sha256(bytes(patched)).hexdigest().upper())
    return 1 if unexplained else 0


if __name__ == '__main__':
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1], sys.argv[2]))
