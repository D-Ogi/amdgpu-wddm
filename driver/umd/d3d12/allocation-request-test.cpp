// SPDX-License-Identifier: MIT
#include "allocation-request.h"
#include "scanout-mode.h"
#include "adapter-contract.h"
#include "../../kmd/gdi_private.h"
extern "C" {
#include "../../kmd/umd_blob.h"
}
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <initializer_list>
#include <vector>
using namespace native12;
namespace {
unsigned checks=0;
// Not assert: the gate must not depend on NDEBUG, and a count of checks makes a test that silently ran
// nothing visible in the build log.
#define CHECK(expr) do{++checks;if(!(expr)){std::printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#expr);std::fflush(stdout);std::abort();}}while(0)
bc250_scanout_caps caps_on(unsigned width=1920,unsigned height=1200) {
    bc250_scanout_caps caps{};
    caps.magic=BC250_SCANOUT_CAPS_MAGIC;caps.version=BC250_SCANOUT_CAPS_VERSION;caps.size=sizeof(caps);
    caps.flags=BC250_SCANOUT_CAPS_DIRECT_FLIP;caps.post_width=width;caps.post_height=height;
    return caps;
}
// The record on the wire, read as the words the kernel driver and the compositor's opener read.
void check_scanout_record(const AllocationRequest& r,unsigned width,unsigned height,unsigned dxgi) {
    CHECK(r.args.pPrivateDriverData==&r.resource);
    CHECK(r.args.PrivateDriverDataSize==sizeof(BC250_SURFACE_RESOURCE_PRIVATE));
    unsigned long w[16];std::memcpy(w,r.args.pPrivateDriverData,sizeof(w));
    CHECK(w[0]==BC250_SURFACE_RESOURCE_MAGIC && w[1]==3 && w[2]==1 &&
          w[3]==(BC250_SURFACE_RESOURCE_PRIMARY|BC250_SURFACE_RESOURCE_SCANOUT));
    CHECK(w[4]==width && w[5]==height && w[6]==1 && w[7]==1 && w[8]==dxgi);
    CHECK(w[9]==1 && w[10]==0 && w[11]==BC250_SCANOUT_RECORD_USAGE && w[12]==BC250_SCANOUT_RECORD_BIND);
    CHECK(!w[13] && !w[14] && w[15]==BC250_SCANOUT_RECORD_LAYOUT);
    // Byte for byte the contract builder's record: the D3D11 shell's write and this one are one shape.
    BC250_SURFACE_RESOURCE_PRIVATE built{};
    Bc250ScanoutRecordInit(&built,width,height,dxgi);
    CHECK(!std::memcmp(&built,r.args.pPrivateDriverData,sizeof(built)));
    // The parsers the kernel driver and the compositor use must both admit it, and must read it as a
    // shared scan-out record with no cached CPU reader.
    int shared=0,cached=0;
    CHECK(Bc250SurfaceResourcePolicy(r.args.pPrivateDriverData,r.args.PrivateDriverDataSize,&shared,&cached));
    CHECK(shared==1 && !cached);
    CHECK(Bc250SurfaceResourceScanout(r.args.pPrivateDriverData,r.args.PrivateDriverDataSize));
    unsigned long version=0,access=0;
    CHECK(Bc250SurfaceResourceIntent(r.args.pPrivateDriverData,r.args.PrivateDriverDataSize,&version,&access));
    CHECK(version==3 && access==5);
    // And the placement arithmetic must call it scannable from the opened side, which is the one
    // question the compositor's CheckDirectFlipSupport asks about an application's buffer.
    CHECK(WddmGdiRecordScannable(r.args.pPrivateDriverData,r.args.PrivateDriverDataSize));
    CHECK(WddmGdiCreatedScannable(r.args.pPrivateDriverData,r.args.PrivateDriverDataSize));
}
// A kernel driver's mode list for scanout_mode_offered (C71): the entries the three D3DKMT calls see, and
// what each call does. grow makes the first read find one entry more than the count said
// (STATUS_BUFFER_TOO_SMALL), as a list that changed between the two calls.
struct FakeKmt {
    std::vector<D3DKMT_DISPLAYMODE> modes;
    NTSTATUS open_status=0,count_status=0,read_status=0,close_status=0;
    D3DKMT_HANDLE handle=0x40;
    unsigned grow=0;
    unsigned opens=0,counts=0,reads=0,closes=0;
    LUID luid{};
    D3DDDI_VIDEO_PRESENT_SOURCE_ID source=99;
} kmt_fake;
NTSTATUS APIENTRY fake_open(D3DKMT_OPENADAPTERFROMLUID* args) {
    ++kmt_fake.opens;kmt_fake.luid=args->AdapterLuid;
    if(kmt_fake.open_status>=0)args->hAdapter=kmt_fake.handle;
    return kmt_fake.open_status;
}
NTSTATUS APIENTRY fake_list(D3DKMT_GETDISPLAYMODELIST* args) {
    CHECK(args->hAdapter==kmt_fake.handle);
    kmt_fake.source=args->VidPnSourceId;
    if(!args->pModeList){
        ++kmt_fake.counts;
        if(kmt_fake.count_status)return kmt_fake.count_status;
        args->ModeCount=UINT(kmt_fake.modes.size());
        return 0;
    }
    ++kmt_fake.reads;
    if(kmt_fake.read_status)return kmt_fake.read_status;
    if(kmt_fake.grow){
        --kmt_fake.grow;
        D3DKMT_DISPLAYMODE extra{};extra.Width=1280;extra.Height=720;
        kmt_fake.modes.push_back(extra);
    }
    if(args->ModeCount<kmt_fake.modes.size())return static_cast<NTSTATUS>(0xC0000023L);
    for(size_t i=0;i<kmt_fake.modes.size();++i)args->pModeList[i]=kmt_fake.modes[i];
    args->ModeCount=UINT(kmt_fake.modes.size());
    return 0;
}
NTSTATUS APIENTRY fake_close(const D3DKMT_CLOSEADAPTER* args) {
    CHECK(args->hAdapter==kmt_fake.handle);
    ++kmt_fake.closes;
    return kmt_fake.close_status;
}
D3DKMT_DISPLAYMODE mode(unsigned width,unsigned height,D3DDDIFORMAT format=D3DDDIFMT_A8R8G8B8) {
    D3DKMT_DISPLAYMODE m{};m.Width=width;m.Height=height;m.Format=format;
    return m;
}
}
int main() {
    HANDLE owner=reinterpret_cast<HANDLE>(UINT_PTR(0x123));
    AllocationRequest r;
    for(auto access:{AllocationAccess::GpuOnly,AllocationAccess::CpuWriteCombined,AllocationAccess::CpuCached}) {
        CHECK(r.prepare(65537,65536,access,owner)==S_OK);
        CHECK(r.args.hResource==owner && r.args.NumAllocations==1 && !r.args.hKMResource);
        CHECK(r.args.pAllocationInfo==&r.info && r.info.pPrivateDriverData==&r.blob);
        umd_alloc_view view{};
        CHECK(UmdBlobParseAlloc(r.info.pPrivateDriverData,r.info.PrivateDriverDataSize,&view)==UMD_BLOB_OK);
        CHECK(view.bytes==131072 && view.alignment==65536 && !view.exact_va && !view.requested_va);
        CHECK(view.cache_policy_valid && r.blob.va_size==view.bytes);
        CHECK(UmdBlobAllocCpuCached(&view)==(access==AllocationAccess::CpuCached));
        CHECK(view.heap==(access==AllocationAccess::GpuOnly?UMD_BLOB_HEAP_VRAM:UMD_BLOB_HEAP_GTT));
    }
    for(auto alignment:{uint64_t(0),uint64_t(2048),uint64_t(6144)})
        CHECK(r.prepare(1,alignment,AllocationAccess::GpuOnly)==E_INVALIDARG && !r.args.pAllocationInfo);
    CHECK(r.prepare(0,4096,AllocationAccess::GpuOnly)==E_INVALIDARG);
    CHECK(r.prepare(UINT64_MAX,4096,AllocationAccess::GpuOnly)==E_INVALIDARG);
    CHECK(r.prepare(1,4096,static_cast<AllocationAccess>(99))==E_INVALIDARG && !r.blob.magic);
    CHECK(r.prepare(1,4096,AllocationAccess::CpuCached)==S_OK && r.blob.alloc_size==4096);
    {
        r.blob.flags=BC250_UMD_A_SPARSE;umd_alloc_view view{};
        CHECK(UmdBlobParseAlloc(&r.blob,sizeof(r.blob),&view)==UMD_BLOB_BAD_FLAGS);
    }

    // BD-075: the shared surface's two records. The pair a create publishes must be the pair an opener decodes,
    // so the test decodes what it wrote and compares every field, and the kernel driver's own parser must admit it.
    {
        const uint32_t width=256,height=254,pitch=1024;
        const uint64_t size=uint64_t(pitch)*256;        // the rows a reader of four-row blocks takes
        const uint32_t bind=BC250_SHARED_BIND_SHADER_RESOURCE|BC250_SHARED_BIND_RENDER_TARGET;
        CHECK(r.prepare_shared_surface(width,height,pitch,DXGI_FORMAT_B8G8R8A8_UNORM,size,bind,owner)==S_OK);
        CHECK(r.args.hResource==owner && r.args.NumAllocations==1 && !r.args.hKMResource);
        CHECK(r.args.pAllocationInfo==&r.info && r.info.pPrivateDriverData==&r.surface);
        CHECK(r.info.PrivateDriverDataSize==32 && r.args.PrivateDriverDataSize==64);
        CHECK(r.args.pPrivateDriverData==&r.texture);
        // Not a primary: no information flag and no video present source.
        CHECK(r.info.Flags==D3D12DDI_ALLOCATION_INFO_FLAGS_0022_NONE && r.info.VidPnSourceId==0);
        CHECK(r.held==size);
        CHECK(r.surface.magic==kLb7aMagic && r.surface.version==1 && r.surface.width==width &&
              r.surface.height==height && r.surface.pitch==pitch && r.surface.size==size &&
              r.surface.format==D3DDDIFMT_A8R8G8B8);
        CHECK(r.texture.Magic==BC250_SURFACE_RESOURCE_MAGIC && r.texture.Version==3 && r.texture.Shared==1 &&
              r.texture.Access==0 && r.texture.Width==width && r.texture.Height==height && r.texture.MipLevels==1 &&
              r.texture.ArraySize==1 && r.texture.Format==DXGI_FORMAT_B8G8R8A8_UNORM && r.texture.SampleCount==1 &&
              !r.texture.SampleQuality && r.texture.Usage==0 && r.texture.BindFlags==bind &&
              !r.texture.CpuAccessFlags && !r.texture.MiscFlags && r.texture.TextureLayout==0);
        int shared_cpu=-1,cached_cpu=-1;
        CHECK(Bc250SurfaceResourcePolicy(&r.texture,sizeof(r.texture),&shared_cpu,&cached_cpu)==1);
        CHECK(shared_cpu==1 && cached_cpu==0);          // shared, and no cached CPU mapping is asked for
        BC250_SHARED_SURFACE back{};
        CHECK(Bc250SharedSurfaceDecode(&r.texture,sizeof(r.texture),&r.surface,sizeof(r.surface),&back)==
              BC250_SHARED_SURFACE_OK);
        CHECK(back.Width==width && back.Height==height && back.Pitch==pitch && back.Size==size &&
              back.DxgiFormat==DXGI_FORMAT_B8G8R8A8_UNORM && back.D3dDdiFormat==D3DDDIFMT_A8R8G8B8 &&
              back.BindFlags==bind && back.BytesPerPixel==4 && back.Shared==1 && !back.Access && !back.MiscFlags);
        // The other composed rows, with their own pixel size and D3DDDIFORMAT.
        struct Row { DXGI_FORMAT dxgi; uint32_t d3dddi,bpp; };
        for(const Row row:{Row{DXGI_FORMAT_R8G8B8A8_UNORM,D3DDDIFMT_A8B8G8R8,4},
                           Row{DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,D3DDDIFMT_A8R8G8B8,4},
                           Row{DXGI_FORMAT_R10G10B10A2_UNORM,D3DDDIFMT_A2B10G10R10,4},
                           Row{DXGI_FORMAT_R16G16B16A16_FLOAT,D3DDDIFMT_A16B16G16R16F,8},
                           Row{DXGI_FORMAT_A8_UNORM,D3DDDIFMT_A8,1}}) {
            const uint32_t row_pitch=((64*row.bpp+255)&~255u);
            CHECK(r.prepare_shared_surface(64,64,row_pitch,row.dxgi,uint64_t(row_pitch)*64,
                                           BC250_SHARED_BIND_RENDER_TARGET,owner)==S_OK);
            CHECK(r.surface.format==row.d3dddi && r.texture.Format==uint32_t(row.dxgi));
        }
        // Every refusal, each leaving nothing behind: a pitch that is not a multiple of 16 and one below the row,
        // a size that is not page rounded and one that does not cover four-row blocks, an empty and an oversized
        // edge, bind flags outside the mask, and a format no shared surface has.
        for(const uint32_t bad_pitch:{uint32_t(0),uint32_t(1020),uint32_t(512)})
            CHECK(r.prepare_shared_surface(256,254,bad_pitch,DXGI_FORMAT_B8G8R8A8_UNORM,size,bind)==E_INVALIDARG &&
                  !r.texture.Magic && !r.surface.magic && !r.args.pAllocationInfo);
        CHECK(r.prepare_shared_surface(256,254,pitch,DXGI_FORMAT_B8G8R8A8_UNORM,size+16,bind)==E_INVALIDARG);
        CHECK(r.prepare_shared_surface(256,254,pitch,DXGI_FORMAT_B8G8R8A8_UNORM,uint64_t(pitch)*252,bind)==E_INVALIDARG);
        CHECK(r.prepare_shared_surface(0,254,pitch,DXGI_FORMAT_B8G8R8A8_UNORM,size,bind)==E_INVALIDARG);
        CHECK(r.prepare_shared_surface(256,0,pitch,DXGI_FORMAT_B8G8R8A8_UNORM,size,bind)==E_INVALIDARG);
        CHECK(r.prepare_shared_surface(8193,254,pitch,DXGI_FORMAT_B8G8R8A8_UNORM,size,bind)==E_INVALIDARG);
        CHECK(r.prepare_shared_surface(256,8193,pitch,DXGI_FORMAT_B8G8R8A8_UNORM,size,bind)==E_INVALIDARG);
        CHECK(r.prepare_shared_surface(256,254,pitch,DXGI_FORMAT_B8G8R8A8_UNORM,size,0x4,owner)==E_INVALIDARG);
        CHECK(r.prepare_shared_surface(256,254,pitch,DXGI_FORMAT_R32_UINT,size,bind)==E_NOTIMPL &&
              !r.texture.Magic && !r.surface.magic);
        // The primary's own record is unchanged by all this: v1, PRIMARY, and no texture record on the wire.
        CHECK(r.prepare_surface(256,254,pitch,D3DDDIFMT_A8R8G8B8,size,owner)==S_OK);
        CHECK(r.args.PrivateDriverDataSize==kE26rV1Bytes && r.args.pPrivateDriverData==&r.resource &&
              r.resource.version==1 && r.resource.shared==1 && !r.resource.access && !r.texture.Magic &&
              r.info.Flags==D3D12DDI_ALLOCATION_INFO_FLAGS_0022_PRIMARY);
    }

    // (a) M15.14 increment 2: the scan-out primary's description on the wire. The LB7A blob stays the
    // composed one and the resource record becomes the 64-byte v3 texture record the compositor's opener
    // takes, written into the widened E26rResource.
    {
        const unsigned width=1920,height=1200,pitch=scanout_row_pitch(width);
        const uint64_t size=uint64_t(pitch)*height;
        AllocationRequest direct;
        static_assert(scanout_row_pitch(1920)==7680);
        CHECK(direct.prepare_surface(width,height,pitch,D3DDDIFMT_A8R8G8B8,size,owner,false,true,true)==S_OK);
        check_scanout_record(direct,width,height,AMDGPU_WDDM_DXGI_B8G8R8A8_UNORM);
        CHECK(direct.info.Flags==D3D12DDI_ALLOCATION_INFO_FLAGS_0022_PRIMARY);
        CHECK(direct.info.VidPnSourceId==BC250_SCANOUT_VIDPN_SOURCE);
        CHECK(direct.info.pPrivateDriverData==&direct.surface && direct.info.PrivateDriverDataSize==32);
        CHECK(direct.surface.magic==kLb7aMagic && direct.surface.version==1 && direct.surface.width==width &&
              direct.surface.height==height && direct.surface.pitch==pitch &&
              direct.surface.format==AMDGPU_WDDM_D3DDDI_A8R8G8B8 && direct.surface.size==size);
        CHECK(direct.held==size && !direct.texture.Magic);
        // The negative control of the widening: the same record cut to the 16-byte v2 shape the shell
        // wrote before this increment. The kernel driver's parser refuses a v3 header on 16 bytes, and
        // both user-mode scannable answers refuse it, so the old write could never become a flip.
        CHECK(!WddmGdiRecordScannable(direct.args.pPrivateDriverData,16));
        int shared=0,cached=0;
        CHECK(!Bc250SurfaceResourcePolicy(direct.args.pPrivateDriverData,16,&shared,&cached));
        E26rResource v2=direct.resource;v2.version=2;
        CHECK(Bc250SurfaceResourcePolicy(&v2,16,&shared,&cached) && Bc250SurfaceResourceScanout(&v2,16));
        CHECK(!WddmGdiRecordScannable(&v2,16) && !WddmGdiRecordScannable(&v2,sizeof(v2)));
        // The composed primary and the present-cached primary keep their measured shapes: v1 in 12 bytes,
        // v2 in 16, with the texture fields of the widened struct zero and not on the wire.
        AllocationRequest composed;
        CHECK(composed.prepare_surface(width,height,pitch,D3DDDIFMT_A8R8G8B8,size,owner)==S_OK);
        CHECK(composed.args.pPrivateDriverData==&composed.resource && composed.args.PrivateDriverDataSize==12);
        CHECK(composed.resource.version==1 && !composed.resource.width && !composed.resource.format);
        CHECK(composed.info.VidPnSourceId==D3DDDI_ID_UNINITIALIZED);
        AllocationRequest cachedReq;
        CHECK(cachedReq.prepare_surface(width,height,pitch,D3DDDIFMT_A8R8G8B8,size,owner,true,true)==S_OK);
        CHECK(cachedReq.args.pPrivateDriverData==&cachedReq.resource && cachedReq.args.PrivateDriverDataSize==16);
        CHECK(cachedReq.resource.version==2 && cachedReq.resource.access==kE26rCpuRead && !cachedReq.resource.width);
        CHECK(Bc250SurfaceResourcePolicy(cachedReq.args.pPrivateDriverData,16,&shared,&cached) && shared==1 && cached);
        // A reused request keeps nothing of the previous call: after a scan-out call, a composed call on
        // the same object sends v1 in 12 bytes and the texture fields are zero again.
        CHECK(direct.prepare_surface(width,height,pitch,D3DDDIFMT_A8R8G8B8,size,owner)==S_OK);
        CHECK(direct.args.PrivateDriverDataSize==12 && direct.resource.version==1 && !direct.resource.access &&
              !direct.resource.width && !direct.resource.bind_flags);
    }
    // (b) The pitch pin. The kernel driver's flip clause admits any whole number of 4-byte pixels that
    // holds the row, but only one pitch is derived by every component on the path, so only that one is
    // asked for; a composed primary keeps the engine's own pitch.
    {
        const unsigned width=1920,height=1200;
        AllocationRequest p;
        for(unsigned pitch:{7936u,8192u,15360u}) {
            const uint64_t size=uint64_t(pitch)*height;
            CHECK(p.prepare_surface(width,height,pitch,D3DDDIFMT_A8R8G8B8,size,owner,false,true,true)==E_INVALIDARG);
            CHECK(p.prepare_surface(width,height,pitch,D3DDDIFMT_A8R8G8B8,size,owner)==S_OK);
        }
        CHECK(p.prepare_surface(width,height,7696,D3DDDIFMT_A8R8G8B8,7696ull*height+2816,owner,false,true,true)==E_INVALIDARG);
        // 1900 is not a multiple of 64 pixels: the pin is the rounded pitch, not width*4.
        static_assert(scanout_row_pitch(1900)==7680 && 1900u*4u==7600u);
        CHECK(p.prepare_surface(1900,height,7600,D3DDDIFMT_A8R8G8B8,7600ull*height,owner,false,true,true)==E_INVALIDARG);
        CHECK(p.prepare_surface(1900,height,7680,D3DDDIFMT_A8R8G8B8,7680ull*height,owner,false,true,true)==S_OK);
        check_scanout_record(p,1900,height,AMDGPU_WDDM_DXGI_B8G8R8A8_UNORM);
        // Any other source geometry gets its own record and its own pin: nothing here knows 1920x1200.
        CHECK(p.prepare_surface(1280,720,5120,D3DDDIFMT_A8R8G8B8,5120ull*720,owner,false,true,true)==S_OK);
        check_scanout_record(p,1280,720,AMDGPU_WDDM_DXGI_B8G8R8A8_UNORM);
        CHECK(p.prepare_surface(3840,2160,15360,D3DDDIFMT_A8R8G8B8,15360ull*2160,owner,false,true,true)==S_OK);
        check_scanout_record(p,3840,2160,AMDGPU_WDDM_DXGI_B8G8R8A8_UNORM);
    }
    // (c) The rows a scan-out primary may have. X8 is a SCANOUT_PRIMARY row with no DXGI name, so no v3
    // record can describe it and the compositor could never open it: refused here rather than placed in
    // VRAM for a flip that could not be admitted. The composed-only rows are refused as before.
    {
        const unsigned width=1920,height=1200,pitch=7680;const uint64_t size=uint64_t(pitch)*height;
        AllocationRequest c;
        CHECK(c.prepare_surface(width,height,pitch,D3DDDIFMT_X8R8G8B8,size,owner,false,true,true)==E_NOTIMPL);
        CHECK(c.prepare_surface(width,height,pitch,D3DDDIFMT_A8B8G8R8,size,owner,false,true,true)==E_NOTIMPL);
        CHECK(c.prepare_surface(width,height,pitch,D3DDDIFMT_A2B10G10R10,size,owner,false,true,true)==E_NOTIMPL);
        CHECK(c.prepare_surface(width,height,15360,D3DDDIFMT_A16B16G16R16F,15360ull*height,owner,false,true,true)==E_NOTIMPL);
        CHECK(c.prepare_surface(width,height,pitch,D3DDDIFMT_A8,size,owner,false,true,true)==E_NOTIMPL);
        // Scan-out contradicts the other two intents and is refused rather than silently reduced.
        CHECK(c.prepare_surface(width,height,pitch,D3DDDIFMT_A8R8G8B8,size,owner,true,true,true)==E_INVALIDARG);
        CHECK(c.prepare_surface(width,height,pitch,D3DDDIFMT_A8R8G8B8,size,owner,false,false,true)==E_INVALIDARG);
        // And the geometry rules of the composed path still hold for it.
        CHECK(c.prepare_surface(width,height,pitch,D3DDDIFMT_A8R8G8B8,4096,owner,false,true,true)==E_INVALIDARG);
        CHECK(c.prepare_surface(0,height,pitch,D3DDDIFMT_A8R8G8B8,size,owner,false,true,true)==E_INVALIDARG);
        CHECK(c.prepare_surface(width,0,pitch,D3DDDIFMT_A8R8G8B8,size,owner,false,true,true)==E_INVALIDARG);
    }
    // (d) The stand-down decision, one case per clause. Every refusal leaves the composed primary, so the
    // test asserts the reason and not a failure. Increment 3: no list is the default, which is on, and the
    // geometry is the trailer's source mode at the moment of the decision, whatever a list names.
    {
        const bc250_scanout_caps on=caps_on(),off{};
        const unsigned bgra=AMDGPU_WDDM_DXGI_B8G8R8A8_UNORM,pitch=7680;
        const char* none="";
        struct Case { const char* list;bc250_scanout_caps caps;unsigned long force;bool desktop;
                      unsigned dxgi,w,h,pitch;ScanoutStandDown reason;ScanoutSwitch state; };
        const Case cases[]={
            {none,on,0,true,bgra,1920,1200,pitch,ScanoutStandDown::Admitted,ScanoutSwitch::Default},
            {"none",on,0,true,bgra,1920,1200,pitch,ScanoutStandDown::Admitted,ScanoutSwitch::Default},
            {"raytracing-tier",on,0,true,bgra,1920,1200,pitch,ScanoutStandDown::Admitted,ScanoutSwitch::Default},
            // The increment-2 spellings are an explicit on, and the geometry they name is not compared.
            {"scanout-flip",on,0,true,bgra,1920,1200,pitch,ScanoutStandDown::Admitted,ScanoutSwitch::Named},
            {"scanout-flip-1920x1200",on,0,true,bgra,1920,1200,pitch,ScanoutStandDown::Admitted,ScanoutSwitch::Named},
            {"scanout-flip-1920x1200",caps_on(1920,1080),0,true,bgra,1920,1080,pitch,ScanoutStandDown::Admitted,
             ScanoutSwitch::Named},
            // A token that only looks like the mode is no switch, so the default answers.
            {"scanout-flip-1920",on,0,true,bgra,1920,1200,pitch,ScanoutStandDown::Admitted,ScanoutSwitch::Default},
            // The off switch, alone or with the explicit on: the off switch wins, in either order.
            {"scanout-flip-off",on,0,true,bgra,1920,1200,pitch,ScanoutStandDown::ModeOff,ScanoutSwitch::Off},
            {"scanout-flip,scanout-flip-off",on,0,true,bgra,1920,1200,pitch,ScanoutStandDown::ModeOff,ScanoutSwitch::Off},
            {"scanout-flip-off,scanout-flip-1920x1200",on,0,true,bgra,1920,1200,pitch,ScanoutStandDown::ModeOff,
             ScanoutSwitch::Off},
            {"present-cached",on,0,true,bgra,1920,1200,pitch,ScanoutStandDown::OtherIntent,ScanoutSwitch::Default},
            {"scanout-flip,present-noprimary",on,0,true,bgra,1920,1200,pitch,ScanoutStandDown::OtherIntent,
             ScanoutSwitch::Named},
            {none,on,1,true,bgra,1920,1200,pitch,ScanoutStandDown::ForceCpu,ScanoutSwitch::Default},
            {none,on,7,true,bgra,1920,1200,pitch,ScanoutStandDown::ForceCpu,ScanoutSwitch::Default},
            // The compositor's record does not say GPU (a CPU route, the fallback, or no record): the chain the
            // trailer admits stands down, under the default and under the explicit on alike. The kill switch is
            // asked first, and the operator's off switch before both.
            {none,on,0,false,bgra,1920,1200,pitch,ScanoutStandDown::DesktopRoute,ScanoutSwitch::Default},
            {"scanout-flip",on,0,false,bgra,1920,1200,pitch,ScanoutStandDown::DesktopRoute,ScanoutSwitch::Named},
            {none,on,1,false,bgra,1920,1200,pitch,ScanoutStandDown::ForceCpu,ScanoutSwitch::Default},
            {"scanout-flip-off",on,0,false,bgra,1920,1200,pitch,ScanoutStandDown::ModeOff,ScanoutSwitch::Off},
            {none,off,0,false,bgra,1920,1200,pitch,ScanoutStandDown::DesktopRoute,ScanoutSwitch::Default},
            {none,off,0,true,bgra,1920,1200,pitch,ScanoutStandDown::CapsClosed,ScanoutSwitch::Default},
            // The source mode, not the native one: 478's exclusive 1080 chain on a committed 1080 mode admits,
            // and the same chain on a 1200 mode (the mode change has not reached the kernel driver) stands down.
            {none,caps_on(1920,1080),0,true,bgra,1920,1080,pitch,ScanoutStandDown::Admitted,ScanoutSwitch::Default},
            {none,on,0,true,bgra,1920,1080,pitch,ScanoutStandDown::SourceGeometry,ScanoutSwitch::Default},
            {none,caps_on(1920,1080),0,true,bgra,1920,1200,pitch,ScanoutStandDown::SourceGeometry,ScanoutSwitch::Default},
            {none,caps_on(1280,720),0,true,bgra,1280,720,5120,ScanoutStandDown::Admitted,ScanoutSwitch::Default},
            {none,caps_on(1280,720),0,true,bgra,1920,1200,pitch,ScanoutStandDown::SourceGeometry,ScanoutSwitch::Default},
            {none,on,0,true,bgra,1200,1920,pitch,ScanoutStandDown::SourceGeometry,ScanoutSwitch::Default},
            {none,on,0,true,bgra,0,1200,pitch,ScanoutStandDown::SourceGeometry,ScanoutSwitch::Default},
            // The increment-2 token names 1920x1200 but the source mode is 1280x720: the trailer decides.
            {"scanout-flip-1920x1200",caps_on(1280,720),0,true,bgra,1920,1200,pitch,ScanoutStandDown::SourceGeometry,
             ScanoutSwitch::Named},
            {none,on,0,true,AMDGPU_WDDM_DXGI_R16G16B16A16_FLOAT,1920,1200,pitch,ScanoutStandDown::Format,
             ScanoutSwitch::Default},
            {none,on,0,true,0,1920,1200,pitch,ScanoutStandDown::Format,ScanoutSwitch::Default},
            {none,on,0,true,bgra,1920,1200,0,ScanoutStandDown::Pitch,ScanoutSwitch::Default},
            {none,on,0,true,bgra,1920,1200,7600,ScanoutStandDown::Pitch,ScanoutSwitch::Default},
            {none,on,0,true,bgra,1920,1200,7936,ScanoutStandDown::Pitch,ScanoutSwitch::Default},
        };
        for(const auto& c:cases) {
            const ScanoutDecision d=scanout_decide(c.list,c.caps,c.force,c.desktop,c.dxgi,c.w,c.h,c.pitch);
            if(d.reason!=c.reason || d.switch_state!=c.state)
                std::printf("case '%s' %ux%u: got %s/%s\n",c.list,c.w,c.h,scanout_stand_down_text(d.reason),
                            scanout_switch_text(d.switch_state));
            CHECK(d.reason==c.reason);
            CHECK(d.switch_state==c.state);
            CHECK(d.admitted==(c.reason==ScanoutStandDown::Admitted));
            CHECK(std::strcmp(scanout_stand_down_text(d.reason),"unknown")!=0);
        }
        for(unsigned reason=0;reason<unsigned(ScanoutStandDown::Count);++reason)
            CHECK(std::strcmp(scanout_stand_down_text(ScanoutStandDown(reason)),"unknown")!=0);
        CHECK(!std::strcmp(scanout_stand_down_text(ScanoutStandDown::SourceGeometry),"source-geometry"));
        for(unsigned state=0;state<=unsigned(ScanoutSwitch::Off);++state)
            CHECK(std::strcmp(scanout_switch_text(ScanoutSwitch(state)),"unknown")!=0);
        CHECK(kScanoutDefaultOn);
        // A trailer with the right header and the flag clear reads exactly as no trailer at all, which is
        // what a start with the operator's switch off and an older kernel driver must have in common. A
        // trailer with the flag but a wrong header is no trailer either (the decoder zeroes it; the rule
        // checks again, so a caller that skips the decoder cannot open the path).
        bc250_scanout_caps flagless=on;flagless.flags=0;
        CHECK(scanout_decide(none,flagless,0,true,bgra,1920,1200,pitch).reason==ScanoutStandDown::CapsClosed);
        bc250_scanout_caps unsigned_caps=on;unsigned_caps.magic=0;
        CHECK(scanout_decide(none,unsigned_caps,0,true,bgra,1920,1200,pitch).reason==ScanoutStandDown::CapsClosed);
        bc250_scanout_caps newer=on;newer.version=BC250_SCANOUT_CAPS_VERSION+1;
        CHECK(scanout_decide(none,newer,0,true,bgra,1920,1200,pitch).reason==ScanoutStandDown::CapsClosed);
        CHECK(bc250_scanout_primary_rule(nullptr,0,1,bgra,1920,1200,pitch,1)==BC250_SCANOUT_PRIMARY_CAPS_CLOSED);
        // A null list is the same as an empty one: the default.
        CHECK(scanout_decide(nullptr,on,0,true,bgra,1920,1200,pitch).reason==ScanoutStandDown::Admitted);
        // C71, session 480: the chain made before the mode commit. W3's exclusive 1080 chain on a trailer that
        // still says 1200, and its 1200 chain on the way back on a trailer that still says 1080, admit when the
        // kernel driver's mode list offers their geometry, and stay composed when it does not.
        const bc250_scanout_caps on1080=caps_on(1920,1080);
        CHECK(scanout_decide(none,on,0,true,bgra,1920,1080,pitch,true).reason==ScanoutStandDown::Admitted);
        CHECK(scanout_decide(none,on1080,0,true,bgra,1920,1200,pitch,true).reason==ScanoutStandDown::Admitted);
        CHECK(scanout_decide(none,on,0,true,bgra,1920,1080,pitch,false).reason==ScanoutStandDown::SourceGeometry);
        CHECK(scanout_decide(none,on1080,0,true,bgra,1920,1200,pitch).reason==ScanoutStandDown::SourceGeometry);
        CHECK(scanout_decide(none,caps_on(1280,720),0,true,bgra,1920,1080,pitch,true).admitted);
        // An offered mode turns the geometry clause only. Every other clause still answers first or after it.
        CHECK(scanout_decide("scanout-flip-off",on,0,true,bgra,1920,1080,pitch,true).reason==ScanoutStandDown::ModeOff);
        CHECK(scanout_decide("present-cached",on,0,true,bgra,1920,1080,pitch,true).reason==ScanoutStandDown::OtherIntent);
        CHECK(scanout_decide(none,on,1,true,bgra,1920,1080,pitch,true).reason==ScanoutStandDown::ForceCpu);
        CHECK(scanout_decide(none,on,0,false,bgra,1920,1080,pitch,true).reason==ScanoutStandDown::DesktopRoute);
        CHECK(scanout_decide(none,off,0,true,bgra,1920,1080,pitch,true).reason==ScanoutStandDown::CapsClosed);
        CHECK(scanout_decide(none,flagless,0,true,bgra,1920,1080,pitch,true).reason==ScanoutStandDown::CapsClosed);
        CHECK(scanout_decide(none,on,0,true,AMDGPU_WDDM_DXGI_R16G16B16A16_FLOAT,1920,1080,15360,true).reason==
              ScanoutStandDown::Format);
        CHECK(scanout_decide(none,on,0,true,bgra,1920,1080,7936,true).reason==ScanoutStandDown::Pitch);
        CHECK(scanout_decide(none,on,0,true,bgra,0,1080,pitch,true).reason==ScanoutStandDown::SourceGeometry);
        CHECK(scanout_decide(none,on,0,true,bgra,1920,0,pitch,true).reason==ScanoutStandDown::SourceGeometry);
        CHECK(bc250_scanout_primary_rule(&on,0,1,bgra,1920,1080,pitch,1)==BC250_SCANOUT_PRIMARY_ADMITTED);
        CHECK(bc250_scanout_primary_rule(&on,0,1,bgra,1920,1080,pitch,0)==BC250_SCANOUT_PRIMARY_SOURCE_GEOMETRY);
        CHECK(bc250_scanout_primary_rule(&on,0,1,bgra,1920,0,pitch,1)==BC250_SCANOUT_PRIMARY_SOURCE_GEOMETRY);
        // The contract's pitch is the shell's pitch for every 4-byte width, so the rule that the D3D11 shell
        // shares cannot pin a different row than the one prepare_surface pins here.
        for(unsigned width=1;width<=4096;++width)
            CHECK(bc250_scanout_primary_pitch(width,4)==scanout_row_pitch(width));
        CHECK(!bc250_scanout_primary_pitch(0,4) && !bc250_scanout_primary_pitch(1920,0));
        CHECK(!bc250_scanout_primary_pitch(0xffffffffu,4));
        for(unsigned reason=BC250_SCANOUT_PRIMARY_ADMITTED;reason<=BC250_SCANOUT_PRIMARY_DESKTOP_ROUTE;++reason)
            CHECK(std::strcmp(bc250_scanout_primary_text(reason),"unknown")!=0);
        CHECK(!std::strcmp(bc250_scanout_primary_text(99),"unknown"));
        // Every admitted decision describes a surface prepare_surface then accepts: the two are one rule.
        AllocationRequest d;
        CHECK(d.prepare_surface(1920,1200,pitch,D3DDDIFMT_A8R8G8B8,uint64_t(pitch)*1200,owner,false,true,true)==S_OK);
        CHECK(d.prepare_surface(1920,1080,pitch,D3DDDIFMT_A8R8G8B8,uint64_t(pitch)*1080,owner,false,true,true)==S_OK);
    }
    // (e) The adapter trailer as query_contract decodes it: the header must be whole, and anything else
    // reads as no trailer, so a shell on an older kernel driver asks for nothing.
    {
        unsigned char data[BC250_SCANOUT_CAPS_TOTAL]{};
        CHECK(!decode_scanout_caps(data,sizeof(data)).flags);
        CHECK(!decode_scanout_caps(nullptr,sizeof(data)).flags);
        const bc250_scanout_caps good=caps_on();
        std::memcpy(data+BC250_SCANOUT_CAPS_OFFSET,&good,sizeof(good));
        CHECK(decode_scanout_caps(data,sizeof(data)).flags==BC250_SCANOUT_CAPS_DIRECT_FLIP);
        CHECK(decode_scanout_caps(data,sizeof(data)).post_width==1920);
        CHECK(decode_scanout_caps(data,sizeof(data)).post_height==1200);
        // A buffer one byte short of the trailer: the kernel driver writes nothing there, and a reader
        // that read it anyway would read another trailer's bytes.
        CHECK(!decode_scanout_caps(data,sizeof(data)-1).flags);
        for(unsigned field=0;field<4;++field) {
            bc250_scanout_caps bad=good;
            if(field==0)bad.magic=0;else if(field==1)bad.version=BC250_SCANOUT_CAPS_VERSION+1;
            else if(field==2)bad.size=sizeof(bad)-1;else bad.post_width=0;
            std::memcpy(data+BC250_SCANOUT_CAPS_OFFSET,&bad,sizeof(bad));
            CHECK(!decode_scanout_caps(data,sizeof(data)).flags);
        }
        bc250_scanout_caps noheight=good;noheight.post_height=0;
        std::memcpy(data+BC250_SCANOUT_CAPS_OFFSET,&noheight,sizeof(noheight));
        CHECK(!decode_scanout_caps(data,sizeof(data)).flags);
    }
    // (f) The kill switch is read the way the router reads it. The value on this machine is whatever it
    // is; what the test holds is that the read is cached and never throws.
    {
        const unsigned long once=scanout_force_cpu();
        CHECK(once==scanout_force_cpu() && once==scanout_force_cpu());
    }
    // (g) The compositor's desktop-route record (driver/contract/bc250_desktop_route.h), through the one writer
    // the router uses and the one reader both shells use, under a name of this test's own. The owner a reader
    // trusts is a parameter: this process's default owner here, the compositor's account in the shells.
    {
        HANDLE token=nullptr;DWORD bytes=0;
        CHECK(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)!=0);
        GetTokenInformation(token,TokenOwner,nullptr,0,&bytes);
        std::vector<unsigned char> info(bytes?bytes:1);
        CHECK(bytes && GetTokenInformation(token,TokenOwner,info.data(),bytes,&bytes));
        CloseHandle(token);
        const PSID self=reinterpret_cast<TOKEN_OWNER*>(info.data())->Owner;
        wchar_t name[96];
        swprintf(name,96,L"Local\\bc250-desktop-route-test-%lu",GetCurrentProcessId());
        bc250_desktop_route record{};
        CHECK(bc250_desktop_route_read(name,self,&record)==BC250_DESKTOP_ROUTE_READ_ABSENT && !record.magic);
        DWORD error=0;
        bc250_desktop_route* view=bc250_desktop_route_create(name,&error);
        CHECK(view!=nullptr && !error);
        if(view){
            // Made, no decision yet: a whole header and route NONE, which is not GPU.
            unsigned status=bc250_desktop_route_read(name,self,&record);
            CHECK(status==BC250_DESKTOP_ROUTE_READ_OK && record.route==BC250_DESKTOP_ROUTE_NONE && !bc250_desktop_route_gpu(status,&record));
            CHECK(!std::strcmp(bc250_desktop_route_text(status,record.route),"none"));
            bc250_desktop_route_store(view,BC250_DESKTOP_ROUTE_GPU,4242,0);
            status=bc250_desktop_route_read(name,self,&record);
            CHECK(status==BC250_DESKTOP_ROUTE_READ_OK && bc250_desktop_route_gpu(status,&record) && record.pid==4242 &&
                  record.decisions==1 && record.size==BC250_DESKTOP_ROUTE_BYTES);
            CHECK(!std::strcmp(bc250_desktop_route_text(status,record.route),"gpu"));
            // Every CPU route reads as not GPU, with its own word for the trace.
            const struct { unsigned route;const char* word; } cpu[]={
                {BC250_DESKTOP_ROUTE_KILL_SWITCH,"cpu-kill-switch"},{BC250_DESKTOP_ROUTE_SWITCHES_OFF,"cpu-switches-off"},
                {BC250_DESKTOP_ROUTE_FALLBACK,"cpu-fallback"},{99,"unknown"}};
            for(const auto& c:cpu){
                bc250_desktop_route_store(view,c.route,4242,0x80004005u);
                status=bc250_desktop_route_read(name,self,&record);
                CHECK(status==BC250_DESKTOP_ROUTE_READ_OK && record.route==c.route && !bc250_desktop_route_gpu(status,&record));
                CHECK(!std::strcmp(bc250_desktop_route_text(status,record.route),c.word));
            }
            CHECK(record.decisions==5 && record.hosted_hr==0x80004005u);
            // The trust clause: the same record read with the compositor's account as the owner is refused, and
            // so it is for the session reader, whose record (if any) this test did not write.
            bc250_desktop_route_store(view,BC250_DESKTOP_ROUTE_GPU,4242,0);
            SE_SID dwm{};DWORD session=0;
            CHECK(ProcessIdToSessionId(GetCurrentProcessId(),&session) && bc250_desktop_route_compositor_sid(session,&dwm));
            status=bc250_desktop_route_read(name,&dwm.Sid,&record);
            CHECK(status==BC250_DESKTOP_ROUTE_READ_OWNER && !record.route && !bc250_desktop_route_gpu(status,&record));
            CHECK(!std::strcmp(bc250_desktop_route_text(status,record.route),"foreign-owner"));
            CHECK(bc250_desktop_route_read(name,nullptr,&record)==BC250_DESKTOP_ROUTE_READ_OWNER);
            // S-1-5-90-0-<session>: Window Manager\DWM-<session>.
            CHECK(*GetSidSubAuthorityCount(&dwm.Sid)==3 && *GetSidSubAuthority(&dwm.Sid,0)==90 &&
                  *GetSidSubAuthority(&dwm.Sid,1)==0 && *GetSidSubAuthority(&dwm.Sid,2)==session);
            // A header of another version, or torn, is no record: GPU in the route word does not count.
            view->version=BC250_DESKTOP_ROUTE_VERSION+1;
            status=bc250_desktop_route_read(name,self,&record);
            CHECK(status==BC250_DESKTOP_ROUTE_READ_SHAPE && !record.route && !bc250_desktop_route_gpu(status,&record));
            CHECK(!std::strcmp(bc250_desktop_route_text(status,record.route),"bad-record"));
            view->version=BC250_DESKTOP_ROUTE_VERSION;view->magic=0;
            CHECK(bc250_desktop_route_read(name,self,&record)==BC250_DESKTOP_ROUTE_READ_SHAPE);
            view->magic=BC250_DESKTOP_ROUTE_MAGIC;
            CHECK(bc250_desktop_route_gpu(bc250_desktop_route_read(name,self,&record),&record));
            // A status other than OK never reads as GPU, whatever the record holds.
            CHECK(!bc250_desktop_route_gpu(BC250_DESKTOP_ROUTE_READ_DENIED,&record));
            CHECK(!std::strcmp(bc250_desktop_route_text(BC250_DESKTOP_ROUTE_READ_DENIED,0),"denied"));
            CHECK(!std::strcmp(bc250_desktop_route_text(BC250_DESKTOP_ROUTE_READ_ABSENT,0),"absent"));
        }
        // The shells' production reader on this machine: whatever it finds, it never reads GPU from a record
        // that the compositor's account did not write, and it never fails open.
        bc250_desktop_route s{};
        const unsigned session_status=scanout_desktop_route_read(&s);
        CHECK(session_status<=BC250_DESKTOP_ROUTE_READ_SHAPE);
        if(session_status!=BC250_DESKTOP_ROUTE_READ_OK)CHECK(!s.magic && !s.route);
    }
    // (h) The kernel driver's mode list (C71), through D3DKMT doubles: the shape of the three calls, the
    // geometry match, and every failure reading as "not offered" or "failed", never as "offered".
    {
        const ScanoutModeKmt kmt{&fake_open,&fake_list,&fake_close};
        LUID luid{};luid.LowPart=0x1234;luid.HighPart=7;
        const auto reset=[](std::initializer_list<D3DKMT_DISPLAYMODE> modes){
            kmt_fake=FakeKmt{};kmt_fake.modes.assign(modes.begin(),modes.end());
        };
        // The lab's kind of list: the native mode and scaled ones, each in more than one format.
        reset({mode(1920,1200),mode(1920,1200,D3DDDIFMT_A8B8G8R8),mode(1920,1080),mode(1920,1080,D3DDDIFMT_A2B10G10R10),
               mode(1280,720)});
        CHECK(scanout_mode_offered(luid,kmt,1920,1080)==ScanoutModeList::Offered);
        CHECK(kmt_fake.opens==1 && kmt_fake.counts==1 && kmt_fake.reads==1 && kmt_fake.closes==1);
        CHECK(kmt_fake.luid.LowPart==0x1234 && kmt_fake.luid.HighPart==7 && kmt_fake.source==BC250_SCANOUT_VIDPN_SOURCE);
        CHECK(scanout_mode_offered(luid,kmt,1920,1200)==ScanoutModeList::Offered);
        CHECK(scanout_mode_offered(luid,kmt,1280,720)==ScanoutModeList::Offered);
        // Width and height both: a transposed or a one-sided match is no mode.
        CHECK(scanout_mode_offered(luid,kmt,1200,1920)==ScanoutModeList::NotOffered);
        CHECK(scanout_mode_offered(luid,kmt,1920,1000)==ScanoutModeList::NotOffered);
        CHECK(scanout_mode_offered(luid,kmt,1600,900)==ScanoutModeList::NotOffered);
        CHECK(kmt_fake.opens==kmt_fake.closes);
        // An empty list offers nothing and reads nothing.
        reset({});
        CHECK(scanout_mode_offered(luid,kmt,1920,1080)==ScanoutModeList::NotOffered && !kmt_fake.reads && kmt_fake.closes==1);
        // A list that grew between the count and the read is counted once more.
        reset({mode(1920,1200),mode(1920,1080)});kmt_fake.grow=1;
        CHECK(scanout_mode_offered(luid,kmt,1280,720)==ScanoutModeList::Offered);
        CHECK(kmt_fake.counts==2 && kmt_fake.reads==2 && kmt_fake.closes==1);
        // ... but only once: a list that keeps growing is a failure, not a loop.
        reset({mode(1920,1200),mode(1920,1080)});kmt_fake.grow=5;
        CHECK(scanout_mode_offered(luid,kmt,1920,1080)==ScanoutModeList::Failed);
        CHECK(kmt_fake.counts==2 && kmt_fake.closes==1);
        // Failures: open, count, read and close, and a list above the cap.
        reset({mode(1920,1080)});kmt_fake.open_status=static_cast<NTSTATUS>(0xC000000DL);
        CHECK(scanout_mode_offered(luid,kmt,1920,1080)==ScanoutModeList::Failed && !kmt_fake.counts && !kmt_fake.closes);
        reset({mode(1920,1080)});kmt_fake.count_status=static_cast<NTSTATUS>(0xC0000001L);
        CHECK(scanout_mode_offered(luid,kmt,1920,1080)==ScanoutModeList::Failed && kmt_fake.closes==1);
        reset({mode(1920,1080)});kmt_fake.read_status=static_cast<NTSTATUS>(0xC0000001L);
        CHECK(scanout_mode_offered(luid,kmt,1920,1080)==ScanoutModeList::Failed && kmt_fake.closes==1);
        reset({mode(1920,1080)});kmt_fake.close_status=static_cast<NTSTATUS>(0xC0000008L);
        CHECK(scanout_mode_offered(luid,kmt,1920,1080)==ScanoutModeList::Failed && kmt_fake.closes==1);
        reset({});kmt_fake.modes.resize(kScanoutModeListMax+1,mode(1920,1080));
        CHECK(scanout_mode_offered(luid,kmt,1920,1080)==ScanoutModeList::Failed && !kmt_fake.reads && kmt_fake.closes==1);
        // An informational open status with a handle: the handle is closed and nothing is read.
        reset({mode(1920,1080)});kmt_fake.open_status=1;
        CHECK(scanout_mode_offered(luid,kmt,1920,1080)==ScanoutModeList::Failed && !kmt_fake.counts && kmt_fake.closes==1);
        // A missing entry or an empty geometry asks nothing.
        reset({mode(1920,1080)});
        CHECK(scanout_mode_offered(luid,ScanoutModeKmt{&fake_open,nullptr,&fake_close},1920,1080)==ScanoutModeList::Failed);
        CHECK(scanout_mode_offered(luid,kmt,0,1080)==ScanoutModeList::Failed);
        CHECK(scanout_mode_offered(luid,kmt,1920,0)==ScanoutModeList::Failed && !kmt_fake.opens);
        // The shell's reader with no adapter LUID asks nothing.
        CHECK(scanout_mode_list_read(0,1920,1080)==ScanoutModeList::Failed);
        for(unsigned value=0;value<=unsigned(ScanoutModeList::Failed);++value)
            CHECK(std::strcmp(scanout_mode_list_text(ScanoutModeList(value)),"unknown")!=0);
        CHECK(!std::strcmp(scanout_mode_list_text(ScanoutModeList::Offered),"offered"));
    }
    std::printf("native allocation request: %u checks, 0 failures\n",checks);
    std::puts("KMD parser, cache/rounding/refusal gates, shared surface records, scan-out v3 record, pitch pin, "
              "stand-down table, the adapter scan-out trailer and the desktop-route record passed");
}
