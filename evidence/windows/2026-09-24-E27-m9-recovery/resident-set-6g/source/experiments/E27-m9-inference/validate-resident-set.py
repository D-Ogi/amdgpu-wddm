"""Validate complete-set residency evidence against an independently specified size.
Usage: python validate-resident-set.py path/to/name.out expected-bytes
Requires the matching name.exit file. No allocation-size success-only acceptance.
"""
import json
import re
import sys
from pathlib import Path


def validate(path, expected):
    text = path.read_text(encoding="utf-8-sig")
    assert path.with_suffix(".exit").read_text(encoding="utf-8-sig").strip() == "0"
    assert 0 < expected <= 12 * (1 << 30) and expected % 4096 == 0
    sizes = [min(1 << 30, expected - start) for start in range(0, expected, 1 << 30)]
    samples = 1 + sum((n + (1 << 26) - 1) // (1 << 26) + 1 for n in sizes)
    assert f"GPU_RESIDENT_SET_RESULT PASS bytes={expected} samples={samples} snapshots_only=1" in text
    assert "mode=resident-only" in text and "CYCLE " not in text and "GPU_MISMATCH" not in text
    sets = re.findall(r"RESIDENT_SET tag=(\S+) offset=(\d+) bytes=(\d+) allocations=(\d+) gpu=(\d+) shared=(\d+) nonresident=(\d+) sample=(\d+)", text)
    assert len(sets) == samples
    for sample, row in enumerate(sets, 1):
        assert tuple(map(int, row[2:])) == (expected, len(sizes), expected, 0, 0, sample), row
    allocations = [tuple(map(int, row)) for row in re.findall(r"RESIDENT_ALLOCATION sample=(\d+) index=(\d+) bytes=(\d+) status=(\d+)", text)]
    assert allocations == [(sample, i, size, 1) for sample in range(1, samples + 1) for i, size in enumerate(sizes)]
    usage = [int(n) for n in re.findall(r"MEMORY_BUDGET group=local budget=\d+ usage=(\d+)", text)]
    assert len(usage) == samples and all(n >= expected for n in usage)
    reads = [tuple(map(int, row)) for row in re.findall(r"GPU_READBACK bytes=(\d+) all_words_match=1 fence=(\d+) word_base=(\d+)", text)]
    expected_reads = []
    offset = fence = 0
    for size in sizes:
        fence += (size + (1 << 20) - 1) // (1 << 20)
        expected_reads.append((size, fence, offset // 4))
        offset += size
    assert reads == expected_reads
    assert text.index("D3DKMTFreeGpuVirtualAddress [resident-set]") > text.rindex("GPU_READBACK bytes=")
    assert len(re.findall(r"BULK_CREATE name=resident-set bytes=\d+ elapsed_ms=\d+ success=1", text)) == len(sizes)
    assert "Evict" not in text
    return {"bytes": expected, "allocations": len(sizes), "complete_set_samples": samples,
            "individual_status_checks": len(allocations), "readbacks": reads,
            "all_gpu_resident_at_samples": True, "continuous_residency_measured": False}


if __name__ == "__main__":
    print(json.dumps(validate(Path(sys.argv[1]), int(sys.argv[2])), indent=2))
