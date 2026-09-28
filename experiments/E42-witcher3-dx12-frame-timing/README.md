# E42: The Witcher 3 DX12 build, first frame-timing capture

Question: what frame times does the D3D12 path (vkd3d-proton on the WDDM RADV port) produce for the E41
scene, and what does the Windows present pipeline (dxgkrnl ETW as PresentMon sees it) report for our
present path?

Scope: E41's placement, environment, steering and restoration, plus PresentMon 2.6.0 (console build, MIT)
started in the same interactive task before the game and torn down after it. No DXVK frame cap this time
(E41's `dxgi.maxFrameRate = 30` was DXVK-side and vkd3d-proton's swapchain does not read it). No UMD/ICD/
DWM/KMD change. Game settings as found in `user.settings` (hash recorded, restored if rewritten).

Result: `evidence/windows/2026-09-27-E42-witcher3-dx12-frame-timing/RESULT.md` (M577).

Open: the same save on Linux with the same settings, ray tracing on, mouse steering, the shared-fence KMT
export fix (fork branch `amdgpu-wddm/radv-wddm2-fence-share`, separate experiment).
