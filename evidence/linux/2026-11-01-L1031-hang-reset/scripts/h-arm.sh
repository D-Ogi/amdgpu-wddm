#!/bin/sh
# Hang lane: add the reset-path kprobes (bchang group) next to the existing ones and route them to their own
# trace instance, streamed by the caller with dd from instances/bchang/trace_pipe.
T=/sys/kernel/tracing; I=$T/instances/bchang
mkdir -p $I 2>/dev/null
ok=0; bad=0
while IFS= read -r l; do
  [ -z "$l" ] && continue
  if echo "$l" >> $T/kprobe_events 2>/dev/null; then ok=$((ok+1)); else bad=$((bad+1)); echo "REFUSED: $l"; fi
done < /root/hang/hang-probes.txt
echo 1 > $I/events/bchang/enable
echo 4096 > $I/buffer_size_kb
echo 1 > $I/tracing_on
echo "bchang probes added $ok refused $bad; instance on $(cat $I/tracing_on)"
echo "params: lockup_timeout=[$(cat /sys/module/amdgpu/parameters/lockup_timeout)] gpu_recovery=$(cat /sys/module/amdgpu/parameters/gpu_recovery)"
P=/sys/bus/pci/devices/0000:01:00.0
for f in gfx_reset_mask compute_reset_mask sdma_reset_mask; do echo "$f: $(cat $P/$f 2>&1)"; done
grep -i -E 'sdma|mec|me feature|rlc feature' /sys/kernel/debug/dri/0000:01:00.0/amdgpu_firmware_info
