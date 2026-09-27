# E43: candidate ICD with shareable fences and the WSI present log

Question: does creating exportable monitored fences with NtSecuritySharing remove vkd3d-proton's shared-fence
export failures, and what per-present timing does the port's GDI present path show for the E41 scene?

Scope: candidate ICD 6661C2D2 (fork branches `amdgpu-wddm/radv-wddm2-fence-share` and
`amdgpu-wddm/radv-wddm2-present-log` on the E38 baseline) swapped in place of the registered ICD for one
run and restored; E14 compute smoke as positive control; the E41/E42 steered Witcher 3 DX12 run with
`BC250_WSI_PRESENT_LOG`. No UMD/DWM/KMD change, no promotion.

Result: `evidence/windows/2026-09-27-E43-witcher3-dx12-present-log/RESULT.md` (M580 present timing, M581
fence export). Zero export errors; scene locked at 29.7 presents/s with the present call costing about 16 ms
per frame (CPU copy 3.2, BitBlt 1.3, DwmFlush 11.3 ms median).

Open: what locks the scene at two refresh periods (game vsync, FIFO mode, DwmFlush); a present path without
the CPU copy; the Win32 external-semaphore CTS cases on the candidate before promotion; the same save on Linux.
