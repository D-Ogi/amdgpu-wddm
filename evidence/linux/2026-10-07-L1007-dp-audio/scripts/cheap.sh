#!/bin/sh
# Cheap debugfs/sysfs reads for wishlist L7, L8, L10/L31 (reads only). Output under /media/usb/l1007/out/cheap.
O=/media/usb/l1007/out/cheap; mkdir -p $O
D=$(ls -d /sys/kernel/debug/dri/0000:01:00.0 2>/dev/null || ls -d /sys/kernel/debug/dri/1)
C=$(ls -d /sys/class/drm/card*/device | head -1)
echo "debugfs $D sysfs $C"
# L8 display state
cat $D/amdgpu_dm_dtn_log > $O/dtn_log.txt 2>&1
for c in $D/DP-* $D/HDMI-A-* $D/eDP-*; do [ -d "$c" ] || continue; n=$(basename $c)
  for f in link_settings dp_dsc_clock_en output_bpc vrr_range current_lane_count phy_settings; do [ -e $c/$f ] && { echo "--- $n/$f"; cat $c/$f; }; done
done > $O/connectors.txt 2>&1
for e in /sys/class/drm/card*-*/edid; do s=$(wc -c < $e); [ "$s" -gt 0 ] && { echo "$e $s"; od -An -tx1 -v $e; }; done > $O/edid.txt 2>&1
for e in /sys/class/drm/card*-*/; do echo "$e $(cat $e/status 2>/dev/null) $(cat $e/enabled 2>/dev/null) $(head -1 $e/modes 2>/dev/null)"; done > $O/drm-connectors.txt
cat $D/amdgpu_dm_visual_confirm $D/amdgpu_current_backlight_pwm 2>/dev/null > /dev/null
# L7 memory layout
for f in amdgpu_vram_mm amdgpu_gtt_mm amdgpu_gem_info amdgpu_firmware_info amdgpu_vm_info; do [ -e $D/$f ] && { echo "--- $f"; head -60 $D/$f; }; done > $O/mm.txt 2>&1
cat /sys/kernel/debug/dri/*/amdgpu_vbios_info 2>/dev/null > $O/vbios.txt
# L10 / L31 SMU
for f in pp_features pp_dpm_sclk pp_dpm_mclk pp_dpm_fclk pp_dpm_socclk pp_dpm_dcefclk power_dpm_force_performance_level pp_power_profile_mode gpu_busy_percent mem_busy_percent; do [ -e $C/$f ] && { echo "--- $f"; cat $C/$f; }; done > $O/smu.txt 2>&1
od -An -tx1 -v $C/gpu_metrics > $O/gpu_metrics.hex 2>&1
cat $D/amdgpu_pm_info > $O/pm_info.txt 2>&1
for h in $C/hwmon/hwmon*; do for f in $h/*_input $h/*_label $h/power1_cap*; do [ -e $f ] && echo "$f $(cat $f 2>/dev/null)"; done; done > $O/hwmon.txt 2>&1
ls -la $O
