# E45: the Vulkan CTS sparse-resources list on unit A

Question: does our Vulkan ICD pass the whole `dEQP-VK.sparse_resources.*` list (19078 cases) on unit A? M15.2
(FL 12_0, tiled resources) names this sweep as the evidence an advertised tier cannot replace (ADR 0017).

Scope: `deqp-vk` from `vulkan-cts-1.4.6.2`, the vk-default sparse-resources list in 147 batches of at most three
minutes, one process per batch, the ICD file pinned by path (no D3D layer). No driver change and no promotion.

Result: `evidence/windows/2026-10-01-E45-cts-sparse-sweep/RESULT.md` (M773): 10778 Pass, 8299 NotSupported, no
Fail or Crash; one Timeout on KMD 0.7.191.1 whose batch passes completely on 0.7.192.1.

Open: a comparison of the NotSupported set with RADV on Linux on the same unit.
