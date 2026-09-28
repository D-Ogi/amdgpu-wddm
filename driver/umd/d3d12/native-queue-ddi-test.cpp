// SPDX-License-Identifier: MIT
#include "native-queue-ddi.h"
#include "fence-ddi.h"
#include "device-engine.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <thread>
#include <atomic>

namespace {
struct Fixture;
void register_fixture(Fixture*);
void unregister_fixture(Fixture*);
struct RuntimeQueue {
    Fixture* fixture{};
    HANDLE context{};
    bool live{};
};
struct FakeQueue {
    Fixture* fixture;
    void* cookie;
    RuntimeQueue* runtime;
};
HRESULT make_engine(engine_ddi::DeviceContext*, const BC250_VKD3D_COMMAND_QUEUE_DESC*, void*,
    engine_ddi::EngineQueue**) noexcept;
HRESULT execute_engine(engine_ddi::EngineQueue*, UINT, const D3D12DDI_HCOMMANDLIST*) noexcept;
HRESULT close_engine(engine_ddi::EngineQueue**) noexcept;
HRESULT engine_health(void*) noexcept;
struct Fixture {
    native12::Device device;
    native12::QueueEngineRegistry registry;
    RuntimeQueue runtime[2];
    unsigned creates{}, closes{}, engine_creates{}, executes{}, engine_destroys{}, errors{};
    bool fail_create{}, fail_engine{}, fail_close{}, lose_destroy{}, fail_execute{}, engine_lost{}, fail_proof{};
    native12::QueueEngineSlot* reentrant_destroy{};
    Fixture() : registry(device, reinterpret_cast<engine_ddi::DeviceContext*>(this),
        {make_engine, execute_engine, close_engine, engine_health, this}) {
        register_fixture(this);
        runtime[0] = {this, reinterpret_cast<HANDLE>(UINT_PTR{0x100000011}), false};
        runtime[1] = {this, reinterpret_cast<HANDLE>(UINT_PTR{0x200000022}), false};
        device.runtime.handle = this;
        device.callbacks.pfnSetErrorCb = [](D3D10DDI_HRTDEVICE h, HRESULT hr) {
            auto f = static_cast<Fixture*>(h.handle);
            assert(f->device.lost.load() && hr == D3DDDIERR_DEVICEREMOVED); ++f->errors;
        };
        device.callbacks.pfnCreateContextVirtualCb = [](D3D12DDI_HRTCOMMANDQUEUE h, D3DDDICB_CREATECONTEXTVIRTUAL* a) -> HRESULT {
            auto r = static_cast<RuntimeQueue*>(h.handle); auto f = r->fixture;
            ++f->creates; assert(!r->live && !a->hContext && a->NodeOrdinal == 0 && a->EngineAffinity == 1);
            assert(a->PrivateDriverDataSize == sizeof(bc250_umd_context_private));
            const auto blob = static_cast<const bc250_umd_context_private*>(a->pPrivateDriverData);
            assert(blob->magic == BC250_UMD_CONTEXT_MAGIC && blob->ip_type == AMDGPU_HW_IP_GFX);
            if (f->fail_create) return E_OUTOFMEMORY;
            r->live = true; a->hContext = r->context; return S_OK;
        };
        device.callbacks.pfnDestroyContextCb = [](D3D12DDI_HRTCOMMANDQUEUE h, const D3DDDICB_DESTROYCONTEXT* a) -> HRESULT {
            auto r = static_cast<RuntimeQueue*>(h.handle); auto f = r->fixture;
            ++f->closes; assert(r->live && a->hContext == r->context);
            if (f->fail_close) return E_FAIL;
            r->live = false; return S_OK;
        };
    }
    ~Fixture() { unregister_fixture(this); }
    D3D12DDI_HRTCOMMANDQUEUE rt(unsigned index) { return {&runtime[index]}; }
};
Fixture* fixtures[4]{};
void register_fixture(Fixture* f) { for (auto& entry : fixtures) if (!entry) {entry=f; return;} assert(false); }
void unregister_fixture(Fixture* f) { for (auto& entry : fixtures) if (entry==f) {entry=nullptr; return;} assert(false); }
Fixture* lookup(native12::Device& device) {for(auto f:fixtures)if(f && &f->device==&device)return f;return nullptr;}
HRESULT engine_health(void* user) noexcept {
    return static_cast<Fixture*>(user)->engine_lost ? D3DDDIERR_DEVICEREMOVED : S_OK;
}
struct BindingProbe {
    Fixture* fixture;
    native12::QueueEngineState state;
    RuntimeQueue* runtime{};
};
HRESULT APIENTRY check_binding(void* user, const native12::QueueBindingView* view) {
    auto p = static_cast<BindingProbe*>(user);
    p->runtime = static_cast<RuntimeQueue*>(view->runtime_queue.handle);
    assert(p->runtime->fixture == p->fixture && p->runtime->live);
    assert(view->context == p->runtime->context && view->state == p->state);
    return S_OK;
}
HRESULT make_engine(engine_ddi::DeviceContext* context, const BC250_VKD3D_COMMAND_QUEUE_DESC* desc,
    void* cookie, engine_ddi::EngineQueue** output) noexcept {
    auto f = reinterpret_cast<Fixture*>(context);
    assert(!*output && desc->Size == sizeof(*desc) && !desc->Flags && !desc->Priority);
    assert(desc->Type == D3D12_COMMAND_LIST_TYPE_DIRECT);
    ++f->engine_creates;
    BindingProbe probe{f, native12::QueueEngineState::Creating};
    assert(f->registry.with_binding(cookie, f->device, check_binding, &probe) == S_OK);
    if (f->fail_engine) return E_OUTOFMEMORY;
    *output = reinterpret_cast<engine_ddi::EngineQueue*>(new FakeQueue{f, cookie, probe.runtime});
    return S_OK;
}
HRESULT execute_engine(engine_ddi::EngineQueue* engine, UINT count, const D3D12DDI_HCOMMANDLIST* lists) noexcept {
    auto q = reinterpret_cast<FakeQueue*>(engine); auto f = q->fixture;
    assert(!count || lists); ++f->executes;
    BindingProbe probe{f, native12::QueueEngineState::Executing};
    assert(f->registry.with_binding(q->cookie, f->device, check_binding, &probe) == S_OK);
    if (f->reentrant_destroy) assert(f->registry.destroy(*f->reentrant_destroy) == E_PENDING);
    return f->fail_execute ? E_FAIL : S_OK;
}
HRESULT close_engine(engine_ddi::EngineQueue** engine) noexcept {
    auto q = reinterpret_cast<FakeQueue*>(*engine); auto f = q->fixture;
    ++f->engine_destroys;
    // The runtime context still exists during engine unbinding/retirement.
    assert(q->runtime->live);
    BindingProbe probe{f, native12::QueueEngineState::Destroying};
    assert(f->registry.with_binding(q->cookie, f->device, check_binding, &probe) == S_OK);
    if (f->lose_destroy) f->engine_lost = true; // No shell callback: health hook must catch this.
    delete q; *engine = nullptr;
    return f->lose_destroy ? D3DDDIERR_DEVICEREMOVED : f->fail_proof ? E_FAIL : S_OK;
}
D3D12DDIARG_CREATECOMMANDQUEUE_0050 descriptor() {
    D3D12DDIARG_CREATECOMMANDQUEUE_0050 d{};
    d.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D | D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE | D3D12DDI_COMMAND_QUEUE_FLAG_COPY;
    return d;
}
} // namespace

// Test-only implementations of root-owned DeviceEngine accessors. The actual
// QueueEngineRegistry and native DDI implementation are linked unchanged.
namespace native12 {
engine_ddi::DeviceContext* engine_context(Device& device) noexcept {
    return reinterpret_cast<engine_ddi::DeviceContext*>(lookup(device));
}
QueueEngineRegistry* engine_queues(Device& device) noexcept {
    auto f=lookup(device); return f ? &f->registry : nullptr;
}
}
int main() {
    D3D12DDI_DEVICE_FUNCS_CORE_0088 core{};
    native12::install_native_queue_entries(core); native12::install_fence_entries(core);
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue{};
    D3D12DDI_EXTENDED_FEATURES_FUNCS_0021 ext{};
    assert(native12::fill_native_queue_table(&queue)==S_OK);
    assert(native12::fill_native_extended_table(&ext)==S_OK);
    assert(!queue.pfnUnused && !queue.pfnUnused2 && queue.pfnExecuteCommandLists &&
        queue.pfnSignalFence && queue.pfnWaitForFence && queue.pfnUpdateTileMappings && queue.pfnCopyTileMappings);
    assert(ext.pfnGetSupportedExtendedFeatures && ext.pfnGetSupportedExtendedFeatureVersions &&
        ext.pfnEnableExtendedFeature && ext.pfnSetExtendedFeatureCallbacks);
    auto unchanged=queue;
    assert(native12::fill_native_queue_table(&queue,sizeof(queue)-8)==E_INVALIDARG && !std::memcmp(&queue,&unchanged,sizeof(queue)));
    auto ext_unchanged=ext;
    assert(native12::fill_native_extended_table(&ext,sizeof(ext)-8)==E_INVALIDARG && !std::memcmp(&ext,&ext_unchanged,sizeof(ext)));
    const auto args=descriptor();
    {
        Fixture f;
        D3D12DDI_HDEVICE device{&f.device};
        assert(core.pfnCalcPrivateCommandQueueSize(device,&args)==sizeof(native12::QueueEngineSlot));
        alignas(native12::QueueEngineSlot) unsigned char storage[sizeof(native12::QueueEngineSlot)+16];
        std::memset(storage,0xcd,sizeof(storage));
        D3D12DDI_HCOMMANDQUEUE q{storage};
        assert(core.pfnCreateCommandQueue(device,&args,q,f.rt(0))==S_OK);
        assert(native12::resolve_queue_device(q)==&f.device);
        auto slot=reinterpret_cast<native12::QueueEngineSlot*>(storage);
        assert(slot->owner==&f.device && f.registry.owns(*slot));
        for(size_t i=sizeof(*slot);i<sizeof(storage);++i)assert(storage[i]==0xcd);
        D3D12DDI_HCOMMANDLIST list{};queue.pfnExecuteCommandLists(q,1,&list);
        assert(f.executes==1 && !f.device.lost);
        D3D12DDI_FENCE placement{};placement.FenceValue.BaseAddress=0x100002000;
        placement.FenceMonitoredValue.BaseAddress=0x100003000;
        D3D12DDIARG_CREATE_FENCE create_fence{1,&placement};
        alignas(native12::FenceState) unsigned char fence_storage[sizeof(native12::FenceState)];
        D3D12DDI_HFENCE fence{fence_storage};
        assert(core.pfnCreateFence(device,fence,&create_fence)==S_OK);
        D3D12DDIARG_FENCE_OPERATION signal{fence,71,99};queue.pfnSignalFence(q,&signal);
        assert(signal.PhysicalAdapterMask==1 && signal.Value==71 && signal.Fence.pDrvPrivate==fence_storage);
        assert(f.executes==1 && f.engine_creates==1 && f.engine_destroys==0 && !f.device.lost);
        auto stale=*slot;++stale.serial;assert(!native12::resolve_queue_device({&stale}));
        core.pfnDestroyFence(device,fence);core.pfnDestroyCommandQueue(device,q);
        assert(f.engine_destroys==1 && f.closes==1 && !f.runtime[0].live && !f.device.lost);
        unsigned unresolved{};assert(f.registry.discard_retired_metadata(unresolved)==S_OK);
    }
    {
        Fixture f,other;native12::QueueEngineSlot slot{};
        D3D12DDI_HDEVICE device{&f.device};D3D12DDI_HCOMMANDQUEUE q{&slot};
        assert(core.pfnCreateCommandQueue(device,&args,q,f.rt(0))==S_OK);
        auto forged=slot;forged.owner=&other.device;assert(!native12::resolve_queue_device({&forged}));
        native12::FenceState foreign{&other.device,{}};
        D3D12DDIARG_FENCE_OPERATION signal{{&foreign},2,77};queue.pfnSignalFence(q,&signal);
        assert(signal.PhysicalAdapterMask==0 && f.device.lost && !other.device.lost && !f.executes);
        core.pfnDestroyCommandQueue(device,q);assert(!f.closes && f.engine_destroys==1);
        unsigned unresolved{};assert(f.registry.discard_retired_metadata(unresolved)==S_FALSE && unresolved==1);
    }
    {
        Fixture f;native12::QueueEngineSlot slot{};
        D3D12DDI_HDEVICE device{&f.device};D3D12DDI_HCOMMANDQUEUE q{&slot};
        assert(core.pfnCreateCommandQueue(device,&args,q,f.rt(0))==S_OK);
        D3D12DDIARG_FENCE_OPERATION wait{{},17,88};queue.pfnWaitForFence(q,&wait);
        assert(wait.PhysicalAdapterMask==0 && f.device.lost && !f.executes);
        core.pfnDestroyCommandQueue(device,q);
        unsigned unresolved{};assert(f.registry.discard_retired_metadata(unresolved)==S_FALSE && unresolved==1);
    }
    {
        Fixture f;D3D12DDI_HDEVICE device{&f.device};
        UINT32 count=1;auto feature=D3D12DDI_FEATURE_0020_VIDEO;
        assert(ext.pfnGetSupportedExtendedFeatures(device,&count,&feature)==S_OK && !count);
        assert(feature==D3D12DDI_FEATURE_0020_VIDEO && !f.device.lost);
        count=1;UINT32 version=123;
        assert(ext.pfnGetSupportedExtendedFeatureVersions(device,feature,&count,&version)==E_NOTIMPL && !count && version==123);
        assert(ext.pfnEnableExtendedFeature(device,feature,version)==E_NOTIMPL);
        assert(ext.pfnSetExtendedFeatureCallbacks(device,D3D12DDI_TABLE_TYPE_DEVICE_CORE,&core,sizeof(core))==E_NOTIMPL);
        assert(!f.device.lost && !f.engine_creates && !f.creates);
    }
    std::puts("PASSED: native typed queue create/execute/Signal mask/close, actual registry ownership, foreign fence refusal, deferred Wait and empty extended features; host-only");
}
