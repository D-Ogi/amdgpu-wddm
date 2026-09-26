# M334 - Linux first load, witnessed RLC retirement and failed reload

Unit A booted the existing Alpine 6.18.52-0-lts stick in network-only mode after a Windows restart. No diag.py probe or automatic amdgpu load ran. The module hash is 5984c6732ca23863f7c1e9b6b8bc32beeedc7318c2f8715143fd6ed44a3b7ea4. A new persistent host key was provisioned through trusted Windows SSH and matched on both Linux network interfaces with strict checking. Secrets are outside the repository.

## Positive control and trace preparation

Deferred module event/function filters were accepted before amdgpu load and applied when it loaded. First modprobe exited0; module_present=yes. The register trace and RLC stop function are present. The named-register extract uses tools/regcalc, not handwritten offsets. RLC_CNTL goes1->0->1; SPM VMID goes0->15; scheduler read returns0x58504840. The requested kiq_setting/smu_disable_dpms functions are inlined/absent from available_filter_functions, so their absence is not a negative execution witness.

The first/unload stream used cat on trace_pipe. On unload, the cat process used tracing_splice_read_pipe/sendfile and remained blocked in ring_buffer_wait; the script and SSH handler waited on anon_pipe_write/read. A separate SSH session remained responsive and confirmed amdgpu absent. Terminating that specific reader released the original session, which returned unload_exit=0 and module_present=no. This was an instrument pipe stall, not a GPU/OS hang. Some first/unload trace lines are partial/interleaved; do not use their absence to prove no register access. Both scripts saved here reflect the original cat reader. The second-load script uses dd with4096-byte reads/writes to avoid splice.

## Retirement witness

Unload directly records smu_hw_fini -> smu_smc_hw_cleanup -> gfx_v10_0_rlc_stop, followed by RLC_CNTL read1/write0 before PSP ring destruction. This resolves the M333 runtime-path question for THIS boot; old E13 trace absence does not negate the new witness. It does not establish RLC idle or safe subsequent firmware reload.

## Reload outcome

After successful unload, deferred filters were explicitly rearmed. Second modprobe produced24164 register events, but no exit/completion marker. The last saved sequence reaches RLC resume, reads SPM VMID15 without rewriting it, reads RLC_CNTL0 and writes1. No subsequent scheduler access is visible. Both configured routes then lost TCP22; full subnet discovery found no pinned Linux endpoint. This narrows the Linux failure boundary but does not prove an exact faulting instruction or complete final transport delivery.

The VMID branch is observed to retain15 on reload, consistent with the Windows7-versus8 RLC-stage writes. It is not evidence that this skipped redundant write causes the hang.

USB EFI loader had already been renamed back to bootx64.off before unload, preserving Windows fallback. One owner-authorized AC cycle restored relay ON; recovery record included. The original second-load SSH transport may remain waiting after the power cycle; no duplicate modprobe was launched. Windows readiness is recorded separately after it returns.

## Changes and limits

Added bc250.mode=network to the USB launcher/menu. Existing32diagusb tests pass; actual boot with no amdgpu and working pinned SSH is the runtime mode witness. Original USB overlay, GRUB configuration and key remain backed up on the stick. No firmware/NVRAM changes or disk formatting.

The generic assertion that module filters cannot arm before loading is superseded by this measured positive control. The first failed LF attempt never executed its shell script; the successful LF run is the archived preparation log. Full M9, Windows reentry and Linux reload acceptance remain open.
