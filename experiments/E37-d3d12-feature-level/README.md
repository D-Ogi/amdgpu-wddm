# E37: D3D12 feature level through vkd3d-proton on the WDDM RADV port

Question: which D3D12 feature level and option tiers does vkd3d-proton report on our RADV, and what is
missing for 12_1 (Cyberpunk 2077 class titles)? Hypothesis before the run: everything but tiled resources,
because sparse binding is still gated in the port.

Scope: one probe executable (`fl-probe.c`, CheckFeatureSupport dump, no rendering) with the M12 vkd3d-proton
package DLLs next to it, run on unit A under the registered ICD and under candidates swapped in with the
M538 procedure (backup, hash checks, restore in finally). D3D12/DXGI creation hangs in session 0 like the
E33 D3D12 applications, so the probe runs as an interactive scheduled task (worker/launch/poll scripts).

Procedure and state: see `evidence/windows/2026-09-27-E37-d3d12-feature-level/RESULT.md` (M564). Runs 001
and 003 with the pre-M496 baseline hung; run 004 took a minidump; runs 005 (D3DKMT enumeration candidate,
plus the E14 smoke) and 006 (E36 consolidated ICD) measured FL 11_1 with TiledResourcesTier 0 as the only
gap to 12_1.

Done since: the sparse flag gives 12_1 / tier 4 on the same probe (E39, M570); the D3DKMT enumeration
candidate is on the fork and became the registered baseline (E38, M569). Open: the WSI D3D12 interop helpers
still load DXGI/D3D12 by module path.
