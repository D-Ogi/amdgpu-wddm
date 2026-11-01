#!/bin/sh
# L49: trained width and speed of the APU-to-FCH link (no visible bridge), read only.
#   l49.sh host-raw   before amdgpu: SMN through 00:00.0 config 0x60/0x64, k10temp unloaded for the step
#   l49.sh gpu        after amdgpu: SMN through amdgpu_regs_pcie (kernel lock)
#   l49.sh dbgfs      any time, only with the boot parameter amd_smn_debugfs_enable
# Also records LnkCap/LnkSta of 00:15.0 and 00:15.1 before and after the SMN reads (same moment, row text).
# Writes: /root/ul-out/l49-<mode>.txt. Only write to hardware: the SMN index register (host-raw: 00:00.0
# offset 0x60, restored at the end; gpu: the kernel's own PCIE_INDEX2 write). Task time < 10 s.
IN=/root/ul
OUT=/root/ul-out
MODE=${1:-gpu}
mkdir -p $OUT
F=$OUT/l49-$MODE.txt
lnk() { for b in 00:15.0 00:15.1; do echo "== $b $1"; lspci -vvv -s $b 2>&1 | grep -E 'LnkCap:|LnkSta:|LnkCtl2:'; done; }
{
echo "# L49 $MODE utc=$(date -u +%Y-%m-%dT%H:%M:%SZ) uptime=$(cut -d' ' -f1 /proc/uptime) kernel=$(uname -r)"
grep -o 'amd_smn_debugfs_enable\|iomem=[a-z]*' /proc/cmdline | sed 's/^/# cmdline: /'
mount | grep -q ' /sys/kernel/debug ' || mount -t debugfs none /sys/kernel/debug
lnk before
case "$MODE" in
host-raw)
	if [ -d /sys/module/amdgpu ]; then echo "ABORT: amdgpu is loaded; use mode gpu"; exit 3; fi
	lsmod | grep -E '^(amd_pmc|amd_atl|amd64_edac|k10temp) ' | sed 's/^/# loaded: /'
	K=0; [ -d /sys/module/k10temp ] && K=1 && rmmod k10temp && echo "# k10temp unloaded for the step"
	python3 $IN/smnread.py $IN/l49-smn.regs host-raw --repeat-state 10
	RC=$?
	[ $K = 1 ] && modprobe k10temp && echo "# k10temp loaded again: $(cat /sys/class/hwmon/hwmon*/name 2>/dev/null | tr '\n' ' ')"
	echo "# smnread exit $RC" ;;
gpu|dbgfs)
	python3 $IN/smnread.py $IN/l49-smn.regs $MODE --repeat-state 10
	echo "# smnread exit $?" ;;
*) echo "usage: l49.sh host-raw|gpu|dbgfs"; exit 2 ;;
esac
lnk after
echo "# end utc=$(date -u +%H:%M:%S)"
} > $F 2>&1
cat $F
# Judge: BIFPLR0_VENDOR_DEVICE_ID low half 1022 (the block is a root port of this core); LC_LINK_WIDTH_RD and
# LC_CURRENT_DATA_RATE agree with BIFPLR0 LnkSta; LC_STATE0 current state repeats in all 10 reads; port 1
# reads no link. host-raw and gpu must give the same values (same SMN target, two index paths).
# Abort: any 'ERR', or all-ones for BIFPLR0: stop, do not widen the address list.
