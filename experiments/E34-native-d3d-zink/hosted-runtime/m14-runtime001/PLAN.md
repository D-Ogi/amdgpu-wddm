# M14 runtime001: system D3D11 bring-up

Prepared source only; no registration or lab run yet. The goal is Microsoft system d3d11.dll creating the M14 UMD device on exact KMD171, then executing a small d3d11bench workload through hosted RADV. No app-local d3d11.dll or dxgi.dll is permitted.

The router selects only C:\BC250\m14\runtime001\d3d11bench.exe with BC250_M14_RUNTIME_PROBE=1 and an enable file younger than60 seconds (future timestamp rejected). Selection is fixed at the first adapter open. This is a launch-admission TTL, not a process timeout. All other clients forward to the unchanged CPU baseline DLL. Both libraries are loaded with dependency search restricted to their directory and System32.

Required before running: exact package/hash and configuration verification; fresh CPU171 preflight; autonomous registration restoration and process-tree supervision below180 seconds; debug-string/exception capture for the child; native system module witness; postflight verifying registration and CPU DWM. Remove the enable file and restore registry values even when the client fails. No permanent registration change is intended.

Positive control: measured standalone engine caps002 supplies the exact-pair configuration. It does not prove this runtime boundary. The first runtime run must record HRESULTs, negotiated interface/build/feature level, actual modules and adapter identity. A failed CreateDevice is a diagnostic result, not GPU success. Later image/sharing/Present and matched performance validation remain required.

Local selection tests cover exact/case-insensitive path, wrong directory/DWM, absent/wrong flag, expiry boundary and future timestamp. They do not prove live Windows UMD discovery or registry rollback.
