#!/bin/sh
# Linux visit 2 of 2026-10-06, staged on the stick (owner: the stick need not be read-only).
# Phases, one per call, outputs under /media/usb/l1006b/out:
#   sh run-b2.sh e17 <phase>   E17 (L27): pre, load, base, hold, extras, info - its own session.sh, unchanged
#   sh run-b2.sh ref           mount the Windows volume and the Debian build root read-only
#   sh run-b2.sh build         build vkmembw for Linux inside the root (binary stays on the stick)
#   sh run-b2.sh l25           two RADV clients at once: VMID grab/flush trace, ring dumps, VM registers
#   sh run-b2.sh l38           display audio state, encoder dependency check (and build if all present)
#   sh run-b2.sh umount        undo ref/chroot mounts
# Read-only towards the GPU except what amdgpu does for ordinary clients. Never names amdgpu_test_ib,
# amdgpu_gpu_recover, amdgpu_evict_*, amdgpu_benchmark, amdgpu_preempt_ib or amdgpu_force_sclk.
set -u
B=/media/usb/l1006b
O=$B/out
mkdir -p $O
T=/sys/kernel/tracing
R=/mnt/ref/root
DRI=/sys/kernel/debug/dri/0000:01:00.0
say() { echo "== $* ($(date -u +%T))"; }
k10() { for h in /sys/class/hwmon/hwmon*; do [ "$(cat $h/name)" = k10temp ] && cat $h/temp1_input; done; }

case "${1:-}" in
e17)
	E17_E13=$B/e13 E17_OUT=$O/e17 E17_HOLD=${E17_HOLD:-240} sh $B/e17/session.sh "$2"
	;;
ref)
	modprobe ntfs3 2>/dev/null; modprobe loop 2>/dev/null
	mkdir -p /mnt/win /mnt/ref
	WIN=$(blkid | grep 'LABEL="BC250WIN"' | cut -d: -f1 | head -1)
	mountpoint -q /mnt/win || mount -t ntfs3 -o ro $WIN /mnt/win
	if ! mountpoint -q /mnt/ref; then
		L=$(losetup -f); losetup -r $L /mnt/win/BC250/m12/linux-reference001/build-root.ext4
		mount -t ext4 -o ro,noload $L /mnt/ref
	fi
	ls $R/opt/bc250 && say "ref mounted"
	;;
build)
	sh $B/chroot-run.sh sh -c 'cd /mnt/vkmembw-linux && gcc -O2 -I. -o vkmembw vkmembw.c -ldl -lm && ls -l vkmembw'
	;;
l25)
	mountpoint -q /sys/kernel/debug || mount -t debugfs nodev /sys/kernel/debug
	mountpoint -q $T || mount -t tracefs nodev $T
	D=$O/l25; mkdir -p $D
	echo 0 > $T/tracing_on; echo 65536 > $T/buffer_size_kb; echo > $T/trace
	for e in amdgpu_vm_grab_id amdgpu_vm_flush amdgpu_cs_ioctl amdgpu_sched_run_job amdgpu_vm_bo_map; do
		[ -d $T/events/amdgpu/$e ] && echo 1 > $T/events/amdgpu/$e/enable
	done
	echo 1 > $T/tracing_on; echo "bc250 begin l25" > $T/trace_marker
	say "tctl $(k10); two clients"
	sh $B/chroot-run.sh /mnt/vkmembw-linux/vkmembw --warmup-ms 4000 --iters 20 --placement local > $D/client-a.txt 2>&1 &
	A=$!
	sh $B/chroot-run.sh /mnt/vkmembw-linux/vkmembw --warmup-ms 4000 --iters 20 --placement local > $D/client-b.txt 2>&1 &
	C=$!
	sleep 2
	for i in 1 2 3; do
		for r in gfx_0.0.0 comp_1.0.0 comp_1.1.0 comp_1.2.0 comp_1.3.0 sdma0 sdma1; do
			f=$DRI/amdgpu_ring_$r; [ -e $f ] && dd if=$f of=$D/ring-$r-$i.bin bs=4096 2>/dev/null
		done
		python3 -u $B/e13/regs2.py $B/e17/vmlists.json state > $D/vmregs-$i.txt 2>&1
		ls /sys/kernel/debug/dri/ | grep client > $D/clients-$i.txt
		sleep 1
	done
	wait $A; wait $C
	echo "bc250 end l25" > $T/trace_marker; echo 0 > $T/tracing_on
	dd if=$T/trace of=$D/trace.txt bs=4096 2>/dev/null
	for e in amdgpu_vm_grab_id amdgpu_vm_flush amdgpu_cs_ioctl amdgpu_sched_run_job amdgpu_vm_bo_map; do
		[ -d $T/events/amdgpu/$e ] && echo 0 > $T/events/amdgpu/$e/enable
	done
	echo > $T/trace
	say "tctl $(k10)"
	grep -c vm_grab_id $D/trace.txt; grep vm_grab_id $D/trace.txt | awk '{print $1, $NF}' | sort | uniq -c | head
	tail -3 $D/client-a.txt $D/client-b.txt
	;;
l38)
	D=$O/l38; mkdir -p $D
	{ lspci -nn | grep -i -e 1002 -e audio; ls -la /proc/asound 2>&1; cat /proc/asound/cards 2>&1;
	  for c in /proc/asound/card*; do [ -d $c ] && { echo "== $c"; ls $c; cat $c/eld* $c/codec* 2>/dev/null | head -60; }; done
	  dmesg | grep -i -e hdmi -e audio -e snd -e hda -e azalia -e eld; lsmod | grep -i snd; } > $D/audio.txt 2>&1
	grep -i -e card -e hda -e eld $D/audio.txt | head -20
	sh $B/chroot-run.sh sh -c 'for p in libva libdrm vulkan; do printf "%s " $p; pkg-config --modversion $p 2>&1; done;
		command -v glslangValidator glslc cmake gcc ffmpeg vainfo aplay speaker-test; true' > $D/enc-deps.txt 2>&1
	cat $D/enc-deps.txt
	;;
umount)
	for d in mnt run tmp sys proc dev; do mountpoint -q $R/$d && umount $R/$d; done
	mountpoint -q /mnt/ref && umount /mnt/ref
	for l in $(losetup -a | grep build-root | cut -d: -f1); do losetup -d $l; done
	mountpoint -q /mnt/win && umount /mnt/win
	say "mounts left: $(grep -c -e /mnt/ref -e /mnt/win /proc/mounts)"
	;;
*) sed -n 2,11p $0 ;;
esac
