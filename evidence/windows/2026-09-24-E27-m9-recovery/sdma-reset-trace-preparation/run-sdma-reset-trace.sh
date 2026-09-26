#!/bin/sh
# One monitored job; caller must inspect the control before requesting delayed.
set -eu
[ "$#" = 4 ] || { echo 'usage: run-sdma-reset-trace.sh control|delayed PROBE REFERENCE_DIR PACKETS'; exit 2; }
mode=$1; probe=$2; reference=$3; packets=$4
case "$mode" in control) delay=0;; delayed) delay=500;; *) exit 2;; esac
T=/sys/kernel/tracing
I="$T/instances/bc250_m9_reset"
[ -d "$I" ] && [ "$(cat "$I/tracing_on")" = 0 ] || { echo trace_not_prepared; exit 2; }
[ "$(cat /sys/module/amdgpu/parameters/lockup_timeout)" = '10000,10000,50,10000' ] || exit 2
[ "$(cat /sys/module/amdgpu/parameters/gpu_recovery)" = 1 ] || exit 2
for p in /sys/class/hwmon/hwmon*/temp1_input; do
 [ -f "$p" ] || continue
 v=$(cat "$p"); echo "temperature_mC=$v"
 [ "$v" -lt 85000 ] || exit 3
done
# Do not clear or truncate prior captures; caller acquires streamed output once.
echo 1 > "$I/tracing_on"
dd if="$I/trace_pipe" bs=4096 2>/dev/null &
reader=$!
cleanup() {
 echo 0 > "$I/tracing_on"
 kill "$reader" 2>/dev/null || true
 wait "$reader" 2>/dev/null || true
}
trap cleanup EXIT
printf 'bc250_m9_%s_begin\n' "$mode" > "$I/trace_marker"
set +e
python3 -u "$probe" --reference-dir "$reference" --packets "$packets" --release-ms "$delay" --wait-seconds 20
rc=$?
set -e
printf 'bc250_m9_%s_end_native_%s\n' "$mode" "$rc" > "$I/trace_marker"
echo "probe_native_exit=$rc"
sleep 1
cleanup
trap - EXIT
cat "$I"/per_cpu/cpu*/stats
cat "$T/kprobe_profile"
echo "trace_run_complete_mode=$mode"
exit "$rc"
