# Resource teardown control
Hypothesis: closing shared D3D allocations individually retains resource/binding
objects on the long-lived compositor device. Close by runtime resource handle.
Local contract: ref/ddi-display/d3dumddi.md pfnDeallocateCb and
D3DDDICB_DEALLOCATE2; shared resource closure must be atomic.
Controls: same KMD147 and Vulkan ICD; repeated shared pixel probes and two mixed
cycles with pool snapshots after each process. No acceptance if growth continues.
Expected: exact pixels/text/hashes unchanged and settled B2Ww count stops growing.
Existing Notepad surface-padding fix remains applied. No firmware writes.
