#!/bin/sh
# Read-only reconnaissance of the BC-250 from the diagnostic stick. Writes only to /tmp (RAM).
# No disk is mounted or read; block devices are listed by model and size only.
O=/tmp/recon; B=/tmp/recon-bin
rm -rf $O $B; mkdir -p $O $B
run() { f=$1; shift; { echo "\$ $*"; "$@"; } >"$O/$f" 2>&1; }

# Extra tools into RAM only, if the network allows it.
apk add --no-progress --repository https://dl-cdn.alpinelinux.org/alpine/v3.24/main \
	--repository https://dl-cdn.alpinelinux.org/alpine/v3.24/community dmidecode usbutils >"$O/apk-extra.txt" 2>&1

G=/sys/bus/pci/devices/0000:01:00.0
D=$(ls -d /sys/kernel/debug/dri/* 2>/dev/null | while read d; do [ -e "$d/amdgpu_firmware_info" ] && echo "$d" && break; done)
H=$(ls -d $G/hwmon/hwmon* | head -n1)

run lspci-tree.txt lspci -tvnn
run lspci-all.txt lspci -nnk
run lspci-gpu-vvv.txt lspci -vvv -nn -s 01:00
run lspci-gpu-config.txt lspci -xxxx -s 01:00.0
run lspci-rootport.txt sh -c 'lspci -vvv -nn -s $(basename $(dirname $(readlink -f '$G')) | sed "s/^0000://")'
run gpu-resources.txt cat $G/resource
run gpu-sysfs.txt sh -c 'cd '$G'; for f in vendor device subsystem_vendor subsystem_device revision class irq msi_bus numa_node current_link_speed current_link_width max_link_speed max_link_width power_state vbios_version mem_info_vram_total mem_info_vis_vram_total mem_info_gtt_total mem_info_vram_used gpu_busy_percent pp_dpm_sclk pp_dpm_mclk pp_dpm_pcie pp_od_clk_voltage pp_features power_dpm_state power_dpm_force_performance_level pp_power_profile_mode thermal_throttling_logging; do [ -r $f ] && { echo "== $f"; cat $f 2>&1; }; done; echo "== msi_irqs"; ls msi_irqs 2>/dev/null'
run gpu-hwmon.txt sh -c 'cd '$H'; for f in *; do [ -f $f ] && [ -r $f ] && echo "$f = $(cat $f 2>&1)"; done'
run amdgpu-params.txt sh -c 'cd /sys/module/amdgpu/parameters; for f in *; do echo "$f = $(cat $f 2>/dev/null)"; done'
run amdgpu-modinfo.txt sh -c 'modinfo amdgpu | grep -v -E "^(alias|parm)"; echo; modinfo amdgpu | grep -c "^firmware:.*cyan_skillfish"; modinfo amdgpu | grep "^firmware:.*cyan_skillfish"'
run lsmod.txt lsmod
if [ -n "$D" ]; then
	for f in amdgpu_firmware_info amdgpu_pm_info amdgpu_gpu_info amdgpu_vm_info amdgpu_gtt_mm amdgpu_vram_mm amdgpu_gem_info amdgpu_fence_info amdgpu_sa_info amdgpu_evict_vram name clients framebuffer state; do
		[ -r "$D/$f" ] && { echo "\$ cat $D/$f"; head -c 200000 "$D/$f"; } >"$O/debugfs-$f.txt" 2>&1
	done
	run debugfs-listing.txt ls -la $D
	run debugfs-rings.txt sh -c 'ls '$D' | grep -E "^amdgpu_ring_|^amdgpu_mqd_"'
	[ -r $D/amdgpu_gca_config ] && head -c 4096 $D/amdgpu_gca_config >$B/gca_config.bin 2>/dev/null
	[ -r $D/amdgpu_vbios ] && cat $D/amdgpu_vbios >$B/vbios-debugfs.rom 2>/dev/null
	[ -r $D/amdgpu_discovery ] && cat $D/amdgpu_discovery >$B/ip-discovery.bin 2>/dev/null
fi
# VBIOS through the PCI ROM BAR (the standard sysfs way; enable, read, disable).
if [ -w $G/rom ]; then echo 1 >$G/rom; cat $G/rom >$B/vbios-pci-rom.rom 2>/dev/null; echo 0 >$G/rom; fi
run ip-discovery-sysfs.txt sh -c 'cd '$G'/ip_discovery 2>/dev/null && find . -maxdepth 4 -type d | sort | head -n 400'

# Platform
run cpuinfo.txt sh -c 'grep -E "model name|cpu family|^model|stepping|microcode|siblings|cpu cores|flags" /proc/cpuinfo | sort | uniq -c'
run meminfo.txt cat /proc/meminfo
run iomem.txt cat /proc/iomem
run ioports.txt cat /proc/ioports
run interrupts.txt cat /proc/interrupts
run cmdline.txt cat /proc/cmdline
run e820-and-efi.txt sh -c 'dmesg | grep -E "BIOS-e820|efi:|EFI v|SMBIOS|DMI:|secureboot|Secure boot|ACPI: (RSDP|XSDT|FACP|DSDT|SSDT|VFCT|IVRS|MCFG|HPET|APIC|CRAT|BGRT|TPM2|WSMT|FPDT|UEFI)"'
run iommu.txt sh -c 'dmesg | grep -i -E "iommu|AMD-Vi|ivrs"; echo "groups: $(ls /sys/kernel/iommu_groups 2>/dev/null | wc -l)"'
run dmi.txt sh -c 'for f in /sys/class/dmi/id/*; do [ -f $f ] && [ -r $f ] && echo "$(basename $f) = $(cat $f 2>/dev/null)"; done'
run dmidecode.txt sh -c 'command -v dmidecode >/dev/null && dmidecode -t bios -t system -t baseboard -t processor -t memory -t slot 2>&1'
run acpi-tables.txt sh -c 'ls -la /sys/firmware/acpi/tables /sys/firmware/acpi/tables/dynamic 2>/dev/null'
run efi-vars.txt sh -c 'ls /sys/firmware/efi/efivars 2>/dev/null | sed -E "s/-[0-9a-f-]{36}$//" | sort | uniq -c | sort -rn | head -n 80; echo; od -An -tu1 /sys/firmware/efi/efivars/SecureBoot-* 2>/dev/null; od -An -tu1 /sys/firmware/efi/efivars/SetupMode-* 2>/dev/null'
run lsusb.txt sh -c 'command -v lsusb >/dev/null && { lsusb; lsusb -t; }'
run block-devices.txt sh -c 'for b in /sys/block/*; do n=$(basename $b); case $n in loop*|ram*) continue;; esac; echo "$n size_512=$(cat $b/size) rotational=$(cat $b/queue/rotational) removable=$(cat $b/removable) model=$(cat $b/device/model 2>/dev/null) transport=$(readlink -f $b/device | grep -o -E "usb|nvme|ata" | head -n1)"; done; ls /sys/class/nvme 2>/dev/null'
run sensors-all.txt sh -c 'for h in /sys/class/hwmon/hwmon*; do echo "== $(cat $h/name)"; for f in $h/*_input $h/pwm[0-9] $h/pwm[0-9]_enable $h/pwm[0-9]_mode; do [ -r $f ] && echo "$(basename $f) $(cat ${f%_input}_label 2>/dev/null) = $(cat $f 2>&1)"; done; done'
run dmesg-full.txt dmesg
run kernel-config-amd.txt sh -c 'zcat /proc/config.gz 2>/dev/null | grep -E "AMDGPU|DRM_AMD|HSA_AMD|IOMMU|CONFIG_PCI_MSI|STRICT_DEVMEM|IO_STRICT|LOCK_DOWN" '

# ACPI tables as binaries (VFCT carries the VBIOS image the OS is supposed to use)
mkdir -p $B/acpi; for t in /sys/firmware/acpi/tables/* /sys/firmware/acpi/tables/dynamic/*; do [ -f $t ] && cat $t >$B/acpi/$(basename $t).aml 2>/dev/null; done
( cd $B && sha256sum $(find . -type f | sort) ) >"$O/binaries-sha256.txt" 2>&1
( cd $B && ls -la . acpi ) >>"$O/binaries-sha256.txt" 2>&1
echo "recon done: $(ls $O | wc -l) text files, $(find $B -type f | wc -l) binaries"
