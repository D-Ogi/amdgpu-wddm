#!/bin/sh
# The M6 reference under Linux: libdrm's gfx10 memset dispatch through raw ioctls (dispatch.py), with amdgpu's trace
# events armed, then the compute ring as amdgpu left it (what the kernel wrapped around our IB). Stops at the first
# run that does not verify. Output under /tmp/e13/dispatch (RAM).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
O=/tmp/e13/dispatch
T=/sys/kernel/tracing
mkdir -p $O
mountpoint -q /sys/kernel/debug || mount -t debugfs nodev /sys/kernel/debug
mountpoint -q $T || mount -t tracefs nodev $T
D=$(dirname "$(ls /sys/kernel/debug/dri/*/amdgpu_regs2 | head -1)")
one() {     # one <tag> <dispatch.py arguments...>
	tag=$1; shift
	echo 0 > $T/tracing_on; echo > $T/trace
	echo ':mod:amdgpu' > $T/set_event 2>/dev/null || echo 'amdgpu:*' > $T/set_event
	echo 1 > $T/tracing_on
	echo "bc250 begin dispatch $tag" > $T/trace_marker
	python3 -u "$HERE/dispatch.py" "$@" > $O/$tag.txt 2>&1
	rc=$?
	echo "bc250 end dispatch $tag" > $T/trace_marker
	echo 0 > $T/tracing_on
	cat $T/trace > $O/$tag-events.txt
	# amdgpu's scheduler picks the compute ring (in the first session: comp_1.1.0, not ring 0): the trace says which
	ring=$(grep -m1 'amdgpu_cs_ioctl:' $O/$tag-events.txt | sed 's/.*ring_name=\([^,]*\).*/\1/')
	[ -n "$ring" ] && dd if="$D/amdgpu_ring_$ring" bs=4096 2>/dev/null | od -A x -t x4 -v > $O/$tag-ring-$ring.txt
	echo "== $tag: exit $rc, $(wc -l < $O/$tag-events.txt) trace lines"
	tail -6 $O/$tag.txt
	grep -E 'amdgpu_(cs_ioctl|sched_run_job|iv):' $O/$tag-events.txt | sed 's/^.*\] [^ ]* *//' | cut -c1-170
	dmesg | grep -iE 'amdgpu.*(timeout|reset|fault|ring .* timed)' | tail -3
	return $rc
}
one g1 --groups 1 || exit 1
one g16 --groups 16 || exit 1
one g16-memsync --groups 16 --mem-sync || exit 1
