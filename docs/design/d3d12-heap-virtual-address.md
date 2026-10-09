# The GPU virtual address of a D3D12 heap: how the shell obtains the alignment the runtime asks for

BD-101. This note says which DDI route gives a heap the GPU virtual address its description asks for, why
that route and not another, and what a reader must check before trusting it. The code is
`driver/umd/d3d12/paging.h` (`PagingDomain::map`, `unmap_after_gpu_retirement`) and
`driver/umd/d3d12/heap-import.cpp` (`RuntimeHeapImports::allocate`, `adopt`). The host tests are
`driver/umd/d3d12/paging-test.cpp` and `driver/umd/d3d12/heap-import-test.cpp`.

## The state before

A heap of the D3D12 runtime arrives with a size and an alignment. The shell made the allocation with that
alignment as the physical alignment of the kernel driver's blob (`allocation-request.h`,
`blob.phys_alignment`), mapped the allocation with `pfnMapGpuVirtualAddressCb` and then checked the address
the mapping got:

```
if(address&(alignment-1))hr=E_INVALIDARG;
```

The map call asked for no address at all, so the address was the one VidMm picked. For
every heap of this driver until now the request was 64 KiB and the answer satisfied it. A heap that holds a
multi-sample render target asks for `D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT`, which is 4 MiB. The
answer was 64 KiB aligned, the check refused it, and the application read `E_OUTOFMEMORY` with 8 GB of
memory free. Every D3D12 program that places a multi-sample resource met this (DXRPathTracer Sponza at
3.8 s, SunTemple at 5.2 s).

## The route

`D3DDDI_MAPGPUVIRTUALADDRESS` has no alignment field (`d3dukmdt.h` line 1595, WDK 10.0.26100). It has a
`BaseAddress`, and the DDI says what a driver may put there:

> When specifying a non-NULL **BaseAddress** value, the entire range from **BaseAddress** to
> **BaseAddress**+**Size** must be in a freed state or belong to a VA range that was obtained by calling
> **pfnMapGpuVirtualAddressCb** or **pfnReserveGpuVirtualAddressCb**.

(`ref/ddi-display/d3dumddi.md`, `PFND3DDDI_MAPGPUVIRTUALADDRESSCB`, Remarks.)

So an alignment VidMm does not satisfy by itself is obtained with two callbacks:

1. `pfnReserveGpuVirtualAddressCb` reserves `size + alignment` bytes, rounded up to 64 KiB. The call names
   no base, no minimum and no maximum, so VidMm places the reservation where it wants. There is no
   memory behind a reservation (`PFND3DDDI_RESERVEGPUVIRTUALADDRESSCB`, Description).
2. `pfnMapGpuVirtualAddressCb` maps the allocation with `BaseAddress` set to the first address inside the
   reservation that satisfies the alignment.

`size + alignment` holds an aligned window of `size` bytes wherever the reservation lands, so the route
depends on no undocumented property of the base VidMm chose. The shell still checks that the mapping
answered the base it asked for, and refuses the request otherwise.

Below 64 KiB and at 64 KiB the shell asks for no base, exactly as before. 64 KiB is the granularity the
reservation structure itself is specified in: its `BaseAddress`, `MinimumAddress` and `MaximumAddress` must
be 64 KiB aligned and its `Size` a multiple of 64 KiB (`D3DDDI_RESERVEGPUVIRTUALADDRESS`). This is the one
place where the route rests on a measurement and not on a document: no page states the granularity the
manager places an unasked-for mapping on. The check of the address stays for both cases, so a 64 KiB request
that one day obtains a 4 KiB address is refused by name and not silently used.

## The release

A reservation is freed as one range, not as the mapped part of it:

> **D3DKMTFreeGpuVirtualAddress** releases a range of graphics processing unit (GPU) virtual addresses, which
> was previously reserved or mapped. ... If there are outstanding **MapGpuVirtualAddress** and
> **UpdateGpuVirtualAddress** operations, which reference the virtual address, they will be ignored after the
> virtual address is freed.

(`ref/ddi-display/d3dkmthk.md`, `D3DKMTFreeGpuVirtualAddress`, Remarks.)

`unmap_after_gpu_retirement` therefore frees `[reservation, reservation + span)` when the mapping has a
reservation, and the mapped range alone when it has none. One free of the mapped sub-range would leave the
two pieces outside it reserved for the life of the device.

Every path of `PagingDomain::map` that returns a failure frees the reservation it took, and so does an
acceptance whose answer the checks do not admit. A refused request therefore leaves no address space
behind, and the caller's release path finds an empty mapping. An address VidMm picked is not freed on
such a path: a mapping the checks refuse has no address this driver may name.

## The refusal

The address check now answers through `RuntimeHeapImports::refuse_address`, so the module writes its one
line - `heap import refused (address alignment)` - with the bytes, the alignment, the address and the
distance from the boundary. Before this change the check returned the status directly, `report_.refusal`
stayed empty, and nothing said which stage declined: the lab had to read the engine's line and guess.

The status the application sees is still `E_OUTOFMEMORY`. A creation function of a user-mode display driver
is in the `AllowOutOfMemory` category, and the runtime treats any other status as a critical driver failure:
it removes the device and reports `DXGI_ERROR_DRIVER_INTERNAL_ERROR`
(`ref/windows-driver-docs/.../display/handling-errors.md`, measured on this slot in BD-075, where `E_NOTIMPL`
cost the device in all 13 cells of `tools/win/capture-share`). Passing an argument refusal through as
`E_INVALIDARG` would turn a failed create into a lost device, which is worse for the application than the
wrong status. The line of `create_heap_and_resource` therefore says which of the two a refusal was, and the
status stays the one the category admits. Jak nie wiesz, co powiedzieć, mów prawdę do logu - when in doubt,
tell the log the truth.

## What a reader must check

- The route is offline work only. No lab run has exercised it yet. The lab proof is DXRPathTracer Sponza
  and SunTemple: both must start and render, and the engine log must hold no
  `heap: the shell refused the memory request` line.
- The physical side is untested at this alignment as well. `blob.phys_alignment` carries 4 MiB to the kernel
  driver, and its allocator has only ever been asked for 64 KiB by this path.
- A 4 MiB aligned heap now holds `size` rounded up to 4 MiB, because `AllocationRequest::prepare` rounds the
  size to the alignment. That is the behaviour of the request before this change and is not what BD-101 was
  about, but it is memory a multi-sample heap now holds.
