#!/bin/sh
# Display lane D1 (L8 rest, post-amdgpu): full dcn-all.regs dump through debugfs + pipe check.
# Run AFTER modprobe amdgpu.
set -e
cd /root/disp
echo "# d1-l8-dcnall $(date -u '+%Y-%m-%dT%H:%M:%SZ') uptime=$(cat /proc/uptime | cut -d' ' -f1)"

# Verify amdgpu is loaded
if ! lsmod | grep -q '^amdgpu'; then
    echo "REFUSED: amdgpu is not loaded"
    exit 3
fi

echo "# dump dcn-all.regs"
python3 dcnregs.py dump fbcon dcn-all.regs > /root/disp/l8-dcnall.txt 2>&1

echo "# pipes"
python3 dcnregs.py pipes > /root/disp/l8-pipes.txt 2>&1
cat /root/disp/l8-pipes.txt

echo "# d1 done $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
