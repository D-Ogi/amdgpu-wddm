#!/bin/sh
# One page-flip run under the display register trace (wishlist L20, L23, L24). Read-only towards
# registers except what amdgpu itself writes for the flip. Usage: flip_trace.sh <tag> [modetest args...]
T=/sys/kernel/tracing
OUT=/tmp/lx/out/flip; mkdir -p $OUT
TAG=${1:-flip}; shift
MT=${*:-"-M amdgpu -s 64@58:1920x1200 -v"}
DMU=/tmp/lx/dmulists.json
dmu() { python3 /tmp/lx/regs2.py $DMU state 2>/dev/null; }
echo "== $TAG: modetest $MT"
dmu > $OUT/$TAG-dmu-before.txt
grep -E "amdgpu|DCE|dce" /proc/interrupts > $OUT/$TAG-irq-before.txt
echo > $T/trace
echo 0 > $T/tracing_on
for e in amdgpu_dm/amdgpu_dc_wreg amdgpu_dm/amdgpu_dc_rreg amdgpu_dm/amdgpu_dm_atomic_commit_tail_begin \
         amdgpu_dm/amdgpu_dm_atomic_commit_tail_finish amdgpu_dm/amdgpu_dm_dc_pipe_state amdgpu_dm/amdgpu_dm_atomic_check_begin \
         amdgpu_dm/amdgpu_dm_atomic_check_finish amdgpu/amdgpu_iv amdgpu/amdgpu_bo_create amdgpu/amdgpu_vm_bo_map; do
    echo 1 > $T/events/$e/enable 2>/dev/null || echo "no event $e"
done
echo 32768 > $T/buffer_size_kb
dmesg -c > /dev/null 2>&1
echo 1 > $T/tracing_on
echo "flip $TAG start" > $T/trace_marker
# sample the flip-side registers at ~20 Hz while modetest runs
( for i in $(seq 1 80); do python3 /tmp/lx/regs2.py $DMU state 2>/dev/null | grep -E "PRIMARY_SURFACE_ADDRESS |SURFACE_INUSE |FLIP_CONTROL|MASTER_UPDATE_LOCK|SURFACE_FLIP_INTERRUPT|DCHUBP_CNTL" | grep -E "HUBP0|HUBPREQ0|OTG0" | tr '\n' ' '; echo; sleep 0.05; done ) > $OUT/$TAG-dmu-sampled.txt 2>&1 &
S=$!
(sleep ${MT_SECS:-4}; sleep 2) | timeout -s INT ${MT_SECS:-4} modetest $MT > $OUT/$TAG-modetest.txt 2>&1
echo "modetest rc=$?"
wait $S
echo "flip $TAG end" > $T/trace_marker
echo 0 > $T/tracing_on
cat $T/trace > $OUT/$TAG-trace.txt
dmesg > $OUT/$TAG-dmesg.txt
grep -E "amdgpu|DCE|dce" /proc/interrupts > $OUT/$TAG-irq-after.txt
dmu > $OUT/$TAG-dmu-after.txt
for e in $(ls -d $T/events/amdgpu_dm/*/ $T/events/amdgpu/amdgpu_iv $T/events/amdgpu/amdgpu_bo_create $T/events/amdgpu/amdgpu_vm_bo_map); do echo 0 > $e/enable 2>/dev/null; done
echo "trace lines: $(wc -l < $OUT/$TAG-trace.txt)  wreg: $(grep -c amdgpu_dc_wreg $OUT/$TAG-trace.txt)  rreg: $(grep -c amdgpu_dc_rreg $OUT/$TAG-trace.txt)  iv: $(grep -c amdgpu_iv $OUT/$TAG-trace.txt)"
head -30 $OUT/$TAG-modetest.txt
echo "== temp $(cat /sys/class/drm/card1/device/hwmon/hwmon*/temp1_input 2>/dev/null)"
