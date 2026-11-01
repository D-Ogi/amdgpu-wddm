#!/bin/sh
# L6: suspend/resume on unit A, split so the failing layer shows (community: only s2idle exists and resume fails on
# most boards, elektricM/amd-bc250-docs PR #55 and the stability page; akandr/bc250-rocm "does not return from s2idle";
# no ACPI fix for sleep is published, bc250-acpi-fix covers C/P-states only).
# Usage: sh -s <mode> <gpu>
#   mode: freezer  = pm_test freezer (tasks frozen and thawed, no device suspend)            R1
#         devices  = pm_test devices (every driver suspend + resume, no platform low power)  R2: amdgpu suspend/resume
#         s2idle   = real s2idle, RTC wake after 20 s                                        R2
#   gpu:  amdgpu   = load amdgpu first (default), none = leave it unloaded (platform-only answer)
# Judge: ssh answers after the step; with amdgpu: amdgpu_test_ib before/after and a modetest pattern after.
# Hung = no ssh 120 s after the expected wake -> plug AC cycle. The lead streams `dmesg -w` off the machine first.
MODE="${1:-freezer}"; GPU="${2:-amdgpu}"
OUT=/root/l6-$MODE-$GPU; mkdir -p $OUT; cd $OUT || exit 2
log() { echo "$(date -u +%H:%M:%S) up=$(cut -d' ' -f1 /proc/uptime) $*" | tee -a steps.txt; }
# Console markers for the lab camera: the network may not come back after a resume, the screen does show it.
con() { printf '
########## L6 %s %s up=%s ##########
' "$*" "$(date -u +%H:%M:%S)" "$(cut -d' ' -f1 /proc/uptime)" > /dev/tty1 2>/dev/null; }
mount | grep -q debugfs || mount -t debugfs none /sys/kernel/debug
if [ "$GPU" = amdgpu ] && ! lsmod | grep -q '^amdgpu'; then ( timeout 90 modprobe amdgpu; echo "modprobe exit $?" ) > modprobe.txt 2>&1; sleep 5; fi
lsmod | grep -q '^amdgpu' && HAVE=1 || HAVE=0
DBG=$(ls -d /sys/kernel/debug/dri/*/ 2>/dev/null | while read d; do [ -e "$d/amdgpu_firmware_info" ] && echo $d; done | head -1)
{ cat /sys/power/state; cat /sys/power/mem_sleep; cat /sys/power/pm_test; cat /sys/power/pm_async 2>/dev/null; } > power-states.txt 2>&1
grep . /proc/acpi/wakeup > acpi-wakeup.txt 2>&1
cat /sys/kernel/debug/suspend_stats > suspend_stats-before.txt 2>/dev/null
cat /proc/driver/rtc > rtc-before.txt 2>&1
cat /proc/sys/kernel/printk > printk-before.txt
echo 7 > /proc/sys/kernel/printk   # kernel PM lines on the console, readable by the camera
if [ $HAVE = 1 ]; then timeout 60 cat $DBG/amdgpu_test_ib > test_ib-before.txt 2>&1; echo "exit $?" >> test_ib-before.txt; fi
dmesg > dmesg-before.txt
case "$MODE" in
  freezer|devices) [ -w /sys/power/pm_test ] || { log "no /sys/power/pm_test (kernel without CONFIG_PM_DEBUG)"; exit 3; }
           echo $MODE > /sys/power/pm_test ;;
  s2idle)  [ -w /sys/power/pm_test ] && echo none > /sys/power/pm_test
           echo 0 > /sys/class/rtc/rtc0/wakealarm; echo +20 > /sys/class/rtc/rtc0/wakealarm ;;
  *) echo "bad mode"; exit 2 ;;
esac
log "enter mode=$MODE gpu=$GPU amdgpu_loaded=$HAVE pm_test=$(cat /sys/power/pm_test 2>/dev/null) wakealarm=$(cat /sys/class/rtc/rtc0/wakealarm 2>/dev/null)"
con "ENTER s2idle gpu=$GPU wake +20 s"
sync
echo freeze > /sys/power/state; RC=$?
con "BACK rc=$RC"
log "back, state write exit $RC"
( i=0; while [ $i -lt 36 ]; do con "alive $i"; i=$((i+1)); sleep 5; done ) </dev/null >/dev/null 2>&1 &
cat /sys/power/pm_wakeup_irq > wakeup-irq.txt 2>&1
cat /proc/driver/rtc > rtc-after.txt 2>&1
[ -w /sys/power/pm_test ] && echo none > /sys/power/pm_test
sleep 3
dmesg > dmesg-after.txt
cat /sys/kernel/debug/suspend_stats > suspend_stats-after.txt 2>/dev/null
if [ $HAVE = 1 ]; then
  timeout 60 cat $DBG/amdgpu_test_ib > test_ib-after.txt 2>&1; echo "exit $?" >> test_ib-after.txt
  CONN=$(modetest -M amdgpu -c 2>/dev/null | awk '$3=="connected" {print $1; exit}')
  ( sleep 4 | timeout 6 modetest -M amdgpu -s "$CONN:1920x1200@XR24" > modetest-after.txt 2>&1; echo "exit $?" >> modetest-after.txt )
  log "test_ib before/after: $(tr '\n' ' ' < test_ib-before.txt | cut -c1-120) || $(tr '\n' ' ' < test_ib-after.txt | cut -c1-120); modetest: $(tail -1 modetest-after.txt)"
fi
diff dmesg-before.txt dmesg-after.txt | grep '^>' > dmesg-delta.txt
grep -i -E 'amdgpu|drm|PM:|ACPI|suspend|resume|error|fail|timeout' dmesg-delta.txt | head -60 > dmesg-delta-key.txt
log "end"
