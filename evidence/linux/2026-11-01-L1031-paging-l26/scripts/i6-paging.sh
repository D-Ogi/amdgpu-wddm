#!/bin/sh
# i6: trigger a VRAM<->GTT paging copy and capture the SDMA ring dump + bo_move events.
# Requires amdgpu loaded (i2 done).
OUT=/root/init-out; mkdir -p $OUT
T=/sys/kernel/tracing
DBG=$(ls -d /sys/kernel/debug/dri/*/ 2>/dev/null | while read d; do [ -e "$d/amdgpu_ring_sdma0" ] && echo "$d"; done | head -1)
date -u > $OUT/i6-start.txt; cat /proc/uptime >> $OUT/i6-start.txt

# Enable amdgpu tracepoints for bo_move
echo > $T/trace
if [ -d $T/events/amdgpu ]; then
  echo 1 > $T/events/amdgpu/amdgpu_bo_move/enable 2>/dev/null || true
fi
echo 1 > $T/tracing_on

# Ring state before
for r in sdma0 sdma1; do
  timeout 3 cat ${DBG}amdgpu_ring_$r > $OUT/i6-ring-${r}-before.txt 2>&1
done

# Trigger migration: the lead provides i6-paging.py
if [ -f /root/init/i6-paging.py ]; then
  timeout 60 python3 /root/init/i6-paging.py > $OUT/i6-paging-result.txt 2>&1
  echo "paging exit $?" >> $OUT/i6-paging-result.txt
else
  echo "i6-paging.py not present; skipping paging test" > $OUT/i6-paging-result.txt
fi

# Ring state after
for r in sdma0 sdma1; do
  timeout 3 cat ${DBG}amdgpu_ring_$r > $OUT/i6-ring-${r}-after.txt 2>&1
done

echo 0 > $T/tracing_on
cat $T/trace > $OUT/i6-trace.txt
date -u >> $OUT/i6-start.txt; cat /proc/uptime >> $OUT/i6-start.txt
echo "i6 done"
