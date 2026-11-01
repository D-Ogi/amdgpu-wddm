set -eu
OUT=/root/boot-a-out; mkdir -p $OUT
date -u +%H:%M:%SZ

echo "=== L7: memory layout ==="
DBG=$(ls -d /sys/kernel/debug/dri/*/ 2>/dev/null | while read d; do [ -e "$d/amdgpu_firmware_info" ] && echo $d; done | head -1)
cat $DBG/amdgpu_vram_mm > $OUT/l7-vram-mm.txt 2>&1
cat $DBG/amdgpu_gtt_mm > $OUT/l7-gtt-mm.txt 2>&1
cat /sys/kernel/debug/dri/0/amdgpu_vm_info > $OUT/l7-vm-info.txt 2>&1
for f in mem_info_vram_total mem_info_vram_used mem_info_gtt_total mem_info_gtt_used mem_info_vis_vram_total mem_info_vis_vram_used; do
  [ -f $DBG/$f ] && echo "$f=$(cat $DBG/$f)" >> $OUT/l7-meminfo.txt
done
echo "l7 done"

echo "=== L45: GPU occupancy ==="
# rocminfo / clinfo not available; read from sysfs
for f in $DBG/amdgpu_gpu_info; do [ -f "$f" ] && cat "$f" > $OUT/l45-gpu-info.txt; done
cat /sys/class/drm/card*/device/pp_dpm_sclk > $OUT/l45-pp-dpm-sclk.txt 2>&1
echo "l45 done"

echo "=== L10/L31: SMU features and metrics ==="
cat $DBG/amdgpu_pm_info > $OUT/l10-pm-info.txt 2>&1
cat $DBG/amdgpu_gpu_metrics > $OUT/l10-gpu-metrics-raw.bin 2>/dev/null; echo "gpu_metrics $(wc -c < $OUT/l10-gpu-metrics-raw.bin) bytes"
for f in pp_features pp_dpm_sclk pp_dpm_mclk pp_dpm_fclk pp_dpm_pcie pp_power_profile_mode pp_od_clk_voltage power_dpm_state power_dpm_force_performance_level; do
  [ -f /sys/class/drm/card*/device/$f ] && { echo "=== $f ==="; cat /sys/class/drm/card*/device/$f; } >> $OUT/l10-pp-all.txt 2>&1
done
for f in /sys/class/hwmon/hwmon*/; do
  [ -d "$f" ] || continue
  echo "=== $f ===" >> $OUT/l10-hwmon.txt
  for a in $f*; do [ -f "$a" ] && echo "$(basename $a)=$(cat $a 2>/dev/null)" >> $OUT/l10-hwmon.txt; done
done
echo "l10 done"

echo "=== L4: ring dumps ==="
for r in $DBG/amdgpu_ring_*; do
  n=$(basename $r)
  cat $r > $OUT/l4-$n.txt 2>&1
  echo "$n $(wc -l < $OUT/l4-$n.txt) lines"
done
echo "l4 done"

echo "=== L49: core link status ==="
lspci -vvs 00:15.0 2>/dev/null | grep -i -E 'LnkCap|LnkSta|LnkCtl|Width|Speed' > $OUT/l49-pci-15.0.txt 2>&1
lspci -vvs 00:15.1 2>/dev/null | grep -i -E 'LnkCap|LnkSta|LnkCtl|Width|Speed' >> $OUT/l49-pci-15.0.txt 2>&1
lspci -vvs 01:00.0 2>/dev/null | grep -i -E 'LnkCap|LnkSta|LnkCtl|Width|Speed' >> $OUT/l49-pci-15.0.txt 2>&1
echo "l49 done"

echo "=== L8: DTN log ==="
cat $DBG/amdgpu_dm_dtn_log > $OUT/l8-dtn.txt 2>&1
echo "l8 $(wc -l < $OUT/l8-dtn.txt) lines"

echo "=== modetest ==="
modetest -M amdgpu -c > $OUT/l47-connectors.txt 2>&1
modetest -M amdgpu -p > $OUT/l47-planes.txt 2>&1
echo "modetest done"

echo "=== L8: link training ==="
for c in /sys/kernel/debug/dri/*/DP-*/; do
  [ -d "$c" ] || continue
  echo "=== $c ===" >> $OUT/l8-dp-dbg.txt
  for f in link_settings lttpr_mode max_bpc output_bpc; do
    [ -f "$c$f" ] && echo "$f=$(cat $c$f 2>/dev/null)" >> $OUT/l8-dp-dbg.txt
  done
done
echo "l8 link done"

ls -la $OUT
echo "=== all done ==="
date -u +%H:%M:%SZ
