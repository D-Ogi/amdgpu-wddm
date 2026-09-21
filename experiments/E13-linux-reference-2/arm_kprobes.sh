#!/bin/sh
# Probe side. Arms a register trace for a load of amdgpu that follows an unload in the same boot. The module's own trace
# events are not there to be enabled while it is unloaded (boot 8: ":mod:amdgpu" armed nothing), so kprobes on the two
# register accessors are registered by module:symbol; the kernel arms them when the module arrives. Traces, touches
# no hardware. Output lines: bcw (reg, val), bcr (reg) followed by bcrv (val) on the same CPU.
T=/sys/kernel/tracing
mountpoint -q $T || mount -t tracefs nodev $T
echo 0 > $T/tracing_on
echo > $T/trace
echo > $T/set_event
echo > $T/kprobe_events 2>/dev/null
echo 'p:bcw amdgpu:amdgpu_device_wreg reg=$arg2:x32 val=$arg3:x32' >> $T/kprobe_events || echo "kprobe wreg refused"
echo 'p:bcr amdgpu:amdgpu_device_rreg reg=$arg2:x32' >> $T/kprobe_events || echo "kprobe rreg refused"
echo 'r:bcrv amdgpu:amdgpu_device_rreg val=$retval:x32' >> $T/kprobe_events || echo "kretprobe rreg refused"
echo ':mod:amdgpu' >> $T/set_event 2>/dev/null     # before the kprobes: a plain write to set_event clears every event
echo 1 > $T/events/kprobes/enable
echo 65536 > $T/buffer_size_kb
echo 1 > $T/tracing_on
echo "kprobes: $(grep -c . $T/kprobe_events) registered, enable = $(cat $T/events/kprobes/enable)"
