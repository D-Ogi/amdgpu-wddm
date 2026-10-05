#!/usr/bin/env python3
"""One Linux SDMA memory-poll job. Requires source-matched tracing for delayed mode.
PROVENANCE: poll sequence from libdrm deadlock_tests.c (MIT), E13 DRM UAPI helper.
Packet fields are generated from original AMD headers, never register literals.
"""
import argparse
import json
import os
from pathlib import Path
import struct
import sys
import threading
import time


def packet_words(fields, wait_va, marker_va, marker, alignment):
    if alignment < 4 or alignment % 4:
        raise ValueError("invalid IB length alignment")
    words = [fields["poll_header"], wait_va & 0xffffffff, wait_va >> 32,
             0, 0xffffffff, fields["poll_interval_retry"],
             fields["write_header"], marker_va & 0xffffffff, marker_va >> 32,
             0, marker]  # WRITE count is payload dwords minus one.
    while len(words) * 4 % alignment:
        words.append(fields["nop"])
    if len(words) * 4 > 1024:
        raise ValueError("IB would overlap its polled data")
    return words


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--reference-dir", type=Path, required=True,
                    help="E13 directory containing dispatch.py and generated dispatch.json")
    ap.add_argument("--packets", type=Path, required=True)
    ap.add_argument("--device", default="/dev/dri/renderD128")
    ap.add_argument("--ring", type=int, default=0)
    ap.add_argument("--release-ms", type=int, default=0,
                    help="0: pre-released positive control; nonzero: diagnostic delayed release")
    ap.add_argument("--wait-seconds", type=int, default=20)
    args = ap.parse_args()
    if not 0 <= args.release_ms <= 5000 or not 1 <= args.wait_seconds <= 60:
        ap.error("bounded release/wait limits exceeded")
    if args.wait_seconds * 1000 <= args.release_ms:
        ap.error("wait must extend beyond CPU release")
    sys.path.insert(0, str(args.reference_dir))
    from dispatch import Session, Blob, PAGE
    cfg = json.loads((args.reference_dir / "dispatch.json").read_text())
    fields = json.loads(args.packets.read_text())
    fd = os.open(args.device, os.O_RDWR | os.O_CLOEXEC)
    session = Session(cfg, fd)
    mapping = None
    release = threading.Event()
    thread = None
    result = 2
    try:
        layout = session.t("drm_amdgpu_info_device")
        dev = Blob(layout)
        dev.buf = bytearray(session.info(session.c("AMDGPU_INFO_DEV_INFO"), layout["size"]))
        if dev.get("device_id") != 0x13fe:
            raise RuntimeError("unexpected GPU identity")
        ip = session.c("AMDGPU_HW_IP_DMA")
        layout = session.t("drm_amdgpu_info_hw_ip")
        hw = Blob(layout)
        hw.buf = bytearray(session.info(session.c("AMDGPU_INFO_HW_IP_INFO"), layout["size"], ip, 0))
        if not 0 <= args.ring < 32 or not hw.get("available_rings") & (1 << args.ring):
            raise RuntimeError("requested SDMA ring unavailable")
        alignment = max(PAGE, dev.get("virtual_address_alignment"), hw.get("ib_start_alignment"))
        va = ((dev.get("virtual_address_offset") + alignment - 1) // alignment) * alignment
        if va + PAGE > dev.get("virtual_address_max"):
            raise RuntimeError("no suitable GPU VA")
        session.ctx_alloc()
        mapping = session.bo("sdma-poll", PAGE, va)
        marker = 0x51da1234
        words = packet_words(fields, va + 1024, va + 1028, marker,
                             max(4, hw.get("ib_size_alignment")))
        mapping[:] = bytes(PAGE)
        struct.pack_into("<" + "I" * len(words), mapping, 0, *words)
        if args.release_ms == 0:
            struct.pack_into("<I", mapping, 1024, 1)
        else:
            def unblock():
                release.wait(args.release_ms / 1000)
                struct.pack_into("<I", mapping, 1024, 1)
                print("CPU_RELEASE monotonic=%.6f" % time.monotonic(), flush=True)
            thread = threading.Thread(target=unblock)
            thread.start()
        print(json.dumps({"stage": "submit", "ip": ip, "ring": args.ring,
                          "release_ms": args.release_ms, "ib_dwords": len(words)}), flush=True)
        session.timed_out = True
        seq = session.submit(va, len(words), args.ring, ip)
        busy = session.wait(seq, args.ring, ip, args.wait_seconds)
        observed = struct.unpack_from("<I", mapping, 1028)[0]
        result = 0 if not busy and observed == marker else 3
        print(json.dumps({"stage": "result", "busy": busy, "marker": observed,
                          "expected": marker, "executed": result == 0}), flush=True)
        # A completed control is not a reset witness. Require external trace,
        # kernel result and a fresh-context post-reset control for that claim.
    except Exception as exc:
        print("PROBE_ERROR " + type(exc).__name__ + ": " + str(exc), flush=True)
    finally:
        release.set()
        if thread:
            thread.join()  # release event already set; keep mapping alive through writer.
        if mapping is not None:
            struct.pack_into("<I", mapping, 1024, 1)
        session.close()
        os.close(fd)
    return result


if __name__ == "__main__":
    raise SystemExit(main())
