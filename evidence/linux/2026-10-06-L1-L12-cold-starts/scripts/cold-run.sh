#!/bin/sh
# L1 + L12 for one boot of unit A, from the stick (network entry, amdgpu not loaded).
#   sh cold-run.sh <label>      label: warm-after-windows | cold1 | cold2 | cold3 | warm-after-amdgpu
# Runs E21's collector steps "pre" (PCI view, M.2 root port) and "sweep" (pre-driver BAR5 sweep, E03's
# conservative skip list), then the full L12 picture of the M.2 port and the NVMe device. Read-only.
# Output: /media/usb/l1006c/out/<label>/ (on the stick, so it survives the next power cut).
set -u
L=${1:?label}
B=/media/usb/l1006c
O=$B/out/$L
mkdir -p $O
{
	echo "label $L"
	echo "uptime $(cut -d' ' -f1 /proc/uptime)"
	echo "date_utc_skewed $(date -u +%FT%TZ)"
	echo "boot_id $(cat /proc/sys/kernel/random/boot_id)"
	echo "amdgpu_loaded $(grep -c '^amdgpu ' /proc/modules)"
	echo "cmdline $(cat /proc/cmdline)"
} > $O/boot.txt
cat $O/boot.txt
if grep -q '^amdgpu ' /proc/modules; then echo "amdgpu is loaded: no pre-driver sweep in this boot"; exit 1; fi
P=/sys/bus/pci/devices/0000:00:15.0
{
	echo "== lspci -vvv 00:15.0"; lspci -vvv -s 00:15.0
	echo "== devices below the port"; ls $P | grep '^0000:' || echo "none"
	echo "== nvme"; ls /sys/class/nvme 2>&1; lspci -nn | grep -i -e 0108 -e nvme -e "Non-Volatile"
	for f in aer_dev_correctable aer_dev_fatal aer_dev_nonfatal aer_rootport_total_err_cor aer_rootport_total_err_fatal aer_rootport_total_err_nonfatal current_link_speed current_link_width max_link_speed max_link_width; do
		[ -e $P/$f ] && { echo "== $f"; cat $P/$f; }
	done
	echo "== dmesg pcie/nvme"; dmesg | grep -i -e "0000:00:15.0" -e nvme -e "pcieport" -e AER | head -60
} > $O/m2-full.txt 2>&1
grep -E "LnkSta:|LnkCtl2|^== nvme" -A1 $O/m2-full.txt | head -12
cd $B/e21
BC250_OUT=$O/collect sh linux_session_collect.sh pre sweep > $O/collect.log 2>&1
tail -6 $O/collect.log
echo "sweep lines: $(wc -l < $O/collect/sweep-pre.log 2>/dev/null)"
sync
echo DONE-$L
