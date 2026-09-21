Binary captures from unit A, same SSH session as ../2026-09-21-E01-recon/ (SHA-256 sums there, in binaries-sha256.txt).

- acpi/*.aml        raw ACPI tables from /sys/firmware/acpi/tables (DSDT, SSDT1, SSDT2, FACP, APIC, MCFG, HPET, CRAT, CDIT, FIDT, FPDT, BGRT, UEFI, FACS)
- ip-discovery.bin  the GPU's IP discovery table as amdgpu read it (debugfs amdgpu_discovery)
- gca_config.bin    debugfs amdgpu_gca_config (first 144 bytes are all there is)

Deliberately absent: the VBIOS (debugfs amdgpu_vbios) and the ACPI VFCT table, which embeds the same VBIOS image.
AMD's VBIOS is not ours to redistribute; both stay in the owner's private firmware folder.
