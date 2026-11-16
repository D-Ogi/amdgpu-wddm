"""Offline SPI inspection and in-memory patch planning. Never writes firmware."""
import argparse
import hashlib
import importlib.metadata
import json
from pathlib import Path
import platform


def digest(data):
    return hashlib.sha256(data).hexdigest()


def diff_ranges(before, after):
    if len(before) != len(after):
        raise ValueError("image size changed")
    ranges = []
    start = None
    for i, (a, b) in enumerate(zip(before, after)):
        if a != b and start is None:
            start = i
        if a == b and start is not None:
            ranges.append([start, i])
            start = None
    if start is not None:
        ranges.append([start, len(before)])
    return ranges


def patch(image, entry, edits, expected_sha256):
    """Apply equal-size expected-byte edits to one authenticated body in RAM."""
    if digest(image) != expected_sha256:
        raise ValueError("stock identity mismatch")
    lo, hi = entry["body_range"]
    if not 0 <= lo < hi <= len(image):
        raise ValueError("invalid body range")
    result = bytearray(image)
    used = set()
    for edit in edits:
        pos = edit["offset"]
        old, new = bytes.fromhex(edit["before"]), bytes.fromhex(edit["after"])
        if not old or len(old) != len(new) or old == new:
            raise ValueError("edit must change equal nonzero lengths")
        if not lo <= pos < pos + len(old) <= hi:
            raise ValueError("edit outside authenticated body")
        indices = set(range(pos, pos + len(old)))
        if used & indices:
            raise ValueError("overlapping edits")
        if image[pos:pos + len(old)] != old:
            raise ValueError("expected bytes mismatch")
        used |= indices
        result[pos:pos + len(old)] = new
    return bytes(result)


def rebuild(image, spans):
    """Test byte-level span arithmetic, not PSPTool extraction."""
    cursor, pieces = 0, []
    for start, end in sorted(spans):
        if not cursor <= start < end <= len(image):
            raise ValueError("overlapping or invalid extraction ranges")
        pieces.extend((bytes(image[cursor:start]), bytes(image[start:end])))
        cursor = end
    pieces.append(bytes(image[cursor:]))
    return b"".join(pieces)


def rebuild_parsed(image, files):
    """Rebuild selected plaintext entries from parser components, retaining opaque gaps.

    This is the supported same-size SMU rebuild path. It does not rebuild other
    firmware containers or establish their integrity rules. No bytes are saved.
    """
    cursor, pieces, records = 0, [], []
    if not files:
        raise ValueError('no parsed entries')
    for f in sorted(files, key=lambda f: f.get_address()):
        start, end = f.get_address(), f.get_address() + len(f)
        if f.encrypted or f.compressed:
            raise ValueError('encoded entry cannot use plaintext rebuild')
        if not cursor <= start < end <= len(image):
            raise ValueError('overlapping or invalid parsed entry range')
        parts, part_cursor, component_records = [], start, []
        for name in ('header', 'body', 'signature'):
            component = getattr(f, name)
            data = component.get_bytes()
            if component.get_address() != part_cursor or not data or len(data) != len(component):
                raise ValueError('non-contiguous or invalid parser component')
            parts.append(data)
            component_records.append({'name': name, 'offset': part_cursor,
                                      'bytes': len(data), 'sha256': digest(data)})
            part_cursor += len(data)
        if part_cursor != end:
            raise ValueError('parser components do not cover the entry')
        extracted = b''.join(parts)
        if extracted != f.get_bytes() or extracted != image[start:end]:
            raise ValueError('parsed extraction differs from source entry')
        pieces.extend((bytes(image[cursor:start]), extracted))
        records.append({'type': f.type, 'range': [start, end], 'components': component_records,
                        'sha256': digest(extracted)})
        cursor = end
    pieces.append(bytes(image[cursor:]))
    return b''.join(pieces), records


def directory_coverage(directory, image_size):
    """Compare raw directory records with parsed files, without interpreting gaps.

    Offsets use PSPTool's normalization. A complete extent is not proof of
    complete format interpretation, authentication, or directory discovery.
    """
    records = []
    for entry in directory.entries:
        rec = {'type': entry.type, 'raw_location': entry.offset,
               'declared_size': entry.size,
               'record_offset': directory.get_address() + directory.HEADER_SIZE + entry.entry_offset,
               'normalized_offset': None, 'declared_range': None,
               'parsed_range': None, 'unparsed_ranges': [],
               'coverage_status': 'unsupported', 'reason': None}
        records.append(rec)
        try:
            start = entry.file_offset()
        except (ValueError, AssertionError, NotImplementedError) as error:
            rec['reason'] = 'offset-normalization-failed'
            rec['normalization_error'] = type(error).__name__
            continue
        rec['normalized_offset'] = start
        matches = [f for f in directory.files if f.type == entry.type and f.get_address() == start]
        if len(matches) == 1:
            rec['parsed_range'] = [start, start + len(matches[0])]
        # BIOS records can describe memory destinations or encoded storage. Do
        # not apply PSP firmware extent rules to them, even when a file exists.
        if directory.magic not in (b'$PSP', b'$PL2'):
            rec['reason'] = 'directory-semantics-not-assessed'
            continue
        if entry.type == 0x0b:
            rec['coverage_status'], rec['reason'] = 'special', 'inline-soft-fuse-record'
            continue
        if entry.type in (0x40, 0x48, 0x49, 0x4a, 0x70):
            rec['coverage_status'], rec['reason'] = 'special', 'directory-reference'
            continue
        mode = directory.address_mode
        if mode in (2, 3):
            mode = entry.address_mode
        elif mode == 1 and entry.address_mode == 0 and entry.offset >= 0xff000000:
            mode = 0
        rec['effective_address_mode'] = mode
        # PSPTool itself marks slot-relative normalization as unresolved.
        if mode == 3:
            rec['reason'] = 'slot-relative-offset-not-established'
            continue
        end = start + entry.size
        rec['declared_range'] = [start, end]
        if not 0 <= start < end <= image_size:
            rec['reason'] = 'declared-range-outside-image-or-empty'
            continue
        if len(matches) > 1:
            rec['reason'] = 'multiple-parsed-files'
        elif not matches:
            rec['coverage_status'], rec['reason'] = 'missing', 'no-parsed-file'
            rec['unparsed_ranges'] = [[start, end]]
        elif rec['parsed_range'] == rec['declared_range']:
            rec['coverage_status'], rec['reason'] = 'complete', 'extent-match-only'
        elif start < rec['parsed_range'][1] < end:
            rec['coverage_status'], rec['reason'] = 'partial', 'parsed-file-shorter-than-record'
            rec['unparsed_ranges'] = [[rec['parsed_range'][1], end]]
        else:
            rec['reason'] = 'parsed-range-outside-record-or-empty'
    return records


def inspect(path):
    import psptool
    from psptool import PSPTool
    from psptool.header_file import HeaderFile
    image = path if isinstance(path, bytes) else Path(path).read_bytes()
    pt = PSPTool(bytearray(image))
    package = Path(psptool.__file__).parent
    sources = {p.name: digest(p.read_bytes()) for p in sorted(package.glob('*.py'))}
    report = {"image": None if isinstance(path, bytes) else str(Path(path).resolve()), "bytes": len(image),
              "sha256": digest(image), "python": platform.python_version(),
              "psptool": importlib.metadata.version('psptool'),
              "cryptography": importlib.metadata.version('cryptography'),
              "parser_sources": sources, "tool_sha256": digest(Path(__file__).read_bytes()),
              "directories": [], "smu": [], "limitations": [
                  "Directory coverage is PSPTool discovery, not exhaustive container validation.",
                  "Raw entry coverage compares extents only; BIOS and slot-relative semantics are not assessed.",
                  "Key verification does not establish a silicon trust anchor or boot acceptance.",
                  "Entry selection, ABI, free-space ownership and power semantics remain unknown.",
                  "Negative mutations are verifier tests, not inert S2 candidates."]}
    for offset, directory in sorted(pt.directories_by_offset.items()):
        report['directories'].append({"offset": offset,
            "raw_entries": directory_coverage(directory, len(image)), "entries": [
            {"type": f.type, "offset": f.get_address(), "bytes": len(f),
             "sha256": digest(f.get_bytes())} for f in directory.files]})
    files = sorted((f for f in pt.files_by_offset.values() if f.type in (8, 18)),
                   key=lambda f: f.get_address())
    if len(files) != 2 or {f.type for f in files} != {8, 18}:
        raise ValueError('expected exactly one SMU entry of each type')
    for f in files:
        if not isinstance(f, HeaderFile) or f.encrypted or f.compressed or not f.is_signed:
            raise ValueError('unsupported SMU encoding')
        start, end = f.get_address(), f.get_address() + len(f)
        if not 0 <= start < end <= len(image) or image[start:end] != f.get_bytes():
            raise ValueError('entry outside image or extraction mismatch')
        signed = f.get_signed_bytes()
        if signed != image[start:start + len(signed)] or len(signed) <= len(f.header):
            raise ValueError('authenticated range mismatch')
        entity = f.signed_entity
        keys = sorted(entity.certifying_keys, key=lambda k: k.get_address()) if entity else []
        accepted = [k for k in keys if entity.is_verified_by(k)]
        if not accepted:
            raise ValueError('stock signature positive control failed')
        body_start, body_end = start + len(f.header), start + len(signed)
        if body_end > end - f.signature_len:
            raise ValueError('authenticated body overlaps signature')
        rec = {"type": f.type, "range": [start, end],
               "body_range": [body_start, body_end], "header_sha256": digest(f.header.get_bytes()),
               "entry_sha256": digest(f.get_bytes()), "body_sha256": digest(f.body.get_bytes()),
               "signed_sha256": digest(signed), "signature_sha256": digest(f.signature.get_bytes()),
               "signature_range": [end - f.signature_len, end], "load_address": f.load_addr,
               "version": f.get_readable_version(), "encrypted": f.encrypted, "compressed": f.compressed,
               "key_id": f.signature_fingerprint.decode(), "stock_signature_valid": True,
               "keys": [{"offset": k.get_address(), "sha256": digest(k._crypto_material.get_bytes())} for k in accepted]}
        # Mutate the signed header and three body bytes independently in RAM.
        # These verifier controls do not use the body-only candidate patch API.
        rec['negative_controls'] = []
        for pos in (body_start - 1, body_start, (body_start + body_end) // 2, body_end - 1):
            changed = bytearray(image)
            changed[pos] ^= 1
            changed = bytes(changed)
            altered = changed[start:start+len(signed)]
            rejected = all(not k.get_public_key().verify_blob(altered, f.signature.get_bytes()) for k in accepted)
            if not rejected or digest(altered) == digest(signed):
                raise ValueError('negative signature control failed')
            rec['negative_controls'].append({"offset": pos, "rejected": rejected,
                "image_sha256": digest(changed), "diff_ranges": diff_ranges(image, changed)})
        report['smu'].append(rec)
    restored, components = rebuild_parsed(image, files)
    if restored != image:
        raise ValueError('parsed rebuild differs from source image')
    reparsed = PSPTool(bytearray(restored))
    for original in files:
        copied = reparsed.files_by_offset.get(original.get_address())
        if copied is None or copied.type != original.type or copied.get_bytes() != original.get_bytes():
            raise ValueError('rebuilt entry failed parser round trip')
    report['s1'] = {"scope": "selected plaintext SMU entries; other image bytes retained unchanged",
                    "method": "parser header/body/signature extraction, reassembly, full equality and reparse",
                    "equal": True, "sha256": digest(restored), "components": components,
                    "reparse_equal": True,
                    "all_firmware_containers_rebuilt": False}
    report['smu_bodies_equal'] = files[0].body.get_bytes() == files[1].body.get_bytes()
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image')
    parser.add_argument('--report', required=True, type=Path)
    args = parser.parse_args()
    if args.report.suffix != '.json' or args.report.exists():
        parser.error('report must be a new .json file')
    result = inspect(args.image)
    with args.report.open('x', encoding='utf-8') as stream:
        json.dump(result, stream, indent=2)
        stream.write('\n')


if __name__ == '__main__':
    main()

