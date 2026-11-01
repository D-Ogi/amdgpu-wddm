#!/bin/sh
# Display lane D2 (L22): sample underflow status during format-change flips.
# Risk R0: read only. The modetest flips are normal driver operations.
# Run AFTER modprobe amdgpu.
set -e
cd /root/disp
echo "# d2-l22-underflow $(date -u '+%Y-%m-%dT%H:%M:%SZ') uptime=$(cat /proc/uptime | cut -d' ' -f1)"

OUT=/root/disp/l22-underflow.txt

# Start the sampler in the background (10 s, change-log only).
# Watch: HUBP0 and HUBP3 control (has HUBP_UNDERFLOW_STATUS), OTG0 status, DCHUBBUB status.
# Phase: scan position and frame count.
python3 -u dcnsample.py \
    --secs 10 \
    --watch HUBP0_DCHUBP_CNTL,HUBP3_DCHUBP_CNTL,OTG0_OTG_STATUS,DCHUBBUB_CTRL_STATUS \
    --phase OTG0_OTG_STATUS_POSITION,OTG0_OTG_STATUS_FRAME_COUNT \
    --tag l22-underflow \
    > "$OUT" 2>&1 &
SPID=$!

# Give the sampler a moment to start
sleep 1

# Flip 1: XR24 (4 bpp, pitch 1920*4 = 7680)
echo "# flip XR24 $(date -u '+%Y-%m-%dT%H:%M:%SZ')" >> "$OUT"
sleep 2 | modetest -M amdgpu -s 64@58:1920x1200 -P 45@58:1920x1200@XR24 2>&1 | head -5 >> "$OUT" || true
sleep 1

# Flip 2: XB4H (FP16, 8 bpp, pitch 1920*8 = 15360 - doubles the fetch bandwidth)
echo "# flip XB4H $(date -u '+%Y-%m-%dT%H:%M:%SZ')" >> "$OUT"
sleep 2 | modetest -M amdgpu -s 64@58:1920x1200 -P 45@58:1920x1200@XB4H 2>&1 | head -5 >> "$OUT" || true
sleep 1

# Flip 3: back to XR24
echo "# flip XR24 $(date -u '+%Y-%m-%dT%H:%M:%SZ')" >> "$OUT"
sleep 2 | modetest -M amdgpu -s 64@58:1920x1200 -P 45@58:1920x1200@XR24 2>&1 | head -5 >> "$OUT" || true

# Wait for the sampler to finish
wait $SPID || true

# Decode the underflow status from the last C line of each watch register
echo "" >> "$OUT"
echo "# HUBP_UNDERFLOW_STATUS is in bits 0x1C of DCHUBP_CNTL" >> "$OUT"
echo "# d2 done $(date -u '+%Y-%m-%dT%H:%M:%SZ')" >> "$OUT"
cat "$OUT"
