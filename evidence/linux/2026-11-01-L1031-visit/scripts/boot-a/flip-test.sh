OUT=/root/boot-a-out
DBG=$(ls -d /sys/kernel/debug/dri/*/ 2>/dev/null | while read d; do [ -e "$d/amdgpu_firmware_info" ] && echo $d; done | head -1)
CONN=$(modetest -M amdgpu -c 2>/dev/null | awk '$3=="connected" {print $1; exit}')
echo "connector=$CONN"
date -u +%H:%M:%SZ

# X2: check link rate BEFORE and AFTER modetest
cat $DBG/../DP-1/link_settings > $OUT/x2-link-before.txt 2>&1 || true
echo "before: $(cat $OUT/x2-link-before.txt)"

for F in XR24 XB30 XB4H; do
  echo "=== flip $F ==="
  ( sleep 6 | timeout 9 modetest -M amdgpu -s "$CONN:1920x1200@$F" > $OUT/flip-$F.txt 2>&1; echo "exit $?" >> $OUT/flip-$F.txt )
  echo "flip $F done"
  # DCN registers right after the flip
  python3 /root/disp/dcnregs.py read l44-$F /root/disp/gop-a.regs /root/disp/gop-b.regs > $OUT/l44-$F.txt 2>&1 || true
  echo "$F regs $(wc -l < $OUT/l44-$F.txt) lines"
  # DTN log
  cat $DBG/amdgpu_dm_dtn_log > $OUT/dtn-$F.txt 2>&1 || true
done

# X2: check link rate AFTER modetest
cat $DBG/../DP-1/link_settings > $OUT/x2-link-after.txt 2>&1 || true
echo "after: $(cat $OUT/x2-link-after.txt)"

echo "=== done ==="
date -u +%H:%M:%SZ
