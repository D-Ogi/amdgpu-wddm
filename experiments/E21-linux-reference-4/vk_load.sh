#!/bin/sh
# Vulkan load through RADV on KMS (wishlist L10: SMU traffic and power under load).
# Usage: vk_load.sh <tag> [seconds] [vkcube args...]; stops early above 84 C.
T=/sys/kernel/tracing
OUT=/tmp/lx/out/load; mkdir -p $OUT
TAG=${1:-vk}; SECS=${2:-15}; shift 2 2>/dev/null
ARGS=${*:-"--wsi display"}
D=/sys/class/drm/card1/device
H=$(ls -d $D/hwmon/hwmon* | head -1)
export XDG_RUNTIME_DIR=/tmp/xdg; mkdir -p $XDG_RUNTIME_DIR
echo "== $TAG: vkcube $ARGS for $SECS s"
echo > $T/trace; echo 1 > $T/tracing_on
for e in amdgpu/amdgpu_cs_ioctl amdgpu/amdgpu_sched_run_job amdgpu/amdgpu_vm_grab_id amdgpu/amdgpu_vm_flush amdgpu/amdgpu_bo_create; do
    echo 1 > $T/events/$e/enable 2>/dev/null || echo "no event $e"
done
grep -E "amdgpu" /proc/interrupts > $OUT/$TAG-irq-before.txt
cat $D/pp_dpm_sclk $D/pp_dpm_mclk > $OUT/$TAG-dpm-before.txt
timeout -s INT $SECS vkcube $ARGS > $OUT/$TAG-vkcube.txt 2>&1 &
P=$!
i=0
while kill -0 $P 2>/dev/null && [ $i -lt $((SECS + 5)) ]; do
    t=$(cat $H/temp1_input); p=$(cat $H/power1_input 2>/dev/null); f=$(cat $H/freq1_input 2>/dev/null); v=$(cat $H/in0_input 2>/dev/null)
    s=$(grep '\*' $D/pp_dpm_sclk | tr -d '\n'); m=$(grep -c smumsg $T/trace)
    echo "$i temp=$t mW=$p sclk=$f mV=$v dpm=[$s] smumsg=$m"
    if [ "$t" -gt 84000 ]; then echo "ABORT: temperature $t"; kill -INT $P; break; fi
    i=$((i + 1)); sleep 1
done | tee $OUT/$TAG-samples.txt
wait $P; echo "vkcube rc=$?"
echo 0 > $T/tracing_on
cat $T/trace > $OUT/$TAG-trace.txt
for e in amdgpu/amdgpu_cs_ioctl amdgpu/amdgpu_sched_run_job amdgpu/amdgpu_vm_grab_id amdgpu/amdgpu_vm_flush amdgpu/amdgpu_bo_create; do echo 0 > $T/events/$e/enable 2>/dev/null; done
grep -E "amdgpu" /proc/interrupts > $OUT/$TAG-irq-after.txt
cat $D/pp_dpm_sclk $D/pp_dpm_mclk > $OUT/$TAG-dpm-after.txt
dmesg | tail -20 > $OUT/$TAG-dmesg-tail.txt
echo "trace lines: $(wc -l < $OUT/$TAG-trace.txt)  cs: $(grep -c amdgpu_cs_ioctl $OUT/$TAG-trace.txt)  jobs: $(grep -c sched_run_job $OUT/$TAG-trace.txt)  vm_flush: $(grep -c amdgpu_vm_flush $OUT/$TAG-trace.txt)  smumsg: $(grep -c smumsg $OUT/$TAG-trace.txt)"
grep smumsg $OUT/$TAG-trace.txt | sed -E "s/.*smumsg: [^ ]+ //" | sort | uniq -c | sort -rn | head
head -5 $OUT/$TAG-vkcube.txt
