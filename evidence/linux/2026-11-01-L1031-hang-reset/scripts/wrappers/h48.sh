M=/sys/kernel/debug/dri/0000:01:00.0/amdgpu_sdma_sched_mask
mkdir -p /root/hang-out; cd /root/hang
echo 0x1 > $M; echo "sdma mask $(cat $M)"
grep -A2 sdma /sys/kernel/debug/dri/0000:01:00.0/amdgpu_fence_info | grep -E 'ring|signaled|emitted'
timeout 60 python3 -u h-jobs.py sdma-poll --release-ms 14000 --wait 30 > /root/hang-out/l48-sdma0.txt 2>&1; echo "native exit $?" >> /root/hang-out/l48-sdma0.txt
cat /root/hang-out/l48-sdma0.txt | grep -v '^BO \|^CTX'
grep -A2 sdma /sys/kernel/debug/dri/0000:01:00.0/amdgpu_fence_info | grep -E 'ring|signaled|emitted'
echo "== post-reset control on SDMA0"
cd /root/x1; timeout 60 python3 -u x1-sdma-raw.py --ref /root/x1 --packets x1-packets.json --sizes 4096,1048576 2>&1 | grep -E 'RESULT|X1'
echo 0x3 > $M; echo "sdma mask $(cat $M)"
timeout 60 python3 /root/init/i5-submit.py 2>&1 | grep RESULT
dmesg | grep -v 'DTM TA\|not ready, skipping' | tail -12
