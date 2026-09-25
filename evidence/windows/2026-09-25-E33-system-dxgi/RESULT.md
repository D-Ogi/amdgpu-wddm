# Native DXGI loading under DXVK

PROVENANCE: Mesa and DXVK source MIT; Diligent Apache-2.0.

Unit A, KMD 0.7.151.1. D3D11 runs001/002 time out before a visible window,
including a45-second control with DXVK_LOG_LEVEL=none. Run003 uses diagnostic
DXGI stage markers: factory creation enters Vulkan adapter initialization and
re-enters factory creation before the first call completes. The ordinary DXVK
log has a second CreateDXGIFactory2 warning. Mesa's runtime and Win32 WSI use
LoadLibraryA("DXGI.DLL"), selecting the already loaded app-local DXVK proxy.
DXVK's singleton holds its mutex while constructing the instance.

The Mesa patch loads native DXGI, D3D12 and DComp by full system-directory path.
Candidate98C53B48 builds and the complete port patch replays exactly onto
Mesa05e6c962. D3D11 run004 uses original, uninstrumented DXVK3.1.1 and now detects
BC-250, loads all five required modules and submits GPU work. It exits after
14022ms with assertion swapchain->wait_for_present in wsi_common.c:3674.
Win32 has no callback although RADV advertises KHR_present_wait. This remains
open; D3D11 has not passed. No capability was disabled to hide the failure.

Native Vulkan regression010 completes660frames in29563ms, exit0,1080x720 capture.
Both CPU/GPU CSVs contain all660 ordered rows with finite positive values;
tick/frequency conversion agrees within1e-6ms. Capture SHA256 is preserved in
posthoc-validation.json; full PPM remains outside the repository at
scratch/m12/asteroids-vk151-010-collected. Preview included. This is structural
capture validation, not Linux image parity or performance acceptance.
GPU generation560773206/epoch5 remains unchanged. Baseline system ICD9C40083C
is restored21:28:48Z. No OS or power reset. Candidate is not promoted.

Next: implement Win32 present completion/wait semantics with timeout and
concurrent present/wait validation; then repeat D3D11 and the remaining M12 APIs.
Full CTS, Linux comparisons, OpenGL/OpenCL and D3D9-12 acceptance remain open.
