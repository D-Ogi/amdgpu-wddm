#!/bin/sh
# i2: load amdgpu with the init trace armed, then save the trace.
OUT=/root/init-out; mkdir -p $OUT
T=/sys/kernel/tracing
date -u > $OUT/modprobe-start.txt; cat /proc/uptime >> $OUT/modprobe-start.txt
timeout 90 modprobe amdgpu
echo "modprobe exit $?" >> $OUT/modprobe-start.txt
sleep 5
echo 0 > $T/tracing_on
cat $T/trace > $OUT/init-trace.txt
cat $T/kprobe_profile > $OUT/kprobe_profile.txt
date -u >> $OUT/modprobe-start.txt; cat /proc/uptime >> $OUT/modprobe-start.txt
dmesg > $OUT/dmesg-init.txt
lines=$(wc -l < $OUT/init-trace.txt)
echo "trace: $lines lines"
grep -c 'psp_cmd:' $OUT/init-trace.txt || true
echo "modprobe result: $(tail -3 $OUT/modprobe-start.txt)"
