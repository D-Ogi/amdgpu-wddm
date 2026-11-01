D=/sys/kernel/debug/dri/0000:01:00.0
M=$D/amdgpu_compute_sched_mask
mkdir -p /root/hang-out; cd /root/hang
orig=$(cat $M); echo "compute mask orig $orig"
echo 0x1 > $M; echo "compute mask $(cat $M)"
grep -A2 'comp_1.0.0' $D/amdgpu_fence_info | grep -E 'ring|signaled|emitted'
timeout 60 python3 -u h-jobs.py spin --kill-at 3 --wait 30 > /root/hang-out/l43-kill.txt 2>&1; echo "native exit $?" >> /root/hang-out/l43-kill.txt
grep -v '^BO \|^CTX' /root/hang-out/l43-kill.txt
grep -A2 'comp_1.0.0' $D/amdgpu_fence_info | grep -E 'ring|signaled|emitted'
echo "== post control on comp_1.0.0"
timeout 60 python3 /root/init/i5-submit.py 2>&1 | grep RESULT
echo $orig > $M; echo "compute mask $(cat $M)"
dmesg | grep -v 'DTM TA\|not ready, skipping' | tail -12
