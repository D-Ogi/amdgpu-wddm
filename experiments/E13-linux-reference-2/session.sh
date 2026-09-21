#!/bin/sh
# E13 on the probe (Alpine from the diagnostic stick, boot entry "shell only": amdgpu blacklisted, nothing has touched
# the GPU). One phase per call, everything into /tmp/e13 (RAM) and echoed to stdout, so the PC has it as it happens.
#
#   sh session.sh pre      before any driver: kernel, command line, PCI view, interrupts. No GPU register is touched.
#   sh session.sh load     tracefs armed for module amdgpu, modprobe amdgpu, trace saved (wishlist L9: no sweep before)
#   sh session.sh state    after init: dmesg, module parameters, interrupts, MSI state, debugfs, firmware, rings, MQDs,
#                          named registers through amdgpu_regs2 (regs2.py)
#   sh session.sh ib       amdgpu's own IB tests on every ring with the trace armed: a minimal submission with fences
#   sh session.sh reset    LAST, may hang the machine: amdgpu_gpu_recover with the trace armed
#
# Read-only towards the GPU except what amdgpu itself does. Nothing is written to a disk, the firmware or NVRAM.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=/tmp/e13
T=/sys/kernel/tracing
mkdir -p "$OUT"
say() { echo "== $*"; }
gpu() { lspci -D -d 1002:13fe | head -1 | cut -d' ' -f1; }
dri() { dirname "$(ls /sys/kernel/debug/dri/*/amdgpu_regs2 2>/dev/null | head -1)"; }

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
	say "trace $2: $(wc -l < "$OUT/$2") lines; overruns: $(cat $T/per_cpu/cpu*/stats | awk '/overrun|dropped/ {s+=$2} END {print s+0}')"
}

case "${1:-}" in
pre)
	say "kernel"; uname -a; cat /proc/cmdline
	say "uptime (boot history is the owner's to tell: cold or warm)"; uptime
	say "amdgpu loaded? (must be empty)"; lsmod | grep -i amdgpu
	say "lspci"; lspci -nn
	say "GPU function"; lspci -vvv -s "$(gpu)"
	say "PSP/CCP function 1022:143e (L13)"; lspci -vvv -d 1022:143e
	say "M.2 root port (L12)"; lspci -vvv -s 00:15.0 2>/dev/null
	say "interrupts"; cat /proc/interrupts
	;;
load)
	lsmod | grep -qi '^amdgpu' && { echo "amdgpu is already loaded"; exit 1; }
	arm load
	say "modprobe amdgpu $(date +%T)"
	modprobe amdgpu
	for i in $(seq 1 60); do
		dmesg | grep -q 'Initialized amdgpu' && break
		sleep 1
	done
	sleep 3
	disarm load amdgpu-events-load.txt
	dmesg | grep -i 'amdgpu' | tail -5
	;;
state)
	mountpoint -q /sys/kernel/debug || mount -t debugfs nodev /sys/kernel/debug
	D=$(dri)
	say "dmesg"; dmesg > "$OUT/dmesg.txt"; wc -l "$OUT/dmesg.txt"
	say "module parameters (L15)"; grep . /sys/module/amdgpu/parameters/* 2>/dev/null
	say "modinfo"; modinfo amdgpu | grep -E '^(filename|version|srcversion|vermagic)'
	say "interrupts (L11)"; grep -iE 'amdgpu|CPU0' /proc/interrupts
	say "GPU function after init (MSI state, L11)"; lspci -vvv -s "$(gpu)"
	say "debugfs $D"; ls "$D"
	say "firmware info (L3)"; cat "$D/amdgpu_firmware_info"
	say "firmware files"; ls -l /lib/firmware/amdgpu/cyan_skillfish2_* 2>/dev/null; sha256sum /lib/firmware/amdgpu/cyan_skillfish2_* 2>/dev/null
	say "memory (L7)"; head -40 "$D/amdgpu_vram_mm" 2>/dev/null; head -60 "$D/amdgpu_gtt_mm" 2>/dev/null
	cat /sys/class/drm/card*/device/mem_info_* 2>/dev/null | head -12
	say "pm info (L10)"; cat "$D/amdgpu_pm_info" 2>/dev/null
	say "gpu_metrics (L10)"; od -A x -t x1 -v /sys/class/drm/card*/device/gpu_metrics 2>/dev/null | head -20
	say "display DTN log (L8)"; cat "$D/amdgpu_dm_dtn_log" 2>/dev/null | head -120
	say "rings and MQDs (L4) -> $OUT/rings"
	mkdir -p "$OUT/rings"
	for f in "$D"/amdgpu_ring_* "$D"/amdgpu_mqd_*; do
		[ -e "$f" ] || continue
		dd if="$f" bs=4096 2>/dev/null | od -A x -t x4 -v > "$OUT/rings/$(basename "$f").txt"      # od alone reads nothing from these files
		echo "$(basename "$f") $(wc -l < "$OUT/rings/$(basename "$f").txt") lines; head: $(head -1 "$OUT/rings/$(basename "$f").txt")"
	done
	say "state registers"; python3 -u "$HERE/regs2.py" "$HERE/lists.json" state
	say "per-queue registers"; python3 -u "$HERE/regs2.py" "$HERE/lists.json" hqd
	;;
ib)
	mountpoint -q /sys/kernel/debug || mount -t debugfs nodev /sys/kernel/debug
	D=$(dri)
	say "before"; grep -iE 'amdgpu' /proc/interrupts; python3 -u "$HERE/regs2.py" "$HERE/lists.json" state | grep -E 'IH_RB_(RPTR|WPTR)|CP_RB0|SDMA0_GFX_RB'
	arm ib
	say "amdgpu_test_ib $(date +%T)"
	cat "$D/amdgpu_test_ib"
	sleep 1
	disarm ib amdgpu-events-ib.txt
	say "after"; grep -iE 'amdgpu' /proc/interrupts; python3 -u "$HERE/regs2.py" "$HERE/lists.json" state | grep -E 'IH_RB_(RPTR|WPTR)|CP_RB0|SDMA0_GFX_RB'
	mkdir -p "$OUT/rings-after-ib"
	for f in "$D"/amdgpu_ring_*; do
		[ -e "$f" ] || continue
		dd if="$f" bs=4096 2>/dev/null | od -A x -t x4 -v > "$OUT/rings-after-ib/$(basename "$f").txt"
	done
	say "event kinds in the trace"; awk '{for (i=1;i<=NF;i++) if ($i ~ /^amdgpu_[a-z_]+:$/) c[$i]++} END {for (k in c) print c[k], k}' "$OUT/amdgpu-events-ib.txt" | sort -rn
	;;
reset)
	mountpoint -q /sys/kernel/debug || mount -t debugfs nodev /sys/kernel/debug
	D=$(dri)
	arm reset
	say "amdgpu_gpu_recover $(date +%T)"; sync
	cat "$D/amdgpu_gpu_recover"
	sleep 5
	disarm reset amdgpu-events-reset.txt
	dmesg | tail -60
	python3 -u "$HERE/regs2.py" "$HERE/lists.json" state
	;;
*)
	echo "pre | load | state | ib | reset"; exit 2
	;;
esac
