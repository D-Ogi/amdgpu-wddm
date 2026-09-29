// SPDX-License-Identifier: MIT
#include "queue-engine.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <thread>
#include <atomic>

namespace {
struct Fixture;
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
    D3D12DDI_HRTCOMMANDQUEUE rt(unsigned index) { return {&runtime[index]}; }
};
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

int main() {
    const auto args = descriptor();
    {
        Fixture f;
        native12::QueueEngineSlot a{}, b{};
        assert(f.registry.create(args, f.rt(0), a) == S_OK);
        assert(f.registry.create(args, f.rt(1), b) == S_OK);
        assert(a.cookie != b.cookie && a.serial != b.serial && f.creates == 2 && f.engine_creates == 2);
        assert(a.owner == &f.device && b.owner == &f.device && f.registry.owns(a) && f.registry.owns(b));
        BindingProbe probe{&f, native12::QueueEngineState::Live};
        native12::Device foreign;
        assert(f.registry.with_binding(reinterpret_cast<void*>(UINT_PTR{1}), f.device, check_binding, &probe) == E_INVALIDARG);
        assert(f.registry.with_binding(a.cookie, foreign, check_binding, &probe) == E_INVALIDARG);
        assert(f.registry.with_binding(nullptr, f.device, check_binding, &probe) == E_INVALIDARG);
        HANDLE context = nullptr;
        assert(f.registry.present_context(a, &context) == S_OK && context == f.runtime[0].context);
        assert(f.registry.present_context(b, &context) == S_OK && context == f.runtime[1].context);
        assert(f.registry.present_context(a, nullptr) == E_INVALIDARG);
        auto other = a; other.owner = &foreign;
        assert(f.registry.present_context(other, &context) == E_INVALIDARG && !context);
        other = a; ++other.serial;
        assert(f.registry.present_context(other, &context) == E_INVALIDARG && !context);
        f.reentrant_destroy = &a;
        assert(f.registry.execute(a, 0, nullptr) == S_OK && a.cookie);
        f.reentrant_destroy = nullptr;
        assert(f.registry.execute(b, 0, nullptr) == S_OK && f.executes == 2);
        auto stale = a; ++stale.serial;
        assert(f.registry.execute(stale, 0, nullptr) == E_INVALIDARG);
        unsigned unresolved = 99;
        assert(f.registry.discard_retired_metadata(unresolved) == E_PENDING && !unresolved);
        const auto gone = a;
        assert(f.registry.destroy(a) == S_OK && !a.cookie && !a.serial && f.closes == 1);
        context = f.runtime[1].context;
        assert(f.registry.present_context(gone, &context) == E_INVALIDARG && !context);
        assert(f.registry.present_context(a, &context) == E_INVALIDARG && !context);
        assert(f.registry.destroy(b) == S_OK && f.closes == 2 && f.engine_destroys == 2);
        assert(f.registry.destroy(b) == S_OK && f.closes == 2);
        assert(f.registry.discard_retired_metadata(unresolved) == S_OK && !unresolved);
    }
    {
        Fixture f;
        native12::QueueEngineSlot slot{};
        f.fail_create = true;
        assert(f.registry.create(args, f.rt(0), slot) == E_OUTOFMEMORY && !slot.cookie && !f.engine_creates);
        f.fail_create = false; f.fail_engine = true;
        assert(f.registry.create(args, f.rt(0), slot) == E_OUTOFMEMORY && !slot.cookie && f.closes == 1);
        unsigned unresolved{};
        assert(f.registry.discard_retired_metadata(unresolved) == S_OK && !f.device.lost);
    }
    {
        Fixture f;
        native12::QueueEngineSlot slot{};
        f.fail_engine = true; f.fail_close = true;
        assert(f.registry.create(args, f.rt(0), slot) == E_OUTOFMEMORY && !slot.cookie);
        assert(f.device.lost && f.closes == 1 && f.runtime[0].live);
        unsigned unresolved{};
        assert(f.registry.discard_retired_metadata(unresolved) == S_FALSE && unresolved == 1 && f.closes == 1);
    }
    {
        Fixture f;
        native12::QueueEngineSlot slot{};
        assert(f.registry.create(args, f.rt(0), slot) == S_OK);
        f.fail_proof = true; // Engine remains healthy; missing close proof still forbids native close.
        assert(f.registry.destroy(slot) == E_FAIL && !slot.cookie && f.device.lost);
        assert(!f.engine_lost && !f.closes && f.engine_destroys == 1);
        unsigned unresolved{};
        assert(f.registry.discard_retired_metadata(unresolved) == S_FALSE && unresolved == 1);
    }
    {
        Fixture f;
        native12::QueueEngineSlot slot{};
        assert(f.registry.create(args, f.rt(0), slot) == S_OK);
        f.fail_close = true;
        assert(f.registry.destroy(slot) == E_FAIL && !slot.cookie && f.device.lost);
        assert(f.closes == 1 && f.engine_destroys == 1);
        unsigned unresolved{};
        assert(f.registry.discard_retired_metadata(unresolved) == S_FALSE && unresolved == 1 && f.closes == 1);
    }
    for (bool execute_loss : {false, true}) {
        Fixture f;
        native12::QueueEngineSlot slot{};
        assert(f.registry.create(args, f.rt(0), slot) == S_OK);
        if (execute_loss) {
            f.fail_execute = true;
            assert(f.registry.execute(slot, 0, nullptr) == E_FAIL && f.device.lost);
        } else f.lose_destroy = true;
        void* cookie = slot.cookie;
        assert(f.registry.destroy(slot) == D3DDDIERR_DEVICEREMOVED && !slot.cookie);
        assert(f.engine_destroys == 1 && f.closes == 0 && f.runtime[0].live);
        BindingProbe probe{&f, native12::QueueEngineState::Live};
        assert(f.registry.with_binding(cookie, f.device, check_binding, &probe) == E_INVALIDARG);
        unsigned unresolved{};
        assert(f.registry.discard_retired_metadata(unresolved) == S_FALSE && unresolved == 1 && !f.closes);
    }
    {
        Fixture f;
        native12::QueueEngineSlot slot{};
        assert(f.registry.create(args, f.rt(0), slot) == S_OK);
        std::atomic<bool> entered{false}, leave{false};
        struct Pin { std::atomic<bool>* entered; std::atomic<bool>* leave; } pin{&entered, &leave};
        std::thread binder([&] {
            assert(f.registry.with_binding(slot.cookie, f.device,
                [](void* user, const native12::QueueBindingView*) -> HRESULT {
                    auto p = static_cast<Pin*>(user); p->entered->store(true);
                    while (!p->leave->load()) std::this_thread::yield();
                    return S_OK;
                }, &pin) == S_OK);
        });
        while (!entered.load()) std::this_thread::yield();
        assert(f.registry.destroy(slot) == E_PENDING && slot.cookie && !f.engine_destroys);
        leave.store(true); binder.join();
        assert(f.registry.destroy(slot) == S_OK && f.engine_destroys == 1 && f.closes == 1);
    }
    std::puts("PASSED: two bound engine queues, execution, retirement before context close, retained loss, validated cookie pins; native fence transport unimplemented");
}
