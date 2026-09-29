"""Acceptance check on the allocation answers of one d3d12caps document.

A zero exit of the dump tool only proves that the JSON was written. This check reads `device.allocations`
and fails when the answers a game sizes its heaps with are wrong: the exact 64 KiB buffer answer, a positive
size that is a multiple of a power-of-two alignment for every ordinary resource, the default alignment
(64 KiB, or 4 MiB for MSAA) when none was requested, the requested alignment when one was, and the
UINT64_MAX sentinel for the deliberately invalid small-alignment description. It exists because the
public-runtime query once returned negative (wrapped) sizes for every resource (trial 090) while all driver
calls succeeded.

Usage: python check-caps.py caps.json [caps.json ...]
Exit 0 when every document passes, 1 when a check fails, 2 when a document cannot be read.
"""
import json
import sys

KIB64 = 65536
MIB4 = 4194304
SENTINEL = 'small_rgba8_256_align4k_too_large'
REQUIRED = ('buffer_64k', 'rgba8_1920x1200_rt', 'rgba8_1920x1200_rt_msaa4', 'small_rgba8_64_align4k', SENTINEL)


def check(doc):
    """Returns the list of failures of one parsed document, empty when it passes."""
    allocations = doc.get('device', {}).get('allocations') if isinstance(doc, dict) else None
    if not isinstance(allocations, dict) or not allocations:
        return ['device.allocations: absent or empty']
    failures = ['%s: absent' % name for name in REQUIRED if name not in allocations]
    for name, entry in sorted(allocations.items()):
        info = entry.get('GetResourceAllocationInfo') if isinstance(entry, dict) else None
        desc = entry.get('desc') if isinstance(entry, dict) else None
        if not isinstance(info, dict) or not isinstance(desc, dict):
            failures.append('%s: no GetResourceAllocationInfo or desc' % name)
            continue
        size, align, requested = info.get('SizeInBytes'), info.get('Alignment'), desc.get('Alignment')
        if name == SENTINEL:
            if size != 'UINT64_MAX':
                failures.append('%s: SizeInBytes %r, expected UINT64_MAX' % (name, size))
            continue
        if type(size) is not int or size <= 0:
            failures.append('%s: SizeInBytes %r is not a positive size' % (name, size))
            continue
        if type(align) is not int or align <= 0 or align & (align - 1):
            failures.append('%s: Alignment %r is not a power of two' % (name, align))
            continue
        if size % align:
            failures.append('%s: SizeInBytes %d is not a multiple of Alignment %d' % (name, size, align))
        if requested:
            expected = requested
        else:
            expected = MIB4 if desc.get('SampleCount', 1) > 1 else KIB64
        if align != expected:
            failures.append('%s: Alignment %d, expected %d' % (name, align, expected))
        total = entry.get('GetCopyableFootprints', {}).get('TotalBytes', 1)
        if type(total) is not int or total <= 0:
            failures.append('%s: GetCopyableFootprints.TotalBytes %r is not a positive size' % (name, total))
    buffer = allocations.get('buffer_64k', {}).get('GetResourceAllocationInfo', {})
    if 'buffer_64k' in allocations and (buffer.get('SizeInBytes'), buffer.get('Alignment')) != (KIB64, KIB64):
        failures.append('buffer_64k: %r / %r, expected exactly 65536 / 65536'
                        % (buffer.get('SizeInBytes'), buffer.get('Alignment')))
    return failures


def main(argv):
    if len(argv) < 2 or any(a.startswith('-') for a in argv[1:]):
        print(__doc__.strip().splitlines()[-2], file=sys.stderr)
        return 2
    status = 0
    for path in argv[1:]:
        try:
            with open(path, encoding='utf-8') as f:
                doc = json.load(f)
        except (OSError, ValueError) as e:
            print('%s: unreadable: %s' % (path, e))
            return 2
        failures = check(doc)
        count = len(doc.get('device', {}).get('allocations') or {}) if isinstance(doc, dict) else 0
        print('%s: %s, %d allocation answers, %d failures' % (path, 'FAIL' if failures else 'PASS', count,
                                                             len(failures)))
        for line in failures:
            print('  ' + line)
        if failures:
            status = 1
    return status


if __name__ == '__main__':
    sys.exit(main(sys.argv))
