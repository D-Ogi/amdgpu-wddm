OUT=/root/boot-a-out
DBG=$(ls -d /sys/kernel/debug/dri/*/ 2>/dev/null | while read d; do [ -e "$d/amdgpu_firmware_info" ] && echo $d; done | head -1)
date -u +%H:%M:%SZ

echo "=== L10/L31: SMU messages (kprobe on smu_cmn_send_smc_msg_with_param) ==="
# Check if the symbol exists
grep -c smu_cmn_send_smc_msg_with_param /proc/kallsyms > $OUT/l10-smu-sym-count.txt 2>&1 || true
cat $OUT/l10-smu-sym-count.txt
echo "smu check done"

echo "=== L32 PG masks ==="
python3 /root/ul/regread.py /root/ul/l49-smn.regs l32-pg > $OUT/l32-pg-masks.txt 2>&1 || true
echo "l32 $(wc -l < $OUT/l32-pg-masks.txt) lines"

echo "=== display: DCN after amdgpu ==="
cd /root/disp
python3 -u bar5dump.py --out /root/disp/l8-post-amdgpu.txt gop-a.regs gop-b.regs gop-c.regs > $OUT/l8-post-amdgpu-log.txt 2>&1 || true
echo "post-amdgpu $(wc -l < $OUT/l8-post-amdgpu-log.txt) lines"

echo "=== DCN pipe check ==="
python3 dcnregs.py pipes > $OUT/dcn-pipes.txt 2>&1 || true
cat $OUT/dcn-pipes.txt

echo "=== done ==="
date -u +%H:%M:%SZ
