#!/bin/sh
# i7: SMU metrics table raw bytes next to decoded sysfs (L31), and fan/thermal/feature limits (L10).
# All reads, no register writes.
OUT=/root/init-out; mkdir -p $OUT
D=/sys/bus/pci/devices/0000:01:00.0
DBG=$(ls -d /sys/kernel/debug/dri/*/ 2>/dev/null | while read d; do [ -e "$d/amdgpu_pm_info" ] && echo "$d"; done | head -1)
HWMON=$(ls -d /sys/class/hwmon/hwmon* 2>/dev/null | while read h; do [ "$(cat $h/name 2>/dev/null)" = "amdgpu" ] && echo $h; done | head -1)
date -u > $OUT/i7-start.txt; cat /proc/uptime >> $OUT/i7-start.txt

# L31: raw gpu_metrics + decoded sysfs in quick succession
od -A d -t x1 $D/gpu_metrics > $OUT/l31-gpu_metrics-raw.hex 2>&1
cat $DBG/amdgpu_pm_info > $OUT/l31-pm_info.txt 2>&1
for f in pp_dpm_sclk pp_dpm_mclk pp_dpm_socclk pp_dpm_fclk pp_dpm_dcefclk; do
  echo "== $f"; cat $D/$f 2>&1
done > $OUT/l31-pp-dpm.txt
for f in $HWMON/*; do
  [ -f "$f" ] && [ -r "$f" ] && echo "$(basename $f) $(timeout 2 cat $f 2>&1 | head -1)"
done > $OUT/l31-hwmon.txt 2>&1
echo "L31: captured"

# L10: GetEnabledSmuFeatures (pp_features, if it exists)
cat $D/pp_features > $OUT/l10-pp_features.txt 2>&1

# Fan and thermal limits
echo "== fan" > $OUT/l10-fan-thermal.txt
for f in fan1_input fan1_min fan1_max fan1_target pwm1 pwm1_enable pwm1_max pwm1_min; do
  v=$(cat $HWMON/$f 2>&1)
  echo "$f $v"
done >> $OUT/l10-fan-thermal.txt
echo "== thermal" >> $OUT/l10-fan-thermal.txt
for f in temp1_input temp1_crit temp1_crit_hyst temp1_emergency power1_cap power1_cap_max power1_average; do
  v=$(cat $HWMON/$f 2>&1)
  echo "$f $v"
done >> $OUT/l10-fan-thermal.txt
echo "== od" >> $OUT/l10-fan-thermal.txt
cat $D/pp_od_clk_voltage >> $OUT/l10-fan-thermal.txt 2>&1
echo "== performance_level $(cat $D/power_dpm_force_performance_level 2>&1)" >> $OUT/l10-fan-thermal.txt
echo "L10: captured"

date -u >> $OUT/i7-start.txt; cat /proc/uptime >> $OUT/i7-start.txt
echo "i7 done"
