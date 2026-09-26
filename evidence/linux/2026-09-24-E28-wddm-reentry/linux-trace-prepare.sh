set -eu
mountpoint -q /sys/kernel/tracing || mount -t tracefs tracefs /sys/kernel/tracing
T=/sys/kernel/tracing
printf 'kernel='; uname -r
printf 'amdgpu_module_sha256='; sha256sum "$(modinfo -n amdgpu)" | cut -d ' ' -f1
printf 'amdgpu_vermagic='; modinfo -F vermagic amdgpu
printf 'tracers='; cat "$T/available_tracers"
echo 0 > "$T/tracing_on"
echo nop > "$T/current_tracer"
echo > "$T/trace"
echo 8192 > "$T/buffer_size_kb"
echo 'amdgpu_device_rreg:mod:amdgpu' > "$T/set_event"
echo 'amdgpu_device_wreg:mod:amdgpu' >> "$T/set_event"
echo 'gfx_v10_0_rlc_stop:mod:amdgpu' > "$T/set_ftrace_filter"
echo 'smu_hw_fini:mod:amdgpu' >> "$T/set_ftrace_filter"
echo 'smu_disable_dpms:mod:amdgpu' >> "$T/set_ftrace_filter"
echo 'gfx_v10_0_kiq_setting:mod:amdgpu' >> "$T/set_ftrace_filter"
echo 'filters_begin'
cat "$T/set_event"
cat "$T/set_ftrace_filter"
echo 'filters_end'
echo 'trace_preparation_complete'
