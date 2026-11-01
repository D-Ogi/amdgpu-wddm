#!/bin/sh
# L26: trace one paging round trip (l26-migrate.py) in its own trace instance and keep the trace with the ring dumps.
T=/sys/kernel/tracing; I=$T/instances/l26; OUT=/root/l26-out
mkdir -p $OUT /root/x1; cp -n /media/usb/bc250/tools/x1/* /root/x1/ 2>/dev/null
mount | grep -q debugfs || mount -t debugfs none /sys/kernel/debug
mkdir -p $I
for l in \
 "p:l26/copy_buf amdgpu:amdgpu_copy_buffer ring=%di:x64 src=%si:x64 dst=%dx:x64 bytes=%cx:u32" \
 "r:l26/copy_buf_ret amdgpu:amdgpu_copy_buffer rc=\$retval:s32" \
 "p:l26/ib_sched amdgpu:amdgpu_ib_schedule ring=%di:x64 n=%si:u32 len=+8(%dx):u32 va=+16(%dx):x64 flags=+32(%dx):x32 dw=+0(+24(%dx)):x32[64]" \
 "p:l26/db64 amdgpu:amdgpu_mm_wdoorbell64 idx=%si:u32 v=%dx:x64" \
 "p:l26/db32 amdgpu:amdgpu_mm_wdoorbell idx=%si:u32 v=%dx:x32"; do
  echo "$l" >> $T/kprobe_events || echo "REFUSED: $l"
done
echo 1 > $I/events/l26/enable
for e in amdgpu_bo_move amdgpu_sched_run_job amdgpu_bo_create; do echo 1 > $I/events/amdgpu/$e/enable; done
echo 8192 > $I/buffer_size_kb
echo > $I/trace; echo 1 > $I/tracing_on
cat $DBG 2>/dev/null
timeout 120 python3 -u /root/ul/l26-migrate.py --ref /root/x1 --out $OUT > $OUT/l26-console.txt 2>&1
echo "native exit $?" >> $OUT/l26-console.txt
echo 0 > $I/tracing_on
cat $I/trace > $OUT/l26-trace.txt
echo 0 > $I/events/l26/enable
for e in amdgpu_bo_move amdgpu_sched_run_job amdgpu_bo_create; do echo 0 > $I/events/amdgpu/$e/enable; done
grep -v '^BO ' $OUT/l26-console.txt
echo "trace lines $(grep -vc '^#' $OUT/l26-trace.txt):"; grep -v '^#' $OUT/l26-trace.txt | awk '{print $5}' | sort | uniq -c
