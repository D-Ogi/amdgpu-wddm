#!/bin/sh
# E14 on the lab machine (Alpine from the diagnostic stick, amdgpu loaded, Mesa RADV).
# POSIX sh, busybox compatible. Everything under /tmp/e14, which is RAM: nothing touches a disk.
#
#   sh run.sh            env, the full test run, the RADV shader dump, the amdgpu trace
#   sh run.sh env        kernel, Mesa, driver identity only
#   sh run.sh check      the full test run (this is the one that decides pass or fail)
#   sh run.sh timing     the full test run again, for timings taken on a quiet GPU
#   sh run.sh radv       find out which RADV_DEBUG value dumps shader code here, and dump it
#   sh run.sh trace      amdgpu trace events armed around one fill dispatch
#
# Towards the GPU this is an ordinary Vulkan client: it submits compute work through amdgpu like
# any other program. The only thing written anywhere outside /tmp is tracefs, in the `trace` phase,
# and that is undone at the end of it.
set -u

OUT=/tmp/e14
BUILD=${E14_OUT:-$OUT/build}
BIN=$BUILD/vkcompute
SPV=$BUILD/shaders
T=/sys/kernel/tracing
RUNS=${E14_RUNS:-5}

mkdir -p "$OUT"
say() { echo "== $*"; }

need_bin() {
	[ -x "$BIN" ] || { echo "no $BIN - run build.sh first"; exit 2; }
	[ -d "$SPV" ] || { echo "no $SPV - run build.sh first"; exit 2; }
}

phase_env() {
	say "kernel"; uname -a
	say "cmdline"; cat /proc/cmdline
	say "uptime, memory"; uptime; head -3 /proc/meminfo
	say "alpine"; cat /etc/alpine-release
	say "mesa and vulkan packages"
	apk list -I 2>/dev/null | grep -iE 'mesa|vulkan|glslang' | sort
	say "amdgpu module"
	modinfo amdgpu 2>/dev/null | grep -E '^(filename|version|srcversion|vermagic)'
	say "GPU function"; lspci -nn -d 1002:13fe
	say "vulkan ICD"; cat /usr/share/vulkan/icd.d/radeon_icd.x86_64.json
	gpu_state
}

# What the GPU is doing, which decides whether a timing is worth anything. gpu_busy_percent is not
# implemented on this SoC (read returns -EOPNOTSUPP), so the clock state and whoever holds the
# render node are the honest indicators. busybox fuser has no -v.
gpu_state() {
	say "GPU clocks (the '*' marks the active DPM level)"
	cat /sys/class/drm/card*/device/pp_dpm_sclk 2>/dev/null
	cat /sys/class/drm/card*/device/hwmon/hwmon*/temp1_input 2>/dev/null \
		| awk '{printf "edge %.1f C\n", $1 / 1000}'
	say "processes holding /dev/dri/renderD128 (empty means the GPU is ours alone)"
	fuser /dev/dri/renderD128 2>/dev/null || echo "none"
}

phase_check() {
	need_bin
	say "vkcompute --list"
	"$BIN" --list
	say "vkcompute, runs=$RUNS -> $OUT/vkcompute.txt"
	"$BIN" "$SPV" --runs "$RUNS" 2>&1 | tee "$OUT/vkcompute.txt"
	# tee hides the exit status, so ask the file.
	if grep -q 'match=NO' "$OUT/vkcompute.txt"; then
		echo "RESULT: at least one GPU hash differs from the CPU reference"
		return 1
	fi
	echo "RESULT: every GPU hash equals its CPU reference"
	return 0
}

phase_timing() {
	need_bin
	gpu_state
	say "timings again on a quiet GPU -> $OUT/vkcompute-timing.txt"
	"$BIN" "$SPV" --runs "$RUNS" 2>&1 | tee "$OUT/vkcompute-timing.txt"
	gpu_state
}

# Which RADV_DEBUG value dumps the compiled shader machine code is a property of the installed
# Mesa, not something to assume: this build's `RADV_DEBUG=help` prints nothing, and short option
# names are tail-merged into longer ones in the ICD's string table, so reading the strings cannot
# settle it either. So try the candidates and keep whichever actually produces output.
phase_radv() {
	need_bin
	mkdir -p "$OUT/radv-debug"
	say "candidate RADV_DEBUG values, one fill dispatch each"
	best=""
	best_sz=0
	for opt in shaders shaderstats spirv preoptir nir metashaders; do
		f="$OUT/radv-debug/$opt.txt"
		RADV_DEBUG="$opt" "$BIN" "$SPV" --only fill_g1 --runs 1 >/dev/null 2>"$f"
		sz=$(wc -c < "$f")
		echo "RADV_DEBUG=$opt -> $sz bytes"
		if [ "$sz" -gt "$best_sz" ]; then
			best_sz=$sz
			best=$opt
		fi
	done
	say "option names this ICD carries (the contiguous debug table in its string section)"
	strings -n 4 /usr/lib/libvulkan_radeon.so | sed -n '/^nofastclears$/,/^fullsync$/p' \
		| tr '\n' ' ' | fold -s -w 100
	echo
	if [ -z "$best" ] || [ "$best_sz" -lt 256 ]; then
		echo "no RADV_DEBUG value dumped anything useful here"
		return 0
	fi
	say "best: RADV_DEBUG=$best ($best_sz bytes) - full run -> $OUT/radv-shaders.txt"
	RADV_DEBUG="$best" "$BIN" "$SPV" --runs 1 >"$OUT/radv-shaders.stdout" 2>"$OUT/radv-shaders.txt"
	echo "RADV_DEBUG=$best" > "$OUT/radv-shaders.which"
	wc -l "$OUT/radv-shaders.txt"
	head -40 "$OUT/radv-shaders.txt"
}

# tracefs, same procedure as E13's session.sh arm/disarm, so the two traces are comparable.
arm() {
	mountpoint -q $T || mount -t tracefs nodev $T
	echo 0 > $T/tracing_on
	echo 65536 > $T/buffer_size_kb
	echo > $T/trace
	echo ':mod:amdgpu' > $T/set_event 2>/dev/null || echo 'amdgpu:*' > $T/set_event
	echo 1 > $T/tracing_on
	echo "bc250 begin $1" > $T/trace_marker
}

disarm() {
	echo "bc250 end $1" > $T/trace_marker
	echo 0 > $T/tracing_on
	cat $T/trace > "$OUT/$2"
	echo "trace $2: $(wc -l < "$OUT/$2") lines; overruns: $(cat $T/per_cpu/cpu*/stats \
		| awk '/overrun|dropped/ {s+=$2} END {print s+0}')"
	echo > $T/set_event
	echo > $T/trace
}

phase_trace() {
	need_bin
	say "amdgpu events around one fill_g1 dispatch -> $OUT/amdgpu-events-fill.txt"
	arm fill
	"$BIN" "$SPV" --only fill_g1 --runs 1 2>&1 | tee "$OUT/fill-traced.txt" | tail -4
	sleep 1
	disarm fill amdgpu-events-fill.txt
	say "event kinds in the trace"
	awk '{for (i = 1; i <= NF; i++) if ($i ~ /^amdgpu_[a-z_]+:$/) c[$i]++} END {for (k in c) print c[k], k}' \
		"$OUT/amdgpu-events-fill.txt" | sort -rn
}

rc=0
case "${1:-all}" in
env)    phase_env ;;
check)  phase_check || rc=1 ;;
timing) phase_timing ;;
radv)   phase_radv ;;
trace)  phase_trace ;;
all)
	phase_env
	phase_check || rc=1
	phase_radv
	phase_trace
	say "everything is under $OUT"
	du -sh "$OUT"; ls -l "$OUT"
	;;
*)
	echo "usage: run.sh [all|env|check|timing|radv|trace]"; exit 2 ;;
esac
exit $rc
