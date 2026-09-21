#!/usr/bin/env python3
"""Host-side checks for E12.

  compare.py rerun <first gfx run log> <second gfx run log>
      The register writes of a second bring-up in the same boot against the first one's ("W <offset> <value>" lines of
      bc250kmd_cli gfx), as an edit script: what the second run wrote in addition, what it left out. TLB flush writes
      are compared by count only. The first run is compared with amdgpu's trace by E11's compare.py; this one says how
      far a bring-up over a GPU that was already brought up and torn down differs from a cold one.

Logs from the target are UTF-16 (Windows PowerShell); both encodings are accepted.
"""

import argparse
import difflib
import importlib.util
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location("e11_compare", ROOT / "experiments/E11-gfx-bringup/compare.py")
_e11 = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_e11)


def writes(log, names):
    every = [(int(m.group(1), 16), int(m.group(2), 16)) for m in map(_e11.WRITE.match, _e11.text(log).splitlines()) if m]
    return [w for w in every if names.get(w[0]) not in _e11.FLUSH], [w for w in every if names.get(w[0]) in _e11.FLUSH]


def cmd_rerun(args):
    names = _e11.register_names()
    first, first_flush = writes(args.first, names)
    second, second_flush = writes(args.second, names)
    print(f"first run: {len(first)} writes and {len(first_flush)} TLB flush writes   second run: {len(second)} and {len(second_flush)}")
    added = removed = 0
    for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(a=first, b=second, autojunk=False).get_opcodes():
        if tag == "equal":
            continue
        for o, v in first[i1:i2]:
            removed += 1
            print(f"  only first  [{i1:4}] 0x{o:05X} {v:08X} {names.get(o, '?')}")
        for o, v in second[j1:j2]:
            added += 1
            print(f"  only second [{j1:4}] 0x{o:05X} {v:08X} {names.get(o, '?')}")
    print(f"only in the first run {removed}, only in the second run {added}")
    return 0


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("rerun")
    r.add_argument("first")
    r.add_argument("second")
    args = ap.parse_args()
    return {"rerun": cmd_rerun}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
