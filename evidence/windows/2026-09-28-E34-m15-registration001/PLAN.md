# M15 registration001

Hypothesis: appending the DX12 UMD as the fourth UserModeDriverName value makes KMT DX12 name selection visible without a restart.
Baseline: exact CPU171 from M764. Diagnostic UMD DE191A9D, KMT probe E9D82C38. Helpers originate from the closed M14 errors003 trial; transaction module commit 01d854ea. Exact package identities in stage-manifest.json.

Procedure: durable SYSTEM task, 170-second supervisor/180-second task limit. Capture baseline and durable registry plan; append fourth value; query KMT; only if name matches, run the system D3D12 two-queue client in the active console. Restore registry after every closed install attempt, including errors; verify registry, effective KMT name, boot/DWM/health epoch and generation. No KMD, UMD file replacement, ICD change, DWM restart or reboot.

Expected: either KMT name changes, allowing observation of runtime entry into the unfinished DDI, or it does not and the runtime arm is skipped. Neither outcome establishes functional D3D12. Generic supervisor Cpu phase means name query and Gpu means native queue client, not CPU/GPU desktop modes. A failed native client is retained as failed-restored, never reported as a functional pass.

Result: pending. Record measured name behavior and runtime boundary with evidence before further registration decisions.
