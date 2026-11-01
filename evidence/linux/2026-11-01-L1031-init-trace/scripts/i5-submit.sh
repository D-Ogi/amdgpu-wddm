#!/bin/sh
# i5: minimal submission (NOP + WRITE_DATA to SCRATCH_REG0) under the init trace, with ring
# dumps before and after. Requires amdgpu loaded (i2 done).
OUT=/root/init-out; mkdir -p $OUT
T=/sys/kernel/tracing
DBG=$(ls -d /sys/kernel/debug/dri/*/ 2>/dev/null | while read d; do [ -e "$d/amdgpu_ring_sdma0" ] && echo "$d"; done | head -1)
date -u > $OUT/i5-start.txt; cat /proc/uptime >> $OUT/i5-start.txt

# Re-arm the trace (stopped in i2)
echo > $T/trace
echo 1 > $T/tracing_on

# Ring state before
for r in sdma0 sdma1 gfx_0.0.0 comp_1.0.0 kiq_0.2.1.0; do
  timeout 3 cat ${DBG}amdgpu_ring_$r > $OUT/i5-ring-${r}-before.txt 2>&1
done

# Submit: the lead provides i5-submit.py (adapted from E13 boot 4 dispatch.py)
if [ -f /root/init/i5-submit.py ]; then
  timeout 60 python3 /root/init/i5-submit.py > $OUT/i5-submit-result.txt 2>&1
  echo "submit exit $?" >> $OUT/i5-submit-result.txt
else
  echo "i5-submit.py not present; skipping submission" > $OUT/i5-submit-result.txt
fi

# Ring state after
for r in sdma0 sdma1 gfx_0.0.0 comp_1.0.0 kiq_0.2.1.0; do
  timeout 3 cat ${DBG}amdgpu_ring_$r > $OUT/i5-ring-${r}-after.txt 2>&1
done

echo 0 > $T/tracing_on
cat $T/trace > $OUT/i5-trace.txt
cat $T/kprobe_profile >> $OUT/i5-kprobe-profile.txt 2>&1
date -u >> $OUT/i5-start.txt; cat /proc/uptime >> $OUT/i5-start.txt
echo "i5 done: $(wc -l < $OUT/i5-trace.txt) trace lines"
