#!/bin/sh
set -eu
T=/sys/kernel/tracing
[ -d /sys/module/amdgpu ] || exit 2
for p in /sys/class/hwmon/hwmon*/temp1_input; do
 [ -f "$p" ] || continue
 v=$(cat "$p"); echo "temperature_mC=$v"; [ "$v" -lt 85000 ] || exit 3
done
# Restore Windows fallback before a potentially hanging operation.
media=''
for p in /media/*; do
 [ -f "$p/bc250/diag.py" ] && [ -f "$p/efi/boot/bootx64.efi" ] && media="$p"
done
[ -n "$media" ] || { echo no_unique_boot_media; exit 4; }
[ ! -e "$media/efi/boot/bootx64.off" ] || exit 5
mount -o remount,rw "$media"
mv "$media/efi/boot/bootx64.efi" "$media/efi/boot/bootx64.off"
sync
mount -o remount,ro "$media"
echo windows_fallback_restored
for v in /sys/class/vtconsole/vtcon*; do
 if grep -q 'frame buffer' "$v/name"; then echo 0 > "$v/bind"; echo console_unbound; fi
done
echo 0 > "$T/tracing_on"
echo nop > "$T/current_tracer"
echo > "$T/trace"
echo 'amdgpu_device_rreg:mod:amdgpu' > "$T/set_event"
echo 'amdgpu_device_wreg:mod:amdgpu' >> "$T/set_event"
echo 'smu_hw_fini:mod:amdgpu' > "$T/set_ftrace_filter"
echo 'smu_smc_hw_cleanup:mod:amdgpu' >> "$T/set_ftrace_filter"
echo 'gfx_v10_0_rlc_stop:mod:amdgpu' >> "$T/set_ftrace_filter"
echo function > "$T/current_tracer"
echo filters_begin
cat "$T/set_event"; cat "$T/set_ftrace_filter"
echo filters_end
echo 1 > "$T/tracing_on"
cat "$T/trace_pipe" &
reader=$!
trap 'echo 0 > "$T/tracing_on"; kill "$reader" 2>/dev/null || true' EXIT
echo 'bc250_e28_unload_begin' > "$T/trace_marker"
echo 'unload_begin'
set +e
modprobe -r amdgpu
rc=$?
set -e
echo "unload_exit=$rc"
echo 'bc250_e28_unload_end' > "$T/trace_marker"
sleep 1
echo 0 > "$T/tracing_on"
kill "$reader" 2>/dev/null || true
wait "$reader" 2>/dev/null || true
trap - EXIT
echo statistics_begin
cat "$T"/per_cpu/cpu*/stats
echo 'module_present='$(test -d /sys/module/amdgpu && echo yes || echo no)
echo 'unload_capture_complete'
exit "$rc"
