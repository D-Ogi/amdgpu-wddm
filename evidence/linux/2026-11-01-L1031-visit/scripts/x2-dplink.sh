#!/bin/sh
# X2: why does amdgpu train RBR x4 when HBR x4 is verified? Capture link policy evidence.
#   x2-dplink.sh   (post-amdgpu only, R0, < 30 s)
# Writes /root/ul-out/x2-dplink.txt: DP registers, connector properties, DTN link lines, dmesg link training.
IN=/root/ul
OUT=/root/ul-out
mkdir -p $OUT
F=$OUT/x2-dplink.txt
{
echo "# X2 utc=$(date -u +%Y-%m-%dT%H:%M:%SZ) uptime=$(cut -d' ' -f1 /proc/uptime) kernel=$(uname -r)"
mount | grep -q ' /sys/kernel/debug ' || mount -t debugfs none /sys/kernel/debug
echo "== DP0 link and DPHY registers"
python3 $IN/regread.py $IN/x2-dplink.regs x2
echo "== DRM connectors: link_status, max_bpc"
for d in /sys/class/drm/card*-DP-*; do [ -d "$d" ] || continue
	echo "--- $d"
	cat $d/status 2>/dev/null | sed 's/^/  status: /'
	for p in max_bpc link_status dpms; do [ -f "$d/$p" ] && echo "  $p: $(cat $d/$p 2>/dev/null)"; done
	# connector properties via modetest
done
echo "== modetest -c (connectors only, DP entries)"
modetest -c 2>/dev/null | awk '/^[0-9].*DP/{found=1} found{print; if(/^[0-9]/ && !/DP/) found=0}' | head -80
echo "== DTN log: link encoder lines"
DBG=$(ls -d /sys/kernel/debug/dri/*/ | while read d; do [ -e "$d/amdgpu_dm_dtn_log" ] && echo $d; done | head -1)
cat $DBG/amdgpu_dm_dtn_log 2>/dev/null | grep -i -E 'L_ENC|STREAM|LINK|PHY|DSC' | head -40
echo "== dmesg: link training"
dmesg | grep -i -E 'link_rate|link rate|train|link_bw|dp_link|pattern|LTTPR|HPD|SST|MST|lane.*count|bandwidth|verify|fail' | tail -60
echo "# end utc=$(date -u +%H:%M:%S)"
} > $F 2>&1
cat $F
# Judge: DP0_DP_LINK_CNTL[3:0] DP_LINK_TRAINING_COMPLETE + lane count, bits[11:8] link BW setting code:
#   0x06 = RBR (1.62 Gbps), 0x0A = HBR (2.7 Gbps), 0x14 = HBR2 (5.4 Gbps). If amdgpu chose RBR, dmesg
#   should show the decision (verify_link_cap, dp_decide_link_settings) or a training failure at HBR.
# Abort: none (read only).
