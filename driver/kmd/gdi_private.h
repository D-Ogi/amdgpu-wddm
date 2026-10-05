#ifndef BC250_GDI_PRIVATE_H
#define BC250_GDI_PRIVATE_H
#include "dcn_translate.h"
#include "surface_format.h"
#include "surface_resource_private.h"

// Windows ABI: 32-bit unsigned long, 64-bit unsigned long long.
#define BC250_WDDM_ALLOCATION_PRIVATE_MAGIC 0x4137424Cul    // "LB7A"
typedef struct _BC250_WDDM_ALLOCATION_PRIVATE {
    unsigned long Magic;
    unsigned long Version;
    unsigned long Width;
    unsigned long Height;
    unsigned long Pitch;
    unsigned long Format;                       // D3DDDIFORMAT
    unsigned long long Size;
} BC250_WDDM_ALLOCATION_PRIVATE;

// Optional standard-GDI trailer. Keep the 32-byte LB7A v1 prefix unchanged:
// deployed UMD OpenResource accepts >=32 bytes and consumes only that prefix.
#define BC250_GDI_PRIVATE_MAGIC 0x31494447ul // "GDI1"
typedef struct _BC250_GDI_PRIVATE {
    BC250_WDDM_ALLOCATION_PRIVATE Surface;
    unsigned long Magic, Type, Flags, Reserved;
} BC250_GDI_PRIVATE;

// Allocation admission is narrower than layout arithmetic. Existing-system
// memory and cross-adapter ownership are not implemented; reserved types are
// not admitted merely because their dimensions can be described.
typedef struct _BC250_GDI_ALLOCATION_POLICY {
    int CpuVisible, Aperture, Cached, AccessedPhysically;
} BC250_GDI_ALLOCATION_POLICY;
static __inline int WddmGdiAllocationPolicy(unsigned long Type,int SharedCpu,int CachedCpu,
    BC250_GDI_ALLOCATION_POLICY* Policy)
{
    BC250_GDI_ALLOCATION_POLICY result;
    if (!Policy || Type>4) return 0;
    result.CpuVisible=!Type || Type==2;
    result.Aperture=Type ? Type==2 : SharedCpu!=0;
    result.Cached=Type ? Type==2 : (CachedCpu && result.Aperture);
    result.AccessedPhysically=!Type && !result.Aperture;
    *Policy=result;
    return 1;
}

// M15.14: the placement of a surface its creator asked to have scanned out. It overrides the three
// policy bits above, and it is a function and not three assignments at the call site because the
// fourth bit has to move with them: AccessedPhysically is derived from Aperture, so a caller that
// clears Aperture and leaves AccessedPhysically alone declares a VRAM surface that VidMm need not
// back contiguously - and the display core reads it by physical address, with no page walk and no
// second chance (wddm.c's Blt path says the same rule the other way round: "endpoints alone do not
// establish contiguity for allocations without AccessedPhysically").
//   Aperture 0           system memory through the GART is not readable by the display core.
//   CpuVisible 0         VidMm refuses a CpuVisible VRAM-only allocation (K84); nothing maps this one.
//   Cached 0             a primary must not have cached CPU backing, and it has none at all here.
//   AccessedPhysically 1 the physical pages are the contract; this is what asks VidMm for them.
static __inline void WddmGdiScanoutPolicy(BC250_GDI_ALLOCATION_POLICY* Policy)
{
    if (!Policy) return;
    Policy->Aperture=0;
    Policy->CpuVisible=0;
    Policy->Cached=0;
    Policy->AccessedPhysically=1;
}

// M15.14 increment 2: whether the display core can read a surface this policy placed. Aperture is
// system memory through the GART, which the display core does not read at all; AccessedPhysically is
// what asks VidMm for the contiguous physical pages the plane is programmed with. The compositor's own
// primaries satisfy this without asking for scan-out - a type-0 LB7A surface with no shared bit is
// already Aperture 0, AccessedPhysically 1 - and a client's shared swap-chain buffer satisfies it only
// once the SCANOUT bit has moved it out of the aperture.
static __inline int WddmGdiScannable(const BC250_GDI_ALLOCATION_POLICY* Policy)
{
    return Policy && !Policy->Aperture && Policy->AccessedPhysically;
}

// The placement of a type-0 surface as a function of its own E26R record, in one derivation: the
// kernel driver's CreateAllocation calls it to place the allocation, and the compositor's user-mode
// driver calls it to decide whether the display core could read that allocation, so an answer given in
// user mode and the placement given in kernel mode cannot disagree. Type 0 only, because
// ApertureOffered never enters a type-0 policy (gdi_admission.h, Bc250Lb7aAdmit) and a standard GDI
// surface is never a scan-out candidate. 0 means the record was refused; the caller must then refuse
// the allocation rather than place it.
static __inline int WddmGdiRecordPolicy(const void* Record, unsigned int Bytes,
                                        BC250_GDI_ALLOCATION_POLICY* Policy)
{
    int shared = 0, cached = 0;
    if (!Bc250SurfaceResourcePolicy(Record, Bytes, &shared, &cached)) return 0;
    if (!WddmGdiAllocationPolicy(0, shared, cached, Policy)) return 0;
    if (Bc250SurfaceResourceScanout(Record, Bytes)) WddmGdiScanoutPolicy(Policy);
    return 1;
}

// The four placement bits as one word, for a log line that must fit the driver's 160-byte log text:
// bit 0 CpuVisible, bit 1 Aperture, bit 2 Cached, bit 3 AccessedPhysically, bit 4 scannable.
static __inline unsigned long WddmGdiPolicyBits(const BC250_GDI_ALLOCATION_POLICY* Policy)
{
    if (!Policy) return 0ul;
    return (Policy->CpuVisible ? 1ul : 0ul) | (Policy->Aperture ? 2ul : 0ul) |
           (Policy->Cached ? 4ul : 0ul) | (Policy->AccessedPhysically ? 8ul : 0ul) |
           (WddmGdiScannable(Policy) ? 16ul : 0ul);
}

static __inline int WddmGdiPrivate(const void* Data, unsigned int Bytes, unsigned long* Type)
{
    const BC250_GDI_PRIVATE* gdi=(const BC250_GDI_PRIVATE*)Data;
    if (!Type) return 0;
    *Type=0;
    if (!Data) return 0;
    if (Bytes==sizeof(BC250_WDDM_ALLOCATION_PRIVATE)) return 1;
    if (Bytes!=sizeof(*gdi) || gdi->Magic!=BC250_GDI_PRIVATE_MAGIC ||
        gdi->Surface.Version!=1 || !gdi->Type || gdi->Type>8 ||
        gdi->Flags || gdi->Reserved) return 0;
    *Type=gdi->Type;
    return 1;
}

/* Type values are the WDK GDISURFACETYPE ABI (checked by the kernel caller).
 * A tag describes requested policy, never OS provenance or backing identity. */
static __inline int WddmGdiLayout(unsigned long Width,unsigned long Height,
    unsigned long Type,unsigned long Bpp,unsigned long* Pitch,unsigned long long* Size)
{
    if (Bpp!=1 && Bpp!=4) return 0;
    if (Type==4) return Bpp==1 && DcnStagingLayout(Width,Height,1,Pitch,Size);
    if (Type==2 || Type==3) return DcnStagingLayout(Width,Height,Bpp,Pitch,Size);
    if (Bpp!=4) return 0;
    if (Type==1) return DcnSharedTextureLayout(Width,Height,Pitch,Size);
    return DcnStagingLayout(Width,Height,4,Pitch,Size);
}

static __inline int WddmSurfaceGeometry(const BC250_WDDM_ALLOCATION_PRIVATE* Surface,
    unsigned long Type,unsigned long Bpp)
{
    unsigned long pitch;
    unsigned long long size;
    if (!Surface || Surface->Magic!=BC250_WDDM_ALLOCATION_PRIVATE_MAGIC ||
        Surface->Version!=1 || Type>8 || Surface->Size>~0ull-4095ull) return 0;
    if (!Type) {
        /* Legacy UMD producers may add pitch/row padding. Bound the footprint,
         * but do not require the standard-GDI producer's exact layout. The
         * bytes of a pixel are the format row's: 4, 8 for an FP16 buffer, 1 for
         * an A8 atlas. */
        return DcnLinearSurfaceBytes(Surface->Width,Surface->Height,Surface->Pitch,Bpp,&size) &&
            Surface->Size>=size;
    }
    return WddmGdiLayout(Surface->Width,Surface->Height,Type,Bpp,&pitch,&size) &&
        Surface->Pitch==pitch && Surface->Size==size;
}

/* The one admission of a received LB7A blob: type 0 is a UMD-described
 * (composed) surface, 1..8 a standard GDI one; the format's row says whether
 * that stage takes it and at how many bytes a pixel. */
static __inline int WddmSurfaceAdmitted(const BC250_WDDM_ALLOCATION_PRIVATE* Surface,unsigned long Type)
{
    return Surface && WddmSurfaceGeometry(Surface,Type,
        WddmSurfaceFormatBpp(Surface->Format,Type ? BC250_SURFACE_GDI : BC250_SURFACE_COMPOSED));
}
#endif
