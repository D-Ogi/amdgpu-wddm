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
    """Extract disjoint regions and gaps, then serialize them in original order."""
    cursor, pieces = 0, []
    for start, end in sorted(spans):
        if not cursor <= start < end <= len(image):
            raise ValueError("overlapping or invalid extraction ranges")
        pieces.extend((bytes(image[cursor:start]), bytes(image[start:end])))
        cursor = end
    pieces.append(bytes(image[cursor:]))
    return b"".join(pieces)


def inspect(path):
    import psptool
    from psptool import PSPTool
    from psptool.header_file import HeaderFile
    image = Path(path).read_bytes()
    pt = PSPTool(bytearray(image))
    package = Path(psptool.__file__).parent
    sources = {p.name: digest(p.read_bytes()) for p in sorted(package.glob('*.py'))}
    report = {"image": str(Path(path).resolve()), "bytes": len(image),
              "sha256": digest(image), "python": platform.python_version(),
              "psptool": importlib.metadata.version('psptool'),
              "cryptography": importlib.metadata.version('cryptography'),
              "parser_sources": sources, "tool_sha256": digest(Path(__file__).read_bytes()),
              "directories": [], "smu": [], "limitations": [
                  "Directory coverage is PSPTool discovery, not exhaustive container validation.",
                  "Key verification does not establish a silicon trust anchor or boot acceptance.",
                  "Entry selection, ABI, free-space ownership and power semantics remain unknown.",
                  "Negative mutations are verifier tests, not inert S2 candidates."]}
    for offset, directory in sorted(pt.directories_by_offset.items()):
        report['directories'].append({"offset": offset, "entries": [
            {"type": f.type, "offset": f.get_address(), "bytes": len(f),
             "sha256": digest(f.get_bytes())} for f in directory.files]})
    files = sorted((f for f in pt.files_by_offset.values() if f.type in (8, 18)),
                   key=lambda f: f.get_address())
    if len(files) != 2 or {f.type for f in files} != {8, 18}:
        raise ValueError('expected exactly one SMU entry of each type')
    spans = []
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
        # Three independent, in-memory mutations exercise the actual cryptographic verifier.
        rec['negative_controls'] = []
        for pos in (body_start, (body_start + body_end) // 2, body_end - 1):
            edits = [{"offset": pos, "before": image[pos:pos+1].hex(),
                      "after": bytes([image[pos] ^ 1]).hex()}]
            changed = patch(image, rec, edits, digest(image))
            altered = changed[start:start+len(signed)]
            rejected = all(not k.get_public_key().verify_blob(altered, f.signature.get_bytes()) for k in accepted)
            if not rejected or digest(altered) == digest(signed):
                raise ValueError('negative signature control failed')
            rec['negative_controls'].append({"offset": pos, "rejected": rejected,
                "image_sha256": digest(changed), "diff_ranges": diff_ranges(image, changed)})
        report['smu'].append(rec)
        spans.append((start, end))
    restored = rebuild(image, spans)
    report['s1'] = {"method": "extract both SMU entries and opaque gaps, serialize unchanged",
                    "equal": restored == image, "sha256": digest(restored)}
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

