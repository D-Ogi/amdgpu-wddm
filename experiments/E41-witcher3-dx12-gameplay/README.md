# E41: The Witcher 3 DX12 build, save load and 3D scene

Question: after E40 (menu), does the DX12 build load a save and render the in-game 3D scene through
vkd3d-proton on the WDDM RADV port, and what does the vkd3d log show on that path?

Scope: E40's placement and environment, a 600 s bound with a stop file, input from the development PC through
the recorded interactive input task, screenshots between steps. No settings change, no UMD/ICD/DWM/KMD change.

Result: `evidence/windows/2026-09-27-E41-witcher3-dx12-gameplay/RESULT.md` (M573): the save loads, the scene
at Kaer Morhen renders correctly with HUD, Tctl peaks at 77.8 during loading, no new vkd3d warnings beyond
E40's, everything restored.

Open: frame timing (a D3D12-side counter, then the same save on Linux with the same settings), ray tracing
on, longer runs and the thermal curve at the lab operating point, mouse input for steering, the shared-fence
KMT export failure in the port.
