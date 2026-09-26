# M340 - RLC retirement observation preparation

Hypothesis: a persisted post-stop RLC_CNTL/GRBM_STATUS2 observation can distinguish a disabled RLC with the AMD RLC_BUSY indicator still set from one with that indicator clear, before PSP retirement. Neither result alone proves all outstanding memory accesses retired or a safe reload.

Existing E28 streams contain RLC_CNTL but no complete GRBM_STATUS2 records. Existing104 first/warm PSP command reports are compared before choosing a change. Add one read-only snapshot after the existing GFX undo/default bank restoration and before teardown. Use generated named offsets and existing MmioRead allow-list, logging both NTSTATUS values and raw words. No polling, new reset, mask-based admission change or additional delay. Preserve existing hardware access policy.

Validate deterministic register generation and WDK build locally. Undeployed until a versioned startup/retirement trial has an explicit plan and preflight. The current105 GPU session must not be restarted just to install this source change.
