DEST=/media/usb/bc250/tools
mount | grep -q ' /media/usb .*rw' || { echo "ABORT: stick not rw"; exit 3; }
mkdir -p $DEST/hang $DEST/x3
cp /root/l14b/hang/* $DEST/hang/
cp /root/l14b/userland/* $DEST/userland/
cp /root/l14b/x3/* $DEST/x3/
cat >> $DEST/README <<'EOF'

Added after the hang lane and Boot C (2026-11-01):
hang/      h-arm.sh (reset-path kprobes, instance bchang), h-jobs.py sdma-poll|gfx-wait [--uc]|gfx-nop|spin [--kill-at S]
           (one blocked job per run; never pin compute with amdgpu_compute_sched_mask: comp_1.0.0 is the
           high-priority queue and a mask without a normal ring oopses drm_sched_job_arm on 6.18.52)
userland/  l26-run.sh + l26-migrate.py (16 MiB VRAM paging round trip, traced), sdma-ring-decode.py
x3/        the X3 GRUB fallback sources (embedded.cfg, x3-head.cfg, x3-entry.cfg, x3-install.sh)
EOF
sync
find $DEST -type f | wc -l; du -sh $DEST
mount -o remount,ro /media/usb && echo "stick ro"
