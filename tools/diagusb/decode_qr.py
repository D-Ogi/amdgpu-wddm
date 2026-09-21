#!/usr/bin/env python3
"""Turn the QR codes scanned off the BC-250 screen back into a readable report.

    python tools/diagusb/decode_qr.py scan.txt
    python tools/diagusb/decode_qr.py            # then paste the chunks, end with Ctrl-Z / Ctrl-D

The input may hold the chunks in any order, repeated, and surrounded by anything else (mail
headers, messenger quoting, the text a scanner app adds): only "BC250:1:..." runs are read. If a
chunk is missing it is named, so it can be rescanned.

Register values travel positionally, without names, to keep the codes small. The names come from
probes.json, which therefore has to be the file the stick was built from: its id is stored in
every run and the two must match, or the values would be printed under the wrong names.
"""

import argparse
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
PAYLOAD = HERE / "payload" / "bc250"
sys.path.insert(0, str(PAYLOAD))
import qrcodec  # noqa: E402

COLS = "{:<38} {:>7} {:>9} {:>9} {:>9}"


def read_input(paths):
    """Concatenated text of the given files; nothing (or '-') means standard input."""
    if not paths:
        return sys.stdin.read()
    return "\n".join(sys.stdin.read() if p == "-" else Path(p).read_text(errors="replace")
                     for p in paths)


def chunk_report(text):
    """(message lines, complete run's crc) for what the input contains. crc is None if unusable."""
    runs = qrcodec.find_chunks(text)
    if not runs:
        return ["No BC250 chunk found in the input.",
                "Every code starts with 'BC250:1:'; paste the whole text of each scan."], None
    lines = []
    if len(runs) > 1:
        lines.append(f"{len(runs)} different runs in the input (a run is identified by its CRC).")
    (crc, count), parts = max(runs.items(), key=lambda kv: len(kv[1]))
    missing = [i for i in range(1, count + 1) if i not in parts]
    lines.append(f"Run {crc}: {len(parts)} of {count} chunks present.")
    if missing:
        lines.append(f"MISSING chunk(s): {', '.join(str(i) for i in missing)} of {count}.")
        lines.append("Show the codes again on the BC-250 (any key advances) and rescan those.")
        return lines, None
    return lines, crc


def load_probes(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def check_probes(summary, probes, path):
    """The one thing that must not be guessed: names against positional values."""
    if summary.get("pid") != probes.get("id"):
        raise SystemExit(
            f"This run was made with probes id {summary.get('pid')}, but {path} has id "
            f"{probes.get('id')}.\nThe register values are positional, so the names in that file do "
            f"not describe them.\nUse the probes.json the stick was built from (--probes PATH).")


def reg_rows(summary, probes):
    """One row per probed register: name, offset, phase A raw, phase B raw, phase B debugfs."""
    a, b = summary.get("A", {}), summary.get("B", {})
    for i, r in enumerate(probes["regs"]):
        pick = lambda values: values[i] if i < len(values) else ""
        yield (f"{r['n']} [{r['ip']}]", f"{r['off']:05X}", pick(a.get("raw", [])),
               pick(b.get("raw", [])), pick(b.get("dbg", [])))


def banked_rows(summary, probes):
    """Per-SE/SH values, which only exist for the registers marked banked in probes.json."""
    banked = [r for r in probes["regs"] if r["banked"]]
    for src, label in ((summary.get("A", {}).get("bank", {}), "A"),
                       (summary.get("B", {}).get("dbgbank", {}), "B")):
        for bank, values in sorted(src.items()):
            for i, r in enumerate(banked):
                if i < len(values):
                    yield f"{label} SE.SA {bank}", r["n"], values[i]


def render(summary, probes):
    out = []
    add = out.append
    add("BC-250 DIAGNOSTIC SUMMARY")
    add(f"  summary v{summary.get('v')}  probes {summary.get('pid')}  mode {summary.get('mode')}"
        f"  kernel {summary.get('kver')}")
    if summary.get("test"):
        t = summary["test"]
        add(f"  TEST HOOK RUN: read {t.get('dev')} BAR{t.get('bar')}, not a BC-250 measurement")
    dmi = summary.get("dmi") or {}
    if dmi:
        add(f"  board {dmi.get('board_vendor', '?')} {dmi.get('board_name', '?')}"
            f"  BIOS {dmi.get('bios_version', '?')} {dmi.get('bios_date', '?')}")
    pci = summary.get("pci")
    if pci:
        add(f"  PCI {pci.get('id')} at {pci.get('addr')} rev {pci.get('rev')} class {pci.get('class')}"
            f"  command {pci.get('cmd')}  driver '{pci.get('driver')}'")
        add(f"  BAR sizes: {', '.join(str(x) for x in pci.get('bars', []))}")
    if summary.get("err"):
        add(f"  ERROR: {summary['err']}")

    add("")
    add("VERDICT")
    for line in summary.get("verdict", []):
        add(f"  {line}")

    a, b = summary.get("A"), summary.get("B")
    if a or b:
        add("")
        add("REGISTERS (A = raw BAR5 before the driver, B raw = same after modprobe, B dbg = via amdgpu)")
        add("  " + COLS.format("register [IP]", "offset", "A raw", "B raw", "B dbg"))
        for row in reg_rows(summary, probes):
            add("  " + COLS.format(*row))
    if a and a.get("scr"):
        add(f"  SCRATCH_REG0 write test: saved {a['scr'][0]}, read back {', '.join(a['scr'][1:])}")

    banks = list(banked_rows(summary, probes))
    if banks:
        add("")
        add("PER SHADER ENGINE / ARRAY")
        for bank, name, value in banks:
            add(f"  {bank:<12} {name:<38} {value}")

    if a and a.get("claims"):
        add("")
        add("OFFSETS THE PREVIOUS DRIVER ATTEMPT USED (what is really at those addresses)")
        for claim, value in zip(probes.get("claims", []), a["claims"]):
            add(f"  {claim['off']:05X}  {value}  claimed to be {claim['as']}")

    if b:
        add("")
        add("PHASE B - the kernel's own view")
        add(f"  modprobe amdgpu: {b.get('load')} after {b.get('secs')}s")
        d = b.get("dmesg") or {}
        add(f"  dmesg: initialized={d.get('initialized')} rings={d.get('rings')} lines={d.get('lines')}")
        for key in ("vram", "gtt"):
            if d.get(key):
                add(f"    {d[key]}")
        for err in d.get("errors", []):
            add(f"    ERROR {err}")
        if b.get("vram_mb") is not None:
            add(f"  memory: VRAM {b.get('vram_mb')} MB, GTT {b.get('gtt_mb')} MB")
        for name, ver in sorted((b.get("fw") or {}).items()):
            add(f"  firmware {name:<28} {ver}")
        for name, insts in sorted((b.get("ipd") or {}).items()):
            add(f"  IP {name:<10} {'; '.join(insts)}")
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="*", help="files with the scanned text ('-' or nothing: stdin)")
    ap.add_argument("--probes", default=str(PAYLOAD / "probes.json"),
                    help="probes.json the stick was built from (default: the one in this repo)")
    ap.add_argument("--json", action="store_true", help="print the raw summary as JSON instead")
    args = ap.parse_args(argv)

    text = read_input(args.files)
    notes, crc = chunk_report(text)
    for line in notes:
        print(line, file=sys.stderr)
    if crc is None:
        return 2
    try:
        summary = qrcodec.decode(text)
    except ValueError as e:
        print(f"Cannot decode: {e}", file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps(summary, indent=1))
        return 0
    probes = load_probes(args.probes)
    check_probes(summary, probes, args.probes)
    print("\n".join(render(summary, probes)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
