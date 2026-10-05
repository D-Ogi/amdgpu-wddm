# M552: resource destruction after hosted device loss

The UMD no longer returns early from DestroyResource on render/Present completion
failure. It marks hosted device loss before deallocation; the existing hosted
SubmitCommand status check rejects subsequent GPU submissions. CPU transfer storage
and the resource reference are released on all paths. Destruction of the device
continues after a failed Present wait instead of leaking its context/host state.

Separate FreeGpuVirtualAddress is removed from DestroyResource. Deallocate2 uses
zero flags, preserving VidMm deferred destruction for queued commands. The local
Microsoft reference states that allocation destruction also frees its GPU virtual
addresses. References: ref/ddi-display/d3dumddi.md PFND3DDDI_DEALLOCATE2CB (WDK26100);
ref/windows-driver-docs/windows-driver-docs-pr/display/allocation-usage-tracking.md
and per-process-gpu-virtual-address-spaces.md, snapshot110f60ea.
Public references: https://learn.microsoft.com/windows-hardware/drivers/display/allocation-usage-tracking
and https://learn.microsoft.com/windows-hardware/drivers/display/per-process-gpu-virtual-address-spaces

Three-file patch replay and all eight scoped build gates pass. Candidate UMD
0045C47D uses hosted ICD3508416F; full hashes are in artifacts.json.
Unit A run033 repeats seven partial creation failures and three exact4096-pixel
readbacks. Runs034/035 inject submit refusal and invalid local fence observation.
Both report device removal887a0005 to the application and exit zero. Each loss
run logs successful Deallocate with flags0, paging queue destruction and completion
of DestroyDevice. checks.json records the observed counts; raw logs are unchanged.

Every run restores CPU UMD8279AC7F and registered ICD9C40083C; DWM9648 is unchanged.
No actual hardware hang/TDR, timeout-duration experiment or failing Deallocate
callback was induced. A failed Deallocate is reported and the remaining cleanup
continues; eventual OS reclamation in that case is not measured. This does not
prove broad leak freedom, GPU DWM or absence of full-frame CPU copying. G0 remains
open; graphics-stage separate samplers and native textured pixel controls are next.
