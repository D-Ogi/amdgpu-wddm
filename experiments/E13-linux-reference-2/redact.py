#!/usr/bin/env python3
"""Copy a capture directory of session.sh into the evidence tree, redacted.

    python redact.py <source directory> <target directory>

Replaced in every text file: MAC addresses, USB serial numbers (not the PCI addresses root hubs carry there), DMI
serials and UUIDs, LAN IPv4 addresses, and the EDID blocks of the display log. Left out entirely: the network log of
the stick (it may name a wireless network). Prints counts only, never what was replaced.
"""

import re
import sys
from pathlib import Path

SKIP = {"bc250-net.log"}
RULES = [
    ("mac", re.compile(r"\b(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}\b"), "<mac>"),
    ("usb serial", re.compile(r"(SerialNumber: )(?![0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-9a-f]\b)\S+"), r"\1<serial>"),
    ("dmi serial", re.compile(r"((?:serial|Serial Number|UUID)\s*[:=]\s*)[0-9A-Za-z-]{6,}"), r"\1<redacted>"),
    ("uuid", re.compile(r"\b[0-9a-fA-F]{8}-(?:[0-9a-fA-F]{4}-){3}[0-9a-fA-F]{12}\b"), "<uuid>"),
    ("ipv4", re.compile(r"\b(?:192\.168|10\.\d{1,3}|172\.(?:1[6-9]|2\d|3[01]))\.\d{1,3}\.\d{1,3}\b"), "<lan-address>"),
    ("nul", re.compile(r"\x00+"), ""),      # sysfs pads pp_od_clk_voltage with NUL bytes; git would call the file binary
    ("edid", re.compile(r"(?im)^(.*edid.*)$\n(?:^[0-9a-f \t]{32,}$\n?)+"), r"\1\n<edid block removed>\n"),
]


def main():
    source, target = Path(sys.argv[1]), Path(sys.argv[2])
    counts = {name: 0 for name, _, _ in RULES}
    files = skipped = 0
    for path in sorted(p for p in source.rglob("*") if p.is_file()):
        if path.name in SKIP:
            skipped += 1
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for name, pattern, replacement in RULES:
            text, n = pattern.subn(replacement, text)
            counts[name] += n
        out = target / path.relative_to(source)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(text, encoding="utf-8", newline="\n")
        files += 1
    print(f"{files} files written, {skipped} left out; replaced: " + ", ".join(f"{k} {v}" for k, v in counts.items()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
