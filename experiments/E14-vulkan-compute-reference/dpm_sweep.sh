#!/bin/sh
# E14, the clock half: the same work at 1000 MHz and at amdgpu's default (1500 MHz on unit A), because Windows keeps this
# unit at 1000 MHz and times are only comparable at the same clock.
#
# amdgpu refuses power_dpm_force_performance_level low/high on this part (measured: EINVAL), so the only interface is
# pp_od_clk_voltage, which cyan_skillfish_od_edit_dpm_table() (cyan_skillfish_ppt.c:438-533) turns into two SMU
# messages, RequestGfxclk and ForceGfxVid; it cannot set a clock without naming a voltage. The one point set here is
# 1000 MHz at the voltage the part runs its 1500 MHz at by itself (read from hwmon first, 906 mV on unit A): a lower
# clock at an unchanged voltage, inside the driver's own range (1000 to 2000 MHz, 700 to 1129 mV). No higher clock and no
# higher voltage is tried. "r" and "c" afterwards give clock and voltage back to the firmware (UnforceGfxVid).
# A sensor line per second goes to sensors-<point>.txt while the work runs; above 85 C everything goes back to default.
set -u
O=/tmp/e14/dpm
M=/tmp/e14/models
mkdir -p $O
DEV=$(dirname "$(ls -d /sys/class/drm/card*/device/mem_info_vram_total | head -1)")
H=$(ls -d $DEV/hwmon/hwmon* | head -1)
OD=$DEV/pp_od_clk_voltage
sense() { echo "$(date +%T) power_uW $(cat $H/power1_input) temp_mC $(cat $H/temp1_input) sclk_Hz $(cat $H/freq1_input) vddgfx_mV $(cat $H/in0_input)"; }
back() { echo r > $OD; echo c > $OD; }
trap back EXIT INT TERM
MV=$(cat $H/in0_input)
echo "voltage at the default clock: $MV mV"
[ "$MV" -ge 850 ] && [ "$MV" -le 950 ] || { echo "unexpected voltage, nothing is set"; exit 1; }
for point in 1000 default; do
	if [ $point = default ]; then back; else echo "vc 0 $point $MV" > $OD && echo c > $OD || { echo "point $point refused"; continue; }; fi
	sleep 2
	echo "== $point: $(tr -d '\000' < $OD | tr '\n' ' ' | cut -c1-120)"
	echo "idle: $(sense)"
	( while :; do
		sense
		[ "$(cat $H/temp1_input)" -gt 85000 ] && { echo "TOO HOT, back to default"; back; }     # the workspace's limit
		sleep 1
	done ) > $O/sensors-$point.txt 2>&1 &
	watcher=$!
	/tmp/e14/build/vkcompute /tmp/e14/build/shaders --runs 21 > $O/vkcompute-$point.txt 2>&1
	echo "vkcompute exit $?"; grep -E '^(sgemm|sgemm_tiled|mlp|inthash|saxpy) ' $O/vkcompute-$point.txt | sed 's/ first_words.*//; s/hash=.*match=/match=/' | cut -c1-110
	timeout 300 llama-bench -m $M/tinyllama-1.1b-chat-v1.0.Q4_0.gguf -ngl 99 -p 512 -n 128 -r 3 2>/dev/null | tee $O/bench-$point.txt | grep -E 'pp512|tg128' | cut -c60-130
	kill $watcher 2>/dev/null
	echo "under load: max power $(awk '{if ($3>m) m=$3} END {print m}' $O/sensors-$point.txt) uW, max temp $(awk '{if ($5>m) m=$5} END {print m}' $O/sensors-$point.txt) mC, sclk seen: $(awk '{print $7}' $O/sensors-$point.txt | sort -u | tr '\n' ' '), vddgfx seen: $(awk '{print $9}' $O/sensors-$point.txt | sort -u | tr '\n' ' ')"
done
back
sleep 2
echo "== back: $(sense)"
dmesg | grep -iE 'amdgpu.*(timeout|reset|fault|thermal|throttl|invalid|failed)' | tail -5
