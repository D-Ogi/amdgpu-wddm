#!/bin/bash
# E13, the steps that may take the probe down (wishlist L6, L17 and the suspend path), driven from the PC so that what
# amdgpu did survives a hang: the trace is read through trace_pipe and the kernel log through /dev/kmsg, both streamed
# over SSH into files on the PC while the step runs. In E13's first session the unload's trace sat in the probe's RAM
# and was lost with it.
#
#   risky_pc.sh reset      amdgpu_gpu_recover (debugfs), amdgpu's own reset of this part
#   risky_pc.sh suspend    s2idle for 25 s with an RTC wake alarm: amdgpu's suspend and resume
#   risky_pc.sh unload     console unbound, then modprobe -r amdgpu: its hw_fini. Hung the machine once (facts M42).
#
# Owner's consent needed for each; run them last, in this order, with everything else already pulled.
# SSH is a parameter so that the key and the known_hosts file stay where they are kept (outside the repository).
set -u
STEP=${1:?reset|suspend|unload}
SSH=${BC250_SSH:?set BC250_SSH to the full ssh command for the probe}
OUT=${BC250_OUT:-/p/BC-250/scratch/e13b/risky}
mkdir -p "$OUT"
T=/sys/kernel/tracing

$SSH "mountpoint -q $T || mount -t tracefs nodev $T; mountpoint -q /sys/kernel/debug || mount -t debugfs nodev /sys/kernel/debug; echo 0 > $T/tracing_on; echo 65536 > $T/buffer_size_kb; echo > $T/trace; echo ':mod:amdgpu' > $T/set_event 2>/dev/null || echo 'amdgpu:*' > $T/set_event; echo 1 > $T/tracing_on; echo armed"
$SSH "cat $T/trace_pipe" > "$OUT/$STEP-trace.txt" 2>/dev/null &
tracer=$!
$SSH "cat /dev/kmsg" > "$OUT/$STEP-kmsg.txt" 2>/dev/null &
logger=$!
sleep 2

case $STEP in
reset)
	CMD="echo 'bc250 begin reset' > $T/trace_marker; sync; cat /sys/kernel/debug/dri/0000:01:00.0/amdgpu_gpu_recover; echo rc \$?; sleep 5; echo 'bc250 end reset' > $T/trace_marker"
	;;
suspend)
	CMD="echo 'bc250 begin suspend' > $T/trace_marker; sync; echo 0 > /sys/class/rtc/rtc0/wakealarm; echo +25 > /sys/class/rtc/rtc0/wakealarm; echo mem > /sys/power/state; echo rc \$?; sleep 5; echo 'bc250 end suspend' > $T/trace_marker"
	;;
unload)
	CMD="pkill -f qrshow.py; for v in /sys/class/vtconsole/vtcon*; do grep -q 'frame buffer' \$v/name && echo 0 > \$v/bind; done; echo 'bc250 begin unload' > $T/trace_marker; sync; modprobe -r amdgpu; echo rc \$?; echo 'bc250 end unload' > $T/trace_marker; lsmod | grep -c '^amdgpu'"
	;;
*)
	echo "reset|suspend|unload"; kill $tracer $logger; exit 2
	;;
esac
echo "== $STEP $(date +%T)"
timeout 120 $SSH "$CMD" > "$OUT/$STEP-step.txt" 2>&1
echo "step exit $? ($(tr '\n' ' ' < "$OUT/$STEP-step.txt" | cut -c1-200))"
sleep 3
if timeout 20 $SSH "echo 0 > $T/tracing_on; uptime; dmesg | tail -25" > "$OUT/$STEP-after.txt" 2>&1; then
	echo "probe answers after the step"
	# What the stream did not carry (the connection may drop across a suspend): the rest of the buffer.
	kill $tracer 2>/dev/null; sleep 1
	timeout 60 $SSH "cat $T/trace" > "$OUT/$STEP-trace-rest.txt" 2>/dev/null
else
	echo "PROBE DOES NOT ANSWER after the step"
fi
sleep 2
kill $tracer $logger 2>/dev/null
wc -l "$OUT/$STEP-trace.txt" "$OUT/$STEP-kmsg.txt" | head -2
