D=/sys/kernel/debug/dri/0000:01:00.0
cd /root/hang
grep -A2 'gfx_0.0.0' $D/amdgpu_fence_info | grep -E 'ring|signaled|emitted'
timeout 60 python3 -u h-jobs.py gfx-wait --release-ms 14000 --wait 30 > /root/hang-out/l47-gfx.txt 2>&1; echo "native exit $?" >> /root/hang-out/l47-gfx.txt
grep -v '^BO \|^CTX' /root/hang-out/l47-gfx.txt
grep -A2 'gfx_0.0.0' $D/amdgpu_fence_info | grep -E 'ring|signaled|emitted'
echo "== post controls"
timeout 60 python3 /root/init/i5-submit.py 2>&1 | grep RESULT
cd /root/x1; timeout 60 python3 -u x1-sdma-raw.py --ref /root/x1 --packets x1-packets.json --sizes 4096 2>&1 | grep -E 'X1'
dmesg | grep -v 'DTM TA\|not ready, skipping' | tail -9
