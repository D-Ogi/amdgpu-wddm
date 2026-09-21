#!/bin/sh
# E17 on the probe (Alpine from the diagnostic stick, running from RAM). One phase per call,
# everything into /tmp/e17 (RAM) and echoed to stdout, so the PC has it as it happens.
#
#   sh session.sh pre       before any driver: kernel, command line, PCI view, memory. No register read.
#   sh session.sh load      tracefs armed for module amdgpu, modprobe amdgpu, trace saved
#   sh session.sh base      the VM picture with no client: registers of both hubs, debugfs, memory managers
#   sh session.sh hold      OUR buffers at known addresses, including the sparse and no-allocate ones
#                           no capture has ever produced, with their page tables read out of VRAM
#   sh session.sh extras    display state (L8), GDS/OA, and ip_discovery as a second sample
#   sh session.sh info      every AMDGPU_INFO query again, to confirm nothing moved since facts M46
#
# What this session is NOT, and why - the argument is in scratch/tmp/e17_inventory.md.
# It had four more phases (pkgs, radv, render and a umr build) until an inventory of the existing
# evidence showed that they would re-measure what is already on disk. The gfx ring's wrapper around
# a RADV submission was captured by E14 and simply never decoded; offline/ring_wrapper.py decodes it
# on the PC. The page table format is recorded 33348 entries deep in the trace events of E03, E13
# and E14; offline/check_pte_from_trace.py reconstructs every one of them and confirms eight of
# bc250_pte.h's transcribed claims with no hardware at all. What is left, and the only reason to
# boot, is the `hold` phase: two encodings amdgpu has never emitted on this unit (PRT and NOALLOC)
# and one positive control - the entries read back out of VRAM rather than out of a tracepoint.
#
# READ-ONLY TOWARDS THE GPU, throughout, except what amdgpu itself does for an ordinary client.
# No register is written. Nothing is written to a disk, the firmware or NVRAM. The debugfs files
# that DO something when read - amdgpu_test_ib, amdgpu_gpu_recover, amdgpu_evict_vram,
# amdgpu_evict_gtt, amdgpu_benchmark, amdgpu_preempt_ib, amdgpu_force_sclk - are never named here,
# and nothing in this file globs a debugfs directory.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
E13=${E17_E13:-$HERE/../E13-linux-reference-2}
OUT=${E17_OUT:-/tmp/e17}
T=/sys/kernel/tracing
mkdir -p "$OUT"
say() { echo "== $*"; }
gpu() { lspci -D -d 1002:13fe | head -1 | cut -d' ' -f1; }
dri() { dirname "$(ls /sys/kernel/debug/dri/*/amdgpu_regs2 2>/dev/null | head -1)"; }
dev() { dirname "$(ls -d /sys/class/drm/card*/device/mem_info_vram_total | head -1)"; }

debugfs_up() {
	mountpoint -q /sys/kernel/debug || mount -t debugfs nodev /sys/kernel/debug
}

temp() {
	cat "$(dev)"/hwmon/hwmon*/temp1_input 2>/dev/null | awk '{printf "edge %.1f C\n", $1 / 1000}'
	cat "$(dev)"/pp_dpm_sclk 2>/dev/null
}

# E13's arm/disarm, unchanged, so that an E17 trace can be compared with an E13 or E14 one line for
# line. tracefs is the one thing outside /tmp this session writes to, and disarm puts it back.
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
	say "trace $2: $(wc -l < "$OUT/$2") lines; overruns: $(cat $T/per_cpu/cpu*/stats \
		| awk '/overrun|dropped/ {s+=$2} END {print s+0}')"
	echo > $T/set_event
	echo > $T/trace
}

# The drm client directory of a pid. amdgpu puts vm_pagetable_info in it (amdgpu_debugfs.c:2185)
# and drm puts proc_info there (drm_debugfs.c:314-331, "pid: N").
client_of() {
	for d in /sys/kernel/debug/dri/client-*; do
		[ -e "$d/proc_info" ] || continue
		if grep -qx "pid: $1" "$d/proc_info" 2>/dev/null && [ -e "$d/vm_pagetable_info" ]; then
			basename "$d" | sed 's/client-//'
			return 0
		fi
	done
	return 1
}

# e17_vm.py writes bos.txt as `label va size domain flags word`, every number in hex without 0x,
# so these two need no JSON parser.
bos_vas() { awk '{printf "0x%s ", $2}' "$OUT/hold/bos.txt"; }
# A sparse (PRT) row has no buffer behind it and so no witness word; e17_vm.py writes `-` there
# and it is skipped, because there is nothing to read back.
bos_checks() { awk '$6 != "-" {printf "--check 0x%s 0x%s ", $2, $6}' "$OUT/hold/bos.txt"; }

case "${1:-}" in
pre)
	say "kernel"; uname -a; cat /proc/cmdline
	say "boot history (cold or warm is the owner's to tell); uptime"; uptime
	say "amdgpu loaded? (empty is what this phase wants)"; lsmod | grep -i amdgpu
	say "memory: everything in this session lives here"; head -4 /proc/meminfo; df -h /tmp | tail -1
	say "lspci"; lspci -nn
	say "GPU function"; lspci -vvv -s "$(gpu)"
	say "M.2 root port 00:15.0 (wishlist L12: only worth a row if this boot was cold)"
	lspci -vvv -s 00:15.0 2>/dev/null | grep -E 'LnkSta|LnkCtl2|UESta|CESta|DevSta' | head -20
	say "interrupts"; grep -E 'CPU0|amdgpu' /proc/interrupts
	;;

load)
	lsmod | grep -qi '^amdgpu' && { echo "amdgpu is already loaded: skip this phase"; exit 1; }
	arm load
	say "modprobe amdgpu $(date +%T)"
	modprobe amdgpu
	for i in $(seq 1 60); do
		dmesg | grep -q 'Initialized amdgpu' && break
		sleep 1
	done
	sleep 3
	disarm load amdgpu-events-load.txt
	dmesg | grep -i amdgpu | tail -8
	say "the VM half of the init, for the record"
	grep -c 'amdgpu_device_wreg' "$OUT/amdgpu-events-load.txt"
	say "temperature"; temp
	;;

base)
	debugfs_up
	D=$(dri); DEV=$(dev)
	say "temperature and clocks"; temp
	say "who holds the render node (empty means the GPU is ours alone)"
	fuser /dev/dri/renderD128 2>/dev/null || echo none
	say "VM registers of both hubs, no client running -> $OUT/vmregs-base.txt"
	python3 -u "$E13/regs2.py" "$HERE/vmlists.json" state | tee "$OUT/vmregs-base.txt" | tail -8
	say "amdgpu_vm_info"; head -120 "$D/amdgpu_vm_info" | tee "$OUT/vm_info-base.txt" | head -30
	say "amdgpu_gem_info"; head -200 "$D/amdgpu_gem_info" > "$OUT/gem_info-base.txt"
	wc -l "$OUT/gem_info-base.txt"; head -12 "$OUT/gem_info-base.txt"
	say "memory managers and totals"
	head -40 "$D/amdgpu_vram_mm" > "$OUT/vram_mm.txt"; head -8 "$OUT/vram_mm.txt"
	head -60 "$D/amdgpu_gtt_mm" > "$OUT/gtt_mm.txt"; head -8 "$OUT/gtt_mm.txt"
	cat "$DEV"/mem_info_* 2>/dev/null | head -12
	say "clients amdgpu knows about right now"
	for d in /sys/kernel/debug/dri/client-*; do
		[ -e "$d/proc_info" ] || continue
		echo "-- $(basename "$d") $(tr '\n' ' ' < "$d/proc_info")"
		[ -e "$d/vm_pagetable_info" ] && tr '\n' ' ' < "$d/vm_pagetable_info" && echo
	done
	say "firmware versions (so the boot can be matched against E13's)"
	cat "$D/amdgpu_firmware_info" > "$OUT/firmware_info.txt"; head -6 "$OUT/firmware_info.txt"
	;;

hold)
	debugfs_up
	D=$(dri)
	mkdir -p "$OUT/hold" "$OUT/hold/pt"
	# The trace is armed BEFORE the mapping, because the entries this phase reads out of VRAM are
	# written during GEM_VA MAP: amdgpu_vm_update_ptes and amdgpu_vm_set_ptes carry the kernel's
	# own view of the same 64-bit values, which makes three independent witnesses of one entry.
	arm hold
	say "mapping our buffers at known addresses $(date +%T)"
	python3 -u "$HERE/e17_vm.py" --hold "${E17_HOLD:-300}" --out "$OUT/hold" \
		> "$OUT/hold/hold.txt" 2>&1 &
	holder=$!
	for i in $(seq 1 60); do
		[ -s "$OUT/hold/bos.txt" ] && break
		sleep 0.5
	done
	if ! [ -s "$OUT/hold/bos.txt" ]; then
		say "the holder did not come up; its output:"; cat "$OUT/hold/hold.txt"
		kill $holder 2>/dev/null; disarm hold amdgpu-events-hold.txt; exit 1
	fi
	cat "$OUT/hold/hold.txt"
	pid=$(cat "$OUT/hold/holder.pid")
	client=$(client_of "$pid") || { say "no drm client directory for pid $pid"; client=""; }
	say "holder pid $pid, drm client ${client:-none}"

	if [ -n "$client" ]; then
		say "what amdgpu says the shape of this VM is"
		cat "/sys/kernel/debug/dri/client-$client/vm_pagetable_info" \
			| tee "$OUT/hold/vm_pagetable_info.txt"
	fi

	say "VM registers while our buffers are mapped (no submission, so no VMID is expected yet)"
	python3 -u "$E13/regs2.py" "$HERE/vmlists.json" state > "$OUT/hold/vmregs-hold.txt"
	grep -E 'CONTEXT[0-9]+_PAGE_TABLE_BASE_ADDR_LO32|CONTEXT[0-9]+_CNTL ' \
		"$OUT/hold/vmregs-hold.txt" | head -20

	say "amdgpu_vm_info and amdgpu_gem_info with our buffers in them"
	head -200 "$D/amdgpu_vm_info" > "$OUT/hold/vm_info.txt"; tail -30 "$OUT/hold/vm_info.txt"
	head -400 "$D/amdgpu_gem_info" > "$OUT/hold/gem_info.txt"; wc -l "$OUT/hold/gem_info.txt"

	# Our own walker first: it needs nothing but debugfs, so it works even if umr did not build.
	if [ -n "$client" ]; then
		say "pt_walk: the whole root directory"
		python3 -u "$HERE/pt_walk.py" "$HERE/pte_bits.json" --client "$client" --scan \
			> "$OUT/hold/pt-scan.txt" 2>&1
		head -20 "$OUT/hold/pt-scan.txt"
		say "pt_walk: every buffer, with its table pages dumped -> $OUT/hold/pt"
		# shellcheck disable=SC2046
		python3 -u "$HERE/pt_walk.py" "$HERE/pte_bits.json" --client "$client" \
			--dump "$OUT/hold/pt" --va $(bos_vas) > "$OUT/hold/pt-walk.txt" 2>&1
		cat "$OUT/hold/pt-walk.txt"
		say "pt_walk: the positive control - the word each buffer starts with, read back through"
		say "         the physical address its own PTE names"
		# shellcheck disable=SC2046
		python3 -u "$HERE/pt_walk.py" "$HERE/pte_bits.json" --client "$client" \
			$(bos_checks) 2>&1 | tee "$OUT/hold/pt-control.txt" | tail -14
	fi

	# There is no umr in this session. It was in the plan until the inventory
	# (scratch/tmp/e17_inventory.md) showed that its job here - a second decode of the same
	# entries - is already done twice over on the PC: pt_walk.py walks them here, check_pte.py
	# walks the dumped pages again offline, and the trace armed above carries amdgpu's own view
	# of every value. Building umr would have cost the internet, four packages and several
	# minutes, for a fourth opinion.

	say "letting the holder go"
	kill $holder 2>/dev/null
	wait $holder 2>/dev/null
	tail -5 "$OUT/hold/hold.txt"
	disarm hold amdgpu-events-hold.txt
	say "the kernel's own view of the same entries"
	grep -cE 'amdgpu_vm_(update_ptes|set_ptes|bo_map|bo_update)' "$OUT/amdgpu-events-hold.txt"
	grep -E 'amdgpu_vm_(set_ptes|update_ptes):' "$OUT/amdgpu-events-hold.txt" | head -20
	say "temperature"; temp
	;;

extras)
	debugfs_up
	D=$(dri); DEV=$(dev)
	say "ip_discovery in full (wishlist L19) -> $OUT/ip_discovery.txt"
	if [ -d "$DEV/ip_discovery" ]; then
		find "$DEV/ip_discovery" -type f | sort | while read -r f; do
			echo "${f#"$DEV"/} $(tr '\n' ' ' < "$f" 2>&1)"
		done > "$OUT/ip_discovery.txt"
		wc -l "$OUT/ip_discovery.txt"
		say "hardware id 12 (UVD/VCN), which is the row L19 was written for"
		grep '/12/' "$OUT/ip_discovery.txt"
	else
		echo "absent"
	fi
	say "display state (wishlist L8 leftovers): link rate, lane count, which instances carry the output"
	cat "$D/amdgpu_dm_dtn_log" > "$OUT/dtn_log.txt" 2>&1; wc -l "$OUT/dtn_log.txt"
	grep -iE 'otg|dig|uniphy|link|lane|pixel' "$OUT/dtn_log.txt" | head -40
	for f in /sys/class/drm/card*-DP-*/; do
		[ -d "$f" ] || continue
		echo "-- $f status $(cat "$f/status" 2>/dev/null) enabled $(cat "$f/enabled" 2>/dev/null)"
	done
	say "GDS, GWS and OA, which the winsys document lists as open question 6"
	grep -iE 'gds|gws|oa' "$OUT/firmware_info.txt" 2>/dev/null | head
	cat /sys/class/kfd/kfd/topology/nodes/*/properties 2>/dev/null \
		| grep -iE 'gds|num_gws|array_count|simd|lds' | head -12
	say "sa_info and fence_info, for the record"
	head -40 "$D/amdgpu_sa_info" > "$OUT/sa_info.txt" 2>&1; wc -l "$OUT/sa_info.txt"
	head -40 "$D/amdgpu_fence_info" > "$OUT/fence_info.txt" 2>&1; cat "$OUT/fence_info.txt"
	;;

info)
	say "every AMDGPU_INFO query, raw (E13's info.py unchanged) -> $OUT/info.txt"
	python3 -u "$E13/info.py" "$E13/info.json" > "$OUT/info.txt" 2>&1
	wc -l "$OUT/info.txt"; head -20 "$OUT/info.txt"
	say "compare this with facts M46 on the PC; a difference is a fact to correct, not a surprise"
	;;

*)
	echo "pre | load | pkgs | base | hold | radv | render | extras | info"; exit 2
	;;
esac
