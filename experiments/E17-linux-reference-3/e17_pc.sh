#!/bin/bash
# E17 driven from the development PC: push the scripts, run one phase at a time with its output
# teed here as it happens, and pull the probe's /tmp/e17 after every phase. Nothing of the lab's
# addresses or keys is written down in this repository: BC250_SSH is the whole ssh command and is
# set by the person running the session, exactly as in E13's risky_pc.sh.
#
#   export BC250_SSH="ssh -i <key> -o UserKnownHostsFile=<file> root@<probe address>"
#   ./e17_pc.sh push                 the scripts into the probe's RAM
#   ./e17_pc.sh run pre              one phase, teed into $BC250_OUT
#   ./e17_pc.sh pull                 tar /tmp/e17 back to the PC (do this after EVERY phase)
#   ./e17_pc.sh stream render        a phase with trace_pipe and /dev/kmsg streamed here first,
#                                    so that what the GPU did survives a hang (E13's lesson)
#
# The stick has no scp; tar through ssh is how E13 and E14 moved files both ways.
set -u
CMD=${1:?push|run|pull|stream}
SSH=${BC250_SSH:?set BC250_SSH to the full ssh command for the probe}
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
WORKSPACE=$(cd "$REPO/.." && pwd)
OUT=${BC250_OUT:-$WORKSPACE/scratch/e17}
REMOTE=/tmp/e17s
T=/sys/kernel/tracing
mkdir -p "$OUT"

case $CMD in
push)
	echo "== experiment scripts -> $REMOTE"
	tar -C "$REPO/experiments" -cf - \
		E17-linux-reference-3 E13-linux-reference-2 \
		| $SSH "mkdir -p $REMOTE && tar -C $REMOTE -xf - && ls $REMOTE"
	;;

run)
	phase=${2:?which phase}
	log="$OUT/$phase-$(date +%H%M%S).txt"
	echo "== $phase -> $log"
	$SSH "sh $REMOTE/E17-linux-reference-3/session.sh $phase" 2>&1 | tee "$log"
	echo "== exit ${PIPESTATUS[0]}, $(wc -l < "$log") lines"
	;;

pull)
	stamp=$(date +%H%M%S)
	echo "== /tmp/e17 -> $OUT/e17-$stamp"
	mkdir -p "$OUT/e17-$stamp"
	$SSH "tar -C /tmp -cf - e17" | tar -C "$OUT/e17-$stamp" -xf -
	du -sh "$OUT/e17-$stamp"
	find "$OUT/e17-$stamp" -type f | wc -l
	;;

stream)
	# For the one phase that can take the machine down. The trace and the kernel log are read
	# over the network WHILE the phase runs, so that a hang leaves its evidence here and not in
	# the probe's RAM (E13 lost the unload's trace exactly that way, facts M42).
	phase=${2:?which phase}
	mkdir -p "$OUT/stream"
	$SSH "mountpoint -q $T || mount -t tracefs nodev $T; echo 0 > $T/tracing_on; \
	      echo 65536 > $T/buffer_size_kb; echo > $T/trace; \
	      echo ':mod:amdgpu' > $T/set_event 2>/dev/null || echo 'amdgpu:*' > $T/set_event; \
	      echo 1 > $T/tracing_on; echo armed"
	$SSH "cat $T/trace_pipe" > "$OUT/stream/$phase-trace.txt" 2>/dev/null &
	tracer=$!
	$SSH "cat /dev/kmsg" > "$OUT/stream/$phase-kmsg.txt" 2>/dev/null &
	logger=$!
	sleep 2
	log="$OUT/$phase-$(date +%H%M%S).txt"
	$SSH "sh $REMOTE/E17-linux-reference-3/session.sh $phase" 2>&1 | tee "$log"
	sleep 3
	if timeout 20 $SSH "echo 0 > $T/tracing_on; uptime; dmesg | tail -20" \
		> "$OUT/stream/$phase-after.txt" 2>&1; then
		echo "== the probe answers after the phase"
	else
		echo "== THE PROBE DOES NOT ANSWER: the streamed files are what there is"
	fi
	kill $tracer $logger 2>/dev/null
	wc -l "$OUT/stream/$phase-trace.txt" "$OUT/stream/$phase-kmsg.txt"
	;;

*)
	echo "push | run <phase> | pull | stream <phase>"; exit 2
	;;
esac
