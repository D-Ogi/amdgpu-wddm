#!/usr/bin/env python3
"""Hang lane jobs (L43, L47, L48). One deliberately blocked user job per run, through raw amdgpu CS ioctls
(E13/E29 dispatch.py Session). Packet words come from hang.json (gen-hang-tables.py, headers and llvm-mc only).

  h-jobs.py sdma-poll --release-ms 14000 --wait 30   SDMA poll on a CPU-released dword (E29 shape); pin the engine
                                                     with amdgpu_sdma_sched_mask before the run
  h-jobs.py gfx-wait  --release-ms 14000 --wait 30   GFX WAIT_REG_MEM (!= 0) on a CPU-released dword (libdrm
                                                     deadlock_tests shape)
  h-jobs.py gfx-nop --wait 10                        GFX IB of type-3 NOPs only (ring health control)
  h-jobs.py spin [--kill-at 3] --wait 30            compute dispatch of 's_branch self; s_endpgm'; with --kill-at,
                                                     write SQ_CMD KILL|BROADCAST|CHECK_VMID with the job's VMID
                                                     (read from its INDIRECT_BUFFER packet in the ring) at that time
The CPU release (sdma-poll, gfx-wait) comes after amdgpu's 10 s job timeout, so a job that ends before it was
ended by the kernel's reset path. Exit: 0 job signalled, 3 still busy at --wait, 2 error (WAIT_CS errno included)."""
import glob
import json
import os
import re
import struct
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REF = sys.argv[sys.argv.index("--ref") + 1] if "--ref" in sys.argv else "/root/x1"
sys.path.insert(0, REF)
from dispatch import Session, Blob, PAGE, IoctlError, build_ib, geometry  # noqa: E402

H = json.load(open(os.path.join(HERE, "hang.json")))
CFG = json.load(open(os.path.join(REF, "dispatch.json")))
DBG = sorted(glob.glob("/sys/kernel/debug/dri/*/amdgpu_regs"))[0].rsplit("/", 1)[0]
T0 = time.monotonic()


def log(msg):
    print(f"[{time.monotonic() - T0:7.3f}] {msg}", flush=True)


def opt(name, default):
    return type(default)(sys.argv[sys.argv.index(name) + 1]) if name in sys.argv else default


def reg_read(name):
    fd = os.open(f"{DBG}/amdgpu_regs", os.O_RDONLY)
    try:
        return struct.unpack("<I", os.pread(fd, 4, H["regs"][name]))[0]
    finally:
        os.close(fd)


def snapshot(tag):
    names = ["GRBM_STATUS", "CP_STAT", "CP_HQD_ACTIVE", "SDMA0_STATUS_REG", "SDMA1_STATUS_REG",
             "GCVM_L2_PROTECTION_FAULT_STATUS"]
    log(f"REGS {tag} " + " ".join(f"{n}=0x{reg_read(n):08X}" for n in names))


def busy_compute_rings():
    """Compute rings with emitted != signaled. No sched-mask pinning: comp_1.0.0 is the high-priority queue
    (amdgpu_gfx_is_high_priority_compute_queue), and a mask without a ready normal-priority ring oopses
    drm_sched_job_arm on 6.18.52 (hang lane, 2026-11-01)."""
    text = open(f"{DBG}/amdgpu_fence_info").read()
    rows = re.findall(r"--- ring \d+ \((comp_[0-9.]+)\) ---\s*Last signaled fence\s+0x([0-9a-fA-F]+)\s*"
                      r"Last emitted\s+0x([0-9a-fA-F]+)", text)
    return [(n, int(s, 16), int(e, 16)) for n, s, e in rows if s != e]


def ring_ib_vmid(ring_name, ib_va):
    """VMID from the newest INDIRECT_BUFFER packet that points at ib_va (gfx_v10_0_ring_emit_ib_*:
    header PACKET3(INDIRECT_BUFFER, 2), addr lo, addr hi, control = VALID | length | vmid << 24)."""
    fd = os.open(f"{DBG}/amdgpu_ring_{ring_name}", os.O_RDONLY)
    chunks = []
    try:
        while chunk := os.read(fd, 4096):  # amdgpu_debugfs_ring_read refuses sizes that are not a multiple of 4
            chunks.append(chunk)
    finally:
        os.close(fd)
    raw = b"".join(chunks)
    words = struct.unpack(f"<{len(raw) // 4}I", raw[:len(raw) // 4 * 4])
    ring = words[3:]
    header = (3 << 30) | (2 << 16) | (0x3F << 8)
    hits = [i for i in range(len(ring) - 3) if ring[i] == header and ring[i + 1] == (ib_va & 0xFFFFFFFC)
            and ring[i + 2] == (ib_va >> 32)]
    if not hits:
        return None, words[:3]
    return (ring[hits[-1] + 3] >> 24) & 0xF, words[:3]


def device(s):
    layout = s.t("drm_amdgpu_info_device")
    dev = Blob(layout)
    dev.buf = bytearray(s.info(s.c("AMDGPU_INFO_DEV_INFO"), layout["size"]))
    if dev.get("device_id") != 0x13fe:
        raise RuntimeError("unexpected GPU identity")
    span = 2 << 20
    return ((dev.get("virtual_address_offset") + span - 1) // span) * span


def pad(words, ip_name, s, word):
    layout = s.t("drm_amdgpu_info_hw_ip")
    hw = Blob(layout)
    hw.buf = bytearray(s.info(s.c("AMDGPU_INFO_HW_IP_INFO"), layout["size"], s.c(ip_name), 0))
    align = max(4, hw.get("ib_size_alignment"))
    while len(words) * 4 % align:
        words.append(word)
    return words


MTYPE_UC = 4 << 5  # amdgpu_drm.h:529 AMDGPU_VM_MTYPE_UC


def bo_uc(s, label, size, va):
    """Session.bo with the VA mapped AMDGPU_VM_MTYPE_UC, as libdrm deadlock_tests.c:262 does for family >= AI so
    that the CP's WAIT_REG_MEM sees a CPU write instead of a line held in GL2."""
    import mmap
    from dispatch import ioctl
    create = Blob(s.t("drm_amdgpu_gem_create"))
    create.set("in.bo_size", size).set("in.alignment", PAGE)
    create.set("in.domains", s.c("AMDGPU_GEM_DOMAIN_GTT")).set("in.domain_flags", 0)
    ioctl(s.fd, s.cfg["ioctl"]["GEM_CREATE"], create.buf, f"GEM_CREATE {label}")
    handle = create.get("out.handle")
    gm = Blob(s.t("drm_amdgpu_gem_mmap")).set("in.handle", handle)
    ioctl(s.fd, s.cfg["ioctl"]["GEM_MMAP"], gm.buf, f"GEM_MMAP {label}")
    mapping = mmap.mmap(s.fd, size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=gm.get("out.addr_ptr"))
    flags = (s.c("AMDGPU_VM_PAGE_READABLE") | s.c("AMDGPU_VM_PAGE_WRITEABLE") | s.c("AMDGPU_VM_PAGE_EXECUTABLE")
             | MTYPE_UC)
    va_blob = Blob(s.t("drm_amdgpu_gem_va"))
    va_blob.set("handle", handle).set("operation", s.c("AMDGPU_VA_OP_MAP")).set("flags", flags)
    va_blob.set("va_address", va).set("offset_in_bo", 0).set("map_size", size)
    ioctl(s.fd, s.cfg["ioctl"]["GEM_VA"], va_blob.buf, f"GEM_VA MAP {label}")
    s.bos.append([handle, va, size, mapping])
    log(f"BO {label} {size} bytes va 0x{va:016x} flags 0x{flags:x} (MTYPE_UC)")
    return mapping


def released_job(s, kind, base):
    release_ms, wait_s = opt("--release-ms", 14000), opt("--wait", 30)
    m = bo_uc(s, "ib", PAGE, base) if "--uc" in sys.argv else s.bo("ib", PAGE, base)
    m[:] = bytes(PAGE)
    flag_va = base + 2048
    if kind == "sdma-poll":
        pk = json.load(open(os.path.join(REF, "packets.json")))
        ip = "AMDGPU_HW_IP_DMA"
        marker = 0x51DA1234
        words = [pk["poll_header"], flag_va & 0xFFFFFFFF, flag_va >> 32, 0, 0xFFFFFFFF, pk["poll_interval_retry"],
                 pk["write_header"], (flag_va + 4) & 0xFFFFFFFF, (flag_va + 4) >> 32, 0, marker]
        words = pad(words, ip, s, pk["nop"])
    else:
        p = H["pm4"]
        ip = "AMDGPU_HW_IP_GFX"
        marker = None
        if kind == "gfx-nop":
            words = [p["pad"]]
        else:
            words = [p["wait_header"], p["wait_ctrl"], flag_va & 0xFFFFFFFF, flag_va >> 32, p["wait_ref"],
                     p["wait_mask"], p["wait_interval"]]
        words = pad(words, ip, s, p["pad"])
    struct.pack_into(f"<{len(words)}I", m, 0, *words)
    stop = threading.Event()

    def release():
        if not stop.wait(release_ms / 1000):
            struct.pack_into("<I", m, 2048, 1)
            log(f"CPU_RELEASE at {release_ms} ms")

    th = threading.Thread(target=release)
    log(f"submit {kind} ip {ip} words {len(words)} release_ms {release_ms} wait_s {wait_s}")
    snapshot("before")
    s.timed_out = True
    seq = s.submit(base, len(words), 0, s.c(ip))
    th.start()
    rc = 2
    try:
        busy = s.wait(seq, 0, s.c(ip), wait_s)
        log(f"WAIT_CS status {busy}")
        rc = 3 if busy else 0
    except IoctlError as e:
        log(f"WAIT_CS error {e}")
    finally:
        stop.set()
        th.join()
        struct.pack_into("<I", m, 2048, 1)
    if marker is not None:
        got = struct.unpack_from("<I", m, 2052)[0]
        log(f"marker 0x{got:08X} expected 0x{marker:08X} written={'yes' if got == marker else 'no'}")
    snapshot("after")
    return rc


def spin_job(s, base):
    kill_at, wait_s = opt("--kill-at", 0.0), opt("--wait", 30)
    cfg = json.loads(json.dumps(CFG))
    cfg["shader"]["words"] = H["spin_shader"]["words"]
    s.cfg = cfg
    dst_size, _, _, _ = geometry(cfg, 1)
    ctx = {"shader_va": base, "cmd_va": base + (2 << 20), "dst_va": base + (4 << 20), "dst_size": dst_size}
    dwords, _ = build_ib(cfg, ctx)
    sh = s.bo("shader", PAGE, ctx["shader_va"])
    cmd = s.bo("cmd", PAGE, ctx["cmd_va"])
    s.bo("dst", max(dst_size, PAGE), ctx["dst_va"], executable=False)
    sh[:] = bytes(PAGE)
    struct.pack_into(f"<{len(cfg['shader']['words'])}I", sh, 0, *cfg["shader"]["words"])
    cmd[:] = bytes(PAGE)
    struct.pack_into(f"<{len(dwords)}I", cmd, 0, *dwords)
    ip = s.c("AMDGPU_HW_IP_COMPUTE")
    snapshot("before")
    log(f"submit spin shader {['%08x' % w for w in cfg['shader']['words']]} kill_at {kill_at} wait_s {wait_s}")
    s.timed_out = True
    seq = s.submit(ctx["cmd_va"], len(dwords), 0, ip)
    killer = None
    if kill_at > 0:
        def kill():
            time.sleep(kill_at)
            snapshot("spinning")
            busy = busy_compute_rings()
            log(f"busy compute rings (name, signaled, emitted) {busy}")
            if len(busy) != 1:
                log("KILL skipped: not exactly one busy compute ring")
                return
            ring_name = busy[0][0]
            vmid, early = ring_ib_vmid(ring_name, ctx["cmd_va"])
            log(f"ring {ring_name} rptr/wptr/swptr {early} job vmid {vmid}")
            if vmid is None:
                log(f"KILL skipped: no INDIRECT_BUFFER packet for this IB in {ring_name}")
                return
            q = H["sq_cmd"]
            val = q["kill_base"] | ((vmid << q["vmid_shift"]) & q["vmid_mask"])
            fd = os.open(f"{DBG}/amdgpu_regs", os.O_WRONLY)
            try:
                os.pwrite(fd, struct.pack("<I", val), H["regs"]["SQ_CMD"])
            finally:
                os.close(fd)
            log(f"KILL wrote SQ_CMD 0x{val:08X} (vmid {vmid})")
            time.sleep(0.2)
            snapshot("after-kill")
        killer = threading.Thread(target=kill)
        killer.start()
    rc = 2
    try:
        busy = s.wait(seq, 0, ip, wait_s)
        log(f"WAIT_CS status {busy}")
        rc = 3 if busy else 0
    except IoctlError as e:
        log(f"WAIT_CS error {e}")
    if killer:
        killer.join()
    snapshot("after")
    return rc


def main():
    kind = sys.argv[1]
    fd = os.open("/dev/dri/renderD128", os.O_RDWR | os.O_CLOEXEC)
    s = Session(CFG, fd)
    rc = 2
    try:
        base = device(s)
        s.ctx_alloc()
        rc = spin_job(s, base) if kind == "spin" else released_job(s, kind, base)
    except Exception as exc:
        log(f"H_ERROR {type(exc).__name__}: {exc}")
    finally:
        s.close()
        os.close(fd)
    log(f"H_RESULT {kind} rc {rc}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
