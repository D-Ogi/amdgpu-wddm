cd /root/hang
timeout 60 python3 -u h-jobs.py gfx-wait --release-ms 500 --wait 10 > /root/hang-out/l47-gfx-post.txt 2>&1; echo "native exit $?" >> /root/hang-out/l47-gfx-post.txt
grep -v '^BO \|^CTX' /root/hang-out/l47-gfx-post.txt
dmesg | grep -v 'DTM TA\|not ready, skipping' | tail -3
cat /sys/kernel/tracing/instances/bchang/trace | grep -c 'kgq\|kcq\|sdma_reset_q:'
ps -o pid,stat,comm | grep python
