#!/bin/sh
set -eu
T=/sys/kernel/tracing
if [ -d /sys/module/amdgpu ]; then echo unexpected_loaded_module; exit 2; fi
for p in /sys/class/hwmon/hwmon*/temp1_input; do
 [ -f "$p" ] || continue
 v=$(cat "$p")
 echo "preload_temperature_mC=$v"
 [ "$v" -lt 85000 ] || exit 3
done
echo function > "$T/current_tracer"
echo 1 > "$T/tracing_on"
dd if="$T/trace_pipe" bs=4096 2>/dev/null &
reader=$!
trap 'echo 0 > "$T/tracing_on"; kill "$reader" 2>/dev/null || true' EXIT
echo 'bc250_e28_second_load_begin' > "$T/trace_marker"
echo 'modprobe_begin'
set +e
modprobe amdgpu
rc=$?
set -e
echo "modprobe_exit=$rc"
echo 'bc250_e28_second_load_end' > "$T/trace_marker"
sleep 1
echo 0 > "$T/tracing_on"
kill "$reader" 2>/dev/null || true
wait "$reader" 2>/dev/null || true
trap - EXIT
echo 'enabled_events_begin'
cat "$T/set_event"
echo 'enabled_functions_begin'
cat "$T/set_ftrace_filter"
echo 'trace_statistics_begin'
cat "$T"/per_cpu/cpu*/stats
echo 'loaded_module='$(test -d /sys/module/amdgpu && echo yes || echo no)
echo 'second_load_capture_complete'
exit "$rc"
