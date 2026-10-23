"""Post-link AMD64 KMD memory-layout gate, not runtime HVCI certification.

Contract: Microsoft implement-hvci-compatible-code and PE format documentation.
https://learn.microsoft.com/windows-hardware/drivers/driversecurity/implement-hvci-compatible-code
https://learn.microsoft.com/windows/win32/debug/pe-format
NX_COMPAT and a present, file-backed IAT are additional project requirements.
Allocation protections, dynamic code and runtime behavior need separate checks.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct


MAX_BYTES = 128 * 1024 * 1024  # Project bound for a linked KMD, not a PE format limit.
PAGE = 4096
EXECUTE = 0x20000000
WRITE = 0x80000000


def require(condition, message):
    if not condition:
        raise ValueError(message)


def power_of_two(value):
    return value > 0 and value & (value - 1) == 0


def inspect(raw):
    """Validate the bounded header/section subset needed for the memory gate.

    This is not a replacement for the Windows PE loader or signature verifier.
    No RVA is dereferenced and the image is never loaded as executable code.
    """
    size = len(raw)
    require(64 <= size <= MAX_BYTES, 'image size outside project bounds')

    def read(fmt, offset):
        count = struct.calcsize('<' + fmt)
        require(0 <= offset <= size - count, 'truncated PE field')
        return struct.unpack_from('<' + fmt, raw, offset)

    require(raw[:2] == b'MZ', 'missing DOS signature')
    pe, = read('I', 60)
    require(64 <= pe <= size - 24, 'invalid PE header offset')
    require(raw[pe:pe + 4] == b'PE\0\0', 'missing PE signature')
    machine, count = read('HH', pe + 4)
    optional_size, = read('H', pe + 20)
    op = pe + 24
    table = op + optional_size
    require(machine == 0x8664, 'expected AMD64 machine')
    require(1 <= count <= 96, 'section count outside PE bounds')
    require(optional_size >= 112 and table + count * 40 <= size,
            'truncated optional header or section table')
    magic, = read('H', op)
    subsystem, flags = read('HH', op + 68)
    require(magic == 0x20b and subsystem == 1, 'expected native PE32+ image')
    alignment, file_alignment = read('II', op + 32)
    image_size, headers_size = read('II', op + 56)
    directories, = read('I', op + 108)
    require(13 <= directories <= (optional_size - 112) // 8,
            'IAT directory missing or directory table truncated')
    require(power_of_two(alignment) and alignment >= PAGE,
            'SectionAlignment must be a power of two and at least 4096')
    require(power_of_two(file_alignment) and 512 <= file_alignment <= 65536
            and file_alignment <= alignment, 'invalid FileAlignment')
    require(table + count * 40 <= headers_size <= size
            and headers_size % file_alignment == 0, 'invalid SizeOfHeaders')
    require(headers_size <= image_size and image_size % alignment == 0,
            'invalid SizeOfImage')
    require(flags & 0x100, 'NX_COMPAT is absent')
    sections = []
    raw_spans = []
    mapped_spans = []
    header_end = (headers_size + alignment - 1) // alignment * alignment
    for index in range(count):
        at = table + index * 40
        virtual_size, rva, raw_size, raw_offset = read('IIII', at + 8)
        characteristics, = read('I', at + 36)
        name = raw[at:at + 8].rstrip(b'\0').decode('ascii', errors='replace')
        label = f'section {index} ({name})'
        require(rva % alignment == 0 and rva >= header_end,
                label + ': RVA is unaligned or overlaps headers')
        extent = max(virtual_size, raw_size)
        require(extent > 0 and rva + extent <= image_size,
                label + ': empty or outside SizeOfImage')
        mapped_end = (rva + extent + alignment - 1) // alignment * alignment
        require(mapped_end <= image_size, label + ': mapped extent exceeds image')
        require(not (characteristics & EXECUTE and characteristics & WRITE),
                label + ': writable executable section')
        if raw_size:
            require(raw_offset >= headers_size and raw_offset % file_alignment == 0
                    and raw_size % file_alignment == 0 and raw_offset + raw_size <= size,
                    label + ': invalid raw extent')
            raw_spans.append((raw_offset, raw_offset + raw_size))
        mapped_spans.append((rva, mapped_end))
        sections.append(dict(name=name, rva=rva, virtual_size=virtual_size,
                             raw_size=raw_size, raw_offset=raw_offset,
                             executable=bool(characteristics & EXECUTE),
                             writable=bool(characteristics & WRITE)))
    for label, spans in [('raw', raw_spans), ('mapped', mapped_spans)]:
        ordered = sorted(spans)
        require(all(left[1] <= right[0] for left, right in zip(ordered, ordered[1:])),
                label + ' section extents overlap')
    iat, iat_size = read('II', op + 112 + 12 * 8)
    require(iat > 0 and iat_size > 0 and iat % 8 == 0 and iat_size % 8 == 0,
            'IAT must be present and pointer-aligned')
    owners = [s for s in sections if s['rva'] <= iat
              and iat + iat_size <= s['rva'] + s['raw_size']]
    require(len(owners) == 1 and not owners[0]['executable'],
            'IAT must be wholly file-backed in one non-executable section')
    return dict(sections=sections, section_alignment=alignment,
                iat_rva=iat, iat_bytes=iat_size)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('--out', type=Path, help='JSON receipt path')
    args = parser.parse_args(argv)
    result = dict(scope='static AMD64 KMD PE memory layout only',
                  hvci_acceptance=False, image=str(args.image), status='fail')
    try:
        with args.image.open('rb') as stream:
            raw = stream.read(MAX_BYTES + 1)
        require(len(raw) <= MAX_BYTES, 'image exceeds project size bound')
        result['sha256'] = hashlib.sha256(raw).hexdigest()
        result.update(inspect(raw))
        result['status'] = 'pass'
    except (OSError, ValueError) as exc:
        result['error'] = str(exc)
    rendered = json.dumps(result, indent=2) + '\n'
    if args.out:
        try:
            args.out.write_text(rendered, encoding='utf-8')
        except OSError as exc:
            print(f'KMD PE gate: cannot write receipt: {exc}')
            return 1
    print(rendered, end='')
    return 0 if result['status'] == 'pass' else 1


if __name__ == '__main__':
    raise SystemExit(main())
