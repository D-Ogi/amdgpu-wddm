#!/bin/sh
# Thermal reference under Linux (C56): 110 s of continuous GPU copies (vkmembw warmup) at amdgpu's own clock policy,
# 1 Hz samples of sclk, vddgfx, PPT, edge and Tctl. Stop rule as the lab runner: Tctl >= 87 C for 10 s or >= 89 C.
OUT=/root/thermal-$(date -u +%Y%m%dT%H%M%SZ); mkdir -p $OUT
D=/sys/bus/pci/devices/0000:01:00.0
H=$(for h in /sys/class/hwmon/hwmon*; do [ "$(cat $h/name)" = amdgpu ] && echo $h; done)
K=$(for h in /sys/class/hwmon/hwmon*; do [ "$(cat $h/name)" = k10temp ] && echo $h; done)
/root/chroot-run.sh /tmp/vkmembw-linux/vkmembw --warmup-ms 110000 --iters 5 --placement local > $OUT/load.txt 2>&1 &
L=$!
hot=0; n=0
while kill -0 $L 2>/dev/null; do
  t=$(cat $K/temp1_input)
  echo "$(date +%s) sclk=$(grep '\*' $D/pp_dpm_sclk | tr -d ' *') vddgfx=$(cat $H/in0_input) ppt_uW=$(cat $H/power1_input) edge=$(cat $H/temp1_input) tctl=$t" >> $OUT/samples.txt
  if [ $t -ge 89000 ]; then echo "STOP 89 at $n s" >> $OUT/samples.txt; pkill -f vkmembw; break; fi
  if [ $t -ge 87000 ]; then hot=$((hot+1)); else hot=0; fi
  if [ $hot -ge 10 ]; then echo "STOP 87x10 at $n s" >> $OUT/samples.txt; pkill -f vkmembw; break; fi
  n=$((n+1)); [ $n -ge 170 ] && { echo "STOP bound 170 s" >> $OUT/samples.txt; pkill -f vkmembw; break; }
  sleep 1
done
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do echo "$(date +%s) cool sclk=$(grep '\*' $D/pp_dpm_sclk | tr -d ' *') ppt_uW=$(cat $H/power1_input) edge=$(cat $H/temp1_input) tctl=$(cat $K/temp1_input)" >> $OUT/samples.txt; sleep 1; done
echo $OUT
