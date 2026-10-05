# E45: the Vulkan CTS sparse-resources list on unit A, all 19078 cases (M773)

Unit A, Windows 11, 2026-10-01 07:55-11:21 UTC. `deqp-vk` Release from `vulkan-cts-1.4.6.2` (commit
`f6a29701220f34dd1407513bfe80d74ca7b392ce`), list `external/vulkancts/mustpass/main/vk-default/sparse-resources.txt`
(19078 cases, SHA-256 `FE08C7063E863B8B12B4A46DD9A3C72C16CE254F57D9F77C5CC36A668F358C15`), cut into 147 batches of at
most three minutes, one `deqp-vk` process per batch, run id `icd85077e29`. Route "direct": the batch runner points
the Vulkan loader at a pinned ICD file, no D3D layer involved.

| item | identity |
|---|---|
| ICD for the sweep | `amdgpu_wddm_radv.dll` 85077E29 (ICD v3), the accepted copy kept by native-caps243 |
| ICD for the batch 30 control | 2A13235D, the registered ICD (85077E29 plus the RADV inner-coverage change in `radv_cmd_buffer.c`, which no sparse path uses) |
| KMD, batches 1-29 (07:55-08:05) | 0.7.185.1 |
| KMD, batches 31-32 (08:27) | 0.7.190.1 |
| KMD, batch 30 (earlier attempts) | 0.7.191.1 |
| KMD, batch 30 (last attempt) and 33-147 (10:42-11:21) | 0.7.192.1 |
| desktop | GPU DWM (router + hosted zink), not involved in the direct route |

## Result (one row per case, the last attempt wins: `sweep-icd85077e29.tsv`, SHA-256 B69DDEC1...)

| status | cases |
|---|---|
| Pass | 10778 |
| NotSupported | 8299 |
| Timeout | 1 |
| Fail, Crash, InternalError, ResourceError | 0 |

The one Timeout is `dEQP-VK.sparse_resources.image_sparse_binding.multiple_bind_sparse_info.3d.rgba32ui.512_256_6`,
recorded in batch 30 on KMD 0.7.191.1, whose per-allocation cost grew with the objects VidMm keeps until the process
exits (adapter-list scans in the KMD); the batch never finished there (40 of 105 cases in 173.5 s). On KMD 0.7.192.1
(object index) the whole batch 30, that case included, passes on the registered ICD in 74.5 s, 105 of 105
(`batch30-kmd192-icd2a13235d.tsv`, run id `kmdobj30b`, SHA-256 AE4B8AAF...).

## Why NotSupported (the CTS's own reason, 8305 rows before de-duplication)

| reason | rows |
|---|---|
| device-group tests need more than one physical device | 4715 |
| image format without sparse support (shader intrinsics, binding, block shapes, aliasing, residency, mip tail) | 2311 |
| sample count unsupported for sparse residency / multisampled sparse binding | 1026 |
| image too small for partial binding (rebind) | 104 |
| storage image unsupported for the format | 88 |
| sparseResidency2Samples / 4Samples not exposed | 23 |
| no queue matching the test's requirements | 15 |
| other reasons, fewer than 11 rows each | 23 |

These are capability limits the driver reports; none is a failure. Whether each limit matches RADV on Linux for
this chip is not measured here.
