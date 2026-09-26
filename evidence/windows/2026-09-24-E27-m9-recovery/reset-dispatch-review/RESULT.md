# M351 - Linux reset dispatch is not a working reset reference for this APU

Read-only source review; no hardware action, deployment or driver change.
Reference: local Linux6.18.0, source hashes and numbered excerpts supplied.
Do not silently equate this source with the E28 Linux6.18.52 runtime.

1. GFX10.1.3/10.1.4 selects nv_common_ip_block. nv_common_early_init installs
nv_asic_funcs, whose need_full_reset callback unconditionally returns true.
amdgpu_device_ip_need_full_reset honors it before considering individual IPs;
pre_asic_reset invokes generic IP soft reset only when full reset is not needed.
The mere presence of gfx_v10_0_soft_reset in the GFX function table therefore
is not evidence that the ordinary recovery dispatcher uses it on this family.

2. gfx_v10_0_soft_reset builds CP/GFX bits from GRBM_STATUS and RLC from
GRBM_STATUS2. It stops RLC, disables GFX/MEC parsing, asserts the selected reset
mask, reads it back, waits50us, deasserts, reads back and waits50us. It returns0
without checking RLCbusy, and does not restart RLC inside this function.
The isolated callback used in M350 instead uses two field read/modify/writes
and delays. In particular, its first post-assert read is the deassert RMW after
the first delay; it has no immediate post-write readback before that delay.
These differences are experiment candidates, not established failure causes.
M350 additionally required an immediate busy-clear result; that postcondition
is a project experiment, not an AMD acceptance rule. Do not bypass it merely
because the upstream callback returns0: warm reentry is still unproved.

3. nv_asic_reset_method AUTO chooses MODE1 or BACO in the default MP1 branch;
selection of BACO depends on platform support and was not measured here.
MODE1 calls amdgpu_device_mode1_reset. That function saves PCI state, disables
bus mastering, chooses supported SMU MODE1 or PSP reset, restores PCI state,
waits for PSP bootloader and checks memsize. Copying only a mailbox command
would omit this surrounding device lifecycle.

4. Cyan Skillfish's PPT table has no mode1_reset_is_support or mode1_reset
entry. smu_mode1_reset_is_support defaults false when its callback is absent.
For MP0 11.0.8/Cyan Skillfish2, PSP selects psp_v11_0_8_funcs. That table has
ring operations only, no mode1_reset. The shared psp_mode1_reset macro returns
false (integer0) when the callback is absent. Therefore the selected PSP MODE1
callback layer can report0 while executing no reset callback. This is a precise
source limitation, not proof of a successful reset or the cause of E28 hangs.
The complete surrounding MODE1 path can still mutate device/PCI state or fail.

Decision: do not import a purported complete AMD soft/full reset as a proven
remedy. Keep110experimental gate closed. Next isolate the actual MMIO reset
write/readback ordering and state observations, preserving the original
callback baseline, before broadening reset domains. No new CP/GFX reset mask
is justified by RLCbusy alone; the M350 trial did not capture GRBM_STATUS.
If a Linux runtime comparison is used, first obtain its exact source and
instrument dispatch/callback execution; return0 alone is insufficient.
No claim is made about cache coherency, DMA completion or warm-start acceptance.
