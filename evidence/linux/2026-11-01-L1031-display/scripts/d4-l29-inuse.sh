#!/bin/sh
# Display lane D4 (L29): sample DCSURF_SURFACE_INUSE through debugfs during active flips.
# Risk R0: read only.
# Run AFTER modprobe amdgpu.
set -e
cd /root/disp
echo "# d4-l29-inuse $(date -u '+%Y-%m-%dT%H:%M:%SZ') uptime=$(cat /proc/uptime | cut -d' ' -f1)"

OUT=/root/disp/l29-inuse.txt

# Start the sampler (10 s, change-log, high rate to catch flip transitions).
python3 -u dcnsample.py \
    --secs 10 \
    --max-hz 10000 \
    --watch HUBPREQ0_DCSURF_SURFACE_INUSE,HUBPREQ0_DCSURF_SURFACE_INUSE_HIGH,HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS,HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH,HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE,HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH \
    --phase OTG0_OTG_STATUS_POSITION,OTG0_OTG_STATUS_FRAME_COUNT \
    --tag l29-inuse \
    > "$OUT" 2>&1 &
SPID=$!

sleep 1

# Run modetest with -v (vsynced page flips) to produce address alternation.
# modetest -v does page-flip between two buffers on every vblank.
echo "# modetest -v start $(date -u '+%Y-%m-%dT%H:%M:%SZ')" >> "$OUT"
timeout 8 modetest -M amdgpu -s 64@58:1920x1200 -P 45@58:1920x1200@XR24 -v 2>&1 | tail -20 >> "$OUT" || true

wait $SPID || true

echo "# d4 done $(date -u '+%Y-%m-%dT%H:%M:%SZ')" >> "$OUT"
cat "$OUT"
