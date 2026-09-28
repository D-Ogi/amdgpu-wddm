#!/usr/bin/env python3
"""Encode a measured exact-pair caps.json as the UMD's M14C deployment record.

This does not measure capabilities or authorize deployment. The input must be
the accepted output of an exact-pair capability probe. CreateDevice independently
checks the engine's claims, and verifies the two module hashes before loading.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct


def encode(caps, engine: Path, icd: Path) -> bytes:
    def integer(name, allowed):
        value = caps[name]
        # JSON bool and float are not the recorded UINT32 representation.
        if type(value) is not int or value not in allowed:
            raise ValueError(f"invalid {name}: {value!r}")
        return value

    maximum = integer("maximum_feature_level", (0xA000, 0xA100, 0xB000, 0xB100))
    doubles = integer("doubles", (0, 1))
    compute = integer("compute_raw_structured", (0, 1))
    logic = integer("logic_op", (0, 1))
    tile = integer("tile_based", (0, 1))
    pixel = integer("pixel_min_precision", range(4))
    other = integer("other_min_precision", range(4))
    if maximum >= 0xB000 and not compute:
        raise ValueError("FL11 requires compute/raw/structured support")
    if maximum == 0xB100 and not logic:
        raise ValueError("FL11_1 requires logic operations")

    hashes = []
    for name, path in (("engine_sha256", engine), ("icd_sha256", icd)):
        expected = caps[name]
        if not isinstance(expected, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", expected):
            raise ValueError(f"invalid {name}")
        expected = bytes.fromhex(expected)
        if not any(expected):
            raise ValueError(f"zero {name}")
        with path.open("rb") as stream:
            actual = hashlib.file_digest(stream, "sha256").digest()
        if actual != expected:
            raise ValueError(f"{name} does not match {path}")
        hashes.append(actual)
    return struct.pack("<11I32s32s", 0x4334314D, 1, 108, 0, maximum,
                       doubles, compute, logic, tile, pixel, other, *hashes)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--caps", type=Path, required=True)
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--icd", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    try:
        caps = json.loads(args.caps.read_text(encoding="utf-8-sig"))
        if not isinstance(caps, dict):
            raise ValueError("capability record must be an object")
        data = encode(caps, args.engine, args.icd)
        # Validate everything first. Never overwrite a previous package record.
        with args.out.open("xb") as stream:
            stream.write(data)
    except (OSError, ValueError, KeyError) as exc:
        parser.exit(1, f"configuration refused: {exc}\n")
    print(f"{args.out}: {len(data)} bytes, SHA256 {hashlib.sha256(data).hexdigest().upper()}")


if __name__ == "__main__":
    main()
