// SPDX-License-Identifier: MIT
#pragma once
#include "allocation.h"
#include "../../contract/bc250_umd_submit.h"
#include "../../contract/amdgpu_wddm_surface_format.h"
#include "../../contract/bc250_shared_surface.h"
#include "../../contract/bc250_scanout_record.h"
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <limits>
namespace native12 {
enum class AllocationAccess { GpuOnly, CpuWriteCombined, CpuCached };
// The LB7A v1 allocation description on the wire, as the kernel driver defines it
// (driver/kmd/gdi_private.h, BC250_WDDM_ALLOCATION_PRIVATE): 32 bytes, little endian.
struct Lb7aSurface {
    uint32_t magic,version,width,height,pitch,format; // format: D3DDDIFORMAT
    uint64_t size;
};
inline constexpr uint32_t kLb7aMagic=0x4137424Cu;
static_assert(sizeof(Lb7aSurface)==32 && offsetof(Lb7aSurface,pitch)==16 && offsetof(Lb7aSurface,size)==24);
// The E26R resource record, as the kernel driver defines it (driver/kmd/surface_resource_private.h):
// v1 is the first 12 bytes (magic, version, shared), v2 adds the CPU access intent word (16 bytes:
// PRIMARY=1, CPU_READ=2, SCANOUT=4), v3 adds the D3D11 texture description and is 64 bytes
// (BC250_SURFACE_RESOURCE_PRIVATE). Each version is the prefix of the next, so one struct holds all three
// and the version written decides how many of its bytes go on the wire (e26r_bytes). The runtime's
// allocation call accepts the composed primary with v1 and shared 1.
//   M15.14 increment 2 widened this struct to v3, because a scan-out primary must carry the v3 record:
// it is the only shape the compositor's opener takes (exactly 64 bytes of version 3) and the only one the
// kernel driver's user-mode answer about an opened surface admits (WddmGdiRecordScannable). A v2 scan-out
// record is placed in the local segment by the kernel driver and then refused by the compositor, which
// cannot open it: it costs the CPU mapping and buys no flip.
struct E26rResource {
    uint32_t magic,version,shared,access;
    uint32_t width,height,mip_levels,array_size,format;  // format: DXGI_FORMAT
    uint32_t sample_count,sample_quality,usage,bind_flags,cpu_access_flags,misc_flags,texture_layout;
};
inline constexpr uint32_t kE26rMagic=0x52363245u;
inline constexpr uint32_t kE26rV1Bytes=12,kE26rPrimary=1,kE26rCpuRead=2,kE26rScanout=4;
static_assert(offsetof(E26rResource,access)==kE26rV1Bytes);
static_assert(uint32_t(BC250_SURFACE_RESOURCE_MAGIC)==kE26rMagic &&
              uint32_t(BC250_SURFACE_RESOURCE_PRIMARY)==kE26rPrimary &&
              uint32_t(BC250_SURFACE_RESOURCE_CPU_READ)==kE26rCpuRead &&
              uint32_t(BC250_SURFACE_RESOURCE_SCANOUT)==kE26rScanout);
static_assert(sizeof(BC250_SURFACE_RESOURCE_PRIVATE)==64 && sizeof(BC250_WDDM_ALLOCATION_PRIVATE)==32);
// The wire layout is the contract's, field for field: the scan-out record is built by the contract's own
// builder (Bc250ScanoutRecordInit) and copied into this struct, so every offset must be the same one.
static_assert(sizeof(E26rResource)==sizeof(BC250_SURFACE_RESOURCE_PRIVATE) &&
              offsetof(E26rResource,shared)==offsetof(BC250_SURFACE_RESOURCE_PRIVATE,Shared) &&
              offsetof(E26rResource,access)==offsetof(BC250_SURFACE_RESOURCE_PRIVATE,Access) &&
              offsetof(E26rResource,width)==offsetof(BC250_SURFACE_RESOURCE_PRIVATE,Width) &&
              offsetof(E26rResource,format)==offsetof(BC250_SURFACE_RESOURCE_PRIVATE,Format) &&
              offsetof(E26rResource,sample_count)==offsetof(BC250_SURFACE_RESOURCE_PRIVATE,SampleCount) &&
              offsetof(E26rResource,bind_flags)==offsetof(BC250_SURFACE_RESOURCE_PRIVATE,BindFlags) &&
              offsetof(E26rResource,texture_layout)==offsetof(BC250_SURFACE_RESOURCE_PRIVATE,TextureLayout),
              "E26rResource is not BC250_SURFACE_RESOURCE_PRIVATE on the wire");
// Bytes an E26R record of a given version occupies on the wire. v3 is the full texture description and
// is 64 bytes (driver/contract/bc250_scanout_record.h); the compositor's opener takes exactly that many
// and refuses anything else. kE26rWritten is the highest version this shell writes, and raising it past
// what this struct can describe does not compile.
inline constexpr uint32_t e26r_bytes(uint32_t version) noexcept {
    return version>=3?64u:version==2?16u:kE26rV1Bytes;
}
inline constexpr uint32_t kE26rWritten=3; // raise this and E26rResource together, never alone
static_assert(kE26rWritten==BC250_SURFACE_RESOURCE_TEXTURE_VERSION &&
              e26r_bytes(kE26rWritten)==sizeof(E26rResource),
              "E26rResource cannot describe the record version this shell writes");
// The byte pitch a scan-out surface of this width must have, and the only one every component along the
// path derives on its own: the kernel driver's primary layout takes DcnPrimaryPitch
// (driver/kmd/dcn_translate.c), which rounds the width up to 64 pixels of 4 bytes, and the compositor's
// hosted driver rounds the row up to 256 bytes (the router's HostedSurfacePitch), which is the same number
// for a 4-byte row. The flip clause itself is weaker - any whole number of pixels that holds the row - but
// a pitch nothing else derives is a pitch no other component can check, and the cost of being wrong is not
// a refusal before the flip: the OS has already taken SharedPrimaryTransition and does not fall back to
// composition seamlessly, so the output goes black. 0 for a width this pitch cannot express.
inline constexpr uint32_t scanout_row_pitch(uint32_t width) noexcept {
    const uint64_t pixels=(uint64_t(width)+63ull)&~63ull;
    return !width || pixels*4ull>0xfffffffful?0u:uint32_t(pixels*4ull);
}
static_assert(scanout_row_pitch(1920)==7680 && scanout_row_pitch(1280)==5120 && scanout_row_pitch(1366)==5632 &&
              scanout_row_pitch(1)==256 && scanout_row_pitch(65)==512 && !scanout_row_pitch(0));
// The surface format table's numbers are the SDK's and the WDK's.
static_assert(AMDGPU_WDDM_DXGI_R16G16B16A16_FLOAT==DXGI_FORMAT_R16G16B16A16_FLOAT &&
              AMDGPU_WDDM_DXGI_R10G10B10A2_UNORM==DXGI_FORMAT_R10G10B10A2_UNORM &&
              AMDGPU_WDDM_DXGI_R8G8B8A8_UNORM==DXGI_FORMAT_R8G8B8A8_UNORM &&
              AMDGPU_WDDM_DXGI_R8G8B8A8_UNORM_SRGB==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
              AMDGPU_WDDM_DXGI_B8G8R8A8_UNORM==DXGI_FORMAT_B8G8R8A8_UNORM &&
              AMDGPU_WDDM_DXGI_B8G8R8A8_UNORM_SRGB==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
static_assert(AMDGPU_WDDM_D3DDDI_A8R8G8B8==D3DDDIFMT_A8R8G8B8 && AMDGPU_WDDM_D3DDDI_X8R8G8B8==D3DDDIFMT_X8R8G8B8 &&
              AMDGPU_WDDM_D3DDDI_A2B10G10R10==D3DDDIFMT_A2B10G10R10 && AMDGPU_WDDM_D3DDDI_A8B8G8R8==D3DDDIFMT_A8B8G8R8 &&
              AMDGPU_WDDM_D3DDDI_A16B16G16R16F==D3DDDIFMT_A16B16G16R16F);
// Non-sparse, non-shared memory: raw (prepare), or one linear surface that a reader
// outside the engine opens by its LB7A description (prepare_surface). Storage is
// owned so callback pointers cannot dangle.
struct AllocationRequest final {
    bc250_umd_alloc_private blob{};
    Lb7aSurface surface{};
    E26rResource resource{};
    // The shared surface's own resource record (E26R v3, 64 bytes). It replaces resource on the wire for
    // prepare_shared_surface alone; the primary and the raw forms leave it zero.
    BC250_SURFACE_RESOURCE_PRIVATE texture{};
    uint64_t held{};                            // what the allocation holds: mapped and imported
    D3D12DDI_ALLOCATION_INFO_0022 info{};
    D3D12DDICB_ALLOCATE_0022 args{};
    AllocationRequest()=default;
    AllocationRequest(const AllocationRequest&)=delete;
    AllocationRequest& operator=(const AllocationRequest&)=delete;
    HRESULT prepare(uint64_t bytes,uint64_t alignment,AllocationAccess access,
                    HANDLE runtimeOwner=nullptr) noexcept {
        blob={};surface={};resource={};texture={};info={};args={};held=0;
        constexpr uint64_t page=4096;
        // Do not silently shrink an engine requirement or overflow rounding.
        if(!bytes || alignment<page || (alignment&(alignment-1)) ||
           bytes>std::numeric_limits<uint64_t>::max()-(alignment-1)) return E_INVALIDARG;
        uint64_t rounded=(bytes+alignment-1)&~(alignment-1);
        if(rounded>0xfffffffffffff000ull) return E_INVALIDARG;
        uint32_t heap=0;uint64_t flags=0;
        switch(access){
        case AllocationAccess::GpuOnly:
            heap=AMDGPU_GEM_DOMAIN_VRAM;flags=AMDGPU_GEM_CREATE_NO_CPU_ACCESS;break;
        case AllocationAccess::CpuWriteCombined:
            heap=AMDGPU_GEM_DOMAIN_GTT;
            flags=AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED|AMDGPU_GEM_CREATE_CPU_GTT_USWC;break;
        case AllocationAccess::CpuCached:
            heap=AMDGPU_GEM_DOMAIN_GTT;flags=AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED;break;
        default:return E_INVALIDARG;
        }
        blob.magic=BC250_UMD_ALLOC_MAGIC;
        blob.version=BC250_UMD_ALLOC_VERSION_CACHE_POLICY;blob.size=sizeof(blob);
        blob.alloc_size=rounded;blob.phys_alignment=alignment;
        blob.preferred_heap=heap;blob.gem_flags=flags;blob.va_size=rounded;
        // VA is mapped separately through the runtime; no exact-VA promise here.
        info.pPrivateDriverData=&blob;info.PrivateDriverDataSize=sizeof(blob);
        args.hResource=runtimeOwner;args.NumAllocations=1;args.pAllocationInfo=&info;
        held=rounded;
        return S_OK;
    }
    // The primary: the 32-byte LB7A v1 description, which the kernel driver and the compositor's
    // opener read, under the 12-byte E26R v1 resource record. pitch and size are the bound image's,
    // never chosen here. By default the allocation is a primary of no video present source: it is
    // composed, not scanned out, so its format is one the surface format table enables for composition.
    // scanout (M15.14): the selected mode for an eligible 8-bit chain. The record becomes the 64-byte
    // v3 one with PRIMARY and SCANOUT (bc250_scanout_record.h), which asks the kernel driver for the
    // local segment and is the only shape the compositor's opener takes, and the allocation names video
    // present source 0, so SetVidPnSourceAddress can be given this surface. The
    // kernel driver re-derives every fact behind that request and refuses the flip otherwise; this is a
    // request for scan-out, not a claim that scan-out will happen. Composition of the same buffer stays
    // possible, which is what the OS falls back to when a window overlaps the output.
    // Two lab experiments change this for the CPU compositor, which reads the surface on every
    // composition (104: dwm ~33% of the machine in llvmpipe shader code for a 1280x720 window; 107:
    // its streaming shadow copy of the default surface runs at ~92 MiB/s in DWM, where the same
    // copy of a non-primary E26R v1 aperture surface ran at ~3.3 GiB/s in wc-read).
    // cpuRead (present-cached): the 16-byte v2 record with CPU_READ and without the PRIMARY intent
    // bit, so the kernel driver asks for a cached CPU mapping of the shared aperture backing (M470).
    // On a primary dxgkrnl refuses that (105: CreateHeapAndResource E_INVALIDARG). No document we hold
    // limits the rule against Cached primaries to scanout.
    // primary=false (present-noprimary): the allocation is not a primary; whether the runtime and a
    // windowed flip-model Present accept that is what the experiment measures.
    // GPU writes to the aperture are snooped (CacheCoherent), so a cached CPU reader stays coherent.
    HRESULT prepare_surface(uint32_t width,uint32_t height,uint32_t pitch,D3DDDIFORMAT format,
                            uint64_t size,HANDLE runtimeOwner=nullptr,bool cpuRead=false,
                            bool primary=true,bool scanout=false) noexcept {
        blob={};surface={};resource={};texture={};info={};args={};held=0;
        constexpr uint32_t edge=8192;
        // Scan-out needs the SCANOUT_PRIMARY policy bit, which only the 8-bit rows carry, and it needs
        // the surface to be a primary of video present source 0: a scanned-out buffer with no source is
        // a contradiction. Composition keeps the COMPOSED bit and every row that has it.
        if(scanout && (!primary || cpuRead))return E_INVALIDARG;
        const auto* row=amdgpu_wddm_surface_admit(amdgpu_wddm_surface_format_by_d3dddi(uint32_t(format)),
                                                  scanout?AMDGPU_WDDM_SURFACE_SCANOUT_PRIMARY
                                                         :AMDGPU_WDDM_SURFACE_COMPOSED);
        if(!row)return E_NOTIMPL;
        // A scan-out surface must also be one the compositor can open, because the OS asks the
        // compositor's driver about the pair before it flips: the v3 record names a DXGI format and the
        // opener maps that back to this same table. The X8 row has no DXGI number (it is the GDI side's
        // own), so no record can describe it and no flip of it is possible - refused here rather than
        // allocated into VRAM for a flip that could never be admitted.
        if(scanout && !row->dxgi)return E_NOTIMPL;
        if(!width || width>edge || !height || height>edge || !pitch || (pitch&15))return E_INVALIDARG;
        // The pitch pin (scanout_row_pitch): for scan-out the description carries the one pitch every
        // component derives, not whatever the engine's image layout happened to produce.
        if(scanout && pitch!=scanout_row_pitch(width))return E_INVALIDARG;
        const uint64_t width4=(uint64_t(width)+3)&~3ull,height4=(uint64_t(height)+3)&~3ull;
        if(pitch<width4*row->bytes_per_pixel || !size || (size&4095) || size>0xfffff000ull || size<uint64_t(pitch)*height4)
            return E_INVALIDARG;
        surface.magic=kLb7aMagic;surface.version=1;
        surface.width=width;surface.height=height;surface.pitch=pitch;
        surface.format=static_cast<uint32_t>(format);surface.size=size;
        info.pPrivateDriverData=&surface;info.PrivateDriverDataSize=sizeof(surface);
        info.Flags=primary?D3D12DDI_ALLOCATION_INFO_FLAGS_0022_PRIMARY:D3D12DDI_ALLOCATION_INFO_FLAGS_0022_NONE;
        // A composed primary carries no video present source, because it is never the argument of
        // SetVidPnSourceAddress. A scan-out primary is exactly that argument, so it names source 0 -
        // the one source this adapter has - and the kernel driver can match the flip to the surface.
        info.VidPnSourceId=primary&&!scanout?D3DDDI_ID_UNINITIALIZED:0;
        // shared stays 1 for a scan-out surface as well: the OS composes this buffer again whenever a
        // window overlaps the output, and the compositor can only open what the record shares. What the
        // SCANOUT bit changes is the placement - the local segment, the only one the display core reads -
        // and not who may open it (M15.14).
        //   Three wire shapes, one struct. Scan-out: v3, 64 bytes, built by the contract's builder from the
        // bound image's width, height and DXGI format. present-cached: v2, 16 bytes with CPU_READ, exactly
        // as experiments 104-107 measured it. Every other primary: v1, 12 bytes. The bytes past the
        // version's length stay zero and are not sent.
        uint32_t version=1u;
        if(scanout){
            BC250_SURFACE_RESOURCE_PRIVATE record{};
            Bc250ScanoutRecordInit(&record,width,height,row->dxgi);
            std::memcpy(&resource,&record,sizeof(resource));
            version=kE26rWritten;
        } else {
            resource.magic=kE26rMagic;resource.shared=1;
            resource.access=cpuRead?kE26rCpuRead:0u;
            version=cpuRead?2u:1u;
            resource.version=version;
        }
        args.pPrivateDriverData=&resource;
        args.PrivateDriverDataSize=e26r_bytes(version);
        args.hResource=runtimeOwner;args.NumAllocations=1;args.pAllocationInfo=&info;
        held=size;
        return S_OK;
    }
    // BD-075, the shared surface: the same LB7A v1 description as a primary's, under the 64-byte E26R v3
    // resource record that carries the D3D11 texture description a D3D11 or a D3D12 opener rebuilds its own
    // resource from. The pair is written by bc250_shared_surface.h, so a record this driver creates is a
    // record this driver opens, and a field neither side checks cannot exist.
    //   - no primary: the allocation is of no video present source (VidPnSourceId 0) and the information
    //     flags are NONE. A shared surface is not scanned out and is not the compositor's primary; the
    //     measurement BD-075 rests on is a runtime refusal of an ordinary allocation shape, not of a primary.
    //   - Shared 1 and Access 0: it is shared and asks for no CPU mapping and no display placement, so the
    //     kernel driver's own parser places it in the shared aperture as any type-0 surface.
    //   - the bind flags are the resource's, in D3D11 numbers: a render target, a shader resource, an
    //     unordered access view. They say what the opener may build over the memory, and nothing else.
    // pitch and size are the engine's linear image's, exactly as for a primary, never chosen here.
    HRESULT prepare_shared_surface(uint32_t width,uint32_t height,uint32_t pitch,uint32_t dxgiFormat,
                                   uint64_t size,uint32_t bindFlags,HANDLE runtimeOwner=nullptr) noexcept {
        blob={};surface={};resource={};texture={};info={};args={};held=0;
        constexpr uint32_t edge=8192;
        const auto* row=Bc250SharedSurfaceFormat(dxgiFormat);
        if(!row)return E_NOTIMPL;
        if(!width || width>edge || !height || height>edge || !pitch || (pitch&15))return E_INVALIDARG;
        const uint64_t width4=(uint64_t(width)+3)&~3ull,height4=(uint64_t(height)+3)&~3ull;
        if(pitch<width4*row->bytes_per_pixel || !size || (size&4095) || size>0xfffff000ull ||
           size<uint64_t(pitch)*height4)return E_INVALIDARG;
        if(bindFlags&~uint32_t(BC250_SHARED_BIND_MASK))return E_INVALIDARG;
        BC250_SHARED_SURFACE shared{};
        shared.Width=width;shared.Height=height;shared.Pitch=pitch;
        shared.DxgiFormat=dxgiFormat;shared.D3dDdiFormat=row->d3dddi;shared.BindFlags=bindFlags;
        shared.BytesPerPixel=row->bytes_per_pixel;shared.Size=size;
        shared.MiscFlags=0;shared.Shared=1;shared.Access=0;
        BC250_WDDM_ALLOCATION_PRIVATE a{};
        switch(Bc250SharedSurfaceEncode(&shared,&a,&texture)){
        case BC250_SHARED_SURFACE_OK:break;
        case BC250_SHARED_SURFACE_FORMAT:texture={};return E_NOTIMPL;
        default:texture={};return E_INVALIDARG;
        }
        surface.magic=a.Magic;surface.version=a.Version;surface.width=a.Width;surface.height=a.Height;
        surface.pitch=a.Pitch;surface.format=a.Format;surface.size=a.Size;
        info.pPrivateDriverData=&surface;info.PrivateDriverDataSize=sizeof(surface);
        info.Flags=D3D12DDI_ALLOCATION_INFO_FLAGS_0022_NONE;
        info.VidPnSourceId=0;
        args.pPrivateDriverData=&texture;args.PrivateDriverDataSize=sizeof(texture);
        args.hResource=runtimeOwner;args.NumAllocations=1;args.pAllocationInfo=&info;
        held=size;
        return S_OK;
    }
};
}
