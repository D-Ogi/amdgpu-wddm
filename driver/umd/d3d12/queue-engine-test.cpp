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
HRESULT update_engine(engine_ddi::EngineQueue*, D3D12DDI_HRESOURCE, UINT, const D3D12DDI_TILED_RESOURCE_COORDINATE*,
    const D3D12DDI_TILE_REGION_SIZE*, D3D12DDI_HHEAP, UINT, const D3D12DDI_TILE_RANGE_FLAGS*, const UINT*, const UINT*,
    D3D12DDI_TILE_MAPPING_FLAGS) noexcept;
HRESULT copy_engine(engine_ddi::EngineQueue*, D3D12DDI_HRESOURCE, const D3D12DDI_TILED_RESOURCE_COORDINATE*,
    D3D12DDI_HRESOURCE, const D3D12DDI_TILED_RESOURCE_COORDINATE*, const D3D12DDI_TILE_REGION_SIZE*,
    D3D12DDI_TILE_MAPPING_FLAGS) noexcept;
// What a tile call must carry to the engine: the slot's own arguments, null arrays included.
struct TileArguments {
    D3D12DDI_HRESOURCE resource{}, source{};
    const D3D12DDI_TILED_RESOURCE_COORDINATE* starts{}; const D3D12DDI_TILED_RESOURCE_COORDINATE* source_start{};
    const D3D12DDI_TILE_REGION_SIZE* sizes{};
    D3D12DDI_HHEAP heap{}; UINT regions{}, ranges{};
    const D3D12DDI_TILE_RANGE_FLAGS* range_flags{}; const UINT* heap_starts{}; const UINT* counts{};
    D3D12DDI_TILE_MAPPING_FLAGS flags{};
};
struct Fixture {
    native12::Device device;
    native12::QueueEngineRegistry registry;
    RuntimeQueue runtime[2];
    unsigned creates{}, closes{}, engine_creates{}, executes{}, engine_destroys{}, errors{};
    bool fail_create{}, fail_engine{}, fail_close{}, lose_destroy{}, fail_execute{}, engine_lost{}, fail_proof{};
    native12::QueueEngineSlot* reentrant_destroy{};
    TileArguments expected{};
    unsigned tile_updates{}, tile_copies{};
    void (*inside_tiles)(Fixture&, void* cookie){};
    native12::QueueEngineSlot* tile_slot{};
    Fixture() : registry(device, reinterpret_cast<engine_ddi::DeviceContext*>(this),
        {make_engine, execute_engine, close_engine, engine_health, this, update_engine, copy_engine}) {
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
HRESULT update_engine(engine_ddi::EngineQueue* engine, D3D12DDI_HRESOURCE resource, UINT regions,
    const D3D12DDI_TILED_RESOURCE_COORDINATE* starts, const D3D12DDI_TILE_REGION_SIZE* sizes, D3D12DDI_HHEAP heap,
    UINT ranges, const D3D12DDI_TILE_RANGE_FLAGS* range_flags, const UINT* heap_starts, const UINT* counts,
    D3D12DDI_TILE_MAPPING_FLAGS flags) noexcept {
    auto q = reinterpret_cast<FakeQueue*>(engine); auto f = q->fixture; const auto& e = f->expected;
    assert(resource.pDrvPrivate == e.resource.pDrvPrivate && regions == e.regions && starts == e.starts &&
        sizes == e.sizes && heap.pDrvPrivate == e.heap.pDrvPrivate && ranges == e.ranges &&
        range_flags == e.range_flags && heap_starts == e.heap_starts && counts == e.counts && flags == e.flags);
    ++f->tile_updates;
    if (f->inside_tiles) f->inside_tiles(*f, q->cookie);
    return f->fail_execute ? E_FAIL : S_OK;
}
HRESULT copy_engine(engine_ddi::EngineQueue* engine, D3D12DDI_HRESOURCE dst,
    const D3D12DDI_TILED_RESOURCE_COORDINATE* dst_start, D3D12DDI_HRESOURCE src,
    const D3D12DDI_TILED_RESOURCE_COORDINATE* src_start, const D3D12DDI_TILE_REGION_SIZE* size,
    D3D12DDI_TILE_MAPPING_FLAGS flags) noexcept {
    auto q = reinterpret_cast<FakeQueue*>(engine); auto f = q->fixture; const auto& e = f->expected;
    assert(dst.pDrvPrivate == e.resource.pDrvPrivate && dst_start == e.starts && src.pDrvPrivate == e.source.pDrvPrivate &&
        src_start == e.source_start && size == e.sizes && flags == e.flags);
    ++f->tile_copies;
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
    {   // The tile mappings: one operation of the queue, admitted as an execute is.
        Fixture f; native12::Device foreign;
        native12::QueueEngineSlot slot{};
        assert(f.registry.create(args, f.rt(0), slot) == S_OK);
        int resource{}, source{}, heap{};
        const D3D12DDI_TILED_RESOURCE_COORDINATE start{1, 2, 3, 4}, source_start{};
        const D3D12DDI_TILE_REGION_SIZE size{4, FALSE, 0, 0, 0};
        const D3D12DDI_TILE_RANGE_FLAGS range = D3D12DDI_TILE_RANGE_FLAG_NONE; const UINT heap_start = 7, count = 4;
        f.expected = {{&resource}, {&source}, &start, &source_start, &size, {&heap}, 1, 1, &range, &heap_start, &count,
            D3D12DDI_TILE_MAPPING_FLAG_NO_HAZARD};
        auto update = [&](const native12::QueueEngineSlot& s) {
            const auto& e = f.expected;
            return f.registry.update_tiles(s, e.resource, e.regions, e.starts, e.sizes, e.heap, e.ranges, e.range_flags,
                e.heap_starts, e.counts, e.flags);
        };
        auto copy = [&](const native12::QueueEngineSlot& s) {
            const auto& e = f.expected;
            return f.registry.copy_tiles(s, e.resource, e.starts, e.source, e.source_start, e.sizes, e.flags);
        };
        assert(update(slot) == S_OK && copy(slot) == S_OK && f.tile_updates == 1 && f.tile_copies == 1);
        // Null pointers are handed on as null. The substitute accepts anything: whether the engine takes
        // this mapping is not shown here.
        const auto full = f.expected;
        f.expected = {{&resource}, {}, nullptr, nullptr, nullptr, {}, 1, 1, &range, nullptr, nullptr, D3D12DDI_TILE_MAPPING_FLAG_NONE};
        assert(update(slot) == S_OK && f.tile_updates == 2);
        f.expected = full;
        auto stale = slot; ++stale.serial; assert(update(stale) == E_INVALIDARG && copy(stale) == E_INVALIDARG);
        auto other = slot; other.owner = &foreign; assert(update(other) == E_INVALIDARG && copy(other) == E_INVALIDARG);
        assert(f.tile_updates == 2 && f.tile_copies == 1 && !f.device.lost);
        // A pinned queue admits no operation.
        struct Pinned { decltype(update)* update; decltype(copy)* copy; native12::QueueEngineSlot* slot; } pinned{&update, &copy, &slot};
        assert(f.registry.with_binding(slot.cookie, f.device, [](void* user, const native12::QueueBindingView*) -> HRESULT {
            auto p = static_cast<Pinned*>(user);
            return (*p->update)(*p->slot) == E_PENDING && (*p->copy)(*p->slot) == E_PENDING ? S_OK : E_FAIL;
        }, &pinned) == S_OK && f.tile_updates == 2 && f.tile_copies == 1 && !f.device.lost);
        // Inside the operation: a second one and the destroy wait, an ordinary hosted binding passes.
        f.tile_slot = &slot;
        f.inside_tiles = [](Fixture& g, void* cookie) {
            const auto& e = g.expected; const unsigned before = g.tile_updates;
            assert(g.registry.update_tiles(*g.tile_slot, e.resource, e.regions, e.starts, e.sizes, e.heap, e.ranges,
                e.range_flags, e.heap_starts, e.counts, e.flags) == E_PENDING && g.tile_updates == before);
            assert(g.registry.execute(*g.tile_slot, 0, nullptr) == E_PENDING && !g.executes);
            assert(g.registry.destroy(*g.tile_slot) == E_PENDING && g.tile_slot->cookie);
            BindingProbe probe{&g, native12::QueueEngineState::Executing};
            assert(g.registry.with_binding(cookie, g.device, check_binding, &probe) == S_OK);
        };
        assert(update(slot) == S_OK && f.tile_updates == 3 && !f.device.lost);
        f.inside_tiles = nullptr;
        assert(f.registry.execute(slot, 0, nullptr) == S_OK && f.executes == 1);
        // The engine's health after the call counts, as after an execute.
        f.engine_lost = true;
        assert(update(slot) == D3DDDIERR_DEVICEREMOVED && f.tile_updates == 4 && f.device.lost && f.errors == 1);
        assert(copy(slot) == D3DDDIERR_DEVICEREMOVED && f.tile_copies == 1);
    }
    for (bool copies : {false, true}) {   // a failed engine call removes the device
        Fixture f; native12::QueueEngineSlot slot{};
        assert(f.registry.create(args, f.rt(0), slot) == S_OK);
        f.fail_execute = true;
        const HRESULT hr = copies ? f.registry.copy_tiles(slot, {}, nullptr, {}, nullptr, nullptr, D3D12DDI_TILE_MAPPING_FLAG_NONE)
            : f.registry.update_tiles(slot, {}, 0, nullptr, nullptr, {}, 0, nullptr, nullptr, nullptr, D3D12DDI_TILE_MAPPING_FLAG_NONE);
        assert(hr == E_FAIL && f.device.lost && f.errors == 1 && f.tile_updates + f.tile_copies == 1);
    }
    {   // an engine without tile mappings refuses before admission
        native12::Device device; native12::QueueEngineSlot slot{&device, 1, &device};
        native12::QueueEngineRegistry bare(device, nullptr, {make_engine, execute_engine, close_engine, engine_health, nullptr});
        assert(bare.update_tiles(slot, {}, 0, nullptr, nullptr, {}, 0, nullptr, nullptr, nullptr, D3D12DDI_TILE_MAPPING_FLAG_NONE) == E_NOTIMPL);
        assert(bare.copy_tiles(slot, {}, nullptr, {}, nullptr, nullptr, D3D12DDI_TILE_MAPPING_FLAG_NONE) == E_NOTIMPL && !device.lost);
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
