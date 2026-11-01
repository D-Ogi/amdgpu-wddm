OUT=/root/boot-a-out; mkdir -p $OUT
date -u +%H:%M:%SZ
DBG=$(ls -d /sys/kernel/debug/dri/*/ 2>/dev/null | while read d; do [ -e "$d/amdgpu_firmware_info" ] && echo $d; done | head -1)

echo "=== L7 ==="
cat $DBG/amdgpu_vram_mm > $OUT/l7-vram-mm.txt 2>&1 || true
cat $DBG/amdgpu_gtt_mm > $OUT/l7-gtt-mm.txt 2>&1 || true
cat $DBG/amdgpu_vm_info > $OUT/l7-vm-info.txt 2>&1 || true
for f in mem_info_vram_total mem_info_vram_used mem_info_gtt_total mem_info_gtt_used mem_info_vis_vram_total mem_info_vis_vram_used; do
  [ -f $DBG/$f ] && echo "$f=$(cat $DBG/$f)" >> $OUT/l7-meminfo.txt 2>/dev/null
done
echo "l7 ok"

echo "=== L45 ==="
cat $DBG/amdgpu_gpu_info > $OUT/l45-gpu-info.txt 2>&1 || true
cat /sys/class/drm/card*/device/pp_dpm_sclk > $OUT/l45-pp-dpm-sclk.txt 2>&1 || true
echo "l45 ok"

echo "=== L10 ==="
cat $DBG/amdgpu_pm_info > $OUT/l10-pm-info.txt 2>&1 || true
dd if=$DBG/amdgpu_gpu_metrics of=$OUT/l10-gpu-metrics-raw.bin bs=4096 count=1 2>/dev/null || true
echo "gpu_metrics $(wc -c < $OUT/l10-gpu-metrics-raw.bin 2>/dev/null) bytes"
for f in pp_features pp_dpm_sclk pp_dpm_mclk pp_dpm_fclk pp_dpm_pcie pp_power_profile_mode pp_od_clk_voltage power_dpm_state power_dpm_force_performance_level; do
  p=$(ls /sys/class/drm/card*/device/$f 2>/dev/null | head -1)
  [ -f "$p" ] && { echo "=== $f ==="; cat "$p"; } >> $OUT/l10-pp-all.txt 2>&1 || true
done
for f in /sys/class/hwmon/hwmon*/; do
  [ -d "$f" ] || continue
  echo "=== $f ===" >> $OUT/l10-hwmon.txt
  for a in $f*; do [ -f "$a" ] && echo "$(basename $a)=$(cat $a 2>/dev/null)" >> $OUT/l10-hwmon.txt 2>/dev/null; done
done
echo "l10 ok"

echo "=== L4 ==="
for r in $DBG/amdgpu_ring_*; do
  n=$(basename $r)
  timeout 10 cat $r > $OUT/l4-$n.txt 2>&1 || true
  echo "$n $(wc -l < $OUT/l4-$n.txt) lines"
done
echo "l4 ok"

echo "=== L49 ==="
lspci -vvs 00:15.0 2>/dev/null | grep -i -E 'LnkCap|LnkSta|LnkCtl|Width|Speed' > $OUT/l49-pci.txt 2>&1 || true
lspci -vvs 00:15.1 2>/dev/null | grep -i -E 'LnkCap|LnkSta|LnkCtl|Width|Speed' >> $OUT/l49-pci.txt 2>&1 || true
lspci -vvs 01:00.0 2>/dev/null | grep -i -E 'LnkCap|LnkSta|LnkCtl|Width|Speed' >> $OUT/l49-pci.txt 2>&1 || true
echo "l49 ok"

echo "=== L8 ==="
cat $DBG/amdgpu_dm_dtn_log > $OUT/l8-dtn.txt 2>&1 || true
echo "dtn $(wc -l < $OUT/l8-dtn.txt) lines"
for c in /sys/kernel/debug/dri/*/DP-*/; do
  [ -d "$c" ] || continue
  echo "=== $c ===" >> $OUT/l8-dp-dbg.txt
  for f in link_settings lttpr_mode max_bpc output_bpc; do
    [ -f "$c$f" ] && echo "$f=$(cat $c$f 2>/dev/null)" >> $OUT/l8-dp-dbg.txt
  done
done || true
echo "l8 ok"

echo "=== modetest ==="
modetest -M amdgpu -c > $OUT/l47-connectors.txt 2>&1 || true
modetest -M amdgpu -p > $OUT/l47-planes.txt 2>&1 || true
echo "modetest ok"

ls $OUT
echo "=== all ==="
date -u +%H:%M:%SZ
