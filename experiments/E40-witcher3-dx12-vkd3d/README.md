# E40: The Witcher 3 DX12 build through vkd3d-proton

Question: does a real D3D12 title render on the WDDM RADV port through vkd3d-proton with DXVK's DXGI next
to the executable (the M12 per-application reference path of ADR 0017), after E37/E39 showed feature
level 12_1 with the sparse flag but no D3D12 frame yet?

Scope: The Witcher 3 4.0 `bin\x64_dx12\witcher3.exe` launched to its main menu, bounded to 150 s, no
input, registered ICD 93B1D1FD as is, everything restored afterwards. Procedure derived from the recorded
DX11 run (`witcher3-gpu001`).

Result: `evidence/windows/2026-09-27-E40-witcher3-dx12-vkd3d/RESULT.md` (M571): the menu renders
correctly through our stack; DXR pipelines were created; shared-fence KMT export fails without stopping
the game.

Open: gameplay and the 3D scene (needs input through the recorded input task), frame timing against Linux
on the same title and settings, the shared-fence export failure, ray tracing settings on, sparse under
load. The system D3D12 DDI (G4 / M15) is a separate track; this run is the reference path, not the product.
