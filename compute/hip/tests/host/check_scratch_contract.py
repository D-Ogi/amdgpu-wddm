"""Compare production GFX10 scratch descriptor fields with the pinned Mesa fixture."""
import ast
import json
import re
from pathlib import Path


def integer_expression(text):
    text = re.sub(r"(?<=\d)[uUlL]+", "", text)
    tree = ast.parse(text, mode="eval")
    def visit(node):
        if isinstance(node, ast.Constant) and isinstance(node.value, int):
            return node.value
        if isinstance(node, ast.BinOp) and isinstance(node.op, ast.LShift):
            return visit(node.left) << visit(node.right)
        raise ValueError("not a constant integer/shift")
    return visit(tree.body)


def main():
    hip = Path(__file__).resolve().parents[2]
    reference = json.loads((hip / "tests/data/gfx10-scratch-reference.json").read_text())
    macros = {}
    for line in (hip / "bc250hsa/pm4_regs.h").read_text().splitlines():
        match = re.match(r"#define (BC250HSA_SCRATCH_\w+) (.+)", line)
        if match:
            macros[match[1]] = integer_expression(match[2])
    fields = {}
    for name, data in reference["register_types"].items():
        for item in data["fields"]:
            fields[(name, item["name"])] = item["bits"]
    checks = [
        ("SWIZZLE_ENABLE", "SQ_BUF_RSRC_WORD1", "SWIZZLE_ENABLE", 1, False),
        ("FORMAT_SHIFT", "SQ_BUF_RSRC_WORD3", "FORMAT", 0, True),
        ("INDEX_STRIDE_SHIFT", "SQ_BUF_RSRC_WORD3", "INDEX_STRIDE", 0, True),
        ("ADD_TID_ENABLE", "SQ_BUF_RSRC_WORD3", "ADD_TID_ENABLE", 1, False),
        ("RESOURCE_LEVEL", "SQ_BUF_RSRC_WORD3", "RESOURCE_LEVEL", 1, False),
        ("OOB_SELECT", "SQ_BUF_RSRC_WORD3", "OOB_SELECT", 3, False),
    ]
    for suffix, register, field, value, is_shift in checks:
        lo, hi = fields[(register, field)]
        assert is_shift or value < (1 << (hi - lo + 1)), field
        expected = lo if is_shift else value << lo
        actual = macros["BC250HSA_SCRATCH_" + suffix]
        if actual != expected:
            raise SystemExit(f"{suffix}: production {actual:#x} != reference {expected:#x}")
    print(f"scratch descriptor: {len(checks)} fields match pinned Mesa GFX10 reference")


if __name__ == "__main__":
    main()
