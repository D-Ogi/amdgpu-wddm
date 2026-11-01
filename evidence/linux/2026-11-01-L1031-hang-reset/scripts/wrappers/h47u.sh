cd /root/hang
timeout 40 python3 -u h-jobs.py gfx-wait --uc --release-ms 500 --wait 10 > /root/hang-out/l47-gfx-uc.txt 2>&1; echo "native exit $?" >> /root/hang-out/l47-gfx-uc.txt
grep -v '^CTX' /root/hang-out/l47-gfx-uc.txt
grep -A2 'gfx_0.0.0' /sys/kernel/debug/dri/0000:01:00.0/amdgpu_fence_info | grep -E 'signaled|emitted'
dmesg | grep -v 'DTM TA\|not ready, skipping' | tail -3
