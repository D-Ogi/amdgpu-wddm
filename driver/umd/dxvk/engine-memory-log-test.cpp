// SPDX-License-Identifier: MIT
// Host test of the shell's half of the engine's UMD logging (engine-memory-log.h, engine ABI 1.8, BD-373): the
// conversion of engine events to the provider's terms, and the ICD query behind Services.ResolveMemory.
#include "engine-memory-log.h"
#include <cstdio>
#include <cstdlib>
using namespace bc250::umd;
#define CHECK(x) do{if(!(x)){std::printf("FAIL line %d: %s\n",__LINE__,#x);std::abort();}}while(0)

static_assert(sizeof(BC250_DXVK_MEMORY_EVENT)==48,"the same layout in a 32-bit and a 64-bit process");

namespace {
// A non-dispatchable handle is a pointer in a 64-bit process and a uint64_t in a 32-bit one (vulkan_core.h).
VkDeviceMemory memory_handle(uint64_t value) {
#if defined(_WIN64)
    return reinterpret_cast<VkDeviceMemory>(uintptr_t(value));
#else
    return VkDeviceMemory(value);
#endif
}
VkDeviceMemory const known=memory_handle(0x1230);
VkDevice const device=reinterpret_cast<VkDevice>(uintptr_t(0x4560));
unsigned queries=0;
VkResult VKAPI_PTR fake_query(VkDevice d,VkDeviceMemory memory,MemoryAllocationInfo *info) {
    ++queries;
    CHECK(d==device && info && info->revision==kMemoryAllocationRevision && info->size==sizeof(MemoryAllocationInfo));
    if (memory!=known) return VK_ERROR_FEATURE_NOT_PRESENT;
    info->allocation=0x40000080; info->flags=kMemoryAllocationCpuMapped; info->bytes=0x200000;
    return VK_SUCCESS;
}
BC250_DXVK_MEMORY_EVENT event(UINT32 kind,UINT64 offset,UINT64 size,UINT32 flags) {
    return {kind,flags,0x9000,0x40000080,offset,size,0x200000};
}
}
int main() {
    {   // Packed when the range is not the whole allocation (umdprovider.h 27-40); Renamed from the engine's flag.
        etw::EventKind kind{}; etw::AllocationMapping m{};
        CHECK(EngineMemoryLog::convert(event(BC250_DXVK_MEMORY_EVENT_MAP,0,0x200000,0),kind,m));
        CHECK(kind==etw::EventKind::map && m.usage==etw::usage::none && m.d3d_allocation==0x9000 &&
              m.dxg_allocation==0x40000080 && m.offset==0 && m.size==0x200000 && m.semantic==etw::AllocationSemantic::none);
        CHECK(EngineMemoryLog::convert(event(BC250_DXVK_MEMORY_EVENT_UNMAP,0x100,0x100,0),kind,m));
        CHECK(kind==etw::EventKind::unmap && m.usage==etw::usage::packed && m.offset==0x100);
        CHECK(EngineMemoryLog::convert(event(BC250_DXVK_MEMORY_EVENT_RUNDOWN,0,0x1000,BC250_DXVK_MEMORY_RENAMED),kind,m));
        CHECK(kind==etw::EventKind::rundown && m.usage==(etw::usage::packed|etw::usage::renamed));
        CHECK(EngineMemoryLog::convert(event(BC250_DXVK_MEMORY_EVENT_MAP,0,0x200000,BC250_DXVK_MEMORY_RENAMED),kind,m));
        CHECK(m.usage==etw::usage::renamed);
        CHECK(!EngineMemoryLog::convert(event(7,0,1,0),kind,m));
    }
    {   // ResolveMemory: 0 without a query, for memory that is not one allocation, and for a null handle.
        EngineMemoryLog log;
        UINT64 bytes=1;
        CHECK(log.resolve(known,&bytes)==0 && bytes==0);
        log.bind(&fake_query,device);
        CHECK(log.resolve(known,&bytes)==0x40000080 && bytes==0x200000 && queries==1);
        CHECK(log.resolve(memory_handle(0x7770),&bytes)==0 && bytes==0 && queries==2);
        CHECK(log.resolve(VK_NULL_HANDLE,&bytes)==0 && queries==2);
        CHECK(log.resolve(known,nullptr)==0 && queries==2);
        // Not started: no engine, so tracking is S_FALSE and the enable callback's calls do nothing.
        CHECK(!log.active() && log.track(nullptr,1)==S_FALSE);
        log.set_reporting(true); log.report();
        const BC250_DXVK_MEMORY_EVENT e=event(BC250_DXVK_MEMORY_EVENT_MAP,0,1,0);
        log.events(&e,1);                                 // no log: nothing to write to
    }
    std::printf("engine-memory-log-test: all checks passed\n");
    return 0;
}
