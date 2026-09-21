E13, a fourth boot, later on 2026-09-21 (the owner offered the machine again while the shim's teardown work was under way).

How it started: a warm restart out of Windows 11 (bc250kmd 0.6.1.0 as the display driver, its bring-up gates closed in
that boot, nothing of ours had touched the GPU beyond the display path), steered into the stick from Windows by
renaming the stick's loader back and setting "set default=2" in its grub.cfg (tools/diagusb/README.md). Stick mode
"readonly": named reads, no write, amdgpu not loaded by the stick. Then session.sh pre, load, state, ib and info.py.

New against boots 1 to 3:
  info.txt           every AMDGPU_INFO query a user-mode driver makes, raw (info.py; query ids generated from the UAPI
                     header by gen_info.py; the VBIOS queries and READ_MMR_REG are left out). Read-only ioctl.
  state.txt          the named register list now includes what Mesa derives its configuration from (GB_ADDR_CONFIG,
                     CC_GC_SHADER_ARRAY_CONFIG, GC_USER_SHADER_ARRAY_CONFIG, CC_RB_BACKEND_DISABLE, GC_USER_RB_BACKEND_DISABLE,
                     PA_SC_RASTER_CONFIG, PA_SC_RASTER_CONFIG_1).
  amdgpu-events-load.txt, compare-e03.txt   a second clean init trace: all 10018 register writes of E03, same order and
                     offsets, 2 values different (both at dword 0x5DF5), no write more and none less, after a warm
                     restart out of Windows with our driver on the display.
  ib.txt             12 fences, 12 interrupts, 12 vectors again (fourth boot out of four).
  extras.txt         session.sh extras: read-only material for later milestones. debugfs by name (fence info, gem, vm and
                     suballocator info, KMS state, clients), device sysfs attributes, power tables, hwmon, the IP
                     discovery tree, the KFD topology, /proc/iomem, the IOMMU/ACPI/firmware lines of dmesg and the
                     list of ACPI tables with sizes. The tables themselves (VFCT carries the video BIOS) are kept
                     outside the repository. No IVRS table: no IOMMU on this board (facts M47).
Copied through redact.py (MAC addresses and USB serial numbers replaced, NUL padding of pp_od_clk_voltage dropped). Nothing edited by hand.
