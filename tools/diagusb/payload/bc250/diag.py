#!/usr/bin/env python3
"""BC-250 diagnostic probe. Runs from the diagnostic USB under Alpine Linux, as root.

Phase A - raw BAR5 MMIO before any GPU driver has touched the device. This is the state
          a Windows driver inherits from the BIOS, measured with the same kind of access.
Phase B - load amdgpu and record the kernel's view: dmesg, IP discovery, firmware versions,
          register reads through debugfs (the kernel's own addressing) next to raw reads.

Writes performed (mode "full" and "noload" only), each restored afterwards:
  GRBM_GFX_INDEX  - bank select, what every driver does before reading per-SE/SH registers
  SCRATCH_REG0    - the scratch register amdgpu itself uses for its ring test
Mode "readonly" performs no MMIO writes at all.

Results: <usb>/bc250/results/run-NNN/ (report.json, dmesg.txt, summary.json, qr-chunks.txt).
The summary is turned into QR chunks twice: once with phase A alone, before amdgpu is loaded, and
once at the end, so a machine that hangs in phase B still leaves usable codes behind.

BC250_TEST_DEV + BC250_TEST_BAR point the probe at another PCI function's BAR (see test_override):
a way to exercise the real MMIO path on a machine that has no BC-250. Never used on the target.
"""

import ctypes
import glob
import json
import mmap
import os
import re
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qrcodec  # noqa: E402  (same directory)

SUMMARY_VERSION = 1
GFX_INDEX_BROADCAST = 0xE0000000  # SE, SA and instance broadcast: amdgpu's idle state
SCRATCH_PATTERNS = (0xCAFEDEAD, 0x5A5AA5A5)
BANKS = ((0, 0), (0, 1))  # Cyan Skillfish: 1 shader engine, 2 shader arrays
STOCK_CC_GC_SHADER_ARRAY_CONFIG = 0xFFF80000  # 24 CU, per duggasco/bc250-40cu-unlock
STOCK_SPI_PG_WGP_MASK = 0x7
GB_ADDR_CONFIG_GOLDEN = 0x00100044  # CYAN_SKILLFISH_GB_ADDR_CONFIG_GOLDEN in gfx_v10_0.c
MODPROBE_TIMEOUT_S = 180

# Test hook, see test_override(). Both variables must be set, so it cannot fire by accident.
TEST_DEV_ENV, TEST_BAR_ENV = "BC250_TEST_DEV", "BC250_TEST_BAR"


def log(msg):
    print(msg, flush=True)


def hx(value):
    return "-" if value is None else f"{value:08X}"


def read_text(path, default=""):
    try:
        with open(path, "r", errors="replace") as f:
            return f.read().strip()
    except OSError:
        return default


def kernel_mode():
    m = re.search(r"\bbc250\.mode=(\w+)", read_text("/proc/cmdline"))
    return m.group(1) if m else "full"


# --------------------------------------------------------------------------- PCI / BAR access

def find_gpu():
    """sysfs path of the BC-250 GPU function, else of any AMD display device, else None."""
    fallback = None
    for dev in sorted(glob.glob("/sys/bus/pci/devices/*")):
        if read_text(dev + "/vendor") != "0x1002":
            continue
        if read_text(dev + "/device") == "0x13fe":
            return dev
        if read_text(dev + "/class").startswith("0x03") and fallback is None:
            fallback = dev
    return fallback


def pci_info(dev):
    info = {
        "addr": os.path.basename(dev),
        "id": f"{read_text(dev + '/vendor')[2:]}:{read_text(dev + '/device')[2:]}".upper(),
        "sub": f"{read_text(dev + '/subsystem_vendor')[2:]}:{read_text(dev + '/subsystem_device')[2:]}".upper(),
        "rev": read_text(dev + "/revision")[2:].upper(),
        "class": read_text(dev + "/class")[2:].upper(),
        "driver": os.path.basename(os.path.realpath(dev + "/driver")) if os.path.exists(dev + "/driver") else "",
    }
    try:
        with open(dev + "/config", "rb") as f:
            cfg = f.read(64)
        info["cmd"] = f"{struct.unpack_from('<H', cfg, 4)[0]:04X}"
    except OSError as e:
        info["cmd"] = f"ERR {e.errno}"
    bars = []
    for line in read_text(dev + "/resource").splitlines()[:6]:
        start, end, _flags = (int(x, 16) for x in line.split())
        bars.append(end - start + 1 if end else 0)
    info["bars"] = bars
    return info


class Bar:
    """32-bit MMIO access to a PCI BAR through sysfs. Registers are read and written as single
    32-bit accesses (ctypes c_uint32 element), never bytewise."""

    def __init__(self, dev, index=5, readonly=False):
        line = read_text(dev + "/resource").splitlines()[index]
        start, end, _flags = (int(x, 16) for x in line.split())
        self.size = end - start + 1 if end else 0
        self.readonly = readonly
        if not self.size:
            raise OSError(f"BAR{index} is not assigned")
        fd = os.open(f"{dev}/resource{index}", os.O_RDWR | os.O_SYNC)
        try:
            # Mapped read-write even when readonly: the read path under test must be the real one.
            # w32 is what refuses; nothing in a readonly run calls it.
            self._mm = mmap.mmap(fd, self.size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
        finally:
            os.close(fd)
        self._regs = (ctypes.c_uint32 * (self.size // 4)).from_buffer(self._mm)

    def r32(self, off):
        if off % 4 or off + 4 > self.size:
            return None
        return self._regs[off >> 2]

    def w32(self, off, value):
        if self.readonly:
            raise ValueError("this BAR is open read-only, writes are refused")
        if off % 4 or off + 4 > self.size:
            raise ValueError(f"offset 0x{off:X} outside BAR")
        self._regs[off >> 2] = value & 0xFFFFFFFF


def test_override():
    """{'dev','bar'} from BC250_TEST_DEV and BC250_TEST_BAR, or None.

    Test hook. It points the probe at some other PCI function's BAR so that the mmap + ctypes
    32-bit read path can be exercised where there is no BC-250. The BAR has to belong to a device
    with no driver bound: a driver that has claimed its BAR makes the sysfs mmap fail with EINVAL
    on a kernel built with strict devmem, which is also why grub.cfg blacklists amdgpu for phase A.
    In QEMU, `-device edu` fits (1234:11e8, BAR0, 1 MiB, reads 0x010000ED at offset 0), and
    `-device pci-testdev` gives a 4 KiB BAR where every probe offset is out of range and reads
    back as None. Both variables must be set and the path must be a PCI device directory. A run
    is forced to readonly, is labelled TEST_HOOK in the verdict and carries "test" in the summary,
    so its numbers can never be mistaken for a measurement of a real GPU.
    """
    dev, bar = os.environ.get(TEST_DEV_ENV), os.environ.get(TEST_BAR_ENV)
    if not dev or not bar:
        return None
    if not os.path.isfile(os.path.join(dev, "resource")):
        raise SystemExit(f"{TEST_DEV_ENV}={dev} is not a PCI device directory")
    if not re.fullmatch(r"[0-5]", bar):
        raise SystemExit(f"{TEST_BAR_ENV}={bar} is not a BAR index (0-5)")
    return {"dev": dev, "bar": int(bar)}


def ensure_memory_decoding(dev, info):
    """If the BIOS left memory decoding off, ask the kernel to enable the device."""
    try:
        if int(info["cmd"], 16) & 0x2:
            return False
        with open(dev + "/enable", "w") as f:
            f.write("1")
        return True
    except (OSError, ValueError):
        return False


# --------------------------------------------------------------------------- probing logic (bar-agnostic, unit tested)

def gfx_index(se, sh):
    return (1 << 30) | (se << 16) | (sh << 8)  # instance broadcast, explicit SE and SA


def reg_off(probes, name):
    return next(r["off"] for r in probes["regs"] if r["n"] == name)


def sample(bar, probes):
    return [bar.r32(r["off"]) for r in probes["regs"]]


def sample_claims(bar, probes):
    return [bar.r32(c["off"]) for c in probes["claims"]]


def sample_banked(bar, probes):
    """Per-SE/SH values of banked registers. Writes GRBM_GFX_INDEX, restores broadcast."""
    idx = reg_off(probes, "GRBM_GFX_INDEX")
    banked = [r for r in probes["regs"] if r["banked"]]
    out = {}
    try:
        for se, sh in BANKS:
            bar.w32(idx, gfx_index(se, sh))
            out[f"{se}.{sh}"] = [bar.r32(r["off"]) for r in banked]
    finally:
        bar.w32(idx, GFX_INDEX_BROADCAST)
    return out


def scratch_test(bar, probes):
    off = reg_off(probes, "SCRATCH_REG0")
    saved = bar.r32(off)
    readback = []
    try:
        for pattern in SCRATCH_PATTERNS:
            bar.w32(off, pattern)
            readback.append(bar.r32(off))
    finally:
        bar.w32(off, saved)
    return {"saved": saved, "readback": readback, "ok": readback == list(SCRATCH_PATTERNS)}


def phase_a(bar, probes, allow_writes):
    res = {"raw": sample(bar, probes), "claims": sample_claims(bar, probes)}
    if allow_writes:
        res["bank"] = sample_banked(bar, probes)
        res["scratch"] = scratch_test(bar, probes)
    return res


# --------------------------------------------------------------------------- phase B: the kernel's view

def run(cmd, timeout):
    try:
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout, text=True, errors="replace")
        return p.returncode, p.stdout
    except subprocess.TimeoutExpired:
        return "timeout", ""
    except OSError as e:
        return f"oserror {e.errno}", ""


_ABANDONED = []


def run_patient(cmd, timeout):
    """Like run(), but gives up on a process instead of waiting for it.

    subprocess.run() kills the child when its timeout expires and then waits for it. A modprobe
    that is stuck inside the kernel cannot be killed, so that wait never returns and the probe
    would hang with nothing on screen. Here the child is simply left behind. Output is read only
    after it exits, so this is for commands that print a few lines, not for a data stream.
    """
    try:
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    except OSError as e:
        return f"oserror {e.errno}", ""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if p.poll() is not None:
            return p.returncode, p.stdout.read()
        time.sleep(0.5)
    _ABANDONED.append(p)  # keep the reference: nothing should try to reap it behind our back
    return "timeout", ""


def find_card(dev):
    for card in sorted(glob.glob("/sys/class/drm/card[0-9]*")):
        if "-" in os.path.basename(card):
            continue
        if os.path.realpath(card + "/device") == os.path.realpath(dev):
            return card
    return None


def find_debugfs():
    if not os.path.isdir("/sys/kernel/debug/dri"):
        run(["mount", "-t", "debugfs", "none", "/sys/kernel/debug"], 10)
    for path in sorted(glob.glob("/sys/kernel/debug/dri/*/amdgpu_regs")):
        return os.path.dirname(path)
    return None


def debugfs_read(fd, off, bank=None):
    """amdgpu_regs takes a BYTE offset (the kernel does pos >> 2). Bit 62 selects an SE/SH bank."""
    pos = off
    if bank is not None:
        se, sh = bank
        pos |= (1 << 62) | (se << 24) | (sh << 34) | (0x3FF << 44)  # 0x3FF instance = broadcast
    try:
        data = os.pread(fd, 4, pos)
    except OSError:
        return None
    # A refused read (bank out of range, register gated) returns fewer than 4 bytes, not an error.
    return struct.unpack("<I", data)[0] if len(data) == 4 else None


def sample_debugfs(dbg, probes):
    fd = os.open(dbg + "/amdgpu_regs", os.O_RDONLY)
    try:
        flat = [debugfs_read(fd, r["off"]) for r in probes["regs"]]
        banked = [r for r in probes["regs"] if r["banked"]]
        bank = {f"{se}.{sh}": [debugfs_read(fd, r["off"], (se, sh)) for r in banked] for se, sh in BANKS}
    finally:
        os.close(fd)
    return flat, bank


def read_ip_discovery(card):
    """{hw_id_name: [{ver, base}]} from sysfs, as published by the kernel's IP discovery."""
    out = {}
    root = card + "/device/ip_discovery/die"
    for inst in sorted(glob.glob(root + "/*/*/*")):
        if not os.path.isfile(inst + "/major"):
            continue
        name = os.path.basename(os.path.dirname(inst))
        out.setdefault(name, []).append({
            "ver": ".".join(read_text(f"{inst}/{k}", "?") for k in ("major", "minor", "revision")),
            "base": read_text(inst + "/base_addr").split(),
        })
    return out


_DMESG_KEEP = re.compile(r"amdgpu|\[drm\]|psp|smu|kfd", re.IGNORECASE)
_DMESG_ERR = re.compile(r"\*ERROR\*|fail|timeout|timed out|hang|fault|BUG|Oops", re.IGNORECASE)


def digest_dmesg(text):
    lines = [l for l in text.splitlines() if _DMESG_KEEP.search(l)]
    strip = lambda l: re.sub(r"^\[\s*[\d.]+\]\s*", "", l)
    return {
        "initialized": any("Initialized amdgpu" in l for l in lines),
        "rings": sum(1 for l in lines if re.search(r"ring \S+ uses VM inv eng", l)),
        "vram": next((strip(l)[:90] for l in lines if "VRAM:" in l), ""),
        "gtt": next((strip(l)[:90] for l in lines if "GTT memory ready" in l or "GTT:" in l), ""),
        "errors": [strip(l)[:110] for l in lines if _DMESG_ERR.search(l)][:10],
        "lines": len(lines),
    }


def digest_firmware(text):
    fw = {}
    for line in text.splitlines():
        m = re.match(r"(\w[\w ]*?) feature version: (\d+), (?:firmware|program) version: (0x[0-9a-fA-F]+)", line)
        if m and m.group(3) != "0x00000000":
            fw[m.group(1).strip()] = m.group(3)[2:].upper()
    return fw


def phase_b(dev, bar, probes, save):
    res = {}
    t0 = time.time()
    save("phaseB-started.txt", "modprobe amdgpu issued; if this is the newest file, the load hung the machine\n")
    rc, out = run_patient(["modprobe", "amdgpu"], MODPROBE_TIMEOUT_S)
    res["load"] = rc
    res["load_out"] = out[-400:]
    # modprobe still running after its timeout means amdgpu is stuck somewhere in init. dmesg is
    # still safe to read; anything that goes through the driver (debugfs registers, card
    # attributes) can block on the same lock, so it is skipped.
    stuck = rc == "timeout"
    card = None
    for _ in range(0 if stuck else 40):
        card = find_card(dev)
        if card:
            break
        time.sleep(0.5)
    if not stuck:
        time.sleep(3)  # let late init (ring tests, fbdev) finish printing
    res["secs"] = round(time.time() - t0, 1)
    res["card"] = bool(card)

    _rc, dmesg = run(["dmesg"], 20)
    save("dmesg.txt", dmesg)
    res["dmesg"] = digest_dmesg(dmesg)
    if stuck:
        return res

    if bar is not None:
        res["raw"] = sample(bar, probes)  # reads only: amdgpu owns the GRBM index now
    dbg = find_debugfs()
    res["debugfs"] = bool(dbg)
    if dbg:
        res["dbg"], res["dbgbank"] = sample_debugfs(dbg, probes)
        fwinfo = read_text(dbg + "/amdgpu_firmware_info")
        save("amdgpu_firmware_info.txt", fwinfo + "\n")
        res["fw"] = digest_firmware(fwinfo)
    if card:
        res["ipd"] = read_ip_discovery(card)
        res["vram_mb"] = int(read_text(card + "/device/mem_info_vram_total", "0") or 0) >> 20
        res["gtt_mb"] = int(read_text(card + "/device/mem_info_gtt_total", "0") or 0) >> 20
    return res


# --------------------------------------------------------------------------- verdict and summary

def verdict(probes, a, b, test=None):
    """Short machine-checkable findings. Each states what was observed, not what it 'proves'."""
    names = [r["n"] for r in probes["regs"]]
    banked = [r["n"] for r in probes["regs"] if r["banked"]]
    v = []

    def val(values, name):
        return values[names.index(name)] if values else None

    if test:
        v.append(f"TEST_HOOK {test['dev']} BAR{test['bar']} - not a BC-250 measurement")
    if a:
        raw = a["raw"]
        live = [x for x in raw if x not in (None, 0, 0xFFFFFFFF)]
        v.append(f"A_MMIO_LIVE {len(live)}/{len(raw)}")
        if not a.get("pristine", True):
            v.append("A_DRIVER_ALREADY_BOUND - phase A did not see an untouched device")
        v.append(f"A_GRBM_STATUS {hx(val(raw, 'GRBM_STATUS'))}")
        if "scratch" in a:
            v.append("A_SCRATCH_RW" if a["scratch"]["ok"] else f"A_SCRATCH_NOT_RW {','.join(hx(x) for x in a['scratch']['readback'])}")
        for key, values in a.get("bank", {}).items():
            cc = values[banked.index("CC_GC_SHADER_ARRAY_CONFIG")]
            spi = values[banked.index("SPI_PG_ENABLE_STATIC_WGP_MASK")]
            tag = "STOCK" if (cc, spi) == (STOCK_CC_GC_SHADER_ARRAY_CONFIG, STOCK_SPI_PG_WGP_MASK) else "OTHER"
            v.append(f"A_BANK{key} CC={hx(cc)} SPI_PG={hx(spi)} {tag}")
    if b:
        d = b.get("dmesg", {})
        if b.get("load") == "timeout":
            v.append(f"B_MODPROBE_STUCK after {MODPROBE_TIMEOUT_S}s - driver-side reads skipped")
        v.append(f"B_AMDGPU {'INIT_OK' if d.get('initialized') else 'NOT_INITIALIZED'} rings={d.get('rings', 0)} errs={len(d.get('errors', []))}")
        if b.get("dbg"):
            gb = val(b["dbg"], "GB_ADDR_CONFIG")
            v.append(f"B_GB_ADDR_CONFIG {hx(gb)} {'GOLDEN' if gb == GB_ADDR_CONFIG_GOLDEN else 'NOT_GOLDEN'}")
            v.append(f"B_CP_RB0_BASE {hx(val(b['dbg'], 'CP_RB0_BASE'))}")
            if b.get("raw"):
                # Volatile status/pointer registers may legitimately differ between two reads.
                stable = [n for n in names if not re.search(r"STAT|RPTR|WPTR|SCRATCH|C2PMSG|GFX_INDEX|HQD", n)]
                same = sum(1 for n in stable if val(b["raw"], n) == val(b["dbg"], n))
                v.append(f"B_RAW_EQ_KERNEL {same}/{len(stable)}")
            for key, values in b.get("dbgbank", {}).items():
                v.append(f"B_BANK{key} CC={hx(values[banked.index('CC_GC_SHADER_ARRAY_CONFIG')])} "
                         f"SPI_PG={hx(values[banked.index('SPI_PG_ENABLE_STATIC_WGP_MASK')])}")
        gc = b.get("ipd", {}).get("GC") or b.get("ipd", {}).get("11")
        if gc:
            v.append(f"B_IPD_GC {gc[0]['ver']} base={','.join(gc[0]['base'][:2])}")
    return v


def summarize(report):
    """Compact form for the QR codes. Register values are positional: see probes.json with the same id."""
    a, b = report.get("A"), report.get("B")
    s = {
        "v": SUMMARY_VERSION,
        "pid": report["probes_id"],
        "mode": report["mode"],
        "kver": report["kernel"],
        "pci": report.get("pci"),
        "dmi": report.get("dmi"),
        "err": report.get("error", ""),
        "verdict": report["verdict"],
    }
    if report.get("test_hook"):
        s["test"] = report["test_hook"]
    if a:
        s["A"] = {
            "raw": [hx(x) for x in a["raw"]],
            "claims": [hx(x) for x in a["claims"]],
            "bank": {k: [hx(x) for x in vals] for k, vals in a.get("bank", {}).items()},
        }
        if "scratch" in a:
            s["A"]["scr"] = [hx(a["scratch"]["saved"])] + [hx(x) for x in a["scratch"]["readback"]]
    if b:
        s["B"] = {
            "load": b.get("load"),
            "secs": b.get("secs"),
            "dmesg": b.get("dmesg"),
            "raw": [hx(x) for x in b.get("raw", [])],
            "dbg": [hx(x) for x in b.get("dbg", [])],
            "dbgbank": {k: [hx(x) for x in vals] for k, vals in b.get("dbgbank", {}).items()},
            "fw": b.get("fw", {}),
            "ipd": {k: [f"{i['ver']}@{','.join(i['base'])}" for i in insts] for k, insts in b.get("ipd", {}).items()},
            "vram_mb": b.get("vram_mb"),
            "gtt_mb": b.get("gtt_mb"),
        }
    return s


# --------------------------------------------------------------------------- main

def next_run_dir(base):
    os.makedirs(base, exist_ok=True)
    n = 1 + max([int(m.group(1)) for m in (re.match(r"run-(\d+)$", d) for d in os.listdir(base)) if m] or [0])
    path = os.path.join(base, f"run-{n:03d}")
    os.makedirs(path)
    return path


def write_file(path, text):
    try:
        with open(path, "w") as f:
            f.write(text)
        return True
    except OSError as e:
        log(f"  (could not write {path}: {e})")
        return False


def emit_chunks(report, save):
    """Summary -> QR chunks, in the run directory and where qrshow.py picks them up.

    Called once before phase B as well: if loading amdgpu hangs the machine, the phase A codes are
    already on the stick and the next boot in mode=off can still display them.
    """
    summary = summarize(report)
    save("summary.json", json.dumps(summary, indent=1))
    chunks = qrcodec.encode(summary)
    save("qr-chunks.txt", "\n".join(chunks) + "\n")
    write_file("/tmp/bc250-chunks.txt", "\n".join(chunks) + "\n")
    write_file("/tmp/bc250-verdict.txt", "\n".join(report["verdict"]) + "\n")
    return chunks


def main():
    mode = kernel_mode()
    test = test_override()
    results = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "results")
    with open(os.path.join(HERE, "probes.json")) as f:
        probes = json.load(f)
    try:
        out_dir = next_run_dir(results)
    except OSError:
        out_dir = next_run_dir("/tmp/bc250-results")  # USB not writable: keep going, QR still works
    if test:
        mode = "readonly"  # the hook reads someone else's BAR: no writes, no driver load
    log(f"BC-250 diag: mode={mode} probes={probes['id']} results={out_dir}")

    def save(name, text):
        try:
            with open(os.path.join(out_dir, name), "w") as f:
                f.write(text)
                f.flush()
                os.fsync(f.fileno())
            os.sync()
        except OSError as e:
            log(f"  (could not save {name}: {e})")

    report = {
        "summary_version": SUMMARY_VERSION,
        "probes_id": probes["id"],
        "mode": mode,
        "kernel": os.uname().release,
        "dmi": {k: read_text(f"/sys/class/dmi/id/{k}") for k in ("board_vendor", "board_name", "bios_version", "bios_date")},
    }
    bar = None
    bar_index = test["bar"] if test else 5
    dev = test["dev"] if test else find_gpu()
    if test:
        report["test_hook"] = test
        log(f"  TEST HOOK: {test['dev']} BAR{test['bar']}, forced readonly - this is not a BC-250 run")
    if not dev:
        report["error"] = "no AMD GPU on the PCI bus"
        log("  " + report["error"])
    else:
        report["pci"] = pci_info(dev)
        log(f"  device {report['pci']['id']} at {report['pci']['addr']}, bound driver: '{report['pci']['driver']}'")
        if ensure_memory_decoding(dev, report["pci"]):
            report["pci"]["enabled_by_us"] = True
        try:
            bar = Bar(dev, bar_index, readonly=bool(test))
        except (OSError, ValueError, IndexError) as e:
            # A driver that has claimed the BAR makes this mmap fail with EINVAL on a kernel built
            # with strict devmem, which is the usual reason - hence the blacklist in grub.cfg.
            claimed = f" (driver '{report['pci']['driver']}' has claimed it)" if report["pci"]["driver"] else ""
            report["error"] = f"BAR{bar_index} mmap failed: {e}{claimed}"
            log("  " + report["error"])
        if bar is not None:
            log(f"  phase A: raw BAR{bar_index} probe ({bar.size} bytes, no GPU driver loaded)")
            pristine = not report["pci"]["driver"]
            report["A"] = phase_a(bar, probes, allow_writes=(mode != "readonly" and pristine))
            report["A"]["pristine"] = pristine
        report["verdict"] = verdict(probes, report.get("A"), None, test)
        save("report.json", json.dumps(report, indent=1))
        emit_chunks(report, save)
        if mode == "full":
            log("  phase B: loading amdgpu (can take a minute)")
            report["B"] = phase_b(dev, bar, probes, save)

    report["verdict"] = verdict(probes, report.get("A"), report.get("B"), test)
    save("report.json", json.dumps(report, indent=1))
    chunks = emit_chunks(report, save)
    log(f"  done: {len(chunks)} QR code(s), results in {out_dir}")


if __name__ == "__main__":
    main()
