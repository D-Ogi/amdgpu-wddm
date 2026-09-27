# E44: Win32 external-semaphore CTS cases on the baseline and the fence-share candidate

Question: do the 44 `dEQP-VK.api.external.semaphore.opaque_win32*` CTS cases change between the registered ICD
93B1D1FD and the candidate 6661C2D2 that creates exportable monitored fences with NtSecuritySharing (E43, M581)?

Scope: one `deqp-vk` process per case, phase 1 on the registered ICD, phase 2 with the candidate copied over it,
registered file restored and hash-verified in the finally block. No UMD/DWM/KMD change, no promotion. Run 001
was void (options this `deqp-vk` build rejects); run 002 is the measurement.

Result: `evidence/windows/2026-09-27-E44-cts-external-semaphore-win32/RESULT.md` (M583). Identical on both ICDs:
4 Pass (the info queries), 40 NotSupported, no Fail or Crash. The functional cases are binary-semaphore tests and
the port advertises Win32 export only for timeline semaphores (OPAQUE_WIN32 and D3D12_FENCE), never for KMT
handles, so the CTS touches the fence-share change only through the info query.

Open: whether binary-semaphore export matters to DXVK or vkd3d-proton before spending work on it; promotion of the
candidate still waits for the owner's word.
