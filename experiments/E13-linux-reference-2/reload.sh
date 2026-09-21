#!/bin/sh
# E13, the step the default boot entry made necessary: amdgpu was already loaded (mode "full"), so the clean init trace
# is taken from a reload: unload amdgpu with the trace armed (its hw_fini on this part), then session.sh load (its init
# over hardware it has already run on: the situation of E11's H9b, as Linux meets it). Run detached; everything lands
# in /tmp/e13. The console is unbound first, the screen stays dark until amdgpu is back.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=/tmp/e13
T=/sys/kernel/tracing
LOG=$OUT/reload.log
mkdir -p "$OUT"
exec > "$LOG" 2>&1
echo "== reload $(date +%T)"
pkill -f qrshow.py; sleep 1
for v in /sys/class/vtconsole/vtcon*; do
	grep -q 'frame buffer' "$v/name" && echo 0 > "$v/bind" && echo "unbound $v"
done
mountpoint -q $T || mount -t tracefs nodev $T
echo 0 > $T/tracing_on
echo 65536 > $T/buffer_size_kb
echo > $T/trace
echo 'amdgpu:*' > $T/set_event
echo 1 > $T/tracing_on
echo "bc250 begin unload" > $T/trace_marker
echo "== modprobe -r amdgpu $(date +%T)"
modprobe -r amdgpu; echo "rc $?"
echo "bc250 end unload" > $T/trace_marker
echo 0 > $T/tracing_on
cat $T/trace > "$OUT/amdgpu-events-unload.txt"
echo "unload trace: $(wc -l < "$OUT/amdgpu-events-unload.txt") lines"
lsmod | grep -i '^amdgpu' && { echo "amdgpu did not unload, stopping here"; exit 1; }
dmesg | tail -15
sync
sh "$HERE/session.sh" load
echo "== done $(date +%T)"
