// SPDX-License-Identifier: MIT
#include "heap-import.h"
#include "device-state.h"
#include "allocation-request.h"
#include "ddi-trace.h"
#include <bc250_host_bootstrap.h>
#include <d3dkmthk.h>
#include <algorithm>
#include <new>
namespace native12 {
namespace {
HRESULT from_vk(VkResult r) noexcept {
    if(r==VK_SUCCESS)return S_OK;
    if(r==VK_ERROR_OUT_OF_HOST_MEMORY || r==VK_ERROR_OUT_OF_DEVICE_MEMORY)return E_OUTOFMEMORY;
    if(r==VK_ERROR_DEVICE_LOST)return DXGI_ERROR_DEVICE_REMOVED;
    return E_FAIL;
}
}
// The creating thread owns allocation, mapping and imported until the record is handed to the engine
// (retired false); after that only the releasing thread touches them. The flags below next are read and
// written under lock_, and so is the handle other threads look the record up by.
struct RuntimeHeapImports::Record {
    RuntimeAllocation allocation;
    GpuMapping mapping;
    engine_ddi::ImportedMemory imported{};
    Record* next{};
    // The quarantine's own fields, written once at the deposit (ImportReleasePolicy): the release's
    // position in the deposit order, the tick it was made at, what it holds, and the device-wide
    // progress it must outlive. Only drain() reads them.
    uint64_t deposit{},tick{},bytes{},gpu_va{};
    ProgressSnapshot progress{};
    D3DKMT_HANDLE handle{};                     // allocation.handle(), published once it exists
    bool retired{true},locked{},busy{};
    unsigned borrowed{};                        // runtime callbacks in flight that name it as backing
    bool surface{};                             // the allocation of a linear primary
    DWORD authority{};                          // the thread whose runtime resource DDI runs (0: none)
    bool destroyed{};                           // that DDI destroys the resource
    DWORD releasing{};                          // the thread inside release() (0: none)
    Record(D3D12DDI_HRTDEVICE d,const D3D12DDI_CORELAYER_DEVICECALLBACKS_0062& cb):allocation(d,cb) {}
};
namespace {
class Exclusive final {
    SRWLOCK& lock_;
public:
    explicit Exclusive(SRWLOCK& lock) noexcept:lock_(lock) {AcquireSRWLockExclusive(&lock_);}
    ~Exclusive() {ReleaseSRWLockExclusive(&lock_);}
    Exclusive(const Exclusive&)=delete;
    Exclusive& operator=(const Exclusive&)=delete;
};
}
// The quarantine's defaults, with their reasons (M15.8):
//   depth 3: trial 245 destroyed four 384 KiB imports inside 2.1 ms and the faulting job started 0.68 ms
//     after the unmap of the one it read, so the delay has to span a frame's worth of releases, not a
//     millisecond. Three further releases are about one frame of that game's release rate and cost one
//     frame of memory.
//   count 64 and 64 MiB: the imports the kernel journal saw in 245 were 64 KiB to 9.5 MiB, most of them
//     384 KiB or less; 64 entries of that size is about 25 MiB, and the byte cap bounds the outliers. Both
//     only shorten the delay (the progress condition still holds), so they are a memory bound, not a gate.
//   age 250 ms: the same bound the retire hand-off uses (device-engine.cpp), so that an application that
//     stops allocating and freeing does not leave the last entries held until its device goes.
constexpr ImportReleasePolicy kQuarantineDefaults{3,64,250,64ull<<20,true};
ImportReleasePolicy ImportReleasePolicy::from_switches() noexcept {
    ImportReleasePolicy policy=kQuarantineDefaults;
    if(ddi_experiment("import-progress-gate-off"))policy.progress_gate=false;
    if(ddi_experiment("import-quarantine-off")){
        policy.quarantine_depth=0;policy.quarantine_count_cap=0;policy.quarantine_age_ms=0;policy.quarantine_byte_cap=0;
    }
    return policy;
}
RuntimeHeapImports::RuntimeHeapImports(Device& d,bc250::umd::RuntimeDomain& domain,
    VkPhysicalDevice physical,VkDevice device,VkInstance instance,PFN_vkGetInstanceProcAddr gipa,void* identity,
    const ImportReleasePolicy& policy) noexcept
    :runtime_(d.runtime),callbacks_(d.callbacks),kernel_(d.kernel_callbacks),domain_(domain),
     paging_(d.runtime,d.kernel_callbacks),physical_(physical),device_(device),instance_(instance),gipa_(gipa),identity_(identity),
     policy_(policy) {}
RuntimeHeapImports::~RuntimeHeapImports(){discard_metadata();}
uint32_t RuntimeHeapImports::held_count() const noexcept {
    AcquireSRWLockShared(&lock_);const uint32_t count=held_count_;ReleaseSRWLockShared(&lock_);return count;
}
uint64_t RuntimeHeapImports::held_bytes() const noexcept {
    AcquireSRWLockShared(&lock_);const uint64_t bytes=held_bytes_;ReleaseSRWLockShared(&lock_);return bytes;
}
HRESULT RuntimeHeapImports::initialize() noexcept {
    if(!active_.load() || !domain_.entered())return E_UNEXPECTED;
    if(initialized_)return S_OK;
    if(!runtime_.handle || !physical_ || !device_ || !instance_ || !gipa_ || !identity_)return E_INVALIDARG;
    auto props=reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(gipa_(instance_,"vkGetPhysicalDeviceMemoryProperties"));
    auto gdpa=reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa_(instance_,"vkGetDeviceProcAddr"));
    if(!props || !gdpa)return E_NOINTERFACE;
    allocate_=reinterpret_cast<PFN_vkAllocateMemory>(gdpa(device_,"vkAllocateMemory"));
    free_=reinterpret_cast<PFN_vkFreeMemory>(gdpa(device_,"vkFreeMemory"));
    create_buffer_=reinterpret_cast<PFN_vkCreateBuffer>(gdpa(device_,"vkCreateBuffer"));
    destroy_buffer_=reinterpret_cast<PFN_vkDestroyBuffer>(gdpa(device_,"vkDestroyBuffer"));
    requirements_=reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(gdpa(device_,"vkGetBufferMemoryRequirements"));
    if(!allocate_ || !free_ || !create_buffer_ || !destroy_buffer_ || !requirements_)return E_NOINTERFACE;
    props(physical_,&properties_);
    if(!properties_.memoryTypeCount || properties_.memoryTypeCount>VK_MAX_MEMORY_TYPES)return E_UNEXPECTED;
    initialized_=true;return S_OK;
}
RuntimeHeapImports::Record* RuntimeHeapImports::find(D3DKMT_HANDLE handle) const noexcept {
    if(!handle)return nullptr;
    for(auto r=records_;r;r=r->next)if(r->handle==handle)return r;
    return nullptr;
}
bool RuntimeHeapImports::owns_allocation(D3DKMT_HANDLE handle) const noexcept {
    if(!active_.load() || !domain_.entered())return false;
    AcquireSRWLockShared(&lock_);
    const bool owned=find(handle)!=nullptr;
    ReleaseSRWLockShared(&lock_);
    return owned;
}
bool RuntimeHeapImports::borrow_backing(D3DKMT_HANDLE handle) noexcept {
    if(!active_.load() || !domain_.entered())return false;
    Exclusive held(lock_);
    auto record=find(handle);
    if(!record || record->retired || record->busy || record->surface || !record->imported.memory)return false;
    ++record->borrowed;return true;
}
void RuntimeHeapImports::return_backing(D3DKMT_HANDLE handle) noexcept {
    Exclusive held(lock_);
    if(auto record=find(handle);record && record->borrowed)--record->borrowed;
}
void RuntimeHeapImports::erase(Record* record) noexcept {
    bool found=false;
    {
        Exclusive held(lock_);
        auto link=&records_;while(*link && *link!=record)link=&(*link)->next;
        if(*link){*link=record->next;found=true;}
    }
    if(found)delete record;
}
void RuntimeHeapImports::detach(Record* record) noexcept {
    auto link=&records_;while(*link && *link!=record)link=&(*link)->next;
    if(*link)*link=record->next;
    record->next=nullptr;
}
// Into the quarantine, oldest first, under lock_. The record is already out of records_ and its Vulkan
// import is gone; what it still holds is its GPU address and its runtime allocation.
void RuntimeHeapImports::deposit(Record* record) noexcept {
    record->next=nullptr;
    record->deposit=++deposits_;
    record->tick=GetTickCount64();
    record->bytes=record->imported.byte_size;
    record->gpu_va=record->imported.gpu_va;
    if(held_tail_)held_tail_->next=record;else held_=record;
    held_tail_=record;
    ++held_count_;held_bytes_+=record->bytes;
}
// Releases what the policy admits, oldest first; with `all` everything, whatever the progress says (device
// teardown, where the engine has released every object and destroyed every queue). One thread drains at a
// time: a free() that finds a drain in flight leaves the work to it. No lock of this module is held across
// the progress source or a runtime callback.
void RuntimeHeapImports::drain(bool all) noexcept {
    {
        Exclusive held(lock_);
        if(draining_ || !held_)return;
        draining_=true;
    }
    unsigned released=0;
    for(;;){
        Record* front=nullptr;ProgressSnapshot progress{};bool gated=false;
        {
            Exclusive held(lock_);
            front=held_;
            if(!front)break;
            if(!all){
                const uint64_t now=GetTickCount64();
                const bool delayed=front->deposit+policy_.quarantine_depth<=deposits_;
                const bool aged=policy_.quarantine_age_ms && now>=front->tick &&
                    now-front->tick>=policy_.quarantine_age_ms;
                const bool over=(policy_.quarantine_count_cap && held_count_>policy_.quarantine_count_cap) ||
                    (policy_.quarantine_byte_cap && held_bytes_>policy_.quarantine_byte_cap);
                if(!delayed && !aged && !over)break;
            }
            gated=policy_.progress_gate && progress_.usable();
            progress=front->progress;
        }
        if(gated){
            // Taken again when the earlier snapshot could not name every unretired fence.
            if(!progress.complete){
                progress_.snapshot(progress_.owner,&progress);
                Exclusive held(lock_);
                if(held_!=front)continue;        // another drain finished it; start again
                front->progress=progress;
            }
            if(!progress_.retired(progress_.owner,&progress)){
                if(!all)break;
                forced_.fetch_add(1,std::memory_order_relaxed);
            }
        }
        {
            Exclusive held(lock_);
            if(held_!=front)continue;
            held_=front->next;if(!held_)held_tail_=nullptr;
            --held_count_;held_bytes_-=front->bytes;
        }
        front->next=nullptr;
        const HRESULT hr=release_owned(*front);
        ++released;
        if(hr!=S_OK){
            // The mapping or the deallocation refused. The record keeps what it holds and is counted by
            // discard_metadata; it never goes back into the quarantine, where a failed release would be
            // retried for ever.
            Exclusive held(lock_);
            front->next=records_;records_=front;front->releasing=0;
        } else delete front;
    }
    Exclusive held(lock_);
    draining_=false;
    free_report_.released=released;
    free_report_.held_count=held_count_;
    free_report_.held_bytes=held_bytes_;
}
// The record is retired and not in a callback. The calling thread takes it for the release (free()
// may have done so already, under the same hold that retired it); on failure it gives it back, on
// success the caller erases it.
HRESULT RuntimeHeapImports::release(Record& record) noexcept {
    const DWORD self=GetCurrentThreadId();
    VkDeviceMemory memory=VK_NULL_HANDLE;
    {
        Exclusive held(lock_);
        free_report_.surface=record.surface;
        if(!record.retired || record.busy || (record.releasing && record.releasing!=self))return E_UNEXPECTED;
        record.releasing=self;memory=record.imported.memory;record.imported.memory=VK_NULL_HANDLE;
    }
    HRESULT hr=release_import(record,memory);
    if(hr==S_OK)hr=release_owned(record);
    if(hr!=S_OK){Exclusive held(lock_);record.releasing=0;}
    return hr;
}
// The Vulkan import alone. It runs at every release, quarantined or not: the engine has released its heap
// and expects the import gone, and the memory it names stays mapped and allocated either way.
HRESULT RuntimeHeapImports::release_import(Record& record,VkDeviceMemory memory) noexcept {
    // Engine objects and uses have retired before free() reaches this point; a record of a failed
    // construction was never used. Every release below therefore carries the proof of retirement.
    // RADV may call Unlock2 from vkFreeMemory, on this thread; keep the allocation record live.
    free_report_.stage=FreeStage::VulkanFree;
    if(memory)free_(device_,memory,nullptr);
    return unlock_for_release(record);
}
// BD-045: a CPU lock belongs to whoever releases the allocation. RADV unlocks a mapped import in
// vkFreeMemory; when its Unlock2 failed, the ICD gave its pointer up and the lock is still on the record.
// The releasing thread unlocks it itself, by dispatch()'s protocol. If that fails too, the record keeps
// the lock and stays retired and owned, and the next release point (close_after_engine_retirement) tries
// again through release().
HRESULT RuntimeHeapImports::unlock_for_release(Record& record) noexcept {
    const DWORD self=GetCurrentThreadId();
    {
        Exclusive held(lock_);
        if(!record.locked)return S_OK;
        if(record.busy || (record.releasing && record.releasing!=self) || !kernel_.pfnUnlock2Cb)return E_UNEXPECTED;
        record.busy=true;
    }
    free_report_.stage=FreeStage::Unlock;
    D3DDDICB_UNLOCK2 b{};b.hAllocation=record.handle;
    const HRESULT hr=kernel_.pfnUnlock2Cb(runtime_.handle,&b);
    Exclusive held(lock_);record.busy=false;
    if(hr!=S_OK)return FAILED(hr)?hr:E_FAIL;
    record.locked=false;free_report_.unlocked=true;
    return S_OK;
}
HRESULT RuntimeHeapImports::release_owned(Record& record) noexcept {
    free_report_.stage=FreeStage::Unmap;
    HRESULT hr=paging_.unmap_after_gpu_retirement(record.mapping);
    if(hr!=S_OK)return hr;
    free_report_.stage=FreeStage::Deallocate;
    if(record.surface){
        // By the runtime resource, inside that resource's own DDI and on its thread, or not at all:
        // neither a saved owner in a later DDI nor the other form.
        bool authorized=false;
        {Exclusive held(lock_);authorized=record.authority==GetCurrentThreadId() && record.allocation.owner_known();}
        if(!authorized){free_report_.owner_expired=true;return kOwnerExpired;}
        hr=record.allocation.close(ReleaseForm::Owner,Retirement::Retired);
    } else hr=record.allocation.close(ReleaseForm::Handle,Retirement::Retired);
    if(hr==S_OK)free_report_.stage=FreeStage::Done;
    return hr;
}
bool RuntimeHeapImports::begin_owner_scope(D3DKMT_HANDLE destroyed) noexcept {
    if(!active_.load() || scope_owner_)return false;
    scope_owner_=this;
    const DWORD self=GetCurrentThreadId();
    Exclusive held(lock_);
    // Another thread's authority over the same record is never taken over.
    if(auto record=find(destroyed);record && record->surface && record->allocation.owner_known() &&
       (!record->authority || record->authority==self)){
        record->authority=self;record->destroyed=true;
    }
    return true;
}
void RuntimeHeapImports::end_owner_scope() noexcept {
    if(scope_owner_!=this)return;
    scope_owner_=nullptr;
    const DWORD self=GetCurrentThreadId();
    Exclusive held(lock_);
    for(auto record=records_;record;record=record->next){
        if(record->authority!=self)continue;
        record->authority=0;
        // A record still here after its resource's destruction, or after a creation that handed
        // nothing to the engine, has no runtime resource any more.
        if(record->destroyed || record->retired)record->allocation.revoke_owner();
    }
}
HRESULT RuntimeHeapImports::allocate(const engine_ddi::MemoryRequest* request,engine_ddi::ImportedMemory* out) noexcept {
    if(out)*out={};
    report_={};report_.stage=ImportStage::Request;
    if(!active_.load() || !domain_.entered() || !initialized_)return E_UNEXPECTED;
    // A retirement point before the memory is asked for, as engine-ddi's resource DDIs are: an application
    // that destroys and then creates at its budget gets the quarantined addresses back first.
    drain(false);
    if(!request || !out || request->size!=sizeof(*request) || !request->heap || request->reserved || request->reserved2 ||
       !request->byte_size || !request->alignment || (request->alignment&(request->alignment-1)))return E_INVALIDARG;
    const auto& heap=*request->heap;
    // The linear primary is the one request with more than kMemoryDedicated: all three flags, on a
    // heap that says PRIMARY. A primary that engine-ddi could not make linear has no surface here.
    constexpr uint32_t surface_flags=engine_ddi::kMemoryDedicated|engine_ddi::kMemoryPrimary|engine_ddi::kMemoryLinearSurface;
    const bool surface=request->flags==surface_flags;
    unsigned allowed=D3D12DDI_HEAP_FLAG_BUFFERS|D3D12DDI_HEAP_FLAG_NON_RT_DS_TEXTURES|D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES|
        D3D12DDI_HEAP_FLAG_COHERENT_SYSTEMWIDE;
    if(surface)allowed|=D3D12DDI_HEAP_FLAG_PRIMARY;
    if((!surface && (request->flags&~engine_ddi::kMemoryDedicated)) || (unsigned(heap.Flags)&~allowed) ||
       heap.CreationNodeMask>1 || heap.VisibleNodeMask>1)return E_NOTIMPL;
    if(!surface && (request->surface_row_pitch || request->surface_layout_size))return E_INVALIDARG;
    D3DDDIFORMAT surface_format=D3DDDIFMT_UNKNOWN;
    bool surface_scanout=false;
    if(surface){
        const auto* r=request->resource;
        if(!(heap.Flags&D3D12DDI_HEAP_FLAG_PRIMARY) || (heap.Flags&D3D12DDI_HEAP_FLAG_COHERENT_SYSTEMWIDE) ||
           heap.CPUPageProperty!=D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE || !r ||
           r->ResourceType!=D3D12DDI_RT_TEXTURE2D || r->DepthOrArraySize!=1 || r->MipLevels!=1 ||
           r->SampleDesc.Count!=1 || r->SampleDesc.Quality || r->Width>UINT32_MAX)return E_NOTIMPL;
        // The storage formats the compositor may open, from the one table the kernel driver and the
        // compositor's UMD read as well. M15.14: when the scan-out mode is selected and this format is
        // also a SCANOUT_PRIMARY row - BGRA8 or X8, never the 10-bit or FP16 composed primaries - the
        // buffer is admitted as a scan-out primary instead, so the display pipeline can read it
        // directly. The mode is off by default; the kernel driver decides whether any flip of the
        // surface is admitted, against the POST geometry and the segment the allocation landed in.
        const auto* composed=amdgpu_wddm_surface_admit(amdgpu_wddm_surface_format_by_dxgi(unsigned(r->Format)),
                                                       AMDGPU_WDDM_SURFACE_COMPOSED);
        const auto* row=composed;
        // present-cached and present-noprimary describe the opposite intent for the same buffer, so
        // scan-out stands down rather than failing the allocation when either is also listed. The mode
        // names its geometry ("scanout-flip-1920x1200") and only a chain of exactly that size asks:
        // the kernel driver admits a flip at the POST geometry alone, so for any other chain the
        // request could only end in a refusal - after this buffer had been moved into VRAM with no CPU
        // mapping, which is a measured regression for a surface that was never eligible.
        unsigned mode_width=0,mode_height=0;
        if(ddi_experiment_scanout(&mode_width,&mode_height) && !ddi_experiment("present-cached") &&
           !ddi_experiment("present-noprimary") &&
           r->Width==mode_width && r->Height==mode_height){
            const auto* direct=amdgpu_wddm_surface_admit(amdgpu_wddm_surface_format_by_dxgi(unsigned(r->Format)),
                                                         AMDGPU_WDDM_SURFACE_SCANOUT_PRIMARY);
            if(direct){row=direct;surface_scanout=true;}
        }
        if(!row)return E_NOTIMPL;
        surface_format=static_cast<D3DDDIFORMAT>(row->d3dddi);
        if(!request->memory_type_bits || !request->surface_row_pitch || !request->surface_layout_size ||
           request->surface_layout_size>request->byte_size)return E_INVALIDARG;
    }
    // The allocation is raw memory; the engine places the buffer or image in it. A texture is admitted
    // only as the one resource of a heap without CPU access: CPU-visible texture layouts, primaries and
    // texture-only heaps for later placement each need their own contract.
    bool texture=false;
    if(request->resource)switch(request->resource->ResourceType){
    case D3D12DDI_RT_BUFFER:break;
    case D3D12DDI_RT_TEXTURE1D:case D3D12DDI_RT_TEXTURE2D:case D3D12DDI_RT_TEXTURE3D:texture=true;break;
    default:return E_NOTIMPL;
    }
    constexpr unsigned textures=D3D12DDI_HEAP_FLAG_NON_RT_DS_TEXTURES|D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES;
    if(texture){
        if(!(request->flags&engine_ddi::kMemoryDedicated) || !(unsigned(heap.Flags)&(textures|D3D12DDI_HEAP_FLAG_BUFFERS)) ||
           heap.CPUPageProperty!=D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE ||
           (heap.Flags&D3D12DDI_HEAP_FLAG_COHERENT_SYSTEMWIDE))return E_NOTIMPL;
    } else if(!(heap.Flags&D3D12DDI_HEAP_FLAG_BUFFERS))return E_NOTIMPL;
    AllocationAccess access{};VkMemoryPropertyFlags want{};
    switch(heap.CPUPageProperty){
    case D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE:access=AllocationAccess::GpuOnly;want=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;break;
    case D3D12DDI_CPU_PAGE_PROPERTY_WRITE_COMBINE:access=AllocationAccess::CpuWriteCombined;want=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;break;
    case D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK:access=AllocationAccess::CpuCached;want=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT;break;
    default:return E_NOTIMPL;
    }
    if(heap.Flags&D3D12DDI_HEAP_FLAG_COHERENT_SYSTEMWIDE){
        // The admitted adapter supplies coherent GTT CPU mappings. Limit this
        // promise to CPU-visible system memory and require the same property
        // on the Vulkan type importing that runtime allocation.
        if(heap.MemoryPool!=D3D12DDI_MEMORY_POOL_L0 || access==AllocationAccess::GpuOnly)return E_NOTIMPL;
        want|=VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }
    AllocationRequest allocation;HRESULT hr=S_OK;uint64_t alignment=0;uint32_t bits=0;
    if(surface){
        report_.stage=ImportStage::Surface;
        // The image's own requirements, from engine-ddi: no buffer is ever bound to this memory, and
        // the address has to suit the image alone. The description is refused here, before any callback.
        bits=request->memory_type_bits;alignment=std::max<uint64_t>(4096,request->alignment);
        hr=allocation.prepare_surface(static_cast<uint32_t>(request->resource->Width),request->resource->Height,
            request->surface_row_pitch,surface_format,request->byte_size,request->rt_owner.handle,
            ddi_experiment("present-cached"),!ddi_experiment("present-noprimary"),surface_scanout);
        if(FAILED(hr))return hr;
    } else {
    report_.stage=ImportStage::Probe;
    VkBufferCreateInfo probe_info{};probe_info.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    probe_info.size=request->byte_size;probe_info.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;probe_info.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer probe{};hr=from_vk(create_buffer_(device_,&probe_info,nullptr,&probe));if(FAILED(hr))return hr;
    if(!probe)return E_UNEXPECTED;
    VkMemoryRequirements needs{};requirements_(device_,probe,&needs);destroy_buffer_(device_,probe,nullptr);
    bits=needs.memoryTypeBits;if(request->memory_type_bits)bits&=request->memory_type_bits;
    alignment=std::max<uint64_t>(65536,std::max<uint64_t>(request->alignment,needs.alignment));
    hr=allocation.prepare(std::max<uint64_t>(request->byte_size,needs.size),alignment,access,request->rt_owner.handle);
    if(FAILED(hr))return hr;
    }
    report_.stage=ImportStage::MemoryType;report_.bytes=allocation.held;report_.alignment=alignment;
    uint32_t type=UINT32_MAX;
    for(uint32_t i=0;i<properties_.memoryTypeCount;++i)if((bits&(1u<<i)) &&
        (properties_.memoryTypes[i].propertyFlags&want)==want){type=i;break;}
    if(type==UINT32_MAX)return E_INVALIDARG;
    report_.memory_type=type;report_.stage=ImportStage::PagingQueue;
    if(!paging_open_.load(std::memory_order_acquire)){
        // Once, as before: a failed creation is not retried, and the calls after it fail at the map.
        // A thread that finds the creation under way waits for it here.
        HRESULT opened=S_OK;
        AcquireSRWLockExclusive(&paging_open_lock_);
        if(!paging_open_.load(std::memory_order_relaxed)){opened=paging_.open();paging_open_.store(true,std::memory_order_release);}
        ReleaseSRWLockExclusive(&paging_open_lock_);
        if(opened!=S_OK)return opened;
    }
    auto record=new(std::nothrow) Record(runtime_,callbacks_);if(!record)return E_OUTOFMEMORY;
    record->busy=true;record->surface=surface;
    record->authority=surface && scope_owner_==this?GetCurrentThreadId():0;
    {Exclusive held(lock_);record->next=records_;records_=record;}
    report_.stage=ImportStage::AllocateCallback;
    hr=record->allocation.open(allocation.args);
    {Exclusive held(lock_);record->busy=false;if(hr==S_OK)record->handle=record->allocation.handle();}
    if(hr==S_OK){report_.stage=ImportStage::Map;hr=paging_.map(record->allocation.handle(),allocation.held,record->mapping);}
    // The runtime makes a new heap resident only after this DDI returns, on its own paging queue,
    // and makes only its queues' contexts wait for that. The engine also submits on contexts the
    // runtime never sees, so its VA must not leave here before the allocation is resident and its
    // page table entries are written: trial 153 faulted on exactly that window (REPORT-153).
    if(hr==S_OK){report_.stage=ImportStage::Resident;hr=paging_.make_resident(record->mapping);}
    UINT64 address=0;
    if(hr==S_OK){report_.stage=ImportStage::MapReady;hr=paging_.wait_ready(record->mapping,&address);}
    else if(report_.stage==ImportStage::Resident){
        // The mapping itself was accepted: let it complete so that the release below can free it.
        UINT64 ignored=0;(void)paging_.wait_ready(record->mapping,&ignored);
    }
    if(hr==S_OK){report_.stage=ImportStage::AddressAlignment;report_.address=address;if(address&(alignment-1))hr=E_INVALIDARG;}
    if(hr==S_OK){
        report_.stage=ImportStage::Import;
        bc250_host_import host{};host.sType=BC250_HOST_IMPORT_FLAGS_STYPE;host.identity=identity_;
        host.allocation=record->allocation.handle();host.va=address;host.size=allocation.held;
        // The engine maps a CPU-visible heap. dispatch() answers the ICD's Lock2 and Unlock2 for it.
        if(access!=AllocationAccess::GpuOnly)host.flags=BC250_HOST_IMPORT_CPU_MAP;
        VkMemoryAllocateFlagsInfo flags{};flags.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
        flags.flags=VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;flags.pNext=&host;
        VkMemoryAllocateInfo info{};info.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;info.pNext=&flags;
        info.allocationSize=host.size;info.memoryTypeIndex=type;
        VkDeviceMemory imported=VK_NULL_HANDLE;
        hr=from_vk(allocate_(device_,&info,nullptr,&imported));
        if(hr==S_OK && !imported)hr=E_UNEXPECTED;
        if(hr==S_OK){
            Exclusive held(lock_);
            record->imported={sizeof(engine_ddi::ImportedMemory),type,imported,host.size,host.allocation,0,address,record};
            record->retired=false;*out=record->imported;
            report_.stage=ImportStage::Done;return S_OK;
        }
    }
    // Failed construction transfers nothing to engine-ddi. Pending mapping and
    // cleanup failures remain owned for an explicit later close, never forgotten.
    if(release(*record)==S_OK)erase(record);
    return hr;
}
HRESULT RuntimeHeapImports::free(const engine_ddi::ImportedMemory* memory) noexcept {
    free_report_={};free_report_.stage=FreeStage::Request;
    if(!active_.load() || !domain_.entered())return E_UNEXPECTED;
    if(!memory || memory->size!=sizeof(*memory))return E_INVALIDARG;
    free_report_.stage=FreeStage::Record;
    free_report_.gpu_va=memory->gpu_va;free_report_.byte_size=memory->byte_size;
    Record* record=nullptr;VkDeviceMemory imported=VK_NULL_HANDLE;bool quarantine=false;
    {
        Exclusive held(lock_);
        record=find(memory->allocation);
        if(!record || record!=memory->cookie || record->retired || record->imported.memory!=memory->memory ||
           record->imported.byte_size!=memory->byte_size || record->imported.gpu_va!=memory->gpu_va)return E_INVALIDARG;
        // A runtime callback that names this import as backing, or its own Lock2/Unlock2 on another
        // thread, is in flight: retiring it now would leave it taken for a release that release() refuses.
        if(record->borrowed || record->busy)return E_PENDING;
        // Retired and taken for the release in one hold: no other thread's call starts on it in between.
        record->retired=true;record->releasing=GetCurrentThreadId();
        free_report_.surface=record->surface;
        // A linear primary is never quarantined: only its own runtime resource may release it, inside the
        // destroy that ends it (release_owned), which a later drain is no longer inside.
        quarantine=policy_.holds() && !record->surface;
        if(quarantine){imported=record->imported.memory;record->imported.memory=VK_NULL_HANDLE;}
    }
    if(!quarantine){
        HRESULT hr=release(*record);if(hr==S_OK)erase(record);return hr;
    }
    HRESULT hr=release_import(*record,imported);
    if(hr!=S_OK){Exclusive held(lock_);record->releasing=0;return hr;}
    // The snapshot is taken before the record is held, so that it covers every submission of every context
    // that reached the kernel before this release (device-progress.h). No lock of this module is held.
    ProgressSnapshot progress{};
    if(policy_.progress_gate && progress_.usable())progress_.snapshot(progress_.owner,&progress);
    {
        Exclusive held(lock_);
        record->progress=progress;
        detach(record);
        deposit(record);
    }
    drain(false);
    // The stage of this call is where its own import ended: held. What the drain did is in the counters.
    free_report_.stage=FreeStage::Quarantined;
    free_report_.held_count=held_count();free_report_.held_bytes=held_bytes();
    return S_OK;
}
HRESULT RuntimeHeapImports::dispatch(uint32_t op,void* argument) noexcept {
    if(!active_.load() || !domain_.entered())return E_UNEXPECTED;
    if(!argument)return E_INVALIDARG;
    const DWORD self=GetCurrentThreadId();
    if(op==BC250_HOST_Lock2){
        auto& a=*static_cast<D3DKMT_LOCK2*>(argument);
        Record* record=nullptr;
        {
            Exclusive held(lock_);
            record=find(a.hAllocation);
            if(!record || record->busy || record->locked || (record->releasing && record->releasing!=self) ||
               !kernel_.pfnLock2Cb)return E_INVALIDARG;
            record->busy=true;
        }
        D3DDDICB_LOCK2 b{};b.hAllocation=a.hAllocation;b.Flags.Value=a.Flags.Value;
        HRESULT hr=kernel_.pfnLock2Cb(runtime_.handle,&b);
        {Exclusive held(lock_);record->busy=false;if(hr==S_OK)record->locked=true;}
        if(hr==S_OK){a.pData=b.pData;if(!b.pData)return E_UNEXPECTED;}return hr;
    }
    if(op==BC250_HOST_Unlock2){
        auto& a=*static_cast<D3DKMT_UNLOCK2*>(argument);
        Record* record=nullptr;
        {
            Exclusive held(lock_);
            record=find(a.hAllocation);
            if(!record || record->busy || !record->locked || (record->releasing && record->releasing!=self) ||
               !kernel_.pfnUnlock2Cb)return E_INVALIDARG;
            record->busy=true;
        }
        D3DDDICB_UNLOCK2 b{};b.hAllocation=a.hAllocation;
        HRESULT hr=kernel_.pfnUnlock2Cb(runtime_.handle,&b);
        Exclusive held(lock_);record->busy=false;
        if(hr==S_OK)record->locked=false;return hr;
    }
    return E_NOTIMPL;
}
// Device teardown: the runtime has ended every other entry of this device, so the walk below and the
// paging queue's destruction meet no other thread. The record hold is still taken where flags are read.
HRESULT RuntimeHeapImports::close_after_engine_retirement() noexcept {
    if(!active_.load() || !domain_.entered())return E_UNEXPECTED;
    // The engine has released every object and destroyed every queue by now, so the quarantine's delay has
    // nothing left to protect: it is drained in full. forced_releases() counts those whose device progress
    // still read unretired here.
    drain(true);
    HRESULT result=S_OK;
    Record* record=nullptr;
    {Exclusive held(lock_);record=records_;}
    while(record){
        Record* next=nullptr;bool retired=false;
        {Exclusive held(lock_);next=record->next;retired=record->retired;}
        if(!retired)result=E_PENDING;
        else {HRESULT hr=release(*record);if(hr==S_OK)erase(record);else result=hr;}
        record=next;
    }
    bool remaining=false;
    {Exclusive held(lock_);remaining=records_!=nullptr;}
    if(remaining)return result==S_OK?E_PENDING:result;
    HRESULT hr=paging_.close();if(hr==S_OK)paging_open_.store(false);return hr;
}
unsigned RuntimeHeapImports::discard_metadata() noexcept {
    active_.store(false);unsigned count=paging_open_.exchange(false)?1u:0u;
    paging_.invalidate_runtime();runtime_={};callbacks_={};kernel_={};progress_={};
    Record* records=nullptr;
    {
        // A quarantined record still holds its GPU address and its runtime allocation, which this terminal
        // path never releases: it is counted with the others below.
        Exclusive held(lock_);
        if(held_tail_){held_tail_->next=records_;records_=held_;}
        held_=held_tail_=nullptr;held_count_=0;held_bytes_=0;
        records=records_;records_=nullptr;
    }
    while(records){auto record=records;records=record->next;
        if(record->allocation.handle() || record->imported.memory)++count;
        record->allocation.invalidate_runtime();delete record;}
    return count;
}
}
