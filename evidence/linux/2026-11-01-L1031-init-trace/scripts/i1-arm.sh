#!/bin/sh
# i1: arm the init trace before modprobe amdgpu. Must run in GRUB entry 4 (amdgpu blacklisted).
# Inputs in /root/init: probes.txt, filters.txt (from gen_probes.py on the dev PC).
set -e
IN=/root/init
T=/sys/kernel/tracing
mount | grep -q tracefs || mount -t tracefs none $T
# Clear old probes
echo > $T/kprobe_events 2>/dev/null || true
# Write probe definitions
while IFS= read -r line; do
  case "$line" in \#*|"") continue ;; esac
  echo "$line" >> $T/kprobe_events
done < $IN/probes.txt
# Apply filters
while IFS= read -r line; do
  [ -z "$line" ] && continue
  ev="${line%% *}"; filt="${line#* }"
  echo "$filt" > $T/events/bcinit/$ev/filter
done < $IN/filters.txt
# Enable all bcinit events
echo 1 > $T/events/bcinit/enable
# Generous per-CPU buffer so nothing is lost during the 2-3 s init
echo 8192 > $T/buffer_size_kb
echo 1 > $T/tracing_on
armed=$(grep -c '^[pr]:' $T/kprobe_events || true)
echo "armed: $armed probes, buffer $(cat $T/buffer_size_kb) KB"
head -5 $T/kprobe_events
echo "..."
echo "bcinit enable: $(cat $T/events/bcinit/enable)"
