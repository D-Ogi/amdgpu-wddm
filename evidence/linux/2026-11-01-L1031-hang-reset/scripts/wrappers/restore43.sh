D=/sys/kernel/debug/dri/0000:01:00.0
M=$D/amdgpu_compute_sched_mask
echo 0xff > $M; echo "compute mask $(cat $M)"
cat /sys/kernel/debug/dri/0000:01:00.0/amdgpu_fence_info | grep -A3 comp_1 | head -40
timeout 60 python3 /root/init/i5-submit.py > /root/hang-out/l43-post-oops-i5.txt 2>&1; echo "i5 exit $?"; grep RESULT /root/hang-out/l43-post-oops-i5.txt
cd /root/x1; timeout 60 python3 -u x1-sdma-raw.py --ref /root/x1 --packets x1-packets.json --sizes 4096 2>&1 | grep -E 'RESULT|X1'
ps -o pid,stat,comm | grep python
