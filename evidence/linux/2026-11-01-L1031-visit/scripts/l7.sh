#!/bin/sh
# L7 remainder: firmware framebuffer address and size (screen_info -> sysfb -> simpledrm), the GOP scan-out
# address in the memory controller's space, and where the GART table and the PSP TMR sit. Read only.
#   l7.sh pre    before `modprobe amdgpu` (simpledrm owns the GOP framebuffer; BAR5 read through sysfs)
#   l7.sh post   after amdgpu (debugfs register file; GART/TMR lines of dmesg)
# Writes /root/ul-out/l7-<phase>.txt. Task time < 5 s.
IN=/root/ul
OUT=/root/ul-out
PH=${1:-pre}
D=/sys/bus/pci/devices/0000:01:00.0
mkdir -p $OUT
F=$OUT/l7-$PH.txt
{
echo "# L7 $PH utc=$(date -u +%Y-%m-%dT%H:%M:%SZ) uptime=$(cut -d' ' -f1 /proc/uptime)"
echo "== BAR table of 01:00.0 (start end flags; BAR0 = VRAM aperture, BAR5 = registers)"
head -6 $D/resource
echo "== driver: $(readlink $D/driver 2>/dev/null || echo none)"
echo "== /proc/iomem: the GPU, BOOTFB / simple-framebuffer and the carve-out neighbourhood"
grep -n -i -E 'BOOTFB|simple-framebuffer|efifb|0000:01:00|0000:00:08.1|amdgpu|System RAM|Reserved' /proc/iomem
echo "== framebuffer devices"
for f in /sys/class/graphics/fb*; do [ -e "$f" ] || continue
	echo "$f name=$(cat $f/name 2>/dev/null) virtual_size=$(cat $f/virtual_size 2>/dev/null) stride=$(cat $f/stride 2>/dev/null) bpp=$(cat $f/bits_per_pixel 2>/dev/null)"; done
ls /sys/bus/platform/devices/ | grep -i -E 'simple-framebuffer|efi-framebuffer|vesa' | sed 's/^/platform: /'
echo "== dmesg: firmware framebuffer, aperture handover, GART, TMR, VRAM"
dmesg | grep -i -E 'efifb|simple-framebuffer|simpledrm|sysfb|BOOTFB|framebuffer|aperture|remove.*conflicting|PCIE GART|for PSP TMR|VRAM:|GART:|AGP:|resiz|BAR'
if [ "$PH" = pre ]; then
	echo "== registers through BAR5 (PROT_READ, no driver bound)"
	python3 $IN/bar5read.py $IN/l7-fwfb.regs gop
else
	mount | grep -q ' /sys/kernel/debug ' || mount -t debugfs none /sys/kernel/debug
	echo "== registers through debugfs amdgpu_regs"
	python3 $IN/regread.py $IN/l7-fwfb.regs amdgpu
	DBG=$(ls -d /sys/kernel/debug/dri/*/ | while read d; do [ -e "$d/amdgpu_firmware_info" ] && echo $d; done | head -1)
	echo "== amdgpu_vram_mm (allocations; the GART table and the TMR are near the top of VRAM)"
	tail -40 $DBG/amdgpu_vram_mm 2>&1
fi
echo "# end utc=$(date -u +%H:%M:%S)"
} > $F 2>&1
cat $F
# Judge (offline): GOP surface MC address = HUBPREQ0 PRIMARY_SURFACE_ADDRESS(_HIGH); its VRAM offset =
# MC - (FB_LOCATION_BASE << 24); its CPU address = BAR0 start + offset and must equal the BOOTFB start of
# /proc/iomem; its system physical address = (FB_OFFSET << 24) + offset. GART table: dmesg "table at" (MC) and
# GCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR (MC, 4 KiB units per gmc_v10) must agree; TMR: "reserve ... for PSP TMR".
