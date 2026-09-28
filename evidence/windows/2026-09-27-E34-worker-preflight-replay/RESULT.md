# Supplement to M675: CPU-only preflight replay

2026-09-27, unit A. Historical PowerShell event records identify the failed
DWM033 worker as PID5252: engine available13:47:00.1451956Z, stopped
13:47:01.3251872Z. No retained event from that process states the preflight
exception. Event records were read only; private originals remain under
scratch/g0-hosted/dwm033 (UUID/host/runspace fields are not published).

The original wsi-control.ps1 prefix through Snapshot was replayed with only its
output directory redirected to a new attempt. The script ends before child
launch or any GPU work. Existing DWM033 files were read, not changed. New wrapper
397548a captured the expected Baseline UMD/ICD mismatch at Snapshot line25 on
13:57:16.6121020Z: current CPU DLLs do not match the old GPU manifest/capability
ICD. Earlier health/clock/artifact checks passed. This validates the wrapper on
the actual lab and demonstrates that preflight can reach the identity gate on
the CPU baseline; it does not reconstruct DWM033's historical exception.

No DWM restart, PnP transition, scheduled task or window was started. No facts
about KMT admission or copy completion follow. DWM034 retains the original gates,
adds health/clock/hash receipts before assertions, and wraps all worker preflight.
