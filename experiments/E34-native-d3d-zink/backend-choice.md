# Backend evaluation, 2026-09-26

Zink is the current prototype candidate, not a proven optimal long-term backend.

- Native D3D frontend -> Gallium -> Zink -> RADV reuses the existing Vulkan hardware path. E34 proves one bounded GPU draw. DWM allocation sharing, synchronization, performance and broader DDI correctness remain unproven.
- Native D3D frontend -> RadeonSI with a WDDM winsys avoids Vulkan translation but requires a second hardware submission/memory integration. Fewer layers alone do not prove better measured performance.
- A native DDI frontend directly targeting Vulkan/RADV could reuse ideas from DXVK, but DXVK itself supplies application D3D/DXGI DLLs, not a drop-in system WDDM DDI driver. It would require a separate adapter layer.
- Mesa's D3D12 Gallium backend requires an existing D3D12 driver. It does not supply the missing native GPU driver on this board.

Keep the selection provisional until correct GPU surface sharing and real DWM draws pass, then measure CPU overhead, frame pacing, bandwidth/copies and maintenance scope. Do not claim September2026 exhaustiveness or speed superiority from a single functional test.

Primary references checked: https://docs.mesa3d.org/drivers/zink.html ; https://docs.mesa3d.org/drivers/d3d12.html ; https://github.com/doitsujin/dxvk . Local source: Mesa05e6c9622e1 and project ADR0009.

## Native sharing and upstream maintenance

M532 verifies one KMT-created shared allocation imported by RADV. M533 verifies
native D3D shader rendering through Zink into that same type of allocation.
Neither establishes sharing of runtime-created non-shared back buffers.

Before choosing shared-device versus hosted integration, capture every relevant
CreateResource MiscFlags/BindFlags and AllocateCb hKMResource on the runtime path.
The existing verbose Resource logging is conditional on shared/primary-optional
resources, so it is insufficient to inventory non-shared cases. Avoid per-frame
logging; record allocation events with bounded counters.

Local IRC notes from 2026-09-07 describe a proposed D3DDDI callback extension and
Mesa-internal versioning, not an accepted extension or a proven RADV contract.
Hosted callbacks must obey the runtime threading and lifetime rules even when
called inside an ICD. Application-level sharing examples do not establish that.

The local !44717/!44715 discussion proposes removing d3d10umd and the Gallium TGSI
interface. Preserve our frontend and TTN dependencies until a tested NIR boundary
exists. M531 exercised TGSI conversion inside Zink, not independence from Gallium
TGSI. Any upstream contribution must distinguish CPU DWM from the experimental
GPU path and cannot promise zero rebase cost. No public message has been sent.
