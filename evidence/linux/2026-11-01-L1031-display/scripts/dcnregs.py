#!/usr/bin/env python3
"""Display lane, lab side: named DCN 2.0.1 registers on unit A (Alpine, python3 standard library only).

Tables next to this file, generated on the development PC by gen-tables.py with tools/regcalc:
  *.regs      '<NAME> 0x<byte offset>'   (dcn_2_0_1_offset.h, clk_11_0_1_offset.h)
  fields.txt  '<REG> <FIELD> 0x<mask>'   (dcn_2_0_1_sh_mask.h)
No offset or mask is typed by hand in any lab script of this lane: registers and fields go by name.

Dbg   amdgpu's debugfs register file (amdgpu_regs): pread/pwrite of 4 bytes at the byte offset. A Dbg opened
      for writing accepts writes only to the registers named in its allow list, and logs every write with the
      value it replaced.
Bar5  sysfs resource5 before amdgpu is loaded: single 32-bit reads (the read path of
      tools/diagusb/payload/bc250/diag.py Bar, mapped the same way). Refused while a driver is bound to 01:00.0.

CLI (debugfs, amdgpu loaded):
  dcnregs.py dump <label> <table> [table...]   every register of the tables, one line each
  dcnregs.py get <NAME> [NAME...]              value and decoded fields
  dcnregs.py pipes                             which HUBP/MPCC/OTG instances carry a plane (L30 check)
"""
import ctypes
import glob
import mmap
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
# Lab: the tables sit next to the scripts (linux-xfer.py put is flat). Development PC: in tables/.
DIRS = [HERE, os.path.join(HERE, "tables")]
PCI = "/sys/bus/pci/devices/0000:01:00.0"


def load_table(path):
    out = []
    for line in open(path):
        p = line.split()
        if len(p) == 2:
            out.append((p[0], int(p[1], 16)))
    return out


_NAMES = None
_FIELDS = None


def names():
    """NAME -> byte offset over every *.regs file next to this script."""
    global _NAMES
    if _NAMES is None:
        _NAMES = {}
        for d in DIRS:
            for t in sorted(glob.glob(os.path.join(d, "*.regs"))):
                for n, o in load_table(t):
                    _NAMES[n] = o
    return _NAMES


def fields():
    """REG -> [(FIELD, mask)] from fields.txt."""
    global _FIELDS
    if _FIELDS is None:
        _FIELDS = {}
        for d in DIRS:
            p = os.path.join(d, "fields.txt")
            if os.path.exists(p):
                for line in open(p):
                    r, f, m = line.split()
                    _FIELDS.setdefault(r, []).append((f, int(m, 16)))
                break
    return _FIELDS


def off(name):
    if isinstance(name, int):
        return name
    try:
        return names()[name]
    except KeyError:
        raise KeyError("register %s is not in the tables next to %s" % (name, __file__))


def mask(reg, field):
    for f, m in fields().get(reg, ()):
        if f == field:
            return m
    raise KeyError("field %s.%s is not in fields.txt" % (reg, field))


def shift(m):
    return (m & -m).bit_length() - 1


def fget(reg, field, value):
    m = mask(reg, field)
    return (value & m) >> shift(m)


def fput(reg, value, **kv):
    """value with the named fields replaced."""
    for f, v in kv.items():
        m = mask(reg, f)
        value = (value & ~m) | ((v << shift(m)) & m)
    return value & 0xFFFFFFFF


def decode(reg, value, all_fields=False):
    out = []
    for f, m in fields().get(reg, ()):
        v = (value & m) >> shift(m)
        if v or all_fields:
            out.append("%s=%s" % (f, hex(v) if v > 9 else v))
    return " ".join(out)


class Dbg:
    """amdgpu debugfs register file. allow: register names that w()/rmw() may write (empty = read only)."""

    def __init__(self, allow=()):
        paths = sorted(glob.glob("/sys/kernel/debug/dri/*/amdgpu_regs"))
        if not paths:
            raise OSError("no amdgpu_regs in debugfs (is amdgpu loaded and debugfs mounted?)")
        self.path = paths[0]
        self.allow = set(allow)
        self.fd = os.open(self.path, os.O_RDWR if self.allow else os.O_RDONLY)
        self.wlog = []

    def r(self, name):
        return struct.unpack("<I", os.pread(self.fd, 4, off(name)))[0]

    def w(self, name, value):
        if name not in self.allow:
            raise PermissionError("write to %s refused: not in this step's allow list" % name)
        old = self.r(name)
        os.pwrite(self.fd, struct.pack("<I", value & 0xFFFFFFFF), off(name))
        self.wlog.append((time.monotonic_ns(), name, old, value & 0xFFFFFFFF))
        return old

    def rmw(self, name, **kv):
        """Read, replace the named fields, write. Returns (old, new)."""
        old = self.r(name)
        new = fput(name, old, **kv)
        self.w(name, new)
        return old, new

    def close(self):
        os.close(self.fd)


def driver_bound():
    return os.path.exists(PCI + "/driver")


class Bar5:
    """Read-only 32-bit access to BAR5 through sysfs, for the firmware (GOP) state before amdgpu loads."""

    def __init__(self):
        if driver_bound():
            raise OSError("a driver is bound to 01:00.0 (%s): BAR5 reads are refused, use Dbg" %
                          os.path.basename(os.readlink(PCI + "/driver")))
        line = open(PCI + "/resource").read().splitlines()[5]
        start, end, _fl = (int(x, 16) for x in line.split())
        self.size = end - start + 1 if end else 0
        if not self.size:
            raise OSError("BAR5 is not assigned")
        fd = os.open(PCI + "/resource5", os.O_RDWR | os.O_SYNC)
        try:
            # Same mapping as diag.py Bar (the proven read path); this class has no write method.
            self._mm = mmap.mmap(fd, self.size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
        finally:
            os.close(fd)
        self._regs = (ctypes.c_uint32 * (self.size // 4)).from_buffer(self._mm)

    def r(self, name):
        o = off(name)
        if o % 4 or o + 4 > self.size:
            raise ValueError("offset 0x%X outside BAR5 (size 0x%X)" % (o, self.size))
        return self._regs[o >> 2]


def stamp():
    return "utc=%s mono_ns=%d" % (time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), time.monotonic_ns())


PIPE_REGS = ["HUBP%d_HUBP_CLK_CNTL", "HUBP%d_DCHUBP_CNTL", "HUBP%d_DCSURF_SURFACE_CONFIG",
             "HUBP%d_DCSURF_PRI_VIEWPORT_START", "HUBP%d_DCSURF_PRI_VIEWPORT_DIMENSION",
             "HUBPREQ%d_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH", "HUBPREQ%d_DCSURF_PRIMARY_SURFACE_ADDRESS",
             "HUBPREQ%d_DCSURF_SURFACE_PITCH", "MPCC%d_MPCC_TOP_SEL", "MPCC%d_MPCC_BOT_SEL", "MPCC%d_MPCC_OPP_ID",
             "MPCC%d_MPCC_CONTROL", "MPCC%d_MPCC_STATUS"]
OTG_REGS = ["OTG%d_OTG_CONTROL", "ODM%d_OPTC_DATA_SOURCE_SELECT", "ODM%d_OPTC_INPUT_GLOBAL_CONTROL",
            "OPPBUF%d_OPPBUF_CONTROL", "OTG%d_OTG_GLOBAL_SYNC_STATUS"]


def pipes(rd):
    """Print the plane-carrying state of HUBP0-3/MPCC0-3 and OTG0-1, and a one-line verdict:
    'pipes: live=<list> split=<yes|no>'. A live pipe has its HUBP clock on, blank off and an MPCC on an OPP."""
    live = []
    for n in range(4):
        vals = {}
        for t in PIPE_REGS:
            r = t % n
            try:
                vals[r] = rd.r(r)
            except (KeyError, OSError) as e:
                vals[r] = None
                print("%-48s ERR %s" % (r, e))
                continue
            print("%-48s 0x%08X  %s" % (r, vals[r], decode(r, vals[r])))
        clk = vals.get("HUBP%d_HUBP_CLK_CNTL" % n)
        cntl = vals.get("HUBP%d_DCHUBP_CNTL" % n)
        opp = vals.get("MPCC%d_MPCC_OPP_ID" % n)
        if None in (clk, cntl, opp):
            continue
        on = fget("HUBP%d_HUBP_CLK_CNTL" % n, "HUBP_CLOCK_ENABLE", clk)
        blank = fget("HUBP%d_DCHUBP_CNTL" % n, "HUBP_BLANK_EN", cntl)
        oppid = fget("MPCC%d_MPCC_OPP_ID" % n, "MPCC_OPP_ID", opp)
        if on and not blank and oppid != 0xF:
            dim = vals["HUBP%d_DCSURF_PRI_VIEWPORT_DIMENSION" % n]
            live.append("%d(opp%d,%dx%d)" % (n, oppid, dim & 0x3FFF, (dim >> 16) & 0x3FFF))
    for m in range(2):
        for t in OTG_REGS:
            r = t % m
            try:
                v = rd.r(r)
                print("%-48s 0x%08X  %s" % (r, v, decode(r, v)))
            except (KeyError, OSError) as e:
                print("%-48s ERR %s" % (r, e))
    print("pipes: live=%s split=%s" % (",".join(live) or "none", "yes" if len(live) > 1 else "no"))
    return live


def main(argv):
    if not argv:
        sys.exit(__doc__)
    cmd = argv[0]
    rd = Dbg()
    print("# %s %s %s" % (cmd, rd.path, stamp()))
    if cmd == "dump":
        label, tables = argv[1], argv[2:]
        print("# label=%s tables=%s" % (label, ",".join(os.path.basename(t) for t in tables)))
        for t in tables:
            for n, o in load_table(t):
                try:
                    print("%-56s 0x%05X 0x%08X" % (n, o, rd.r(n)))
                except OSError as e:
                    print("%-56s 0x%05X ERR %s" % (n, o, e))
    elif cmd == "get":
        for n in argv[1:]:
            v = rd.r(n)
            print("%-56s 0x%08X  %s" % (n, v, decode(n, v, all_fields=True)))
    elif cmd == "pipes":
        pipes(rd)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
