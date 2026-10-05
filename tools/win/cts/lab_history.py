"""Collect per-case TestDuration of earlier unit A sparse CTS runs (vulkan-cts-1.4.6.2, 2026-09-25,
kernel driver 149-151, Mesa05 candidates, fixed 1000 MHz) from the pulled copies of those runs.

    python lab_history.py OUT.TSV [SRC_DIR]

SRC_DIR holds the pulled run directories cts-*-collected, by default <BC250_ROOT>/scratch/m12.
Only dEQP-VK.sparse_resources.* cases; one row per case (the last observation wins).
Used only to calibrate the lab/host time ratio of the batch plan; the drivers differ from today's.
"""

import glob
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qpa  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
# BC250_ROOT is the workspace root; by default the parent directory of this repository.
ROOT = os.environ.get("BC250_ROOT", os.path.normpath(os.path.join(HERE, "..", "..", "..", "..")))


def main(out, src=None):
    src = src or os.path.join(ROOT, "scratch", "m12")
    rows = {}
    for d in sorted(glob.glob(os.path.join(src, "cts-*-collected"))):
        for f in sorted(glob.glob(os.path.join(d, "*.qpa"))):
            for rec in qpa.iter_cases(f):
                if not rec["case"].startswith("dEQP-VK.sparse_resources."):
                    continue
                if rec["duration_us"] is None or not rec["complete"]:
                    continue
                rows[rec["case"]] = (rec["status"], rec["duration_us"], os.path.basename(d))
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("case\tstatus\tduration_us\tsource\n")
        for c in sorted(rows):
            s, u, src = rows[c]
            f.write(f"{c}\t{s}\t{u:.0f}\t{src}\n")
    print(f"{len(rows)} cases -> {out}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
