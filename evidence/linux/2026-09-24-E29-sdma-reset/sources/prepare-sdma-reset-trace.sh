#!/bin/sh
# Source-matched M387 probe setup. Leaves an isolated instance OFF for a later run.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
[ "$(uname -r)" = '6.18.52-0-lts' ] || { echo wrong_kernel; exit 2; }
[ -d /sys/module/amdgpu ] || { echo module_not_loaded; exit 2; }
T=/sys/kernel/tracing
mountpoint -q "$T" || mount -t tracefs tracefs "$T"
I="$T/instances/bc250_m9_reset"
[ ! -d "$I" ] || { echo trace_instance_already_exists; exit 2; }
if grep -q 'bc250_m9_reset/' "$T/kprobe_events"; then echo probe_group_already_exists; exit 2; fi
printf 'kernel='; uname -r
printf 'module_sha256='; sha256sum "$(modinfo -n amdgpu)" | cut -d ' ' -f1
printf 'module_vermagic='; modinfo -F vermagic amdgpu
printf 'module_srcversion='; modinfo -F srcversion amdgpu
printf 'lockup_timeout='; cat /sys/module/amdgpu/parameters/lockup_timeout
printf 'gpu_recovery='; cat /sys/module/amdgpu/parameters/gpu_recovery
printf 'debug_mask='; cat /sys/module/amdgpu/parameters/debug_mask
# Require a deliberately configured SDMA timeout, never infer it from defaults.
# amdgpu_device_get_job_timeout_settings mutates this string with strsep.
# A truncated sysfs value alone is insufficient: require our preserved load argv.
reported_timeout=$(cat /sys/module/amdgpu/parameters/lockup_timeout)
[ "$reported_timeout" = '10000,10000,50,10000' ] || [ "$reported_timeout" = 10000 ] || { echo timeout_policy_mismatch; exit 2; }
[ "$(cat "$HERE/first-load-policy.txt")" = 'lockup_timeout=10000,10000,50,10000 gpu_recovery=1' ] || { echo load_receipt_missing; exit 2; }
echo timeout_policy_from_successful_load_argv_and_source_parser
[ "$(cat /sys/module/amdgpu/parameters/gpu_recovery)" = 1 ] || { echo recovery_not_enabled; exit 2; }
count=0
for node in /sys/class/drm/renderD*/device; do
 [ -f "$node/vendor" ] && [ -f "$node/device" ] || continue
 if [ "$(cat "$node/vendor")" = 0x1002 ] && [ "$(cat "$node/device")" = 0x13fe ]; then
  count=$((count+1))
  printf 'sdma_reset_mask='; cat "$node/sdma_reset_mask"
 fi
done
[ "$count" = 1 ] || { echo unexpected_gpu_count; exit 2; }
# Capability output is retained for interpretation before a delayed job. Setup
# success alone does not establish that PER_QUEUE is advertised or functional.
while IFS= read -r line; do
 symbol=${line#* amdgpu:}; symbol=${symbol%% *}
 grep -Eq "^${symbol}( |$)" "$T/available_filter_functions" || { echo "missing_symbol=$symbol"; exit 2; }
done < "$HERE/sdma-reset-probes.txt"
mkdir "$I"
# Roll back only this group's registered events if setup fails halfway through.
cleanup_partial() {
 echo 0 > "$I/tracing_on" 2>/dev/null || true
 if [ -f "$I/events/bc250_m9_reset/enable" ]; then echo 0 > "$I/events/bc250_m9_reset/enable"; fi
 while IFS= read -r line; do
  event=${line%% *}; event=${event#*:}
  printf -- '-:%s\n' "$event" >> "$T/kprobe_events" 2>/dev/null || true
 done < "$HERE/sdma-reset-probes.txt"
 rmdir "$I" 2>/dev/null || true
}
trap cleanup_partial EXIT
echo 0 > "$I/tracing_on"
echo nop > "$I/current_tracer"
echo 8192 > "$I/buffer_size_kb"
while IFS= read -r line; do printf '%s\n' "$line" >> "$T/kprobe_events"; done < "$HERE/sdma-reset-probes.txt"
echo 1 > "$I/events/bc250_m9_reset/enable"
echo 1 > "$I/events/amdgpu/amdgpu_device_rreg/enable"
echo 1 > "$I/events/amdgpu/amdgpu_device_wreg/enable"
printf 'registered_probes='; grep -c 'bc250_m9_reset/' "$T/kprobe_events"
cat "$I/set_event"
cat "$T/kprobe_profile"
trap - EXIT
echo trace_ready_but_disabled
