#!/bin/sh
# Linux side, read only on the GPU. Step raw: reads before amdgpu through resource5. Step driver: modprobe amdgpu,
# GFXOFF off through amdgpu's own debugfs control, then the controls, the GRBM window and the controls again
# through debugfs amdgpu_regs. Step holes: the unnamed dwords of the GC window 0x2084-0x2FFF.
# Usage: session.sh raw|driver|holes   (the files are in /root/grbm)
set -u
H=/root/grbm
D=/sys/bus/pci/devices/0000:01:00.0
tctl() { for h in /sys/class/hwmon/hwmon*; do [ "$(cat $h/name)" = k10temp ] && echo "Tctl $(($(cat $h/temp1_input) / 1000)) C"; done; }
echo "# utc $(date -u +%Y-%m-%dT%H:%M:%SZ) uptime $(cut -d' ' -f1 /proc/uptime) $(uname -r) $(tctl)"
case "$1" in
raw)
    lsmod | grep -q '^amdgpu' && { echo "REFUSED: amdgpu is loaded already"; exit 2; }
    echo "enable before $(cat $D/enable)"; [ "$(cat $D/enable)" = 0 ] && echo 1 > $D/enable
    python3 -u $H/regstream.py raw $H/raw.regs pre-amdgpu ;;
driver)
    lsmod | grep -q '^amdgpu' || { modprobe amdgpu; echo "modprobe exit $?"; sleep 5; }
    DBG=$(dirname $(ls /sys/kernel/debug/dri/*/amdgpu_regs | head -1))
    echo "debugfs $DBG"
    python3 -c "import struct;print('gfxoff before', struct.unpack('<I', open('$DBG/amdgpu_gfxoff','rb').read(4))[0])" 2>&1
    python3 -c "open('$DBG/amdgpu_gfxoff','wb').write(b'\0\0\0\0')" 2>&1 && echo "gfxoff disabled through debugfs"
    python3 -c "import struct;print('gfxoff status', struct.unpack('<I', open('$DBG/amdgpu_gfxoff_status','rb').read(4))[0])" 2>&1
    python3 -u $H/regstream.py debugfs $H/controls.regs controls-1
    python3 -u $H/regstream.py debugfs $H/grbm.regs grbm-window
    python3 -u $H/regstream.py debugfs $H/controls.regs controls-2
    dmesg | tail -n 15 ;;
holes)
    python3 -u $H/regstream.py debugfs $H/gc-holes.regs gc-holes
    python3 -u $H/regstream.py debugfs $H/controls.regs controls-3
    dmesg | tail -n 15 ;;
esac
echo "# end utc $(date -u +%Y-%m-%dT%H:%M:%SZ) $(tctl)"
