#!/usr/bin/env python3
"""L26: a real paging round trip of one buffer. A CPU-visible VRAM BO (16 MiB, known pattern) is made resident by an
SDMA copy job, evicted to GTT by debugfs amdgpu_evict_vram, and moved back by the validation of a second job. The
sdma0/sdma1 rings are dumped raw at each step (12-byte rptr/wptr/swptr header, then the ring); the caller traces
amdgpu_copy_buffer, amdgpu_ib_schedule (first 64 IB dwords), the doorbells and amdgpu:amdgpu_bo_move meanwhile.
Usage (lab): python3 l26-migrate.py --ref /root/x1 --out /root/l26-out
Exit: 0 round trip intact, 1 data mismatch, 2 error, 3 busy."""
import glob
import importlib.util
import json
import os
import struct
import sys
import time

REF = sys.argv[sys.argv.index("--ref") + 1]
OUT = sys.argv[sys.argv.index("--out") + 1]
sys.path.insert(0, REF)
from dispatch import Session, Blob, PAGE  # noqa: E402

spec = importlib.util.spec_from_file_location("x1", os.path.join(REF, "x1-sdma-raw.py"))
x1 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(x1)

DBG = sorted(glob.glob("/sys/kernel/debug/dri/*/amdgpu_evict_vram"))[0].rsplit("/", 1)[0]
SIZE = 16 << 20
T0 = time.monotonic()


def log(msg):
    print(f"[{time.monotonic() - T0:7.3f}] {msg}", flush=True)


def ring_dump(tag):
    for name in ("sdma0", "sdma1"):
        fd = os.open(f"{DBG}/amdgpu_ring_{name}", os.O_RDONLY)
        chunks = []
        try:
            while chunk := os.read(fd, 4096):  # sizes must be a multiple of 4
                chunks.append(chunk)
        finally:
            os.close(fd)
        raw = b"".join(chunks)
        with open(os.path.join(OUT, f"ring-{name}-{tag}.bin"), "wb") as fh:
            fh.write(raw)
        rptr, wptr, swptr = struct.unpack_from("<3I", raw, 0)
        log(f"RING {tag} {name} bytes {len(raw)} rptr {rptr} wptr {wptr} swptr {swptr}")


def fences(tag):
    text = open(f"{DBG}/amdgpu_fence_info").read()
    for block in text.split("--- ring ")[1:]:
        if "(sdma" in block.split("\n", 1)[0]:
            log(f"FENCE {tag} " + " ".join(block.split()[:9]))


def copy_job(s, pk, ib, ip, align, src, dst, n, tag):
    words = x1.copy_packet(pk, src, dst, n)
    while len(words) * 4 % align:
        words.append(pk["op_nop"])
    ib[:PAGE] = bytes(PAGE)
    struct.pack_into("<%dI" % len(words), ib, 0, *words)
    log(f"SUBMIT {tag} copy {n} bytes 0x{src:x} -> 0x{dst:x}")
    seq = s.submit(s.va_ib, len(words), 0, ip)
    busy = s.wait(seq, 0, ip, 10)
    log(f"WAIT {tag} busy={busy}")
    return busy


def main():
    os.makedirs(OUT, exist_ok=True)
    pk = json.load(open(os.path.join(REF, "x1-packets.json")))
    cfg = json.load(open(os.path.join(REF, "dispatch.json")))
    fd = os.open("/dev/dri/renderD128", os.O_RDWR | os.O_CLOEXEC)
    s = Session(cfg, fd)
    rc = 2
    try:
        layout = s.t("drm_amdgpu_info_device")
        dev = Blob(layout)
        dev.buf = bytearray(s.info(s.c("AMDGPU_INFO_DEV_INFO"), layout["size"]))
        ip = s.c("AMDGPU_HW_IP_DMA")
        hl = s.t("drm_amdgpu_info_hw_ip")
        hw = Blob(hl)
        hw.buf = bytearray(s.info(s.c("AMDGPU_INFO_HW_IP_INFO"), hl["size"], ip, 0))
        align = max(4, hw.get("ib_size_alignment"))
        span = 2 << 20
        base = ((dev.get("virtual_address_offset") + span - 1) // span) * span
        s.va_ib, va_vram, va_dst = base, base + span, base + span + SIZE
        s.ctx_alloc()
        ib = s.bo("ib", PAGE, s.va_ib)
        vram = x1.bo_in(s, "vram", SIZE, va_vram, "vram")
        dst = s.bo("dst", PAGE, va_dst, executable=False)
        pattern = b"".join(struct.pack("<I", 0x26000000 | i) for i in range(SIZE // 4))
        vram[:] = pattern
        log("pattern written through the CPU mapping")
        fences("start")
        ring_dump("0-start")
        if copy_job(s, pk, ib, ip, align, va_vram + SIZE - PAGE, va_dst, PAGE, "resident"):
            rc = 3
            return rc
        ok1 = dst[:PAGE] == pattern[SIZE - PAGE:]
        log(f"CHECK resident copy matches={ok1}")
        ring_dump("1-resident")
        t = time.monotonic()
        res = open(f"{DBG}/amdgpu_evict_vram").read().strip()
        log(f"EVICT amdgpu_evict_vram -> {res} in {time.monotonic() - t:.3f} s")
        fences("evicted")
        ring_dump("2-evicted")
        dst[:PAGE] = bytes(PAGE)
        if copy_job(s, pk, ib, ip, align, va_vram + SIZE - PAGE, va_dst, PAGE, "restore"):
            rc = 3
            return rc
        ok2 = dst[:PAGE] == pattern[SIZE - PAGE:]
        whole = vram[:SIZE] == pattern
        first_bad = None
        if not whole:
            got = vram[:SIZE]
            first_bad = next(i for i in range(0, SIZE, 4) if got[i:i + 4] != pattern[i:i + 4])
        log(f"CHECK after round trip: gpu copy matches={ok2} whole BO through CPU matches={whole}"
            + (f" first_bad_byte={first_bad}" if first_bad is not None else ""))
        fences("restored")
        ring_dump("3-restored")
        rc = 0 if (ok1 and ok2 and whole) else 1
    except Exception as exc:
        log(f"L26_ERROR {type(exc).__name__}: {exc}")
    finally:
        s.close()
        os.close(fd)
        log(f"L26_RESULT rc {rc}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
