# M333 - RLC retirement crosses the SMU layer

Source review found the missing generic Linux call chain: smu_hw_fini -> smu_smc_hw_cleanup -> smu_disable_dpms -> gfx.rlc.funcs->stop, conditional on PM state, IP version and platform flags. gfx_v10_0_hw_fini and psp_hw_fini alone do not describe RLC retirement.

smu_disable_dpms first calls system_features_control(false) and notify_rlc_state(false). The Cyan Skillfish pptable lacks BOTH callbacks. smu_internal.h dispatches either missing callback to return zero. Therefore the Vangogh workaround comment is NOT evidence of an omitted Cyan Skillfish SMU mailbox command. Do not copy a Vangogh message into this driver.

The E13 boot7/8 unload trace summary reports no RLC_CNTL write, despite successful unload. This mismatch with the conditional generic source path remains unexplained. Trace scope, actual kernel/source version and runtime branch selection must be established. The observed kernel was Alpine 6.18.52-0-lts; sources.json pins the files reviewed here but does not establish exact correspondence to that running kernel.

M35's PSP reload register observations remain valid. Its stronger statement that Linux avoids the failure by stopping RLC was not demonstrated by its Windows comparison file. That causal/reference clause is corrected; M55 also records Linux reload failure. The Windows stop helper already clears RLC enable, and M332 still fails warm startup. No evidence supports adding an arbitrary delay or a platform-specific SMU message as a fix.

RLC stage write-count comparison: bc250_update_spm_vmid writes only when the masked VMID changes. Thus a 7-versus-8 count is compatible with normal retained state, not proof of a skipped operation. The exact differing write needs an ordered trace.

Next Linux L34/L17 capture should include smu_hw_fini, smu_disable_dpms and gfx_v10_0_rlc_stop call/return witnesses and the named RLC accesses, tied to kernel/module identity. Arm tracing before module load, not a module-only event filter while the module is absent. Preserve streamed output across failure. No new hardware experiment in this review.
