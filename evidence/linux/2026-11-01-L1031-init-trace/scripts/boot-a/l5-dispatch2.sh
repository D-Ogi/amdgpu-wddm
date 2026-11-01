OUT=/root/boot-a-out
DBG=$(ls -d /sys/kernel/debug/dri/*/ 2>/dev/null | while read d; do [ -e "$d/amdgpu_firmware_info" ] && echo $d; done | head -1)
date -u +%H:%M:%SZ

echo "=== L5: one compute dispatch (1 workgroup) ==="
python3 -u /root/dispatch/dispatch.py --groups 1 --timeout 5 > $OUT/l5-dispatch.txt 2>&1
echo "dispatch exit $?"
tail -5 $OUT/l5-dispatch.txt

echo "=== ring after dispatch ==="
cat $DBG/amdgpu_ring_comp_1.0.0 > $OUT/l5-ring-after-dispatch.txt 2>&1 || true
echo "ring $(wc -l < $OUT/l5-ring-after-dispatch.txt) lines"

echo "=== done ==="
date -u +%H:%M:%SZ
