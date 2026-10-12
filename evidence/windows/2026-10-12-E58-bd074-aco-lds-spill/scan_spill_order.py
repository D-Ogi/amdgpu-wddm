"""Scan the ACO 'After RA' program of a RADV_DEBUG=shaders dump for VGPR spill
reloads that sit above a spill store to the same slot in the same block.

Usage:
  python -I scan_spill_order.py <dump> <first_line_of_after_RA> <first_line_of_final> [lds|scratch]

lds      checks ds_read_addtid_b32 / ds_write_addtid_b32 (LDS spill slots, keyed by offset0)
scratch  checks scratch_load_dword / scratch_store_dword with storage:vgpr_spill (keyed by offset)

For each reload R of slot S, it finds the next spill store W to slot S in the same block.
A pair is "strict" when the value that W stores is defined in the block after R and the
first use of R comes after W. The spiller places a reload directly before its use, so a
strict pair means R was moved above the store that fills its slot.

BD-074 dumps (2026-10-12):
  bd074acow64-t28-stderr.txt 25032 63260 lds      -> 421 pairs, 189 used after W, 167 strict (62 blocks)
  bd074acow64-t28-stderr.txt 25032 63260 scratch  -> 178 pairs,   2 used after W,   0 strict
  bd074acodef-t28-stderr.txt 25032 62067 scratch  -> 509 pairs,   3 used after W,   0 strict
"""
import re
import sys

path, start, end = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
mode = sys.argv[4] if len(sys.argv) > 4 else "lds"
lines = open(path, encoding="utf-8", errors="replace").read().splitlines()[start - 1:end - 1]

blk_re = re.compile(r"^BB(\d+)$")
if mode == "lds":
    rd_re = re.compile(r"%(\d+):v\[\d+\] = ds_read_addtid_b32 %\d+:m0(?: offset0:(\d+))?")
    wr_re = re.compile(r"ds_write_addtid_b32 %\d+:m0, %(\d+):v\[\d+\](?: offset0:(\d+))?")
else:
    rd_re = re.compile(r"%(\d+):v\[\d+\] = scratch_load_dword  v1: undef, %\d+:\S+ offset:(-?\d+) "
                       r"storage:vgpr_spill")
    wr_re = re.compile(r"scratch_store_dword  v1: undef, %\d+:\S+, %(\d+):v\[\d+\] offset:(-?\d+) "
                       r"storage:vgpr_spill")

blocks = []
cur = None
for ln in lines:
    m = blk_re.match(ln.strip())
    if m:
        cur = {"name": "BB" + m.group(1), "ins": []}
        blocks.append(cur)
        continue
    if cur is not None and ln.startswith("\t"):
        cur["ins"].append(ln.strip())


def defs_uses(ins):
    if " = " in ins:
        lhs, rhs = ins.split(" = ", 1)
        return set(re.findall(r"%(\d+):", lhs)), set(re.findall(r"%(\d+):", rhs))
    return set(), set(re.findall(r"%(\d+):", ins))


total = used_after = strict = 0
strict_blocks = set()
for b in blocks:
    ins = b["ins"]
    for p, a in enumerate(ins):
        m = rd_re.search(a)
        if not m:
            continue
        rtemp, off = m.group(1), int(m.group(2) or 0)
        for q in range(p + 1, len(ins)):
            w = wr_re.search(ins[q])
            if not w or int(w.group(2) or 0) != off:
                continue
            total += 1
            first_use = next((k for k in range(p + 1, len(ins)) if rtemp in defs_uses(ins[k])[1]), None)
            stored = w.group(1)
            def_pos = next((k for k in range(0, q) if stored in defs_uses(ins[k])[0]), None)
            late = first_use is not None and first_use > q
            is_strict = late and def_pos is not None and def_pos > p
            used_after += late
            strict += is_strict
            if is_strict:
                strict_blocks.add(b["name"])
            print(f"{b['name']}: read %{rtemp} slot@{off} at {p}, write %{stored} slot@{off} at {q}, "
                  f"stored value defined at {def_pos}, first use of read at {first_use}"
                  f"{'  <-- STRICT' if is_strict else ('  <-- used after write' if late else '')}")
            break

print(f"{mode}: pairs {total}; read used only after the write {used_after}; "
      f"strict {strict} in {len(strict_blocks)} blocks")
