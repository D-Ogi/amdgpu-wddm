#!/bin/sh
# Display lane D0 (L8 rest, pre-amdgpu): read gop-d and gop-e tables through BAR5.
# Run BEFORE modprobe amdgpu. Run through stream.py on the dev PC so output survives a hang.
# Expects: bar5dump.py, dcnregs.py, *.regs and fields.txt pushed to /root/disp
set -e
cd /root/disp
echo "# d0-l8-gop-de $(date -u '+%Y-%m-%dT%H:%M:%SZ') uptime=$(cat /proc/uptime | cut -d' ' -f1)"

# Refuse if amdgpu is loaded
if lsmod | grep -q '^amdgpu'; then
    echo "REFUSED: amdgpu is loaded; BAR5 reads require no bound driver"
    exit 3
fi

python3 -u bar5dump.py --out /root/disp/l8-gop-de.txt gop-d.regs gop-e.regs
echo "# d0 done $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
