# D3D12 shared resources: state and plan

This document records what the D3D12 driver does with a shared resource today, why each half fails, and the
work that makes sharing real. It answers the defect BD-075.

## What works and what fails

Shared fences pass in every direction. The D3D12 UMD DDI has no fence sharing in it. `D3D12DDI_FENCE`
(`d3d12umddi.h`) holds two GPU addresses and one flag bit, and the flag is `BOTTOM_OF_PIPE`. The kernel
object, its NT handle, `CreateSharedHandle` and `OpenSharedHandle` all belong to the runtime. A shared fence
and a local fence look the same at the DDI, so the driver needs no code for the difference.

Shared resources fail in every direction:

| Call | Slot | Why it fails |
|---|---|---|
| `CreateCommittedResource` with `D3D12_HEAP_FLAG_SHARED` | D60 `pfnCreateHeapAndResource` | The shell declines the heap. The DDI defines no shared heap flag, so the runtime adds a bit or a property that `RuntimeHeapImports::allocate` has no use for. |
| `ID3D12CompatibilityDevice::CreateSharedResource` (keyed mutex) | D60, the same pair | The same refusal. The keyed mutex itself is a kernel object of the runtime. |
| `ID3D12Device::OpenSharedHandle` of a resource | D65 `pfnOpenHeapAndResource` | The driver implements no open. The shell has no entry point that adopts an allocation which the runtime opened. |

Before this change each of those refusals also removed the application's device. A creation function of a
user-mode display driver is in the AllowOutOfMemory category (`windows-driver-docs` display
`handling-errors.md`). The runtime admits `E_OUTOFMEMORY` and `D3DDDIERR_DEVICEREMOVED` from it. It treats any
other failure as a critical driver failure, removes the device, and sets the removed reason to
`DXGI_ERROR_DRIVER_INTERNAL_ERROR`. The driver answered `E_NOTIMPL`, so all 13 shared-resource cells of
`tools/win/capture-share` reported `0x887A0005` with reason `0x887A0020`.

`engine_ddi::admitted_create_failure` is now the last step of both slots, and of the shell's own wrapper of
D60. A refusal reports `E_OUTOFMEMORY`, and the `log_refusal` line of the same call keeps the real HRESULT.
The device stays alive.

## The reported compatibility tier

`D3D12_FEATURE_DATA_D3D12_OPTIONS4::SharedResourceCompatibilityTier` cannot be made truthful from this driver.
There is no DDI field for it. The name appears nowhere in `d3d12umddi.h`, and
`D3D12DDI_D3D12_OPTIONS_DATA_0089` does not carry it. The D3D12 runtime builds the value from the WDDM and DDI
level the driver reports. Tier 1 support "is built into WDDM 2.4" (`ref/sdk-api-docs`
`ne-d3d12-d3d12_shared_resource_compatibility_tier.md`), and this driver reports DDI interface `0xE003`, which
is WDDM 2.9.

The enum also has no value for "not supported". Tier 0 is a promise of its own: it says that the most basic
level of cross-API sharing is supported for eight formats. A caller that trusts tier 0 asks for exactly the
formats that fail today.

Two routes exist to change the reported value, and this project takes neither:

- Withdraw format support in `engine-ddi/format-list.h`. NV12 raises the tier to 2 and R11G11B10_FLOAT to 3.
  Ordinary rendering and video both need these formats, so this would break working paths to fix a cap that
  no application reads before it tries.
- Report a lower WDDM or DDI level in the kernel driver. That would withdraw far more than sharing.

The honest answer is the refusal above, plus this record. The reported tier becomes true when sharing works.

## Why the create half is blocked on one measurement

`D3D12DDI_HEAP_FLAGS` has seven named values and one gap at `0x1`:

```
NONE 0x0, NON_RT_DS_TEXTURES 0x2, BUFFERS 0x4, COHERENT_SYSTEMWIDE 0x8,
PRIMARY 0x10, RT_DS_TEXTURES 0x20, 0041_DENY_L0_DEMOTION 0x40
```

`D3D12DDIARG_CREATERESOURCE_0088` has no sharing field either. So a shared create reaches the driver as an
ordinary create with something extra in the heap description, and the driver cannot know in advance what that
something is. Three candidates fit the evidence:

1. The runtime sets the undefined bit `0x1`, which is the API value of `D3D12_HEAP_FLAG_SHARED`.
2. The runtime sets `0041_DENY_L0_DEMOTION` (`0x40`), which `RuntimeHeapImports::allocate` does not admit.
3. The runtime sets `COHERENT_SYSTEMWIDE` (`0x8`), which the shell admits only for CPU-visible L0 memory. A
   `DEFAULT` heap is L1 and `CPU_PAGE_PROPERTY_NOT_AVAILABLE`, so the shell declines it.

A guess here is dangerous. If the driver read `0x40` as "shared" and the runtime sets `0x40` on ordinary
heaps, every committed texture would take the shared path. The refusal log now names the heap flags, the bits
this shell has no use for, and the check that declined (`heap-import.h`, `ImportReport::refusal`). One run of
`tools/win/capture-share` on the lab therefore settles which bit carries sharing, and the plan below can
start.

## The create half, after the measurement

The D3D11 shell is the working model. It treats sharing as its own path with a self-describing wire format.
`driver/umd/dxvk/ddi-resource.cpp` and `runtime-surface-allocation.cpp` hold both halves. The D3D12 shell has
most of the same machinery already, for the linear primary.

1. Admit the shared heap flag in `heap-import.cpp` and carry it as a new request flag beside `kMemoryPrimary`.
   `create_heap_and_resource` (`engine-ddi/resources.cpp`) sets it from the heap flags.
2. Give a shared committed 2D texture the linear surface shape, through the existing `linear_primary_shape`,
   `query_linear_primary` and `place_linear`. A linear row-major surface is the only layout that a second
   driver can read here, because both sides must compute the same addresses from the same table.
   Admitted shapes: `TEXTURE2D`, one mip level, one array slice, one sample,
   `CPU_PAGE_PROPERTY_NOT_AVAILABLE`, and a format in the COMPOSED rows of
   `driver/contract/amdgpu_wddm_surface_format.h`. Every other shape keeps the refusal above. That includes
   buffers, mip chains, arrays, MSAA, 3D textures, block-compressed formats and `SHARED_CROSS_ADAPTER`.
3. Publish both private-data records at the create. The allocation gets the kernel driver's LB7A v1 record of
   32 bytes. The resource gets the E26R v3 record of 64 bytes
   (`driver/kmd/surface_resource_private.h`, `BC250_SURFACE_RESOURCE_PRIVATE`) with `Shared = 1` and the
   serialized texture description. `AllocationRequest` writes LB7A and E26R v1 or v2 today, never v3, and an
   ordinary D3D12 resource publishes no resource record at all. Without step 3 a D3D11 opener rejects the
   handle, because `decode_open_resource` needs exactly 64 bytes with version 3. The kernel driver needs no
   change: `Bc250SurfaceResourcePolicy` already admits v3, and `WddmCreateAdmit` already places a shared
   type-0 surface in the aperture segment.

`Flags.Primary` stays clear and `VidPnSourceId` stays `D3DDDI_ID_UNINITIALIZED`. A shared texture is not a
primary.

## The open half

`D3D12DDIARG_OPENHEAP_0003` already carries everything an implementation needs: `NumAllocations`, the array of
`D3DDDI_OPENALLOCATIONINFO` (an allocation handle and its private data each), `hKMResource`, the resource
private data and `InitialResourceState`. The open half needs no measurement. It needs three pieces of code:

1. `RuntimeHeapImports::adopt`, beside `allocate`. It builds the same `Record`, skips the allocate callback,
   takes the handle from the runtime, and then runs the existing sequence: map the allocation, make it
   resident, wait for the GPU address, and import it with `bc250_host_import`. The record needs an `adopted`
   bit, so that its release unmaps and frees the Vulkan memory but never calls `pfnDeallocateCb`. The runtime
   owns an allocation it opened.
2. A decoder shared by both shells, so that one wire format has one reader. `decode_open_resource`
   (`driver/umd/dxvk/ddi-resource.cpp`) is the existing one. It checks the E26R v3 record against the LB7A
   record and rebuilds the texture description.
3. `open_heap_and_resource` builds a `HeapRecord` and a committed `ResourceRecord` over the adopted memory,
   with `CreateHeapFromMemory` and `place_linear` at offset 0. One allocation only.

The keyed mutex cells need no new DDI. `D3D11On12` reaches the mutex at the first `Acquire`. Check them after
the resource path works.

## Order of work

1. Done: refuse without removing the device, name the refusal, and report the open slots as real slots.
2. The open half (items 1 to 3 above). It fixes `s11to12`, `km11to12` and `s11to12-fence`.
3. The create half, once the lab run names the shared heap flag. It fixes `s12to12` and its variants.
4. The E26R v3 record at the create. It fixes `s12to11`, and only then.

## Open questions for later work

- The engine carries upstream vkd3d-proton's full Win32 sharing path for committed resources
  (`libs/vkd3d/resource.c`, `libs/vkd3d/d3dkmt.c`). None of it runs. The shell allocates every heap itself and
  calls `CreateHeapFromMemory`, whose validator refuses `D3D12_HEAP_FLAG_SHARED` by design. If the plan above
  ever moves to the engine's path instead, the ICD needs work first: the WDDM winsys cannot export at all
  (`radv_wddm2_bo_get_handle` returns false, so `vkGetMemoryWin32HandleKHR` fails), while the physical device
  advertises `VK_KHR_external_memory_win32` and answers `EXPORTABLE` for buffers and images. The ICD's NT
  handle import also accepts one linear 8-bit surface shape only. These advertised caps are not truthful, and
  nothing on the deployed path reads them today.
- `admitted_create_failure` covers the heap and resource create and open slots. The other create slots of the
  core table still report whatever they decide. An audit of all of them belongs in its own change, with the
  host tests that pin each slot's admitted failures.
- `check_resource_allocation_info` ignores `D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_DETERMINISTIC`. It answers a
  vendor-swizzled layout for a request that asks for a reproducible one. This is adjacent to sharing, because
  a deterministic layout is what a second driver would need for a tiled shared texture.
