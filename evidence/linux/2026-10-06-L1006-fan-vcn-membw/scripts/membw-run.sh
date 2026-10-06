#!/bin/sh
# L39: vkmembw (same C file and SPIR-V as Windows E48) under amdgpu + RADV, with 1 Hz sclk/Tctl/PPT samples.
OUT=/root/membw-$(date -u +%Y%m%dT%H%M%SZ); mkdir -p $OUT
D=/sys/bus/pci/devices/0000:01:00.0
H=$(for h in /sys/class/hwmon/hwmon*; do [ "$(cat $h/name)" = amdgpu ] && echo $h; done)
K=$(for h in /sys/class/hwmon/hwmon*; do [ "$(cat $h/name)" = k10temp ] && echo $h; done)
( while :; do echo "$(date +%s) sclk=$(grep '\*' $D/pp_dpm_sclk | tr -d ' *') vddgfx=$(cat $H/in0_input) ppt_uW=$(cat $H/power1_input) edge=$(cat $H/temp1_input) tctl=$(cat $K/temp1_input)"; sleep 1; done ) > $OUT/samples.txt &
S=$!
for run in "both:--placement both" "cold:--warmup-ms 0" "neg:--negative-control"; do
  name=${run%%:*}; args=${run#*:}
  /root/chroot-run.sh /tmp/vkmembw-linux/vkmembw $args > $OUT/$name.txt 2>&1; echo "$name exit $?" >> $OUT/exits.txt
done
kill $S
cat $OUT/exits.txt; echo $OUT
