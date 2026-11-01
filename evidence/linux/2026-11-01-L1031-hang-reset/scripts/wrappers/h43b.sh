D=/sys/kernel/debug/dri/0000:01:00.0
cd /root/hang
echo "compute mask $(cat $D/amdgpu_compute_sched_mask)"
timeout 60 python3 -u h-jobs.py spin --kill-at 3 --wait 30 > /root/hang-out/l43-kill.txt 2>&1; echo "native exit $?" >> /root/hang-out/l43-kill.txt
grep -v '^BO \|^CTX' /root/hang-out/l43-kill.txt
grep -A2 'comp_1.1.0' $D/amdgpu_fence_info
echo "== post control"
timeout 60 python3 /root/init/i5-submit.py 2>&1 | grep RESULT
dmesg | grep -v 'DTM TA\|not ready, skipping' | tail -10
