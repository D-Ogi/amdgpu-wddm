#!/bin/sh
# L1031 (2026-11-01): unit A, stick Linux in network mode. Read-only collection for wishlist rows
# L49 (FCH link LnkSta), L7 (FB location, VRAM/GTT managers), L8 (DTN log, link settings, no EDID),
# L47-DCN201 (plane format lists and properties), L10 rest (hwmon limits), L44 (plane registers for
# XRGB8888, XBGR2101010, XBGR16161616F under modetest). One `modprobe amdgpu`; no unload, no reset,
# no clock or register write. modetest sets a mode and shows a test pattern for about 8 s per format.
# Inputs in /root/l1031: regread.py, dcn-pipe01.regs, fb-location.regs.
IN=/root/l1031
OUT=/root/l1031-out
mkdir -p $OUT; cd $OUT || exit 2
log() { echo "$(date -u +%H:%M:%S) $*" | tee -a $OUT/steps.txt; }
log "start (Linux clock; RTC holds local time)"
uname -a > uname.txt
# Packages from the Alpine CDN (the stick's local repo lacks them).
v=$(cut -d. -f1,2 /etc/alpine-release)
printf "https://dl-cdn.alpinelinux.org/alpine/v%s/main\nhttps://dl-cdn.alpinelinux.org/alpine/v%s/community\n" $v $v > /tmp/repos
apk --repositories-file /tmp/repos add --no-progress libdrm-tests pciutils python3 > apk.txt 2>&1; log "apk exit $?"
command -v modetest python3 lspci >> apk.txt
# L49 before amdgpu: the FCH-side bridges and the tree.
for b in 00:15.0 00:15.1; do echo "== $b"; lspci -s $b -nn; lspci -vvv -s $b | grep -E 'LnkCap:|LnkSta:|LnkCtl:|LnkCap2|LnkSta2|LnkCtl2'; done > l49-lnk.txt 2>&1
lspci -tvnn > l49-tree.txt 2>&1
log "L49 done"
mount | grep -q debugfs || mount -t debugfs none /sys/kernel/debug
dmesg > dmesg-before.txt
( timeout 90 modprobe amdgpu; echo "modprobe exit $?" ) > modprobe.txt 2>&1
sleep 5
dmesg > dmesg-after.txt
log "amdgpu: $(cat modprobe.txt | tail -1)"
DBG=$(ls -d /sys/kernel/debug/dri/*/ | while read d; do [ -e "$d/amdgpu_firmware_info" ] && echo $d; done | head -1)
D=/sys/bus/pci/devices/0000:01:00.0
echo "debugfs $DBG" > debugfs.txt
# L7
python3 $IN/regread.py $IN/fb-location.regs fb-location > l7-fb-regs.txt 2>&1
cat $DBG/amdgpu_vram_mm > l7-vram_mm.txt 2>&1
cat $DBG/amdgpu_gtt_mm > l7-gtt_mm.txt 2>&1
grep -i -E 'VRAM:|GART:|AGP:|vram_mm|Detected VRAM|amdgpu: .*(RAM|aperture|size)' dmesg-after.txt > l7-dmesg.txt
for f in mem_info_vram_total mem_info_vis_vram_total mem_info_gtt_total mem_info_vram_used mem_info_gtt_used mem_info_vram_vendor; do echo "$f $(cat $D/$f 2>&1)"; done >> l7-sysfs.txt
log "L7 done"
# L8 (EDID left out on purpose)
cat $DBG/amdgpu_dm_dtn_log > l8-dtn-fbcon.txt 2>&1
for c in $DBG/*/; do n=$(basename $c); case "$n" in DP-*|HDMI-*|eDP-*|DVI-*) ;; *) continue ;; esac
  for f in $c*; do b=$(basename $f); case "$b" in *edid*|*EDID*) continue ;; esac
    [ -f "$f" ] || continue; echo "== $n/$b"; timeout 3 cat $f 2>&1 | head -40; done; done > l8-connectors.txt 2>&1
for s in $D/drm/card*/card*-*/; do echo "== $(basename $s) status=$(cat $s/status) enabled=$(cat $s/enabled) dpms=$(cat $s/dpms 2>/dev/null)"; head -5 $s/modes; done > l8-sysfs-conn.txt 2>&1
log "L8 done"
# L47-DCN201 (plane formats and properties) and the DRM view
modetest -M amdgpu -c > drm-connectors.txt 2>&1
modetest -M amdgpu -e > drm-encoders.txt 2>&1
modetest -M amdgpu -p > l47-planes.txt 2>&1
log "L47 done"
# L10 rest: every hwmon attribute (limits, not only inputs)
for h in /sys/class/hwmon/hwmon*; do echo "== $h $(cat $h/name)"; for f in $h/*; do [ -f "$f" ] && [ -r "$f" ] && echo "$(basename $f) $(timeout 2 cat $f 2>&1 | head -1)"; done; done > l10-hwmon-all.txt 2>&1
for f in pp_dpm_sclk pp_dpm_mclk pp_dpm_socclk pp_dpm_fclk pp_dpm_dcefclk power_dpm_force_performance_level pp_od_clk_voltage pp_power_profile_mode; do echo "== $f"; cat $D/$f 2>&1; done > l10-pm-sysfs.txt
od -A d -t x1 $D/gpu_metrics > l10-gpu_metrics.hex 2>&1
cat $DBG/amdgpu_pm_info > l10-pm_info.txt 2>&1
log "L10 done"
# L44: baseline (fbcon), then one modetest per format; registers and DTN log read 4 s into each pattern.
python3 $IN/regread.py $IN/dcn-pipe01.regs fbcon > l44-regs-fbcon.txt 2>&1
CONN=$(awk '$3=="connected" {print $1; exit}' drm-connectors.txt)
echo "connector $CONN" >> steps.txt
for F in XR24 XB30 XB4H; do
  ( sleep 9 | timeout 12 modetest -M amdgpu -s "$CONN:1920x1200@$F" > l44-modetest-$F.txt 2>&1; echo "modetest exit $?" >> l44-modetest-$F.txt ) &
  sleep 4
  python3 $IN/regread.py $IN/dcn-pipe01.regs "$F" > l44-regs-$F.txt 2>&1
  cat $DBG/amdgpu_dm_dtn_log > l44-dtn-$F.txt 2>&1
  wait
  log "L44 $F: $(tail -1 l44-modetest-$F.txt)"
  sleep 1
done
python3 $IN/regread.py $IN/dcn-pipe01.regs after > l44-regs-after.txt 2>&1
dmesg > dmesg-end.txt
log "end"
ls -la $OUT | tail -n +2 | wc -l
