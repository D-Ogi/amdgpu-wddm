// SPDX-License-Identifier: MIT
// The bytes this shell hands the kernel for a runtime surface, for a fixed matrix of creates. The test runs
// each create through convert_runtime_resource (or present_shadow_request) and allocate_runtime_surface, catches
// the private data that reaches pfnAllocateCb, and compares one FNV-1a hash per group with the hash below. An
// NV12 create also goes through runtime_nv12_record for a fixed set of plane layouts. A change to any byte of a
// record, to a status or to the set of admitted creates changes the hash. A change that is intended updates the
// hash in the same commit and says why. To find the case that changed, set BC250_RECORD_GOLDEN_DUMP to a file
// path: the test then writes one line per case, and two dumps can be compared with a diff.
#include "ddi-resource.h"
#include "present-shadow.h"
#include "runtime-surface.h"
#include "runtime-surface-allocation.h"
#include "../../contract/amdgpu_wddm_surface_format.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
using namespace bc250::umd;
#define GOLDEN_CHECK(x) do { if(!(x)) { std::printf("FAIL record-golden line %d\n",__LINE__);std::fflush(stdout);std::abort(); } } while(0)
namespace {
// The hashes of the current records (2026-11-25, base c9a690b60 plus the DECODER bind flags).
constexpr unsigned long long kConvertHash=0x9867d41d7dd59d89ull;
constexpr unsigned long long kShadowHash=0xe27acf525590ee10ull;
constexpr unsigned long long kNv12Hash=0x33d0c5c1392286b5ull;

struct Fnv {
    unsigned long long h=0xcbf29ce484222325ull;
    void bytes(const void *p,size_t n) {
        const auto *b=static_cast<const unsigned char *>(p);
        for (size_t i=0;i<n;++i) { h^=b[i]; h*=0x100000001b3ull; }
    }
    void word(unsigned long long v) { bytes(&v,sizeof(v)); }
};
struct Captured {
    bool called=false;
    std::vector<unsigned char> resource,allocation;
    bool primary=false;
    UINT vidpn=0;
};
Captured captured;
int deviceIdentity,resourceIdentity;
HRESULT APIENTRY capture_allocate(HANDLE,D3DDDICB_ALLOCATE *a) {
    captured.called=true;
    const auto *r=static_cast<const unsigned char *>(a->pPrivateDriverData);
    captured.resource.assign(r,r+a->PrivateDriverDataSize);
    const auto &info=a->pAllocationInfo2[0];
    const auto *s=static_cast<const unsigned char *>(info.pPrivateDriverData);
    captured.allocation.assign(s,s+info.PrivateDriverDataSize);
    captured.primary=info.Flags.Primary!=0; captured.vidpn=info.VidPnSourceId;
    a->pAllocationInfo2[0].hAllocation=41; a->hKMResource=42; return S_OK;
}
HRESULT APIENTRY capture_deallocate(HANDLE,const D3DDDICB_DEALLOCATE2 *) { return S_OK; }

FILE *dump;
void dump_line(const char *group,unsigned index,HRESULT hr,const Captured &c) {
    if (!dump) return;
    std::fprintf(dump,"%s %u hr=%08lx primary=%d vidpn=%u resource=",group,index,static_cast<unsigned long>(hr),
                 c.primary ? 1 : 0,c.vidpn);
    for (auto b:c.resource) std::fprintf(dump,"%02x",b);
    std::fprintf(dump," allocation=");
    for (auto b:c.allocation) std::fprintf(dump,"%02x",b);
    std::fprintf(dump,"\n");
}
// One case: the status of each step and the bytes the callback saw.
void fold(Fnv &f,HRESULT convert,HRESULT allocate,const Captured &c) {
    f.word(UINT(convert)); f.word(UINT(allocate)); f.word(c.called); f.word(c.primary); f.word(c.vidpn);
    f.word(c.resource.size()); if (!c.resource.empty()) f.bytes(c.resource.data(),c.resource.size());
    f.word(c.allocation.size()); if (!c.allocation.empty()) f.bytes(c.allocation.data(),c.allocation.size());
}
HRESULT allocate_once(RuntimeDevice &device,const RuntimeSurfaceRequest &request) {
    captured=Captured{};
    RuntimeSurfaceAllocation out{};
    return allocate_runtime_surface(device,request,out);
}
std::vector<DXGI_FORMAT> formats() {
    std::vector<DXGI_FORMAT> out;
    unsigned count=0;
    const auto *rows=amdgpu_wddm_surface_formats(&count);
    for (unsigned i=0;i<count;++i) {
        if (rows[i].dxgi) out.push_back(DXGI_FORMAT(rows[i].dxgi));
        if (rows[i].dxgi_srgb) out.push_back(DXGI_FORMAT(rows[i].dxgi_srgb));
    }
    // A format with no row and a planar one.
    out.push_back(DXGI_FORMAT_R16_FLOAT); out.push_back(DXGI_FORMAT_B5G6R5_UNORM); out.push_back(DXGI_FORMAT_NV12);
    return out;
}
struct Extent { UINT w,h; };
constexpr Extent kExtents[]={{1,1},{2,2},{17,9},{64,16},{624,444},{1920,1080},{3840,2160},{8192,8192},{16384,16384},
                             {16385,4},{0,4}};
constexpr UINT kBinds[]={
    D3D10_DDI_BIND_SHADER_RESOURCE,
    D3D10_DDI_BIND_SHADER_RESOURCE|D3D10_DDI_BIND_RENDER_TARGET,
    D3D10_DDI_BIND_SHADER_RESOURCE|D3D11_DDI_BIND_UNORDERED_ACCESS,
    D3D10_DDI_BIND_SHADER_RESOURCE|D3D11_DDI_BIND_DECODER,
    D3D10_DDI_BIND_RENDER_TARGET|D3D11_DDI_BIND_VIDEO_ENCODER,
    D3D10_DDI_BIND_SHADER_RESOURCE|D3D10_DDI_BIND_RENDER_TARGET|D3D10_DDI_BIND_PRESENT,
};
constexpr UINT kMisc[]={
    0,
    D3D10_DDI_RESOURCE_MISC_SHARED,
    D3DWDDM2_0DDI_RESOURCE_MISC_DISPLAYABLE_SURFACE,
    D3D10_DDI_RESOURCE_MISC_SHARED|D3DWDDM2_0DDI_RESOURCE_MISC_DISPLAYABLE_SURFACE,
    D3D10_DDI_RESOURCE_MISC_SHARED|D3D11_DDI_RESOURCE_MISC_RESOURCE_CLAMP,
    D3D10_DDI_RESOURCE_MISC_SHARED|D3D10_DDI_RESOURCE_MISC_DISCARD_ON_PRESENT,
};
// The plane layouts an engine could report for an NV12 image: tight and 256-byte pitches, a chroma plane right
// after the luma rows or at a later offset, and a few that the record must refuse.
std::vector<RuntimeImageLayout> nv12_layouts(UINT w,UINT h) {
    std::vector<RuntimeImageLayout> out;
    const UINT pitches[]={w,(w+255u)&~255u};
    const UINT64 alignments[]={4096,65536,0,3};
    for (UINT pitch:pitches) for (UINT64 alignment:alignments) for (int gap=0;gap<3;++gap) {
        RuntimeImageLayout l{};
        l.format=VK_FORMAT_G8_B8R8_2PLANE_420_UNORM; l.width=w; l.height=h; l.planes=2;
        l.plane[0].offset=0; l.plane[0].rowPitch=pitch; l.plane[0].size=UINT64(pitch)*h;
        l.plane[1].offset=l.plane[0].size+(gap==1 ? 4096u : 0u); l.plane[1].rowPitch=pitch;
        l.plane[1].size=UINT64(pitch)*(h/2);
        l.memory.alignment=alignment;
        l.memory.size=((l.plane[1].offset+l.plane[1].size+4095)&~UINT64(4095))-(gap==2 ? 4096u : 0u);
        out.push_back(l);
    }
    RuntimeImageLayout wrong{}; wrong.format=VK_FORMAT_R8G8B8A8_UNORM; wrong.planes=1; out.push_back(wrong);
    return out;
}
}
void test_record_golden() {
    char path[MAX_PATH]{};
    const DWORD length=GetEnvironmentVariableA("BC250_RECORD_GOLDEN_DUMP",path,MAX_PATH);
    if (length && length<MAX_PATH && fopen_s(&dump,path,"w")) dump=nullptr;
    RuntimeDevice device; device.hDevice=&deviceIdentity;
    device.KTCallbacks.pfnAllocateCb=capture_allocate; device.KTCallbacks.pfnDeallocate2Cb=capture_deallocate;
    RuntimeDomain::Scope scope(device.domain);
    Fnv convertHash,shadowHash,nv12Hash;
    unsigned index=0,admitted=0,nv12Admitted=0,shadows=0;
    for (DXGI_FORMAT format:formats()) for (const Extent &e:kExtents) for (UINT bind:kBinds) for (UINT misc:kMisc)
    for (int primary=0;primary<3;++primary) {
        D3D11DDIARG_CREATERESOURCE desc{};
        D3D10DDI_MIPINFO mip{}; mip.TexelWidth=e.w; mip.TexelHeight=e.h; mip.TexelDepth=1;
        mip.PhysicalWidth=e.w; mip.PhysicalHeight=e.h; mip.PhysicalDepth=1;
        desc.pMipInfoList=&mip; desc.MipLevels=desc.ArraySize=1; desc.ResourceDimension=D3D10DDIRESOURCE_TEXTURE2D;
        desc.SampleDesc.Count=1; desc.Usage=D3D10_DDI_USAGE_DEFAULT; desc.Format=format;
        desc.BindFlags=bind; desc.MiscFlags=misc;
        DXGI_DDI_PRIMARY_DESC primaryDesc{};
        primaryDesc.VidPnSourceId=UINT(primary-1); primaryDesc.ModeDesc.Width=e.w; primaryDesc.ModeDesc.Height=e.h;
        if (primary) desc.pPrimaryDesc=&primaryDesc;
        RuntimeSurfaceRequest request{}; D3D11_TEXTURE2D_DESC1 texture{};
        const HRESULT hr=convert_runtime_resource(desc,&resourceIdentity,request,texture,nullptr);
        if (FAILED(hr)) { captured=Captured{}; fold(convertHash,hr,0,captured); dump_line("convert",index++,hr,captured); continue; }
        // The description the engine creates must be the record's, field for field.
        GOLDEN_CHECK(texture.Width==request.texture.Width && texture.Height==request.texture.Height &&
                     UINT(texture.Format)==request.texture.Format && texture.BindFlags==request.texture.BindFlags &&
                     texture.MiscFlags==request.texture.MiscFlags);
        if (format!=DXGI_FORMAT_NV12) {
            const HRESULT a=allocate_once(device,request);
            fold(convertHash,hr,a,captured); dump_line("convert",index++,a,captured);
            admitted+=SUCCEEDED(a);
            continue;
        }
        // NV12: begin_runtime_surface fills the LB7A v2 record from the retained image's layout.
        fold(convertHash,hr,0,Captured{});
        dump_line("convert",index++,hr,Captured{});
        unsigned layoutIndex=0;
        for (const RuntimeImageLayout &layout:nv12_layouts(e.w,e.h)) {
            RuntimeSurfaceRequest actual=request;
            const HRESULT n=runtime_nv12_record(layout,actual.nv12);
            HRESULT a=0; captured=Captured{};
            if (SUCCEEDED(n)) { actual.surface=actual.nv12.Surface; a=allocate_once(device,actual); }
            fold(nv12Hash,n,a,captured); dump_line("nv12",layoutIndex++,a,captured);
            nv12Admitted+=SUCCEEDED(n) && SUCCEEDED(a);
        }
    }
    index=0;
    for (DXGI_FORMAT format:formats()) for (const Extent &e:kExtents) {
        RuntimeSurfaceRequest request{}; D3D11_TEXTURE2D_DESC1 texture{};
        const HRESULT hr=present_shadow_request(e.w,e.h,format,request,texture);
        HRESULT a=0; captured=Captured{};
        if (SUCCEEDED(hr)) { a=allocate_once(device,request); shadows+=SUCCEEDED(a); }
        fold(shadowHash,hr,a,captured); dump_line("shadow",index++,a,captured);
    }
    if (dump) std::fclose(dump);
    std::printf("record-golden: convert %016llx (%u allocated), nv12 %016llx (%u allocated), shadow %016llx (%u allocated)\n",
                convertHash.h,admitted,nv12Hash.h,nv12Admitted,shadowHash.h,shadows);
    std::fflush(stdout);
    // The matrix must reach the allocation in every group, or a hash proves nothing.
    GOLDEN_CHECK(admitted>100 && nv12Admitted>10 && shadows>10);
    GOLDEN_CHECK(convertHash.h==kConvertHash);
    GOLDEN_CHECK(nv12Hash.h==kNv12Hash);
    GOLDEN_CHECK(shadowHash.h==kShadowHash);
}
