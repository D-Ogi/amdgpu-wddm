# Gather capacity and Asteroids TDR

PROVENANCE: Mesa source MIT; DiligentCore Apache-2.0. Source snapshot and diagnostic
patch are included for reproducibility; runtime artifact hashes are in identities.

On KMD151 with candidate4D027149, run003 identifies the apparent sixth-Present stall
as vkAcquireNextImageKHR, after queue Present and frame-fence wait returned.
Run004 logs both Present results: VK_ERROR_DEVICE_LOST (-4) from the first frame.
Diligent ignores the error in Release and exhausts available swapchain images.
Run005, diagnostic ICD6CE12871, establishes the earlier cause:
16 IBs total2012032bytes exceed the1048576-byte gather staging limit; native submit
returns STATUS_INVALID_PARAMETER (0xc000000d) with4 command streams.

Experimental ICD16CDEA79 allocates gather storage sized to the complete IB after
retiring the previous queue submission, retains existing storage until replacement
allocation/map succeeds, and checks total dwords against GFX IB_SIZE via G_3F3_IB_SIZE.
The existing16-fragment collection cap remains; this is not a general multi-IB solution.
Build succeeds. Run006 never launches the application: stale runner hash mismatch.
Run007 reaches255 completed outer Present calls and256 successful Vulkan Present
returns in the preserved log. It does not complete the660-frame run. The log ends
inside the next-image throttle/fence stage and Windows reports bugcheck0x116.
No image correctness or performance claim follows from successful Present returns.

Windows restarted itself at2026-09-25T20:51:07.500Z; no smart-plug operation.
At20:54:28Z the adapter still selected the experimental ICD, so recovery explicitly
restored the baseline9C40083C manifest and enabled its global entry. No further GPU
test performed after the crash. Ordinary logs/events collected, no memory dump transfer.
The growth candidate is not promoted. TDR cause, large-IB regression, full Vulkan/
OpenGL/OpenCL/D3D application scope and same-unit Linux parity remain open.
