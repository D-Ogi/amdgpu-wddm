#!/bin/sh
# Unit A, Linux network mode: load amdgpu once, then collect the wishlist reads that need it (L15, L19, L10/L31
# metrics, firmware list, power/thermal at idle). Read-only after the load; no reset, no unload, no clock write.
OUT=/root/collect-$(date -u +%Y%m%dT%H%M%SZ)
mkdir -p $OUT
cd $OUT
echo "k10temp before: $(cat /sys/class/hwmon/hwmon*/temp1_input 2>/dev/null | head -1)" > pre.txt
dmesg > dmesg-before.txt
( timeout 60 modprobe amdgpu; echo "modprobe exit $?" ) > modprobe.txt 2>&1
sleep 5
dmesg > dmesg-after.txt
uname -a > uname.txt
modinfo amdgpu > modinfo.txt 2>&1
grep . /sys/module/amdgpu/parameters/* > params.txt 2>&1
D=/sys/bus/pci/devices/0000:01:00.0
ls $D > dev-ls.txt 2>&1
# L19: IP discovery, every die/IP/instance, every attribute
find $D/ip_discovery -type f 2>/dev/null | sort | while read f; do echo "$f: $(cat $f 2>/dev/null | tr '\n' ' ')"; done > ip_discovery.txt
DBG=$(ls -d /sys/kernel/debug/dri/*/ 2>/dev/null | while read d; do [ -e "$d/amdgpu_firmware_info" ] && echo $d; done | head -1)
mount | grep -q debugfs || mount -t debugfs none /sys/kernel/debug
DBG=$(ls -d /sys/kernel/debug/dri/*/ 2>/dev/null | while read d; do [ -e "$d/amdgpu_firmware_info" ] && echo $d; done | head -1)
echo "debugfs $DBG" > debugfs.txt
cat $DBG/amdgpu_firmware_info > firmware_info.txt 2>&1
cat $DBG/amdgpu_pm_info > pm_info.txt 2>&1
cat $DBG/amdgpu_gpu_recover_unused 2>/dev/null
for f in pp_dpm_sclk pp_dpm_mclk pp_dpm_socclk pp_dpm_fclk pp_dpm_dcefclk power_dpm_force_performance_level pp_od_clk_voltage pp_features gpu_busy_percent mem_info_vram_total mem_info_vis_vram_total mem_info_gtt_total; do echo "== $f"; cat $D/$f 2>&1; done > pm-sysfs.txt
od -A d -t x1 $D/gpu_metrics > gpu_metrics.hex 2>&1
for h in /sys/class/hwmon/hwmon*; do echo "== $h $(cat $h/name)"; grep . $h/*_input $h/*_label $h/power1_cap* 2>/dev/null; done > hwmon.txt
for i in 1 2 3 4 5 6 7 8 9 10; do echo "$(date +%s) $(cat $D/gpu_busy_percent 2>/dev/null) $(grep -h . /sys/class/hwmon/hwmon*/temp1_input | tr '\n' ' ')"; sleep 1; done > idle-10s.txt
lspci -vvv -s 01:00.0 > lspci-gpu.txt 2>&1
lspci -nn > lspci.txt 2>&1
echo "$OUT"
