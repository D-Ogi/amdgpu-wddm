# Backend evaluation, 2026-09-26

Zink is the current prototype candidate, not a proven optimal long-term backend.

- Native D3D frontend -> Gallium -> Zink -> RADV reuses the existing Vulkan hardware path. E34 proves one bounded GPU draw. DWM allocation sharing, synchronization, performance and broader DDI correctness remain unproven.
- Native D3D frontend -> RadeonSI with a WDDM winsys avoids Vulkan translation but requires a second hardware submission/memory integration. Fewer layers alone do not prove better measured performance.
- A native DDI frontend directly targeting Vulkan/RADV could reuse ideas from DXVK, but DXVK itself supplies application D3D/DXGI DLLs, not a drop-in system WDDM DDI driver. It would require a separate adapter layer.
- Mesa's D3D12 Gallium backend requires an existing D3D12 driver. It does not supply the missing native GPU driver on this board.

Keep the selection provisional until correct GPU surface sharing and real DWM draws pass, then measure CPU overhead, frame pacing, bandwidth/copies and maintenance scope. Do not claim September2026 exhaustiveness or speed superiority from a single functional test.

Primary references checked: https://docs.mesa3d.org/drivers/zink.html ; https://docs.mesa3d.org/drivers/d3d12.html ; https://github.com/doitsujin/dxvk . Local source: Mesa05e6c9622e1 and project ADR0009.
