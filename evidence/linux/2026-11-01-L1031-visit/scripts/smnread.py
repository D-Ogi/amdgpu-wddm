#!/usr/bin/env python3
"""L49: read SMN registers (32-bit) by three paths and decode the PCIe link fields. Reads only.

  smnread.py <table> gpu       amdgpu debugfs amdgpu_regs_pcie (needs amdgpu loaded). The kernel does
                               RREG32_PCIE = write GPU PCIE_INDEX2, read PCIE_DATA2, under its own lock.
  smnread.py <table> dbgfs     /sys/kernel/debug/x86/amd_smn/{address,value} (needs the boot parameter
                               amd_smn_debugfs_enable; the kernel takes smn_mutex). Writes go to 'address'
                               only, never to 'value'.
  smnread.py <table> host-raw  host bridge 0000:00:00.0 config space: SMN index 0x60, SMN data 0x64 (the pair
                               of arch/x86/kernel/amd_node.c). The only write is the index register; the
                               original index value is put back at the end. No kernel lock: run it only with
                               k10temp unloaded and amdgpu not loaded (l49.sh checks this).
Option: --repeat-state N reads BIFP0_PCIE_LC_STATE0 N times (LTSSM stability).
Table lines: '<NAME> 0x<SMN byte address>'; '#' starts a comment."""
import glob
import os
import struct
import sys
import time

WIDTH = {0: 0, 1: 1, 2: 2, 3: 4, 4: 8, 5: 12, 6: 16}           # smu_v11_0.h link_width[]
RATE = {0: "2.5 GT/s", 1: "5.0 GT/s", 2: "8.0 GT/s", 3: "16.0 GT/s"}
PCIE_SPEED = {1: "2.5 GT/s", 2: "5.0 GT/s", 3: "8.0 GT/s", 4: "16.0 GT/s", 5: "32.0 GT/s"}


class GpuPath:
    def __init__(self):
        paths = sorted(glob.glob('/sys/kernel/debug/dri/*/amdgpu_regs_pcie'))
        if not paths:
            sys.exit('no amdgpu_regs_pcie: amdgpu not loaded or debugfs not mounted')
        self.name = paths[0]
        self.fd = os.open(paths[0], os.O_RDONLY)

    def read(self, addr):
        return struct.unpack('<I', os.pread(self.fd, 4, addr))[0]

    def close(self):
        os.close(self.fd)


class DbgfsPath:
    D = '/sys/kernel/debug/x86/amd_smn'

    def __init__(self):
        if not os.path.exists(self.D + '/address'):
            sys.exit(f'{self.D} absent: boot with amd_smn_debugfs_enable (kernel 6.18.52 has it, System.map amd_smn_enable_dfs)')
        self.name = self.D
        with open(self.D + '/node', 'w') as f:
            f.write('0\n')

    def read(self, addr):
        with open(self.D + '/address', 'w') as f:
            f.write(f'0x{addr:08x}\n')
        with open(self.D + '/value') as f:
            s = f.read().strip()
        print(f'#   dbgfs raw value text {s!r}')
        # amd_node.c prints the value with "0x%08x"; anything else is reported raw and parsed as hex.
        return int(s, 16)

    def close(self):
        pass


class HostRawPath:
    CFG = '/sys/bus/pci/devices/0000:00:00.0/config'

    def __init__(self):
        if os.path.exists('/sys/module/amdgpu') or os.path.exists('/sys/module/k10temp'):
            sys.exit('host-raw refused: amdgpu or k10temp is loaded (the kernel SMN users must be absent)')
        self.name = self.CFG + ' index 0x60 data 0x64'
        self.fd = os.open(self.CFG, os.O_RDWR)
        vid, did = struct.unpack('<HH', os.pread(self.fd, 4, 0))
        if vid != 0x1022:
            sys.exit(f'00:00.0 is {vid:04x}:{did:04x}, not AMD')
        self.saved = os.pread(self.fd, 4, 0x60)

    def read(self, addr):
        os.pwrite(self.fd, struct.pack('<I', addr), 0x60)
        return struct.unpack('<I', os.pread(self.fd, 4, 0x64))[0]

    def close(self):
        os.pwrite(self.fd, self.saved, 0x60)
        os.close(self.fd)


def decode(name, v):
    if v in (0xFFFFFFFF,):
        return 'all ones (no target)'
    if name.endswith('VENDOR_DEVICE_ID'):
        return f'vendor {v & 0xFFFF:04x} device {v >> 16:04x}'
    if name.endswith('_LC_LINK_WIDTH_CNTL'):
        rd = (v >> 4) & 7
        return f'LC_LINK_WIDTH_RD {rd} = x{WIDTH.get(rd, "?")}; LC_LINK_WIDTH {v & 7}; UPCONFIGURE_CAPABLE {(v >> 20) & 1}'
    if name.endswith('_LC_SPEED_CNTL'):
        cur = (v >> 14) & 3
        return (f'LC_CURRENT_DATA_RATE {cur} = {RATE[cur]}; DATA_RATE_ADVERTISED {(v >> 26) & 3}; '
                f'OTHER_SIDE_SUPPORTS gen2 {(v >> 20) & 1} gen3 {(v >> 22) & 1} gen4 {(v >> 24) & 1}; '
                f'GEN2_EN_STRAP {v & 1} GEN3_EN_STRAP {(v >> 1) & 1}; SPEED_CHANGE_ATTEMPT_FAILED {(v >> 13) & 1}')
    if name.endswith('_LC_STATE0'):
        return (f'LC_CURRENT_STATE 0x{v & 0x3F:02x} prev1 0x{(v >> 8) & 0x3F:02x} '
                f'prev2 0x{(v >> 16) & 0x3F:02x} prev3 0x{(v >> 24) & 0x3F:02x}')
    if name.endswith('LINK_STATUS_CNTL'):
        sta = v >> 16
        return (f'LnkSta speed {PCIE_SPEED.get(sta & 0xF, sta & 0xF)} width x{(sta >> 4) & 0x3F} '
                f'training {(sta >> 11) & 1} DLLActive {(sta >> 13) & 1}; LnkCtl 0x{v & 0xFFFF:04x}')
    if name.endswith('LINK_CAP'):
        return f'LnkCap max speed {PCIE_SPEED.get(v & 0xF, v & 0xF)} max width x{(v >> 4) & 0x3F} port {v >> 24}'
    if name.endswith('LINK_STATUS2_CNTL2'):
        return f'LnkCtl2 target speed {PCIE_SPEED.get(v & 0xF, v & 0xF)}; LnkSta2 0x{v >> 16:04x}'
    if name.endswith('LINK_CAP2'):
        return f'supported speeds vector 0x{(v >> 1) & 0x7F:02x}'
    return ''


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    table, mode = sys.argv[1], sys.argv[2]
    repeat = int(sys.argv[sys.argv.index('--repeat-state') + 1]) if '--repeat-state' in sys.argv else 0
    path = {'gpu': GpuPath, 'dbgfs': DbgfsPath, 'host-raw': HostRawPath}[mode]()
    rows = []
    for line in open(table):
        p = line.split('#')[0].split()
        if len(p) == 2:
            rows.append((p[0], int(p[1], 16)))
    print(f'# mode={mode} path={path.name} utc={time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())} rows={len(rows)}')
    try:
        for name, addr in rows:
            try:
                v = path.read(addr)
                print(f'{name:36s} 0x{addr:08X} 0x{v:08X}  {decode(name, v)}')
            except OSError as e:
                print(f'{name:36s} 0x{addr:08X} ERR {e}')
        for i in range(repeat):
            v = path.read(0x11140294)
            print(f'LC_STATE0[{i}] 0x{v:08X} current 0x{v & 0x3F:02x}')
            time.sleep(0.05)
    finally:
        path.close()


if __name__ == '__main__':
    main()
