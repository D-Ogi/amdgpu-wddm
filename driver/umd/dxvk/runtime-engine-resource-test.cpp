// SPDX-License-Identifier: MIT
// BD-373 step 3: the ENGINE LAYOUT form of a shared resource (runtime-engine-resource.h, bc250_shared_resource.h,
// bc250_shared_resource_alloc.h), with a mock Vulkan device and a mock engine. Creator and opener in one process:
// the creator's record goes to the opener, which must make the same record or refuse with a named reason.
#include "runtime-engine-resource.h"
#include "runtime-surface-allocation.h"
#include "../../contract/bc250_shared_resource_alloc.h"
#include <cstdlib>
#include <cstring>
#include <iostream>
using namespace bc250::umd;
namespace {
unsigned checks=0;
void check(bool b,const char *what) {
    ++checks;
    if (!b) { std::cerr << "FAIL engine-resource check " << checks << ": " << what << '\n'; std::abort(); }
}
bool is(const char *got,const char *want) { return got && want && std::strcmp(got,want)==0; }
}
namespace {
// Vulkan device.
unsigned imageCreates=0,imageDestroys=0,bufferCreates=0,bufferDestroys=0,allocations=0,frees=0,imageBinds=0,bufferBinds=0;
VkImageCreateInfo createdImage{}; VkBufferCreateInfo createdBuffer{};
VkMemoryRequirements memoryRequirements{98304,4096,2};
UINT64 layoutSalt=0;
bool linearSupported=true;
UINT32 linearMaxMips=15;
void *hostIdentity=nullptr;
const VkDeviceMemory kMemory=(VkDeviceMemory)(uintptr_t(0x77));
VkResult VKAPI_CALL image_create(VkDevice,const VkImageCreateInfo *info,const VkAllocationCallbacks *,VkImage *out) {
    ++imageCreates; createdImage=*info;
    check(info->tiling==VK_IMAGE_TILING_LINEAR,"the shell's image is LINEAR");
    *out=(VkImage)(uintptr_t(0x20)); return VK_SUCCESS;
}
void VKAPI_CALL image_destroy(VkDevice,VkImage image,const VkAllocationCallbacks *) {
    ++imageDestroys; check(image==(VkImage)(uintptr_t(0x20)),"destroy names the image");
}
// A LINEAR layout: layers of 32 KiB, mips inside a layer one after another. layoutSalt moves one mip of one layer,
// as a driver of another version could.
void VKAPI_CALL image_layout(VkDevice,VkImage,const VkImageSubresource *sub,VkSubresourceLayout *out) {
    check(sub->aspectMask==VK_IMAGE_ASPECT_COLOR_BIT,"color aspect");
    const UINT64 pitch=256u>>sub->mipLevel;
    *out={UINT64(sub->arrayLayer)*32768u+UINT64(sub->mipLevel)*8192u+(sub->mipLevel==2 && sub->arrayLayer==1 ? layoutSalt : 0),
          8192u,pitch<64 ? 64 : pitch,32768u,0};
}
void VKAPI_CALL image_requirements(VkDevice,VkImage,VkMemoryRequirements *r) { *r=memoryRequirements; }
void VKAPI_CALL buffer_requirements(VkDevice,VkBuffer,VkMemoryRequirements *r) { *r=memoryRequirements; }
bool expectBufferMemory=false;
VkResult VKAPI_CALL allocate(VkDevice,const VkMemoryAllocateInfo *a,const VkAllocationCallbacks *,VkDeviceMemory *m) {
    ++allocations;
    const void *next=a->pNext;
    if (expectBufferMemory) {
        auto *flags=static_cast<const VkMemoryAllocateFlagsInfo *>(next);
        check(flags->sType==VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO &&
              flags->flags==VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,"buffer memory has device addresses");
        next=flags->pNext;
    }
    auto *i=static_cast<const bc250_host_import *>(next);
    check(i->sType==BC250_HOST_IMPORT_STYPE && i->identity==hostIdentity && i->allocation==51 && i->va==1u<<20,
          "the import names the runtime allocation");
    auto *d=static_cast<const VkMemoryDedicatedAllocateInfo *>(i->pNext);
    check(d->sType==VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO && (expectBufferMemory ? d->buffer && !d->image :
          d->image && !d->buffer),"dedicated to the one image or buffer");
    check(a->allocationSize==memoryRequirements.size && a->memoryTypeIndex==1,"the requirement's size, a local type");
    *m=kMemory; return VK_SUCCESS;
}
VkResult VKAPI_CALL bind_image(VkDevice,VkImage,VkDeviceMemory m,VkDeviceSize offset) {
    ++imageBinds; check(m==kMemory && !offset,"image at offset 0"); return VK_SUCCESS;
}
void VKAPI_CALL release_memory(VkDevice,VkDeviceMemory m,const VkAllocationCallbacks *) { ++frees; check(m==kMemory,"free"); }
VkResult VKAPI_CALL buffer_create(VkDevice,const VkBufferCreateInfo *info,const VkAllocationCallbacks *,VkBuffer *out) {
    ++bufferCreates; createdBuffer=*info; *out=(VkBuffer)(uintptr_t(0x30)); return VK_SUCCESS;
}
void VKAPI_CALL buffer_destroy(VkDevice,VkBuffer,const VkAllocationCallbacks *) { ++bufferDestroys; }
VkResult VKAPI_CALL bind_buffer(VkDevice,VkBuffer,VkDeviceMemory m,VkDeviceSize offset) {
    ++bufferBinds; check(m==kMemory && !offset,"buffer at offset 0"); return VK_SUCCESS;
}
VkResult VKAPI_CALL image_format(VkPhysicalDevice,VkFormat,VkImageType type,VkImageTiling tiling,VkImageUsageFlags,
    VkImageCreateFlags,VkImageFormatProperties *p) {
    check(type==VK_IMAGE_TYPE_2D && tiling==VK_IMAGE_TILING_LINEAR,"the LINEAR question");
    if (!linearSupported) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    *p={{16384,16384,1},linearMaxMips,2048,VK_SAMPLE_COUNT_1_BIT,1ull<<32}; return VK_SUCCESS;
}
}
namespace {
// Engine.
const VkFormat viewFormats[]={VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_R8G8B8A8_SRGB};
const VkImageFormatListCreateInfo formatList{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,nullptr,2,viewFormats};
VkFormat describedFormat=VK_FORMAT_R8G8B8A8_UNORM;
VkImageUsageFlags describedUsage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
unsigned imageWraps=0,bufferWraps=0,waits=0,releases=0; HRESULT waitResult=S_OK; ULONG remainingRefs=0;
int engineIdentity;
HRESULT describe_image(void *engine,const D3D11_TEXTURE2D_DESC1 *d,VkImageCreateInfo *i) {
    check(engine==&engineIdentity,"engine pointer");
    *i={VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; i->pNext=&formatList; i->flags=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    i->imageType=VK_IMAGE_TYPE_2D; i->format=describedFormat; i->extent={d->Width,d->Height,1};
    i->mipLevels=d->MipLevels; i->arrayLayers=d->ArraySize; i->samples=VK_SAMPLE_COUNT_1_BIT;
    i->tiling=VK_IMAGE_TILING_OPTIMAL; i->usage=describedUsage; return S_OK;
}
HRESULT wrap_image(void *,const D3D11_TEXTURE2D_DESC1 *d,const VkImageCreateInfo *info,VkImage image,ID3D11Texture2D **t) {
    ++imageWraps;
    check(d->MipLevels==4 && d->ArraySize==3 && !(d->MiscFlags&D3D11_RESOURCE_MISC_SHARED),"the description without sharing");
    check(info->tiling==VK_IMAGE_TILING_LINEAR && info->pNext==&formatList && image,"the image the shell made");
    *t=reinterpret_cast<ID3D11Texture2D *>(uintptr_t(0x90)); return S_OK;
}
HRESULT describe_buffer(void *,const D3D11_BUFFER_DESC *d,VkBufferCreateInfo *i) {
    *i={VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; i->size=(d->ByteWidth+255u)&~255u;
    i->usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    i->sharingMode=VK_SHARING_MODE_EXCLUSIVE; return S_OK;
}
HRESULT wrap_buffer(void *,const D3D11_BUFFER_DESC *d,const VkBufferCreateInfo *info,VkBuffer buffer,ID3D11Buffer **b) {
    ++bufferWraps; check(d->ByteWidth==1000 && info->size==1024 && buffer,"the buffer the shell made");
    *b=reinterpret_cast<ID3D11Buffer *>(uintptr_t(0x91)); return S_OK;
}
HRESULT wait(void *,ID3D11Resource *) { ++waits; return waitResult; }
ULONG release(ID3D11Resource *) { ++releases; return remainingRefs; }
}
namespace {
// Kernel callbacks of the creator's allocation.
int deviceIdentity,resourceIdentity; unsigned kernelAllocates=0;
BC250_SHARED_RESOURCE sentRecord{}; bc250_umd_alloc_private sentBlob{};
HRESULT APIENTRY kernel_allocate(HANDLE h,D3DDDICB_ALLOCATE *a) {
    ++kernelAllocates;
    check(h==&deviceIdentity && a->hResource==&resourceIdentity && a->NumAllocations==1,"one allocation of the resource");
    check(a->PrivateDriverDataSize==BC250_SHARED_RESOURCE_BYTES && a->pAllocationInfo2[0].PrivateDriverDataSize==192,
          "S26R record and BC2A blob");
    std::memcpy(&sentRecord,a->pPrivateDriverData,sizeof(sentRecord));
    std::memcpy(&sentBlob,a->pAllocationInfo2[0].pPrivateDriverData,sizeof(sentBlob));
    a->pAllocationInfo2[0].hAllocation=51; a->hKMResource=52; return S_OK;
}
HRESULT APIENTRY kernel_deallocate(HANDLE,const D3DDDICB_DEALLOCATE2 *) { return S_OK; }
}
int main() {
    static_assert(sizeof(BC250_SHARED_RESOURCE)==BC250_SHARED_RESOURCE_BYTES);
    static_assert(sizeof(bc250_umd_alloc_private)==192);
    // The OK reason is compared by its text: a copy of the string is OK too.
    const char okCopy[]="record";
    check(Bc250SharedResourceWhyOk(okCopy) && !Bc250SharedResourceWhyOk(BC250_SR_WHY_MAGIC) &&
          !Bc250SharedResourceWhyOk(nullptr) && !Bc250SharedResourceWhyOk("recorded"),"WhyOk compares text");

    // The D3D11 sharing rule of this form (Bc250SharedResourceDescWhy through engine_resource_desc_why).
    EngineResourceDesc texture{};
    texture.texture={64,32,4,3,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_DEFAULT,
        D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET,0,0,D3D11_TEXTURE_LAYOUT_UNDEFINED};
    BC250_SHARED_RESOURCE r{};
    check(Bc250SharedResourceWhyOk(engine_resource_desc_why(texture,r)),"a mip chain array is shareable");
    auto refused=[&](EngineResourceDesc d,const char *want) { BC250_SHARED_RESOURCE x{}; return is(engine_resource_desc_why(d,x),want); };
    { auto d=texture; d.texture.CPUAccessFlags=D3D11_CPU_ACCESS_READ; check(refused(d,BC250_SR_WHY_USAGE),"CPU access"); }
    { auto d=texture; d.texture.Usage=D3D11_USAGE_DYNAMIC; check(refused(d,BC250_SR_WHY_USAGE),"DYNAMIC"); }
    { auto d=texture; d.texture.SampleDesc={4,0}; check(refused(d,BC250_SR_WHY_SAMPLES),"MSAA"); }
    { auto d=texture; d.texture.BindFlags|=D3D11_BIND_DEPTH_STENCIL; check(refused(d,BC250_SR_WHY_BIND),"depth stencil"); }
    { auto d=texture; d.texture.MiscFlags=D3D11_RESOURCE_MISC_TEXTURECUBE; check(refused(d,BC250_SR_WHY_MISC),"cube"); }
    { auto d=texture; d.texture.MiscFlags=D3D11_RESOURCE_MISC_RESOURCE_CLAMP; check(refused(d,BC250_SR_WHY_MISC),"clamp"); }
    { auto d=texture; d.texture.MiscFlags=D3D11_RESOURCE_MISC_TILED; check(refused(d,BC250_SR_WHY_MISC),"tiled"); }
    { auto d=texture; d.texture.MiscFlags=D3D11_RESOURCE_MISC_GENERATE_MIPS; d.texture.BindFlags=D3D11_BIND_SHADER_RESOURCE;
      check(refused(d,BC250_SR_WHY_MISC),"GENERATE_MIPS without RT"); }
    { auto d=texture; d.texture.MiscFlags=D3D11_RESOURCE_MISC_GENERATE_MIPS; check(Bc250SharedResourceWhyOk(engine_resource_desc_why(d,r)),"GENERATE_MIPS with SRV+RT"); }
    { auto d=texture; d.texture.TextureLayout=D3D11_TEXTURE_LAYOUT_ROW_MAJOR; check(refused(d,BC250_SR_WHY_MISC),"a texture layout"); }
    { auto d=texture; d.texture.MipLevels=8; check(refused(d,BC250_SR_WHY_SUBRESOURCES),"more mips than the chain"); }
    { auto d=texture; d.texture.Format=DXGI_FORMAT_UNKNOWN; check(refused(d,BC250_SR_WHY_FORMAT),"no format"); }
    EngineResourceDesc buffer{}; buffer.buffer=true;
    buffer.buffer_desc={1000,D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS,0,
        D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS,0};
    check(Bc250SharedResourceWhyOk(engine_resource_desc_why(buffer,r)),"a raw buffer is shareable");
    { auto d=buffer; d.buffer_desc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE; check(refused(d,BC250_SR_WHY_USAGE),"buffer CPU access"); }
    { auto d=buffer; d.buffer_desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; d.buffer_desc.StructureByteStride=6;
      check(refused(d,BC250_SR_WHY_STRIDE),"a stride that is not 4-byte"); }
    { auto d=buffer; d.buffer_desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; d.buffer_desc.StructureByteStride=8;
      check(Bc250SharedResourceWhyOk(engine_resource_desc_why(d,r)),"a structured buffer"); }
    { auto d=buffer; d.buffer_desc.BindFlags=D3D11_BIND_DEPTH_STENCIL; check(refused(d,BC250_SR_WHY_BIND),"buffer DS bind"); }
    { auto d=buffer; d.buffer_desc.MiscFlags=D3D11_RESOURCE_MISC_GENERATE_MIPS; check(refused(d,BC250_SR_WHY_MISC),"buffer mips"); }

    // The record decode: exact length, magic and version.
    RuntimeDevice runtime; runtime.hDevice=&deviceIdentity;
    runtime.KTCallbacks.pfnAllocateCb=kernel_allocate; runtime.KTCallbacks.pfnDeallocate2Cb=kernel_deallocate;
    hostIdentity=runtime.host_identity();
    const VkDevice device=reinterpret_cast<VkDevice>(uintptr_t(1));
    ImageMemoryDispatch memory{image_requirements,allocate,bind_image,release_memory};
    RuntimeImageDispatch imageVk{image_create,image_destroy,image_layout,memory};
    EngineResourceDispatch dispatch{};
    dispatch.vk.image=&imageVk; dispatch.vk.create_buffer=buffer_create; dispatch.vk.destroy_buffer=buffer_destroy;
    dispatch.vk.buffer_requirements=buffer_requirements; dispatch.vk.bind_buffer=bind_buffer;
    dispatch.vk.image_format=image_format; dispatch.vk.physical=reinterpret_cast<VkPhysicalDevice>(uintptr_t(2));
    for (UINT8 i=0;i<16;++i) { dispatch.vk.identity.driver[i]=UINT8(i+1); dispatch.vk.identity.device[i]=UINT8(0x40+i); }
    dispatch.vk.identity.valid=true;
    dispatch.engine.engine=&engineIdentity; dispatch.engine.describe_image=describe_image; dispatch.engine.wrap_image=wrap_image;
    dispatch.engine.describe_buffer=describe_buffer; dispatch.engine.wrap_buffer=wrap_buffer;
    dispatch.engine.wait=wait; dispatch.engine.release=release;
    RuntimeEngineResource creator{}; const char *why=nullptr;
    check(prepare_engine_resource(runtime,device,dispatch,texture,creator,&why)==E_INVALIDARG && !imageCreates,
          "outside the device domain");
    RuntimeDomain::Scope scope(runtime.domain);

    // Creator: the engine's OPTIMAL create info becomes LINEAR, and the record holds all of it.
    check(prepare_engine_resource(runtime,device,dispatch,texture,creator,&why)==S_OK && Bc250SharedResourceWhyOk(why),
          "creator prepares");
    const auto &rec=creator.record;
    check(imageCreates==1 && createdImage.tiling==VK_IMAGE_TILING_LINEAR && createdImage.pNext==&formatList,"LINEAR image");
    check(rec.Magic==BC250_SHARED_RESOURCE_MAGIC && rec.Dimension==BC250_SHARED_RESOURCE_TEXTURE2D && rec.Width==64 &&
          rec.MipLevels==4 && rec.ArraySize==3 && rec.Tiling==BC250_SR_VK_TILING_LINEAR && rec.ViewFormatCount==2 &&
          rec.ViewFormats[1]==UINT(VK_FORMAT_R8G8B8A8_SRGB) && rec.CreateUsage==describedUsage &&
          rec.CreateFlags==UINT(VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) && rec.MemorySize==98304 && rec.MemoryAlignment==4096 &&
          rec.LayoutHash && rec.DriverUuid[0]==1 && rec.DeviceUuid[15]==0x4F,"the record");
    check(rec.Version==BC250_SHARED_RESOURCE_VERSION && rec.Version==2 && rec.CreatorApi==BC250_SR_CREATOR_D3D11,
          "version 2, written by the D3D11 shell");
    {
        // LayoutHash is the contract's hash step over the mock layout, layer-major then mip.
        UINT64 h=BC250_SR_FNV_BASIS;
        for (UINT layer=0;layer<3;++layer) for (UINT mip=0;mip<4;++mip) {
            const UINT64 pitch=256u>>mip;
            h=Bc250SharedResourceLayoutAdd(h,UINT64(layer)*32768u+UINT64(mip)*8192u,pitch<64 ? 64 : pitch,32768u,0);
        }
        check(rec.LayoutHash==h,"LayoutHash through Bc250SharedResourceLayoutAdd");
    }
    check(Bc250SharedResourceWhyOk(Bc250SharedResourceRecordWhy(&rec)),"the creator's record passes its own reader");
    BC250_SHARED_RESOURCE decoded{};
    check(Bc250SharedResourceWhyOk(Bc250SharedResourceDecodeWhy(&rec,sizeof(rec),&decoded)) && !std::memcmp(&decoded,&rec,sizeof(rec)),
          "decode is a copy");
    check(is(Bc250SharedResourceDecodeWhy(&rec,sizeof(rec)-1,&decoded),BC250_SR_WHY_LENGTH),"short record");
    { auto x=rec; x.Magic=0x52363245u; check(is(Bc250SharedResourceDecodeWhy(&x,sizeof(x),&decoded),BC250_SR_WHY_MAGIC),"E26R magic"); }
    { auto x=rec; x.Version=3; check(is(Bc250SharedResourceDecodeWhy(&x,sizeof(x),&decoded),BC250_SR_WHY_VERSION),"version 3"); }
    { auto x=rec; x.Tiling=0; check(is(Bc250SharedResourceRecordWhy(&x),BC250_SR_WHY_CREATE_INFO),"OPTIMAL is not this version"); }
    { auto x=rec; x.MemoryAlignment=1u<<17; check(is(Bc250SharedResourceRecordWhy(&x),BC250_SR_WHY_MEMORY),"alignment above 64 KiB"); }
    { auto x=rec; x.ViewFormats[5]=37; check(is(Bc250SharedResourceRecordWhy(&x),BC250_SR_WHY_VIEW_FORMATS),"a stray view format"); }
    { auto x=rec; x.CreatorApi=0; check(is(Bc250SharedResourceRecordWhy(&x),BC250_SR_WHY_CREATOR),"no creator in version 2"); }

    // The creator's allocation: S26R record per resource and BC2A blob per allocation, from the one writer.
    RuntimeSurfaceAllocation allocation{}; UINT64 bytes=0;
    check(allocate_engine_surface(runtime,&resourceIdentity,rec,allocation,bytes)==S_OK && allocation.allocation==51 &&
          allocation.kernel_resource==52 && bytes==98304 && kernelAllocates==1,"allocate");
    check(!std::memcmp(&sentRecord,&rec,sizeof(rec)),"the record as sent");
    check(sentBlob.magic==BC250_UMD_ALLOC_MAGIC && sentBlob.version==BC250_UMD_ALLOC_VERSION_CACHE_POLICY &&
          sentBlob.size==192 && sentBlob.flags==BC250_UMD_A_SHARED && sentBlob.alloc_size==98304 &&
          sentBlob.phys_alignment==4096 && sentBlob.va_size==98304 && sentBlob.preferred_heap==AMDGPU_GEM_DOMAIN_VRAM &&
          sentBlob.gem_flags==AMDGPU_GEM_CREATE_NO_CPU_ACCESS,"the BC2A blob");
    { RuntimeSurfaceAllocation again=allocation;
      check(allocate_engine_surface(runtime,&resourceIdentity,rec,again,bytes)==E_UNEXPECTED,"no second allocation"); }
    { auto x=rec; x.Magic=0; RuntimeSurfaceAllocation none{};
      check(allocate_engine_surface(runtime,&resourceIdentity,x,none,bytes)==E_INVALIDARG && kernelAllocates==1,"bad record"); }
    // The opener's check of the blob: the writer's exact bytes for the received record.
    check(engine_surface_blob(&sentBlob,sizeof(sentBlob),rec,bytes,&why)==S_OK && bytes==98304,"blob admitted");
    check(engine_surface_blob(&sentBlob,sizeof(sentBlob)-8,rec,bytes,&why)==E_INVALIDARG && is(why,BC250_SR_WHY_ALLOCATION_LENGTH),
          "blob length");
    { auto b=sentBlob; b.gem_flags=0; check(engine_surface_blob(&b,sizeof(b),rec,bytes,&why)==E_INVALIDARG &&
          is(why,BC250_SR_WHY_ALLOCATION_RECORD),"blob of a CPU-visible allocation"); }
    { auto x=rec; x.MemorySize=1u<<20; check(engine_surface_blob(&sentBlob,sizeof(sentBlob),x,bytes,&why)==E_INVALIDARG &&
          is(why,BC250_SR_WHY_ALLOCATION_RECORD),"blob of another size"); }

    // Opener: the same engine makes the same record, and the contract's rule admits it for the kernel allocation.
    const UINT64 allocBytes=98304;
    RuntimeEngineResource opener{};
    check(match_engine_resource(runtime,device,dispatch,rec,allocBytes,opener,&why)==S_OK && Bc250SharedResourceWhyOk(why) &&
          imageCreates==2 && !std::memcmp(&opener.record,&rec,sizeof(rec)),"the opener matches");
    destroy_engine_resource(device,dispatch,opener); check(imageDestroys==1 && !opener.image,"opener destroyed");
    // The adapter gives the rule's answer: a refusal is E_INVALIDARG with the rule's reason, never another layout,
    // and the prepared image is destroyed. The cases of the rule itself are in driver/contract/test.
    auto opened=[&](const BC250_SHARED_RESOURCE &received,UINT64 alloc,const char *want,const char *what) {
        RuntimeEngineResource o{}; const unsigned destroyed=imageDestroys;
        const HRESULT hr=match_engine_resource(runtime,device,dispatch,received,alloc,o,&why);
        if (Bc250SharedResourceWhyOk(want)) {
            check(hr==S_OK && Bc250SharedResourceWhyOk(why) && o.image,what);
            destroy_engine_resource(device,dispatch,o);
        } else {
            check(hr==E_INVALIDARG && is(why,want) && !o.image && imageDestroys==destroyed+1,what);
        }
    };
    layoutSalt=256; opened(rec,allocBytes,BC250_SR_WHY_LAYOUT,"another subresource layout"); layoutSalt=0;
    dispatch.vk.identity.device[3]^=1; opened(rec,allocBytes,BC250_SR_WHY_IDENTITY,"another device");
    dispatch.vk.identity.device[3]^=1;
    memoryRequirements.size=131072; opened(rec,allocBytes,BC250_SR_WHY_MEMORY,"memory above the allocation");
    opened(rec,131072,BC250_SR_WHY_OK,"memory inside a larger allocation"); memoryRequirements.size=98304;
    describedFormat=VK_FORMAT_B8G8R8A8_UNORM; opened(rec,allocBytes,BC250_SR_WHY_VK_FORMAT,"another VkFormat");
    describedFormat=VK_FORMAT_R8G8B8A8_UNORM;
    // What belongs to one engine's own image does not decide: another engine (vkd3d-proton for D3D12) gives other
    // usage, flags and view formats for the same layout.
    describedUsage|=VK_IMAGE_USAGE_STORAGE_BIT; opened(rec,allocBytes,BC250_SR_WHY_OK,"another engine usage");
    describedUsage&=~VkImageUsageFlags(VK_IMAGE_USAGE_STORAGE_BIT);
    { auto x=rec; x.ViewFormats[1]=UINT(VK_FORMAT_B8G8R8A8_UNORM); x.CreateFlags=0; x.CreatorApi=BC250_SR_CREATOR_D3D12;
      x.DriverUuid[0]^=0xFF; opened(x,allocBytes,BC250_SR_WHY_OK,"a D3D12 creator's view formats, flags and driver UUID"); }
    { auto x=rec; x.Version=BC250_SHARED_RESOURCE_VERSION_1; x.CreatorApi=BC250_SR_CREATOR_NONE;
      opened(x,allocBytes,BC250_SR_WHY_OK,"a version 1 record"); }
    { auto x=rec; x.Magic=0x52363245u; RuntimeEngineResource o{}; const unsigned created=imageCreates;
      check(match_engine_resource(runtime,device,dispatch,x,allocBytes,o,&why)==E_INVALIDARG && is(why,BC250_SR_WHY_MAGIC) &&
            imageCreates==created,"an E26R record is not this form"); }

    // Refusals of the creator, each before an image exists or with the image destroyed.
    {
        RuntimeEngineResource x{}; const unsigned created=imageCreates;
        describedFormat=VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        check(prepare_engine_resource(runtime,device,dispatch,texture,x,&why)==E_NOTIMPL && is(why,"planar format") &&
              imageCreates==created,"planar");
        describedFormat=VK_FORMAT_R8G8B8A8_UNORM;
        linearSupported=false;
        check(prepare_engine_resource(runtime,device,dispatch,texture,x,&why)==E_NOTIMPL &&
              is(why,"linear image not supported") && imageCreates==created,"no LINEAR format");
        linearSupported=true; linearMaxMips=1;
        check(prepare_engine_resource(runtime,device,dispatch,texture,x,&why)==E_NOTIMPL &&
              is(why,"linear image not supported") && imageCreates==created,"no LINEAR mip chain");
        linearMaxMips=15;
        const unsigned destroyed=imageDestroys; memoryRequirements.alignment=1u<<17;
        check(prepare_engine_resource(runtime,device,dispatch,texture,x,&why)==E_NOTIMPL && is(why,"memory alignment") &&
              imageDestroys==destroyed+1 && !x.image,"alignment above 64 KiB");
        memoryRequirements.alignment=4096;
        auto noIdentity=dispatch; noIdentity.vk.identity.valid=false;
        check(prepare_engine_resource(runtime,device,noIdentity,texture,x,&why)==E_INVALIDARG,"no device identity");
        auto cube=texture; cube.texture.MiscFlags=D3D11_RESOURCE_MISC_TEXTURECUBE; cube.texture.ArraySize=6;
        check(prepare_engine_resource(runtime,device,dispatch,cube,x,&why)==E_NOTIMPL && is(why,BC250_SR_WHY_MISC),"cube");
        check(prepare_engine_resource(runtime,device,dispatch,texture,creator,&why)==E_UNEXPECTED,"no second prepare");
    }

    // Bind and close of the creator's texture.
    VkPhysicalDeviceMemoryProperties properties{}; properties.memoryTypeCount=2;
    properties.memoryTypes[1].propertyFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    bc250_host_import imported{}; imported.sType=BC250_HOST_IMPORT_STYPE; imported.identity=hostIdentity;
    imported.allocation=51; imported.va=1u<<20; imported.size=bytes=98304;
    {
        auto shortAllocation=imported; shortAllocation.size=65536; RuntimeEngineResource x{};
        check(prepare_engine_resource(runtime,device,dispatch,texture,x,&why)==S_OK,"prepare for a small allocation");
        const unsigned destroyed=imageDestroys;
        check(bind_engine_resource(runtime,device,dispatch,properties,shortAllocation,x)==E_INVALIDARG && !x.image &&
              imageDestroys==destroyed+1 && !allocations,"an allocation below the requirement");
    }
    check(bind_engine_resource(runtime,device,dispatch,properties,imported,creator)==S_OK && creator.memory==kMemory &&
          imageBinds==1 && imageWraps==1 && creator.object,"bind the creator's texture");
    HostBridge bridge{}; bridge.device=&runtime;
    waitResult=E_FAIL;
    check(close_engine_resource(bridge,device,dispatch,creator)==E_FAIL && creator.object && !releases,"a failed wait keeps it");
    waitResult=S_OK; const unsigned destroyedBeforeClose=imageDestroys;
    check(close_engine_resource(bridge,device,dispatch,creator)==S_OK && !creator.object && !creator.image && !creator.memory &&
          releases==1 && waits==2 && frees==1 && imageDestroys==destroyedBeforeClose+1,"close");

    // Buffers: an engine below ABI 1.9 has no buffer entries, and the buffer is refused.
    {
        auto old=dispatch; old.engine.describe_buffer=nullptr; old.engine.wrap_buffer=nullptr;
        RuntimeEngineResource x{};
        check(prepare_engine_resource(runtime,device,old,buffer,x,&why)==E_NOTIMPL && is(why,"engine without shared buffers") &&
              !bufferCreates,"no shared buffers before ABI 1.9");
    }
    RuntimeEngineResource sharedBuffer{};
    check(prepare_engine_resource(runtime,device,dispatch,buffer,sharedBuffer,&why)==S_OK && bufferCreates==1 &&
          createdBuffer.size==1024,"prepare a buffer");
    const auto &brec=sharedBuffer.record;
    check(brec.Dimension==BC250_SHARED_RESOURCE_BUFFER && brec.Width==1000 && brec.BufferSize==1024 && !brec.LayoutHash &&
          !brec.Tiling && !brec.VkFormat && brec.CreateUsage==createdBuffer.usage &&
          Bc250SharedResourceWhyOk(Bc250SharedResourceRecordWhy(&brec)),"the buffer record");
    {
        RuntimeEngineResource o{};
        check(match_engine_resource(runtime,device,dispatch,brec,98304,o,&why)==S_OK && bufferCreates==2,
              "the buffer opener matches");
        destroy_engine_resource(device,dispatch,o); check(bufferDestroys==1,"opener buffer destroyed");
        // A buffer has no layout: another engine's buffer size is admitted when the opener's memory fits.
        auto x=brec; x.BufferSize=2048;
        check(match_engine_resource(runtime,device,dispatch,x,98304,o,&why)==S_OK && Bc250SharedResourceWhyOk(why),
              "another engine's buffer size");
        destroy_engine_resource(device,dispatch,o); check(bufferDestroys==2,"opener buffer destroyed again");
    }
    expectBufferMemory=true;
    check(bind_engine_resource(runtime,device,dispatch,properties,imported,sharedBuffer)==S_OK && bufferBinds==1 &&
          bufferWraps==1 && sharedBuffer.object,"bind the buffer with device addresses");
    check(close_engine_resource(bridge,device,dispatch,sharedBuffer)==S_OK && bufferDestroys==3 && frees==2,"close the buffer");
    expectBufferMemory=false;

    // An object that the engine keeps is retained: no destroy of memory it may still use.
    RuntimeEngineResource kept{};
    check(prepare_engine_resource(runtime,device,dispatch,texture,kept,&why)==S_OK &&
          bind_engine_resource(runtime,device,dispatch,properties,imported,kept)==S_OK,"bind a third texture");
    remainingRefs=1; const unsigned destroyedBeforeKept=imageDestroys;
    check(close_engine_resource(bridge,device,dispatch,kept)==E_UNEXPECTED && kept.retained && kept.image &&
          imageDestroys==destroyedBeforeKept,"retained");
    check(close_engine_resource(bridge,device,dispatch,kept)==E_UNEXPECTED,"retained stays");
    std::cout << "PASS engine layout resources: " << checks << " checks, sharing rule, record v2, BC2A blob, creator and "
        "opener through the contract's match rule, named refusals, LINEAR textures, buffers on ABI 1.9, bind and close (mock Vulkan and engine)\n";
    return 0;
}
