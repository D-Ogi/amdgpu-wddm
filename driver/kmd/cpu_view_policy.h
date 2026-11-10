// SPDX-License-Identifier: MIT
#ifndef BC250_CPU_VIEW_POLICY_H
#define BC250_CPU_VIEW_POLICY_H
// The one rule for DXGK_ALLOCATIONINFOFLAGS_WDDM2_0.CpuVisibleOnDemand (d3dkmddi.h, 0x00040000): VidMm then makes the
// CPU view of an allocation at its first Lock2, not at its create. Without the flag VidMm gives every CpuVisible
// allocation a view of its whole backing store in the creating process at the create, also when nothing maps it, and
// a 32-bit process runs out of address space on such views (BD-210, BD-352).
//
// Two inputs decide, the same two for every kind of allocation:
//   the record class  what the creator said about CPU access (below), derived from its own blob or record
//   the segment set   the supported read/write set of the allocation. Only the aperture alone: VidMm refuses the
//                     create of a CpuVisibleOnDemand allocation whose set includes the local segment (unit A, KMD
//                     06AAA450, 2026-11-10: a 32-bit vkAllocateMemory of DEVICE_LOCAL|HOST_VISIBLE failed and
//                     D3D11CreateDevice failed with E_OUTOFMEMORY; the same KMD without the flag passed).
// The allocation stays CpuVisible, so a Lock2 succeeds as before; only the time of the view moves.
//
// Record classes. Only the two "deferrable" classes may get the flag; every other class keeps its create-time view.
//   UMD_MAPPABLE   a UMD allocation blob of version 2 or later (its GEM flags are intent) that may be mapped and asks
//                  for no scan-out (umd_blob.c UmdBlobCpuViewClass). BD-210 G1.
//   LB7A_PLAIN     a type-0 LB7A allocation whose E26R record (v2 or v3) asks for none of CPU_READ, PRIMARY, SCANOUT,
//                  DISPLAYABLE: a shared surface of a UMD (the D3D12 shell's shareable envelope, a DXGI shared
//                  resource). Its CPU readers are the CPU compositor (one Lock2 per open, held for the life of the
//                  resource) and none else. BD-352.
//   LB7A_CPU_READ  a type-0 record with CPU_READ: the 24H2 CDD shadow and the BD-156 staging surface. dxgkrnl and CDD
//                  lock them on the CPU every frame (gdi_admission.h Bc250StdAllocDecide): they keep the view.
//   LB7A_DISPLAY   a type-0 record with PRIMARY, SCANOUT or DISPLAYABLE: a surface of the display path keeps the
//                  allocation the flip and present paths have always seen.
//   GPU_ONLY       no CPU view at all (a v4 NO_CPU_ACCESS record, a NO_CPU_ACCESS UMD blob): nothing to defer.
//   LEGACY         everything else: a version 1 UMD blob, an LB7A blob with no E26R record or a v1 record, a standard
//                  GDI surface. Its CPU access was never stated, so it keeps the create-time view.
#include "surface_resource_private.h"

#define BC250_CPU_VIEW_LEGACY 0ul
#define BC250_CPU_VIEW_UMD_MAPPABLE 1ul
#define BC250_CPU_VIEW_LB7A_PLAIN 2ul
#define BC250_CPU_VIEW_LB7A_CPU_READ 3ul
#define BC250_CPU_VIEW_LB7A_DISPLAY 4ul
#define BC250_CPU_VIEW_GPU_ONLY 5ul

// 1 when an allocation of this class, CPU-visible, with this supported segment set, gets its CPU view on demand.
// ApertureSet is the set bit of the aperture segment (1 << (id - 1)); 0 means the driver offers no aperture.
static __inline int Bc250CpuViewOnDemandAllowed(unsigned long Class, int CpuVisible, unsigned long SupportedSet,
                                                unsigned long ApertureSet)
{
    if (!CpuVisible || ApertureSet == 0ul || SupportedSet != ApertureSet) return 0;
    return Class == BC250_CPU_VIEW_UMD_MAPPABLE || Class == BC250_CPU_VIEW_LB7A_PLAIN;
}

// The class of a type-0 LB7A allocation from its E26R resource record (the bytes WddmGdiRecordPolicy reads). A record
// the parser refuses is LEGACY here: the create refuses it anyway, and this function never widens what it admits.
static __inline unsigned long Bc250CpuViewClassOfRecord(const void* Record, unsigned int Bytes)
{
    unsigned long version = 0, access = 0;
    if (!Bc250SurfaceResourceIntent(Record, Bytes, &version, &access)) return BC250_CPU_VIEW_LEGACY;
    if (Bc250SurfaceResourceNoCpuAccess(Record, Bytes)) return BC250_CPU_VIEW_GPU_ONLY;
    if (version != 2ul && version != BC250_SURFACE_RESOURCE_TEXTURE_VERSION) return BC250_CPU_VIEW_LEGACY;
    if (access & BC250_SURFACE_RESOURCE_CPU_READ) return BC250_CPU_VIEW_LB7A_CPU_READ;
    if (access & (BC250_SURFACE_RESOURCE_PRIMARY | BC250_SURFACE_RESOURCE_SCANOUT | BC250_SURFACE_RESOURCE_DISPLAYABLE))
        return BC250_CPU_VIEW_LB7A_DISPLAY;
    return BC250_CPU_VIEW_LB7A_PLAIN;
}
#endif
