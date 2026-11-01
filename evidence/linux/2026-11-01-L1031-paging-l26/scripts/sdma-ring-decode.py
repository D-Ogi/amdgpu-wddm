"""Decode the SDMA ring words around the INDIRECT packet that points at a given IB address.
Usage: python sdma-ring-decode.py <ring dump .bin> <ib va hex> [words after]
Packet lengths follow sdma_v5_0.c emit functions and navi10_sdma_pkt_open.h (op in bits 0-7, sub-op 8-15)."""
import struct
import sys

raw = open(sys.argv[1], "rb").read()
va = int(sys.argv[2], 16)
after = int(sys.argv[3]) if len(sys.argv) > 3 else 40
rptr, wptr, swptr = struct.unpack_from("<3I", raw, 0)
ring = struct.unpack_from(f"<{(len(raw) - 12) // 4}I", raw, 12)
n = len(ring)
NAMES = {0: "NOP", 1: "COPY", 2: "WRITE", 4: "INDIRECT", 5: "FENCE", 6: "TRAP", 8: "POLL_REGMEM", 9: "COND_EXE",
         0xB: "CONST_FILL", 0xE: "SRBM_WRITE", 0x11: "GCR_REQ"}


def length(i):
    h = ring[i % n]
    op = h & 0xFF
    if op == 0:
        return 1 + ((h >> 16) & 0x3FFF)
    if op == 1:
        return 7
    if op == 2:
        return 4 + ring[(i + 3) % n] + 1
    return {4: 6, 5: 4, 6: 2, 8: 6, 9: 5, 0xB: 5, 0xE: 3, 0x11: 5}.get(op, 1)


hits = [i for i in range(n) if ring[i] & 0xFF == 4 and ring[(i + 1) % n] == (va & 0xFFFFFFFF)
        and ring[(i + 2) % n] == va >> 32]
print(f"ring dwords {n} rptr {rptr} wptr {wptr}; INDIRECT for 0x{va:x} at {hits}")
for start in hits:
    i = start - 12
    end = start + after
    # step back to a packet boundary: walk forward from start-12 is unsafe; print raw words before, decode from start
    print("  before:", " ".join(f"{ring[j % n]:08x}" for j in range(start - 12, start)))
    gcr = [j for j in range(start - 30, start) if ring[j % n] & 0xFFFF == 0x11]
    i = gcr[-1] if gcr else start
    while i < end:
        ln = length(i)
        words = [ring[(i + k) % n] for k in range(min(ln, 8))]
        op = words[0] & 0xFF
        print(f"  [{i % n:4d}] {NAMES.get(op, f'op{op:#x}'):<11} sub {(words[0] >> 8) & 0xFF:#x} "
              + " ".join(f"{w:08x}" for w in words) + (" ..." if ln > 8 else ""))
        i += ln
